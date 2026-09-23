// Package sqlite is the SQLite backend of store.Store.
//
// SQLite takes one writer at a time. Writes therefore go through a pool
// of exactly one connection, where they queue in Go instead of failing
// with SQLITE_BUSY, and reads use a separate read-only pool. In WAL
// mode the readers never block the writer.
package sqlite

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"embed"
	"errors"
	"fmt"
	"io/fs"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"

	"modernc.org/sqlite"

	"github.com/salmonization/shyake/server/go/internal/store"
)

//go:embed migrations/*.sql
var migrations embed.FS

type DB struct {
	w *sql.DB // single writer
	r *sql.DB // read-only pool
}

var _ store.Store = (*DB)(nil)

// Open opens (creating if needed) the database file at path and brings
// its schema up to date.
func Open(ctx context.Context, path string) (*DB, error) {
	if path == "" || strings.ContainsAny(path, "?#") {
		return nil, fmt.Errorf("sqlite: unusable database path %q", path)
	}
	pragmas := "_pragma=busy_timeout(10000)&_pragma=foreign_keys(1)" +
		"&_pragma=journal_mode(WAL)&_pragma=synchronous(NORMAL)"

	w, err := sql.Open("sqlite", "file:"+path+"?"+pragmas+"&_txlock=immediate")
	if err != nil {
		return nil, err
	}
	w.SetMaxOpenConns(1)
	w.SetConnMaxLifetime(0)

	db := &DB{w: w}
	if err := db.migrate(ctx); err != nil {
		w.Close()
		return nil, err
	}

	r, err := sql.Open("sqlite", "file:"+path+"?"+pragmas+"&_pragma=query_only(1)")
	if err != nil {
		w.Close()
		return nil, err
	}
	r.SetMaxOpenConns(max(4, runtime.NumCPU()))
	db.r = r
	return db, nil
}

func (db *DB) Close() error {
	return errors.Join(db.r.Close(), db.w.Close())
}

func (db *DB) Ping(ctx context.Context) error {
	var one int
	return db.r.QueryRowContext(ctx, "SELECT 1").Scan(&one)
}

// migrate applies each embedded migration once, in file-name order.
func (db *DB) migrate(ctx context.Context) error {
	if _, err := db.w.ExecContext(ctx, `CREATE TABLE IF NOT EXISTS schema_migrations (
		version    INTEGER PRIMARY KEY,
		applied_at INTEGER NOT NULL)`); err != nil {
		return err
	}
	names, err := fs.Glob(migrations, "migrations/*.sql")
	if err != nil {
		return err
	}
	sort.Strings(names)
	for _, name := range names {
		base := strings.TrimPrefix(name, "migrations/")
		version, err := strconv.Atoi(strings.SplitN(base, "_", 2)[0])
		if err != nil {
			return fmt.Errorf("sqlite: bad migration name %s", name)
		}
		if err := db.applyMigration(ctx, version, name); err != nil {
			return fmt.Errorf("sqlite: migration %s: %w", base, err)
		}
	}
	return nil
}

func (db *DB) applyMigration(ctx context.Context, version int, name string) error {
	tx, err := db.w.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()

	var done int
	err = tx.QueryRowContext(ctx,
		"SELECT COUNT(*) FROM schema_migrations WHERE version = ?", version).Scan(&done)
	if err != nil || done > 0 {
		return err
	}
	body, err := migrations.ReadFile(name)
	if err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, string(body)); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx,
		"INSERT INTO schema_migrations (version, applied_at) VALUES (?, ?)",
		version, time.Now().Unix()); err != nil {
		return err
	}
	return tx.Commit()
}

func isConstraint(err error) bool {
	var se *sqlite.Error
	return errors.As(err, &se) && se.Code()&0xff == 19 // SQLITE_CONSTRAINT
}

func notFound(err error) error {
	if errors.Is(err, sql.ErrNoRows) {
		return store.ErrNotFound
	}
	return err
}

// ------------------------------------------------------------------ users

func (db *DB) CreateUser(ctx context.Context, u store.User) error {
	_, err := db.w.ExecContext(ctx,
		`INSERT INTO users (username, kem_pubkey, sig_pubkey, created_at)
		 VALUES (?, ?, ?, ?)`,
		u.Username, u.KEMPubkey, u.SigPubkey, u.CreatedAt)
	if isConstraint(err) {
		return store.ErrConflict
	}
	return err
}

func (db *DB) GetUser(ctx context.Context, username string) (store.User, error) {
	u := store.User{Username: username}
	err := db.r.QueryRowContext(ctx,
		`SELECT kem_pubkey, sig_pubkey, created_at FROM users WHERE username = ?`,
		username).Scan(&u.KEMPubkey, &u.SigPubkey, &u.CreatedAt)
	return u, notFound(err)
}

func (db *DB) RotateKeys(ctx context.Context, username, kem, sig string) error {
	return db.tx(ctx, func(tx *sql.Tx) error {
		res, err := tx.ExecContext(ctx,
			`UPDATE users SET kem_pubkey = ?, sig_pubkey = ? WHERE username = ?`,
			kem, sig, username)
		if err != nil {
			return err
		}
		if n, _ := res.RowsAffected(); n == 0 {
			return store.ErrNotFound
		}
		_, err = tx.ExecContext(ctx,
			`DELETE FROM mail WHERE sender = ? OR recipient = ?`, username, username)
		return err
	})
}

