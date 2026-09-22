package api

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"math"
	"net/http"
	"strings"

	"github.com/salmonization/shyake/server/go/internal/federation"
	"github.com/salmonization/shyake/server/go/internal/protocol"
	"github.com/salmonization/shyake/server/go/internal/store"
)

// kemPubkeySize is the size of an ML-KEM-768 public key.
const kemPubkeySize = 1184

// smallBody bounds every request body except a mail's.
const smallBody = 64 << 10

func (s *Server) health(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	if err := s.db.Ping(r.Context()); err != nil {
		s.log.Error("health", "err", err)
		w.WriteHeader(http.StatusInternalServerError)
		w.Write([]byte("500 Internal Server Error"))
		return
	}
	w.Write([]byte("200 OK"))
}

// validKEM reports whether s is a base64 ML-KEM-768 public key.
func validKEM(s string) bool {
	raw, err := protocol.DecodeKEMKey(s)
	return err == nil && len(raw) == kemPubkeySize
}

// ------------------------------------------------------------ register

func (s *Server) register(w http.ResponseWriter, r *http.Request) {
	if !s.cfg.RegistrationEnabled {
		fail(w, http.StatusForbidden, "Registration is disabled")
		return
	}
	var b struct {
		Username  string `json:"username"`
		KEMPubkey string `json:"kem_pubkey"`
		SigPubkey string `json:"sig_pubkey"`
		Timestamp string `json:"timestamp"`
		Signature string `json:"signature"`
		Pow       string `json:"pow"`
	}
	if !readJSON(w, r, smallBody, &b) {
		return
	}
	if b.Username == "" || b.KEMPubkey == "" || b.SigPubkey == "" ||
		b.Timestamp == "" || b.Signature == "" || b.Pow == "" {
		fail(w, http.StatusBadRequest, "Missing required fields")
		return
	}
	if !protocol.ValidUsername(b.Username) {
		fail(w, http.StatusBadRequest, "Invalid username format")
		return
	}
	if protocol.IsReserved(b.Username, s.cfg.ReservedUsernames) {
		fail(w, http.StatusForbidden, "Username is reserved")
		return
	}
	if protocol.VerifyPoW(b.Pow, b.Username, s.now()) != nil {
		fail(w, http.StatusForbidden, "Invalid Proof of Work")
		return
	}
	if _, ok := s.inWindow(b.Timestamp); !ok {
		fail(w, http.StatusForbidden, "Timestamp out of window")
		return
	}
	pk, err := protocol.ParsePublicKey(b.SigPubkey)
	if err != nil || !validKEM(b.KEMPubkey) {
		fail(w, http.StatusBadRequest, "Invalid public key")
		return
	}
	msg := protocol.RegisterMessage(b.Username, b.KEMPubkey, b.SigPubkey, b.Timestamp)
	if !pk.Verify(msg, b.Signature) {
		fail(w, http.StatusUnauthorized, "Invalid signature")
		return
	}
	if !s.fresh(b.Signature, b.Pow) {
		fail(w, http.StatusForbidden, "Replayed request")
		return
	}

	err = s.db.CreateUser(r.Context(), store.User{Username: b.Username,
		KEMPubkey: b.KEMPubkey, SigPubkey: b.SigPubkey, CreatedAt: s.now().Unix()})
	switch {
	case errors.Is(err, store.ErrConflict):
		fail(w, http.StatusConflict, "Username already taken")
	case err != nil:
		s.internal(w, "register", err)
	default:
		writeJSON(w, http.StatusCreated, message{Message: "Registered successfully"})
	}
}

// -------------------------------------------------------------- pubkey

type pubkeys struct {
	KEM string `json:"kem_pubkey"`
	Sig string `json:"sig_pubkey"`
}

