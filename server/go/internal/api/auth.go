package api

import (
	"crypto/sha256"
	"errors"
	"net/http"
	"strconv"

	"github.com/salmonization/shyake/server/go/internal/protocol"
	"github.com/salmonization/shyake/server/go/internal/store"
)

// window is the allowed clock difference for signed timestamps (SPEC §3.4).
const window = 300

// inWindow parses a signed timestamp and checks it against server time.
func (s *Server) inWindow(ts string) (int64, bool) {
	t, err := strconv.ParseInt(ts, 10, 64)
	if err != nil {
		return 0, false
	}
	d := s.now().Unix() - t
	return t, d >= -window && d <= window
}

// fresh reports that neither the signature nor the PoW token was used
// before, and marks both used. The timestamp window alone lets a
// captured request replay for five minutes, and an unbound PoW token
// could pay for any number of requests.
func (s *Server) fresh(signature, pow string) bool {
	now := s.now()
	if !s.seen.Add(sha256.Sum256([]byte(signature)), now) {
		return false
	}
	return s.spent.Add(sha256.Sum256([]byte(pow)), now)
}

// headerAuth authenticates a request signed in X-Shyake-* headers
// (SPEC §3.3). signedPath is the path as it appears in the signed
// message. On failure it has already answered, and returns false.
//
// Checks run in the Worker's order, so failures map to the same status
// codes: headers 401, PoW 403, timestamp 403, user 401, signature 401.
func (s *Server) headerAuth(w http.ResponseWriter, r *http.Request, signedPath string) (store.User, bool) {
	username := r.Header.Get("X-Shyake-Username")
	ts := r.Header.Get("X-Shyake-Timestamp")
	sig := r.Header.Get("X-Shyake-Signature")
	pow := r.Header.Get("X-Shyake-Pow")
	if username == "" || ts == "" || sig == "" || pow == "" {
		fail(w, http.StatusUnauthorized, "Missing auth headers")
		return store.User{}, false
	}
	if protocol.VerifyPoW(pow, username, s.now()) != nil {
		fail(w, http.StatusForbidden, "Invalid Proof of Work")
		return store.User{}, false
	}
	if _, ok := s.inWindow(ts); !ok {
		fail(w, http.StatusForbidden, "Timestamp out of window")
		return store.User{}, false
	}

	user, err := s.db.GetUser(r.Context(), username)
	switch {
	case errors.Is(err, store.ErrNotFound):
		fail(w, http.StatusUnauthorized, "User not found")
		return store.User{}, false
	case err != nil:
		s.internal(w, "auth: get user", err)
		return store.User{}, false
	case user.SigPubkey == "":
		fail(w, http.StatusUnauthorized, "User not found or destroyed")
		return store.User{}, false
	}

	pk, err := protocol.ParsePublicKey(user.SigPubkey)
	if err != nil || !pk.Verify(protocol.HeaderMessage(r.Method, signedPath, username, ts), sig) {
		fail(w, http.StatusUnauthorized, "Invalid signature")
		return store.User{}, false
	}
	if !s.fresh(sig, pow) {
		fail(w, http.StatusForbidden, "Replayed request")
		return store.User{}, false
	}
	return user, true
}