func (db *DB) DestroyUser(ctx context.Context, username string) error {
	return db.tx(ctx, func(tx *sql.Tx) error {
		res, err := tx.ExecContext(ctx,
			`UPDATE users SET kem_pubkey = '', sig_pubkey = '' WHERE username = ?`, username)
		if err != nil {
			return err
		}
		if n, _ := res.RowsAffected(); n == 0 {
			return store.ErrNotFound
		}
		if _, err := tx.ExecContext(ctx,
			`DELETE FROM mail WHERE sender = ? OR recipient = ?`, username, username); err != nil {
			return err
		}
		_, err = tx.ExecContext(ctx,
			`DELETE FROM blocks WHERE blocker = ? OR blocked = ?`, username, username)
		return err
	})
}

// ------------------------------------------------------------------- mail

func sigHash(signature string) []byte {
	h := sha256.Sum256([]byte(signature))
	return h[:]
}

func (db *DB) InsertMail(ctx context.Context, m store.Mail) (string, bool, error) {
	for attempt := 0; attempt < 3; attempt++ {
		m.ID = store.NewMailID()
		_, err := db.w.ExecContext(ctx,
			`INSERT INTO mail (mail_id, sender, recipient, enc_key_sender,
				   enc_key_recipient, enc_subject, enc_body, size, signature,
				   timestamp, sig_hash)
				 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			m.ID, m.Sender, m.Recipient, m.EncKeySender, m.EncKeyRecipient,
			m.EncSubject, m.EncBody, m.Size, m.Signature, m.Timestamp,
			sigHash(m.Signature))
		if err == nil {
			return m.ID, false, nil
		}
		if !isConstraint(err) {
			return "", false, err
		}
		// same signature stored already, or a mail_id collision
		if id, lerr := db.MailIDBySignature(ctx, m.Signature); lerr == nil {
			return id, true, nil
		}
	}
	return "", false, errors.New("sqlite: could not allocate a mail id")
}

func (db *DB) MailIDBySignature(ctx context.Context, signature string) (string, error) {
	var id string
	err := db.r.QueryRowContext(ctx,
		`SELECT mail_id FROM mail WHERE sig_hash = ?`, sigHash(signature)).Scan(&id)
	return id, notFound(err)
}

func (db *DB) ListMail(ctx context.Context, username string, sent bool) ([]store.Mail, error) {
	q := `SELECT mail_id, sender, recipient, enc_key_sender, enc_key_recipient,
	        enc_subject, size, timestamp
	      FROM mail WHERE recipient = ? ORDER BY timestamp DESC`
	if sent {
		q = strings.Replace(q, "WHERE recipient", "WHERE sender", 1)
	}
	rows, err := db.r.QueryContext(ctx, q, username)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []store.Mail{}
	for rows.Next() {
		var m store.Mail
		if err := rows.Scan(&m.ID, &m.Sender, &m.Recipient, &m.EncKeySender,
			&m.EncKeyRecipient, &m.EncSubject, &m.Size, &m.Timestamp); err != nil {
			return nil, err
		}
		out = append(out, m)
	}
	return out, rows.Err()
}

func (db *DB) GetMail(ctx context.Context, id, username string) (store.Mail, error) {
	var m store.Mail
	err := db.r.QueryRowContext(ctx,
		`SELECT mail_id, sender, recipient, enc_key_sender, enc_key_recipient,
		   enc_subject, enc_body, size, signature, timestamp
		 FROM mail WHERE mail_id = ? AND (sender = ? OR recipient = ?)`,
		id, username, username).Scan(&m.ID, &m.Sender, &m.Recipient,
		&m.EncKeySender, &m.EncKeyRecipient, &m.EncSubject, &m.EncBody,
		&m.Size, &m.Signature, &m.Timestamp)
	return m, notFound(err)
}

func (db *DB) DeleteMail(ctx context.Context, id, username string) error {
	res, err := db.w.ExecContext(ctx,
		`DELETE FROM mail WHERE mail_id = ? AND (sender = ? OR recipient = ?)`,
		id, username, username)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return store.ErrNotFound
	}
	return nil
}

// ----------------------------------------------------------------- blocks

func (db *DB) Blocked(ctx context.Context, blocker string, candidates ...string) (bool, error) {
	if len(candidates) == 0 {
		return false, nil
	}
	args := []any{blocker}
	for _, c := range candidates {
		args = append(args, c)
	}
	q := `SELECT 1 FROM blocks WHERE blocker = ? AND blocked IN (?` +
		strings.Repeat(", ?", len(candidates)-1) + `) LIMIT 1`
	var one int
	err := db.r.QueryRowContext(ctx, q, args...).Scan(&one)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	return err == nil, err
}

func (db *DB) AddBlock(ctx context.Context, blocker, blocked string, at int64) error {
	_, err := db.w.ExecContext(ctx,
		`INSERT OR REPLACE INTO blocks (blocker, blocked, created_at) VALUES (?, ?, ?)`,
		blocker, blocked, at)
	return err
}

func (db *DB) RemoveBlock(ctx context.Context, blocker string, blocked ...string) error {
	for _, b := range blocked {
		if _, err := db.w.ExecContext(ctx,
			`DELETE FROM blocks WHERE blocker = ? AND blocked = ?`, blocker, b); err != nil {
			return err
		}
	}
	return nil
}

func (db *DB) ListBlocks(ctx context.Context, blocker string) ([]store.Block, error) {
	rows, err := db.r.QueryContext(ctx,
		`SELECT blocked, created_at FROM blocks WHERE blocker = ? ORDER BY created_at DESC`,
		blocker)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []store.Block{}
	for rows.Next() {
		var b store.Block
		if err := rows.Scan(&b.Blocked, &b.CreatedAt); err != nil {
			return nil, err
		}
		out = append(out, b)
	}
	return out, rows.Err()
}

func (db *DB) tx(ctx context.Context, fn func(*sql.Tx) error) error {
	tx, err := db.w.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if err := fn(tx); err != nil {
		return err
	}
	return tx.Commit()
}