func (s *Server) pubkey(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("username")
	if strings.Contains(name, "@") {
		addr, err := protocol.ParseAddress(name, s.cfg.InstanceDomain)
		if err != nil {
			fail(w, http.StatusNotFound, "User not found")
			return
		}
		if !addr.IsLocal() {
			if !s.cfg.FederationEnabled {
				fail(w, http.StatusForbidden, "Federation disabled")
				return
			}
			k, err := s.keys.Lookup(r.Context(), addr, false)
			switch {
			case errors.Is(err, federation.ErrRemoteNotFound):
				fail(w, http.StatusNotFound, "External user not found")
			case err != nil:
				fail(w, http.StatusBadGateway, "Failed to connect")
			default:
				writeJSON(w, http.StatusOK, pubkeys{KEM: k.KEM, Sig: k.Sig})
			}
			return
		}
		name = addr.Local()
	}
	u, err := s.db.GetUser(r.Context(), name)
	switch {
	case errors.Is(err, store.ErrNotFound):
		fail(w, http.StatusNotFound, "User not found")
	case err != nil:
		s.internal(w, "pubkey", err)
	default:
		writeJSON(w, http.StatusOK, pubkeys{KEM: u.KEMPubkey, Sig: u.SigPubkey})
	}
}

// ---------------------------------------------------------------- mail

// keysOf returns an address's public keys: from the database for a local
// user, from its instance for a remote one. cached reports whether a
// remote answer came from the key cache, so a caller can re-ask on
// mismatch. A missing user is store.ErrNotFound; a remote instance that
// cannot be reached is federation.ErrUnreachable.
func (s *Server) keysOf(ctx context.Context, a protocol.Address, fresh bool) (kem, sig string, cached bool, err error) {
	if a.IsLocal() {
		u, err := s.db.GetUser(ctx, a.Local())
		return u.KEMPubkey, u.SigPubkey, false, err
	}
	if !s.cfg.FederationEnabled {
		return "", "", false, store.ErrNotFound
	}
	k, err := s.keys.Lookup(ctx, a, fresh)
	if errors.Is(err, federation.ErrRemoteNotFound) {
		err = store.ErrNotFound
	}
	return k.KEM, k.Sig, !fresh, err
}

func fingerprint(kem string) string {
	raw, err := protocol.DecodeKEMKey(kem)
	if err != nil {
		return ""
	}
	sum := sha256.Sum256(raw)
	return hex.EncodeToString(sum[:])
}

