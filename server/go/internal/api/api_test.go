package api

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/cloudflare/circl/sign/mldsa/mldsa65"

	"github.com/salmonization/shyake/server/go/internal/config"
	"github.com/salmonization/shyake/server/go/internal/federation"
	"github.com/salmonization/shyake/server/go/internal/protocol"
	"github.com/salmonization/shyake/server/go/internal/store/sqlite"
)

const domain = "test.example"

type env struct {
	t   *testing.T
	srv *httptest.Server
	s   *Server
}

func newEnv(t *testing.T, tweak ...func(*config.Config)) *env {
	cfg, err := config.Load(func(k string) string {
		return map[string]string{"SHYAKE_INSTANCE_DOMAIN": domain, "SHYAKE_RATE_LIMIT": "1000",
			"SHYAKE_RATE_BURST": "1000"}[k]
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range tweak {
		f(&cfg)
	}
	db, err := sqlite.Open(context.Background(), filepath.Join(t.TempDir(), "api.db"))
	if err != nil {
		t.Fatal(err)
	}
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	client := federation.NewClient(true, "test")
	s := New(cfg, db, federation.NewKeys(client), client, log)
	srv := httptest.NewServer(s.Handler())
	t.Cleanup(func() { srv.Close(); db.Close() })
	return &env{t: t, srv: srv, s: s}
}

// user is a protocol-level client: what libshyake does, in Go.
type user struct {
	name       string
	sk         *mldsa65.PrivateKey
	sigB64     string
	kemB64, fp string
}

func newUser(name string) *user {
	pk, sk, _ := mldsa65.GenerateKey(rand.Reader)
	raw, _ := pk.MarshalBinary()
	kem := make([]byte, 1184)
	rand.Read(kem)
	sum := sha256.Sum256(kem)
	return &user{name: name, sk: sk, sigB64: base64.StdEncoding.EncodeToString(raw),
		kemB64: base64.StdEncoding.EncodeToString(kem), fp: hex.EncodeToString(sum[:])}
}

func (u *user) sign(msg []byte) string {
	sig := make([]byte, mldsa65.SignatureSize)
	mldsa65.SignTo(u.sk, msg, nil, true, sig)
	return base64.StdEncoding.EncodeToString(sig)
}

func pow(resource string) string {
	date := time.Now().UTC().Format("060102")
	salt := make([]byte, 6)
	rand.Read(salt)
	for c := 0; ; c++ {
		tok := fmt.Sprintf("1:20:%s:%s::%x:%x", date, resource, salt, c)
		if h := sha1.Sum([]byte(tok)); h[0] == 0 && h[1] == 0 && h[2]>>4 == 0 {
			return tok
		}
	}
}

func now() string { return strconv.FormatInt(time.Now().Unix(), 10) }

func (e *env) do(method, path string, body []byte, hdr http.Header) (int, map[string]any) {
	e.t.Helper()
	req, _ := http.NewRequest(method, e.srv.URL+path, bytes.NewReader(body))
	for k, v := range hdr {
		req.Header[k] = v
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	defer resp.Body.Close()
	var out map[string]any
	json.NewDecoder(resp.Body).Decode(&out)
	return resp.StatusCode, out
}

func (e *env) register(u *user) int {
	ts := now()
	body, _ := json.Marshal(map[string]string{"username": u.name, "kem_pubkey": u.kemB64,
		"sig_pubkey": u.sigB64, "timestamp": ts, "pow": pow(u.name),
		"signature": u.sign(protocol.RegisterMessage(u.name, u.kemB64, u.sigB64, ts))})
	code, _ := e.do("POST", "/api/register", body, nil)
	return code
}

func (u *user) headers(method, path string) http.Header {
	ts := now()
	return u.headersFor(ts, protocol.HeaderMessage(method, path, u.name, ts))
}

// bodyHeaders signs a request with a body (protocol 2).
func (u *user) bodyHeaders(method, path string, body []byte) http.Header {
	ts := now()
	return u.headersFor(ts, protocol.HeaderBodyMessage(method, path, u.name, ts, body))
}

func (u *user) headersFor(ts string, msg []byte) http.Header {
	return http.Header{
		"X-Shyake-Username":  {u.name},
		"X-Shyake-Timestamp": {ts},
		"X-Shyake-Signature": {u.sign(msg)},
		"X-Shyake-Pow":       {pow(u.name)},
	}
}

func (e *env) authed(u *user, method, path string, body []byte) (int, map[string]any) {
	signed := path
	if i := strings.Index(path, "?"); i >= 0 && !strings.HasPrefix(path, "/api/mail?") {
		signed = path[:i]
	}
	if body != nil {
		return e.do(method, path, body, u.bodyHeaders(method, signed, body))
	}
	return e.do(method, path, body, u.headers(method, signed))
}

// mailBody builds a POST /api/mail body; sender and recipient are the
// strings the client would send.
func mailBody(from *user, sender, recipient, fp string) []byte {
	ts := now()
	subj, enc := "c3Vi", "Ym9keQ=="
	sig := from.sign(protocol.MailMessage(sender, recipient, fp, subj, enc, ts, 4))
	b, _ := json.Marshal(map[string]any{"sender": sender, "recipient": recipient,
		"recipient_kem_fingerprint": fp, "enc_subject": subj, "enc_body": enc,
		"timestamp": ts, "size": 4, "enc_key_sender": "ks", "enc_key_recipient": "kr",
		"signature": sig, "pow": pow(sender)})
	return b
}

func TestFullFlow(t *testing.T) {
	e := newEnv(t)
	alice, bobby := newUser("alice"), newUser("bobby")
	if c := e.register(alice); c != 201 {
		t.Fatalf("register alice: %d", c)
	}
	if c := e.register(bobby); c != 201 {
		t.Fatalf("register bobby: %d", c)
	}

	code, out := e.do("POST", "/api/mail", mailBody(alice, "alice", "bobby", bobby.fp), nil)
	if code != 201 || out["id"] == "" {
		t.Fatalf("send: %d %v", code, out)
	}
	id := out["id"].(string)

	code, out = e.authed(bobby, "GET", "/api/mail?type=inbox", nil)
	list := out["mail"].([]any)
	if code != 200 || len(list) != 1 {
		t.Fatalf("inbox: %d %v", code, out)
	}
	item := list[0].(map[string]any)
	// the C client reads these with ->valueint: they must be numbers
	if _, ok := item["timestamp"].(float64); !ok {
		t.Error("timestamp is not a JSON number")
	}
	if _, ok := item["enc_body"]; ok {
		t.Error("listing includes bodies")
	}

	code, out = e.authed(bobby, "GET", "/api/mail/"+id, nil)
	if code != 200 || out["enc_body"] != "Ym9keQ==" || out["signature"] == nil {
		t.Fatalf("fetch: %d %v", code, out)
	}
	if code, _ = e.authed(newUserRegistered(e, "carol"), "GET", "/api/mail/"+id, nil); code != 404 {
		t.Errorf("third party fetch: %d", code)
	}
	if code, _ = e.authed(bobby, "DELETE", "/api/mail/"+id, nil); code != 200 {
		t.Errorf("burn: %d", code)
	}

	code, out = e.do("GET", "/api/pubkey/alice@"+domain, nil, nil)
	if code != 200 || out["sig_pubkey"] != alice.sigB64 {
		t.Errorf("pubkey of qualified local user: %d", code)
	}
}

func newUserRegistered(e *env, name string) *user {
	u := newUser(name)
	if c := e.register(u); c != 201 {
		e.t.Fatalf("register %s: %d", name, c)
	}
	return u
}

func TestCaseVariantRegistration(t *testing.T) {
	e := newEnv(t)
	newUserRegistered(e, "alice")
	for _, n := range []string{"Alice", "ALICE"} {
		if c := e.register(newUser(n)); c != 409 {
			t.Errorf("register %s: %d, want 409", n, c)
		}
	}
	if c := e.register(newUser("Admin")); c != 403 {
		t.Errorf("reserved name in other case: %d", c)
	}
}

func TestReplayRejected(t *testing.T) {
	e := newEnv(t)
	bobby := newUserRegistered(e, "bobby")
	h := bobby.headers("GET", "/api/block")
	if c, _ := e.do("GET", "/api/block", nil, h); c != 200 {
		t.Fatalf("first request: %d", c)
	}
	if c, _ := e.do("GET", "/api/block", nil, h); c != 403 {
		t.Errorf("replayed request: %d, want 403", c)
	}
}

func TestPoWReuseAndBinding(t *testing.T) {
	e := newEnv(t)
	bobby := newUserRegistered(e, "bobby")
	newUserRegistered(e, "carol")

	h := bobby.headers("GET", "/api/block")
	e.do("GET", "/api/block", nil, h)
	h2 := bobby.headers("GET", "/api/block")
	h2.Set("X-Shyake-Pow", h.Get("X-Shyake-Pow")) // fresh signature, spent token
	if c, _ := e.do("GET", "/api/block", nil, h2); c != 403 {
		t.Errorf("reused PoW: %d, want 403", c)
	}

	h3 := bobby.headers("GET", "/api/block")
	h3.Set("X-Shyake-Pow", pow("carol")) // minted for someone else
	if c, _ := e.do("GET", "/api/block", nil, h3); c != 403 {
		t.Errorf("PoW for another user: %d, want 403", c)
	}
}

func TestIdempotentMail(t *testing.T) {
	e := newEnv(t)
	alice, bobby := newUserRegistered(e, "alice"), newUserRegistered(e, "bobby")
	body := mailBody(alice, "alice", "bobby", bobby.fp)
	_, first := e.do("POST", "/api/mail", body, nil)
	code, again := e.do("POST", "/api/mail", body, nil)
	if code != 201 || again["id"] != first["id"] {
		t.Errorf("resubmission: %d %v, want 201 %v", code, again["id"], first["id"])
	}
	_, out := e.authed(bobby, "GET", "/api/mail?type=inbox", nil)
	if n := len(out["mail"].([]any)); n != 1 {
		t.Errorf("stored %d copies", n)
	}
}

func TestBlocklistNormalization(t *testing.T) {
	e := newEnv(t)
	alice, bobby := newUserRegistered(e, "alice"), newUserRegistered(e, "bobby")
	body, _ := json.Marshal(map[string]string{"target": "alice@" + strings.ToUpper(domain)})
	if c, _ := e.authed(bobby, "POST", "/api/block", body); c != 201 {
		t.Fatalf("block: %d", c)
	}
	for _, recipient := range []string{"bobby", "bobby@" + domain} {
		if c, _ := e.do("POST", "/api/mail", mailBody(alice, "alice", recipient, bobby.fp), nil); c != 403 {
			t.Errorf("to %q while blocked: %d, want 403", recipient, c)
		}
	}
	_, out := e.authed(bobby, "GET", "/api/block", nil)
	if b := out["blocks"].([]any); len(b) != 1 || b[0].(map[string]any)["blocked"] != "alice" {
		t.Errorf("stored block: %v", out)
	}
	e.authed(bobby, "DELETE", "/api/block", body)
	if c, _ := e.do("POST", "/api/mail", mailBody(alice, "alice", "bobby", bobby.fp), nil); c != 201 {
		t.Errorf("after unblock: %d", c)
	}
}

func TestMailRejections(t *testing.T) {
	e := newEnv(t, func(c *config.Config) { c.MaxMailSize = 8192 })
	alice, bobby := newUserRegistered(e, "alice"), newUserRegistered(e, "bobby")

	big := bytes.Repeat([]byte("x"), 9000)
	if c, _ := e.do("POST", "/api/mail", big, nil); c != 413 {
		t.Errorf("oversized: %d, want 413", c)
	}
	if c, _ := e.do("POST", "/api/mail",
		mailBody(alice, "mallory@one.example", "evelyn@two.example", bobby.fp), nil); c != 403 {
		t.Errorf("third-party relay: %d, want 403", c)
	}
	if c, _ := e.do("POST", "/api/mail", mailBody(alice, "alice", "bobby", strings.Repeat("0", 64)), nil); c != 409 {
		t.Errorf("stale fingerprint: %d, want 409", c)
	}
	if c, _ := e.do("POST", "/api/mail", mailBody(bobby, "alice", "bobby", bobby.fp), nil); c != 401 {
		t.Errorf("forged sender: %d, want 401", c)
	}
	if c, _ := e.do("POST", "/api/mail", mailBody(alice, "alice", "nobody", bobby.fp), nil); c != 404 {
		t.Errorf("unknown recipient: %d, want 404", c)
	}
}

func TestRateLimit(t *testing.T) {
	e := newEnv(t, func(c *config.Config) { c.RateLimit, c.RateBurst = 1, 3 })
	e.s.limiter = newLimiter(1, 3)
	var got429 bool
	for i := 0; i < 6; i++ {
		if c, _ := e.do("GET", "/health", nil, nil); c == 429 {
			got429 = true
		}
	}
	if !got429 {
		t.Error("no 429 after bursting past the limit")
	}
}

func TestClientIP(t *testing.T) {
	s := &Server{cfg: config.Config{TrustedProxies: []netip.Prefix{
		netip.MustParsePrefix("127.0.0.1/32"), netip.MustParsePrefix("10.0.0.0/8")}}}
	for _, c := range []struct{ remote, xff, want string }{
		{"203.0.113.9:1234", "1.2.3.4", "203.0.113.9"},                        // untrusted peer: ignore header
		{"127.0.0.1:1234", "", "127.0.0.1"},                                   // no header
		{"127.0.0.1:1234", "198.51.100.7", "198.51.100.7"},                    // one proxy
		{"127.0.0.1:1234", "6.6.6.6, 198.51.100.7, 10.0.0.2", "198.51.100.7"}, // spoofed head, two proxies
	} {
		r := httptest.NewRequest("GET", "/", nil)
		r.RemoteAddr = c.remote
		if c.xff != "" {
			r.Header.Set("X-Forwarded-For", c.xff)
		}
		if got := s.clientIP(r).String(); got != c.want {
			t.Errorf("remote %s xff %q: %s, want %s", c.remote, c.xff, got, c.want)
		}
	}
}

// Rotate and block sign their bodies: a body swapped under a captured
// signature, or the pre-protocol-2 format, is refused.
func TestBodySignature(t *testing.T) {
	e := newEnv(t)
	bobby := newUser("bobby")
	e.register(bobby)
	mallory := newUser("mallory")
	rotate := func(u *user) []byte {
		b, _ := json.Marshal(map[string]string{"new_kem_pubkey": u.kemB64, "new_sig_pubkey": u.sigB64})
		return b
	}

	h := bobby.bodyHeaders("POST", "/api/rotate", rotate(bobby))
	if c, _ := e.do("POST", "/api/rotate", rotate(mallory), h); c != 401 {
		t.Errorf("rotate with swapped keys: %d, want 401", c)
	}
	if c, _ := e.do("POST", "/api/rotate", rotate(mallory), bobby.headers("POST", "/api/rotate")); c != 401 {
		t.Errorf("rotate signed without the body: %d, want 401", c)
	}
	block := []byte(`{"target":"alice"}`)
	if c, _ := e.do("POST", "/api/block", block, bobby.headers("POST", "/api/block")); c != 401 {
		t.Errorf("block signed without the body: %d, want 401", c)
	}
	h = bobby.bodyHeaders("DELETE", "/api/block", block)
	if c, _ := e.do("DELETE", "/api/block", []byte(`{"target":"carol"}`), h); c != 401 {
		t.Errorf("unblock with a swapped target: %d, want 401", c)
	}

	if c, _ := e.authed(bobby, "POST", "/api/block", block); c != 201 {
		t.Errorf("block: %d", c)
	}
	next := newUser("bobby")
	if c, _ := e.authed(bobby, "POST", "/api/rotate", rotate(next)); c != 200 {
		t.Errorf("rotate: %d", c)
	}
	if c, _ := e.authed(next, "GET", "/api/block", nil); c != 200 {
		t.Errorf("new key after rotate: %d", c)
	}
}
