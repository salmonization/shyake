// Package store defines the persistence contract of the server.
//
// The interface speaks the domain (users, mail, blocks, relays), never
// SQL, so each backend writes its own queries in its own dialect. SQLite
// ships now. A PostgreSQL backend implements the same interface and must
// pass the same suite in store/storetest before it is wired in.
package store

import (
	"context"
	"crypto/rand"
	"errors"
)

var (
	// ErrNotFound: no such row.
	ErrNotFound = errors.New("not found")
	// ErrConflict: a unique constraint rejected the write.
	ErrConflict = errors.New("conflict")
)

// User is a registered account. Empty keys mean a destroyed account,
// whose row stays to lock the name (SPEC §4).
type User struct {
	Username  string
	KEMPubkey string
	SigPubkey string
	CreatedAt int64
}

// Mail is one stored message. Sender and Recipient are in stored form:
// bare for local users, "user@domain" for remote ones.
type Mail struct {
	ID              string
	Sender          string
	Recipient       string
	EncKeySender    string
	EncKeyRecipient string
	EncSubject      string
	EncBody         string
	Size            int64
	Signature       string
	Timestamp       int64
}

// Block is one blocklist entry of a user.
type Block struct {
	Blocked   string
	CreatedAt int64
}

// Store is implemented by every database backend.
type Store interface {
	Ping(ctx context.Context) error
	Close() error

	// CreateUser returns ErrConflict when the name is taken, compared
	// case-insensitively.
	CreateUser(ctx context.Context, u User) error
	// GetUser matches the name exactly.
	GetUser(ctx context.Context, username string) (User, error)
	// RotateKeys replaces both keys and deletes the user's mail.
	RotateKeys(ctx context.Context, username, kem, sig string) error
	// DestroyUser empties the keys and deletes the user's mail and
	// every block row that names the user.
	DestroyUser(ctx context.Context, username string) error

	// InsertMail stores m under a new ID. A mail with the same
	// signature already stored is not stored again: InsertMail then
	// returns its ID and dup = true.
	InsertMail(ctx context.Context, m Mail) (id string, dup bool, err error)
	// MailIDBySignature finds an already stored copy of a submission.
	MailIDBySignature(ctx context.Context, signature string) (string, error)
	// ListMail lists a mailbox newest first, without bodies.
	ListMail(ctx context.Context, username string, sent bool) ([]Mail, error)
	// GetMail returns a mail only to its sender or recipient.
	GetMail(ctx context.Context, id, username string) (Mail, error)
	// DeleteMail deletes a mail for its sender or recipient.
	DeleteMail(ctx context.Context, id, username string) error

	// Blocked reports whether blocker blocks any of the candidates.
	Blocked(ctx context.Context, blocker string, candidates ...string) (bool, error)
	AddBlock(ctx context.Context, blocker, blocked string, at int64) error
	// RemoveBlock removes each of the given forms of one target.
	RemoveBlock(ctx context.Context, blocker string, blocked ...string) error
	ListBlocks(ctx context.Context, blocker string) ([]Block, error)
}

// NewMailID returns a 10-character base58 id, the Worker's format.
func NewMailID() string {
	const alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
	var b [10]byte
	rand.Read(b[:])
	for i := range b {
		// 256 % 58 != 0: a slight bias, harmless for an id that is
		// not a secret (only its sender and recipient can read it)
		b[i] = alphabet[int(b[i])%len(alphabet)]
	}
	return string(b[:])
}