func (s *Server) sendMail(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	raw, ok := readBody(w, r, s.cfg.MaxMailSize)
	if !ok {
		return
	}
	var m struct {
		Sender          string       `json:"sender"`
		Recipient       string       `json:"recipient"`
		Fingerprint     string       `json:"recipient_kem_fingerprint"`
		EncKeySender    string       `json:"enc_key_sender"`
		EncKeyRecipient string       `json:"enc_key_recipient"`
		EncSubject      *string      `json:"enc_subject"`
		EncBody         string       `json:"enc_body"`
		Size            *json.Number `json:"size"`
		Timestamp       string       `json:"timestamp"`
		Signature       string       `json:"signature"`
		Pow             string       `json:"pow"`
	}
	if err := json.Unmarshal(raw, &m); err != nil {
		fail(w, http.StatusBadRequest, "Invalid JSON")
		return
	}
	if m.Sender == "" || m.Recipient == "" || m.Fingerprint == "" ||
		m.EncKeySender == "" || m.EncKeyRecipient == "" || m.EncSubject == nil ||
		m.EncBody == "" || m.Size == nil || m.Timestamp == "" ||
		m.Signature == "" || m.Pow == "" {
		fail(w, http.StatusBadRequest, "Missing required fields")
		return
	}
	// JSON.stringify prints an integral size as plain digits; anything
	// else could not have been signed the way the client signs it
	f, err := m.Size.Float64()
	if err != nil || f < 0 || f != math.Trunc(f) || f > 1<<53 {
		fail(w, http.StatusBadRequest, "Invalid size")
		return
	}
	size := int64(f)

	sender, err := protocol.ParseAddress(m.Sender, s.cfg.InstanceDomain)
	if err != nil {
		fail(w, http.StatusUnauthorized, "Sender not registered")
		return
	}
	recipient, err := protocol.ParseAddress(m.Recipient, s.cfg.InstanceDomain)
	if err != nil {
		fail(w, http.StatusNotFound, "Recipient not found")
		return
	}
	// a submission from our own user, or a relay to our own user;
	// never a relay between two other instances
	if !sender.IsLocal() && !recipient.IsLocal() {
		fail(w, http.StatusForbidden, "Relaying denied")
		return
	}
	// the remote side resolves the sender by the domain in this string
	if !recipient.IsLocal() && !strings.Contains(m.Sender, "@") {
		fail(w, http.StatusBadRequest, "Sender must be qualified for a remote recipient")
		return
	}
	if protocol.VerifyPoW(m.Pow, m.Sender, s.now()) != nil {
		fail(w, http.StatusForbidden, "Invalid Proof of Work")
		return
	}
	ts, ok := s.inWindow(m.Timestamp)
	if !ok {
		fail(w, http.StatusForbidden, "Timestamp out of window")
		return
	}

	_, senderSig, senderCached, err := s.keysOf(ctx, sender, false)
	switch {
	case errors.Is(err, federation.ErrUnreachable):
		// 503 so a relaying instance tries again
		fail(w, http.StatusServiceUnavailable, "Sender instance unreachable")
		return
	case errors.Is(err, store.ErrNotFound) || err == nil && senderSig == "":
		fail(w, http.StatusUnauthorized, "Sender not registered")
		return
	case err != nil:
		s.internal(w, "mail: sender keys", err)
		return
	}

	recipKEM, _, recipCached, err := s.keysOf(ctx, recipient, false)
	switch {
	case errors.Is(err, store.ErrNotFound):
		fail(w, http.StatusNotFound, "Recipient not found")
		return
	case errors.Is(err, federation.ErrUnreachable):
		fail(w, http.StatusBadGateway, "Recipient instance unreachable")
		return
	case err != nil:
		s.internal(w, "mail: recipient keys", err)
		return
	}
	if recipKEM == "" {
		fail(w, http.StatusGone, "USER_DESTROYED")
		return
	}
	if fingerprint(recipKEM) != m.Fingerprint && recipCached {
		recipKEM, _, _, err = s.keysOf(ctx, recipient, true)
		if err != nil {
			recipKEM = ""
		}
	}
	if fingerprint(recipKEM) != m.Fingerprint {
		fail(w, http.StatusConflict, "KEY_MISMATCH")
		return
	}

	msg := protocol.MailMessage(m.Sender, m.Recipient, m.Fingerprint,
		*m.EncSubject, m.EncBody, m.Timestamp, size)
	if !s.verify(senderSig, msg, m.Signature) && (!senderCached ||
		!s.verifyFresh(ctx, sender, msg, m.Signature)) {
		fail(w, http.StatusUnauthorized, "Invalid signature")
		return
	}

	if recipient.IsLocal() {
		blocked, err := s.db.Blocked(ctx, recipient.Stored(), sender.Stored(), sender.Domain())
		if err != nil {
			s.internal(w, "mail: blocklist", err)
			return
		}
		if blocked {
			fail(w, http.StatusForbidden, "Recipient has blocked this sender")
			return
		}
	}

	// the same signed submission again (a client retry, or a relay
	// retry after a lost reply) gets the original answer
	if id, err := s.db.MailIDBySignature(ctx, m.Signature); err == nil {
		writeJSON(w, http.StatusCreated, message{Message: "Mail sent", ID: id})
		return
	}
	if !s.spent.Add(sha256.Sum256([]byte(m.Pow)), s.now()) {
		fail(w, http.StatusForbidden, "Invalid Proof of Work")
		return
	}

	now := s.now().Unix()
	var relay *store.Relay
	if !recipient.IsLocal() {
		relay = &store.Relay{Domain: recipient.Domain(), Payload: string(raw),
			SignedAt: ts, NextTryAt: now}
	}
	id, _, err := s.db.InsertMail(ctx, store.Mail{
		Sender:          sender.Stored(),
		Recipient:       recipient.Stored(),
		EncKeySender:    m.EncKeySender,
		EncKeyRecipient: m.EncKeyRecipient,
		EncSubject:      *m.EncSubject,
		EncBody:         m.EncBody,
		Size:            size,
		Signature:       m.Signature,
		Timestamp:       now,
	}, relay)
	if err != nil {
		s.internal(w, "mail: insert", err)
		return
	}
	if relay != nil {
		s.outbox.Kick()
	}
	writeJSON(w, http.StatusCreated, message{Message: "Mail sent", ID: id})
}

