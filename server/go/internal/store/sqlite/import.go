package sqlite

import (
	"bytes"
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"path/filepath"

	"github.com/salmonization/shyake/server/go/internal/protocol"
)

// ImportReport says what an import copied and what it had to leave out.
type ImportReport struct {
	Users, Mail, Blocks int
	// SkippedUsers differ from an imported name only by case; the
	// Worker used to allow that, this schema does not
	SkippedUsers []string
	// DuplicateMail repeats a stored signature (a retried submission)
	DuplicateMail int
}

// ImportD1 copies users, mail, and blocks from a Worker database into
// an empty one. src is either the D1 SQLite file that `wrangler dev`
// keeps under .wrangler/state, or the SQL text of
// `wrangler d1 export --remote`.
//
// Block targets are rewritten to the normalized form (SPEC §4) using
// instanceDomain, which must be the domain the Worker instance used.
func ImportD1(ctx context.Context, dst *DB, src, instanceDomain string) (ImportReport, error) {
	var rep ImportReport
	var n int
	if err := dst.w.QueryRowContext(ctx, "SELECT COUNT(*) FROM users").Scan(&n); err != nil {
		return rep, err
	}
	if n > 0 {
		return rep, errors.New("import: the target database already has users; import into a new one")
	}

	srcPath, cleanup, err := sourceFile(ctx, src)
	if err != nil {
		return rep, err
	}
	defer cleanup()
	s, err := sql.Open("sqlite", "file:"+srcPath+"?mode=ro&_pragma=query_only(1)")
	if err != nil {
		return rep, err
	}
	defer s.Close()
	var tables int
	if err := s.QueryRowContext(ctx, `SELECT COUNT(*) FROM sqlite_master
		WHERE type = 'table' AND name IN ('users', 'mail', 'blocks')`).Scan(&tables); err != nil {
		return rep, fmt.Errorf("import: cannot read %s: %w", src, err)
	}
	if tables != 3 {
		// .wrangler/state/v3/d1 also holds a metadata.sqlite
		return rep, fmt.Errorf("import: %s is not a Shyake D1 database "+
			"(no users, mail and blocks tables)", src)
	}

	err = dst.tx(ctx, func(tx *sql.Tx) error {
		if err := importUsers(ctx, s, tx, &rep); err != nil {
			return fmt.Errorf("users: %w", err)
		}
		if err := importMail(ctx, s, tx, &rep); err != nil {
			return fmt.Errorf("mail: %w", err)
		}
		if err := importBlocks(ctx, s, tx, instanceDomain, &rep); err != nil {
			return fmt.Errorf("blocks: %w", err)
		}
		return nil
	})
	return rep, err
}

// sourceFile returns a SQLite file for src, loading a SQL dump into a
// scratch database first.
func sourceFile(ctx context.Context, src string) (string, func(), error) {
	head := make([]byte, 16)
	f, err := os.Open(src)
	if err != nil {
		return "", nil, err
	}
	f.Read(head)
	f.Close()
	if bytes.HasPrefix(head, []byte("SQLite format 3\x00")) {
		return src, func() {}, nil
	}

	dump, err := os.ReadFile(src)
	if err != nil {
		return "", nil, err
	}
	dir, err := os.MkdirTemp("", "shyake-import-")
	if err != nil {
		return "", nil, err
	}
	cleanup := func() { os.RemoveAll(dir) }
	path := filepath.Join(dir, "d1.sqlite")
	scratch, err := sql.Open("sqlite", "file:"+path)
	if err != nil {
		cleanup()
		return "", nil, err
	}
	defer scratch.Close()
	if _, err := scratch.ExecContext(ctx, string(dump)); err != nil {
		cleanup()
		return "", nil, fmt.Errorf("import: %s is neither a SQLite file nor a SQL dump: %w", src, err)
	}
	return path, cleanup, nil
}

func importUsers(ctx context.Context, s *sql.DB, tx *sql.Tx, rep *ImportReport) error {
	rows, err := s.QueryContext(ctx,
		`SELECT username, kem_pubkey, sig_pubkey, created_at FROM users ORDER BY created_at, username`)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		var name, kem, sig string
		var at int64
		if err := rows.Scan(&name, &kem, &sig, &at); err != nil {
			return err
		}
		// the earliest registration keeps the name
		_, err := tx.ExecContext(ctx,
			`INSERT INTO users (username, kem_pubkey, sig_pubkey, created_at) VALUES (?, ?, ?, ?)`,
			name, kem, sig, at)
		switch {
		case isConstraint(err):
			rep.SkippedUsers = append(rep.SkippedUsers, name)
		case err != nil:
			return err
		default:
			rep.Users++
		}
	}
	return rows.Err()
}

func importMail(ctx context.Context, s *sql.DB, tx *sql.Tx, rep *ImportReport) error {
	rows, err := s.QueryContext(ctx,
		`SELECT mail_id, sender, recipient, enc_key_sender, enc_key_recipient,
		   enc_subject, enc_body, size, signature, timestamp FROM mail ORDER BY timestamp`)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		var id, snd, rcp, ks, kr, subj, body, sig string
		var size, ts int64
		if err := rows.Scan(&id, &snd, &rcp, &ks, &kr, &subj, &body, &size, &sig, &ts); err != nil {
			return err
		}
		_, err := tx.ExecContext(ctx,
			`INSERT INTO mail (mail_id, sender, recipient, enc_key_sender, enc_key_recipient,
			   enc_subject, enc_body, size, signature, timestamp, sig_hash)
			 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			id, snd, rcp, ks, kr, subj, body, size, sig, ts, sigHash(sig))
		switch {
		case isConstraint(err):
			rep.DuplicateMail++
		case err != nil:
			return err
		default:
			rep.Mail++
		}
	}
	return rows.Err()
}

func importBlocks(ctx context.Context, s *sql.DB, tx *sql.Tx, domain string, rep *ImportReport) error {
	rows, err := s.QueryContext(ctx, `SELECT blocker, blocked, created_at FROM blocks`)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		var blocker, blocked string
		var at int64
		if err := rows.Scan(&blocker, &blocked, &at); err != nil {
			return err
		}
		// rows from before normalization existed get the stored form
		if t, err := protocol.ParseBlockTarget(blocked, domain); err == nil {
			blocked = t.Stored()
		}
		if _, err := tx.ExecContext(ctx,
			`INSERT OR REPLACE INTO blocks (blocker, blocked, created_at) VALUES (?, ?, ?)`,
			blocker, blocked, at); err != nil {
			return err
		}
		rep.Blocks++
	}
	return rows.Err()
}
