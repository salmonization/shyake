// Package api is the HTTP interface of the server. Routes, status codes,
// and response bodies match the Worker in server/cf, so any client works
// against either implementation (SPEC §5).
package api

import (
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/netip"
	"runtime/debug"
	"strings"
	"sync"
	"time"

	"golang.org/x/time/rate"

	"github.com/salmonization/shyake/server/go/internal/config"
	"github.com/salmonization/shyake/server/go/internal/federation"
	"github.com/salmonization/shyake/server/go/internal/store"
	"github.com/salmonization/shyake/server/go/internal/ttlset"
)

type Server struct {
	cfg     config.Config
	db      store.Store
	keys    *federation.Keys
	fed     *federation.Client
	seen    *ttlset.Set // signatures already used
	spent   *ttlset.Set // PoW tokens already used
	limiter *limiter
	version *versionCache
	log     *slog.Logger
	now     func() time.Time
}

func New(cfg config.Config, db store.Store, keys *federation.Keys,
	fed *federation.Client, log *slog.Logger) *Server {
	return &Server{
		cfg:  cfg,
		db:   db,
		keys: keys,
		fed:  fed,
		// a signature is valid for 600 s (±300); a PoW token for up to
		// three calendar days (see protocol.VerifyPoW)
		seen:    ttlset.New(10*time.Minute, 250_000),
		spent:   ttlset.New(72*time.Hour, 250_000),
		limiter: newLimiter(rate.Limit(cfg.RateLimit), cfg.RateBurst),
		version: newVersionCache(),
		log:     log,
		now:     time.Now,
	}
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", s.health)
	mux.HandleFunc("GET /api/version", s.serverVersion)
	mux.HandleFunc("GET /api/client/version", s.clientVersion)
	mux.HandleFunc("POST /api/register", s.register)
	mux.HandleFunc("GET /api/pubkey/{username}", s.pubkey)
	mux.HandleFunc("POST /api/mail", s.sendMail)
	mux.HandleFunc("GET /api/mail", s.listMail)
	mux.HandleFunc("GET /api/mail/{id}", s.getMail)
	mux.HandleFunc("DELETE /api/mail/{id}", s.burnMail)
	mux.HandleFunc("POST /api/block", s.block)
	mux.HandleFunc("DELETE /api/block", s.unblock)
	mux.HandleFunc("GET /api/block", s.listBlocks)
	mux.HandleFunc("POST /api/rotate", s.rotate)
	mux.HandleFunc("DELETE /api/destroy", s.destroy)
	return s.recoverPanics(s.logRequests(s.rateLimit(mux)))
}

// ------------------------------------------------------------ middleware

// logRequests logs the route pattern, never the concrete path: paths
// carry usernames and mail ids, and client addresses are not logged
// at all.
func (s *Server) logRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		sw := &statusWriter{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(sw, r)
		route := r.Pattern
		if route == "" {
			route = "unmatched"
		}
		s.log.Info("request", "route", route, "status", sw.status,
			"ms", time.Since(start).Milliseconds())
	})
}

type statusWriter struct {
	http.ResponseWriter
	status int
}

func (w *statusWriter) WriteHeader(code int) {
	w.status = code
	w.ResponseWriter.WriteHeader(code)
}

func (s *Server) recoverPanics(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if v := recover(); v != nil {
				if v == http.ErrAbortHandler {
					panic(v)
				}
				s.log.Error("panic", "err", v, "stack", string(debug.Stack()))
				fail(w, http.StatusInternalServerError, "Internal error")
			}
		}()
		next.ServeHTTP(w, r)
	})
}

func (s *Server) rateLimit(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !s.limiter.allow(s.clientIP(r), s.now()) {
			w.Header().Set("Retry-After", "1")
			fail(w, http.StatusTooManyRequests, "Too many requests")
			return
		}
		next.ServeHTTP(w, r)
	})
}

// clientIP is the peer address, or, when the peer is a trusted proxy,
// the rightmost untrusted address in X-Forwarded-For. Entries to the
// left of that one are client-supplied and prove nothing.
func (s *Server) clientIP(r *http.Request) netip.Addr {
	ap, err := netip.ParseAddrPort(r.RemoteAddr)
	if err != nil {
		return netip.Addr{}
	}
	ip := ap.Addr().Unmap()
	if !s.trusted(ip) {
		return ip
	}
	hops := strings.Split(strings.Join(r.Header.Values("X-Forwarded-For"), ","), ",")
	for i := len(hops) - 1; i >= 0; i-- {
		a, err := netip.ParseAddr(strings.TrimSpace(hops[i]))
		if err != nil {
			break
		}
		ip = a.Unmap()
		if !s.trusted(ip) {
			break
		}
	}
	return ip
}

func (s *Server) trusted(ip netip.Addr) bool {
	for _, p := range s.cfg.TrustedProxies {
		if p.Contains(ip) {
			return true
		}
	}
	return false
}

// limiter keeps a token bucket per client. IPv6 clients are keyed by
// their /64, the smallest block a host usually controls.
type limiter struct {
	mu        sync.Mutex
	r         rate.Limit
	burst     int
	buckets   map[netip.Addr]*bucket
	lastSweep time.Time
}

type bucket struct {
	l    *rate.Limiter
	used time.Time
}

func newLimiter(r rate.Limit, burst int) *limiter {
	return &limiter{r: r, burst: burst, buckets: make(map[netip.Addr]*bucket)}
}

func (l *limiter) allow(ip netip.Addr, now time.Time) bool {
	if ip.Is6() {
		p, _ := ip.Prefix(64)
		ip = p.Addr()
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	if now.Sub(l.lastSweep) > time.Minute {
		for k, b := range l.buckets {
			if now.Sub(b.used) > 10*time.Minute {
				delete(l.buckets, k)
			}
		}
		l.lastSweep = now
	}
	b, ok := l.buckets[ip]
	if !ok {
		b = &bucket{l: rate.NewLimiter(l.r, l.burst)}
		l.buckets[ip] = b
	}
	b.used = now
	return b.l.AllowN(now, 1)
}

// --------------------------------------------------------------- helpers

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

// fail answers {"error": msg}, the Worker's error shape.
func fail(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

type message struct {
	Message string `json:"message"`
	ID      string `json:"id,omitempty"`
}

// readBody reads at most limit bytes of the request body. It answers
// 413 itself when the body is larger.
func readBody(w http.ResponseWriter, r *http.Request, limit int64) ([]byte, bool) {
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, limit))
	var tooBig *http.MaxBytesError
	switch {
	case errors.As(err, &tooBig):
		fail(w, http.StatusRequestEntityTooLarge, "Payload too large")
		return nil, false
	case err != nil:
		fail(w, http.StatusBadRequest, "Invalid JSON")
		return nil, false
	}
	return body, true
}

func readJSON(w http.ResponseWriter, r *http.Request, limit int64, v any) bool {
	body, ok := readBody(w, r, limit)
	if !ok {
		return false
	}
	if err := json.Unmarshal(body, v); err != nil {
		fail(w, http.StatusBadRequest, "Invalid JSON")
		return false
	}
	return true
}

func (s *Server) internal(w http.ResponseWriter, what string, err error) {
	s.log.Error(what, "err", err)
	fail(w, http.StatusInternalServerError, "Database error")
}