func (s *Server) verify(sigPub string, msg []byte, signature string) bool {
	pk, err := protocol.ParsePublicKey(sigPub)
	return err == nil && pk.Verify(msg, signature)
}

// verifyFresh retries with a newly fetched key: the cached one may
// predate a rotation.
func (s *Server) verifyFresh(ctx context.Context, a protocol.Address, msg []byte, signature string) bool {
	_, sig, _, err := s.keysOf(ctx, a, true)
	return err == nil && s.verify(sig, msg, signature)
}

type mailJSON struct {
	ID              string  `json:"mail_id"`
	Sender          string  `json:"sender"`
	Recipient       string  `json:"recipient"`
	EncKeySender    string  `json:"enc_key_sender"`
	EncKeyRecipient string  `json:"enc_key_recipient"`
	EncSubject      string  `json:"enc_subject"`
	EncBody         *string `json:"enc_body,omitempty"`
	Size            int64   `json:"size"`
	Signature       *string `json:"signature,omitempty"`
	Timestamp       int64   `json:"timestamp"`
}

func toJSON(m store.Mail, full bool) mailJSON {
	j := mailJSON{ID: m.ID, Sender: m.Sender, Recipient: m.Recipient,
		EncKeySender: m.EncKeySender, EncKeyRecipient: m.EncKeyRecipient,
		EncSubject: m.EncSubject, Size: m.Size, Timestamp: m.Timestamp}
	if full {
		j.EncBody, j.Signature = &m.EncBody, &m.Signature
	}
	return j
}

func (s *Server) listMail(w http.ResponseWriter, r *http.Request) {
	typ := r.URL.Query().Get("type")
	if typ == "" {
		typ = "inbox"
	}
	user, ok := s.headerAuth(w, r, "/api/mail?type="+typ)
	if !ok {
		return
	}
	list, err := s.db.ListMail(r.Context(), user.Username, typ == "sent")
	if err != nil {
		s.internal(w, "list mail", err)
		return
	}
	out := make([]mailJSON, len(list))
	for i, m := range list {
		out[i] = toJSON(m, false)
	}
	writeJSON(w, http.StatusOK, map[string]any{"mail": out})
}

func (s *Server) getMail(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	user, ok := s.headerAuth(w, r, "/api/mail/"+id)
	if !ok {
		return
	}
	m, err := s.db.GetMail(r.Context(), id, user.Username)
	switch {
	case errors.Is(err, store.ErrNotFound):
		fail(w, http.StatusNotFound, "Mail not found")
	case err != nil:
		s.internal(w, "get mail", err)
	default:
		writeJSON(w, http.StatusOK, toJSON(m, true))
	}
}

func (s *Server) burnMail(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	user, ok := s.headerAuth(w, r, "/api/mail/"+id)
	if !ok {
		return
	}
	err := s.db.DeleteMail(r.Context(), id, user.Username)
	switch {
	case errors.Is(err, store.ErrNotFound):
		fail(w, http.StatusNotFound, "Mail not found")
	case err != nil:
		s.internal(w, "burn mail", err)
	default:
		writeJSON(w, http.StatusOK, message{Message: "Mail burned"})
	}
}

// -------------------------------------------------------------- blocks

func (s *Server) blockTarget(w http.ResponseWriter, r *http.Request) (store.User, protocol.Address, string, bool) {
	user, ok := s.headerAuth(w, r, "/api/block")
	if !ok {
		return store.User{}, protocol.Address{}, "", false
	}
	var b struct {
		Target string `json:"target"`
	}
	if !readJSON(w, r, smallBody, &b) {
		return store.User{}, protocol.Address{}, "", false
	}
	if b.Target == "" {
		fail(w, http.StatusBadRequest, "Missing target")
		return store.User{}, protocol.Address{}, "", false
	}
	t, err := protocol.ParseBlockTarget(b.Target, s.cfg.InstanceDomain)
	if err != nil {
		fail(w, http.StatusBadRequest, "Invalid target")
		return store.User{}, protocol.Address{}, "", false
	}
	return user, t, b.Target, true
}

func (s *Server) block(w http.ResponseWriter, r *http.Request) {
	user, t, _, ok := s.blockTarget(w, r)
	if !ok {
		return
	}
	if err := s.db.AddBlock(r.Context(), user.Username, t.Stored(), s.now().Unix()); err != nil {
		s.internal(w, "block", err)
		return
	}
	writeJSON(w, http.StatusCreated, message{Message: "Blocked"})
}

func (s *Server) unblock(w http.ResponseWriter, r *http.Request) {
	user, t, raw, ok := s.blockTarget(w, r)
	if !ok {
		return
	}
	// the raw form clears rows imported from before normalization
	if err := s.db.RemoveBlock(r.Context(), user.Username, t.Stored(), raw); err != nil {
		s.internal(w, "unblock", err)
		return
	}
	writeJSON(w, http.StatusOK, message{Message: "Unblocked"})
}

func (s *Server) listBlocks(w http.ResponseWriter, r *http.Request) {
	user, ok := s.headerAuth(w, r, "/api/block")
	if !ok {
		return
	}
	list, err := s.db.ListBlocks(r.Context(), user.Username)
	if err != nil {
		s.internal(w, "list blocks", err)
		return
	}
	type blockJSON struct {
		Blocked   string `json:"blocked"`
		CreatedAt int64  `json:"created_at"`
	}
	out := make([]blockJSON, len(list))
	for i, b := range list {
		out[i] = blockJSON{b.Blocked, b.CreatedAt}
	}
	writeJSON(w, http.StatusOK, map[string]any{"blocks": out})
}

// ------------------------------------------------------------- account

func (s *Server) rotate(w http.ResponseWriter, r *http.Request) {
	user, ok := s.headerAuth(w, r, "/api/rotate")
	if !ok {
		return
	}
	var b struct {
		KEM string `json:"new_kem_pubkey"`
		Sig string `json:"new_sig_pubkey"`
	}
	if !readJSON(w, r, smallBody, &b) {
		return
	}
	if b.KEM == "" || b.Sig == "" {
		fail(w, http.StatusBadRequest, "Missing new keys")
		return
	}
	if _, err := protocol.ParsePublicKey(b.Sig); err != nil || !validKEM(b.KEM) {
		fail(w, http.StatusBadRequest, "Invalid public key")
		return
	}
	if err := s.db.RotateKeys(r.Context(), user.Username, b.KEM, b.Sig); err != nil {
		s.internal(w, "rotate", err)
		return
	}
	writeJSON(w, http.StatusOK, message{Message: "Keys rotated and old mails deleted"})
}

func (s *Server) destroy(w http.ResponseWriter, r *http.Request) {
	user, ok := s.headerAuth(w, r, "/api/destroy")
	if !ok {
		return
	}
	if err := s.db.DestroyUser(r.Context(), user.Username); err != nil {
		s.internal(w, "destroy", err)
		return
	}
	writeJSON(w, http.StatusOK, message{Message: "Account destroyed"})
}
