package sqlite

import (
	"context"
	"database/sql"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The Worker's schema, verbatim from server/cf/migrations.
func workerSchema(t *testing.T) string {
	b, err := os.ReadFile("../../../../cf/migrations/0001_initial.sql")
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

const d1Rows = `
INSERT INTO users VALUES ('alice', 'kemA', 'sigA', 100);
INSERT INTO users VALUES ('Alice', 'kemA2', 'sigA2', 200);
INSERT INTO users VALUES ('bobby', 'kemB', 'sigB', 150);
INSERT INTO mail VALUES ('m1', 'alice', 'bobby', 'ks', 'kr', 'sub', 'body', 4, 'sig-1', 1000);
INSERT INTO mail VALUES ('m2', 'alice', 'bobby', 'ks', 'kr', 'sub', 'body', 4, 'sig-1', 1001);
INSERT INTO mail VALUES ('m3', 'mallory@Evil.Example', 'bobby', 'ks', 'kr', 's', 'b', 1, 'sig-3', 1002);
INSERT INTO blocks VALUES ('bobby', 'mallory@Evil.Example', 5);
INSERT INTO blocks VALUES ('bobby', 'alice@old.example', 6);
INSERT INTO blocks VALUES ('bobby', 'Evil.Example', 7);
`

func check(t *testing.T, dst *DB, rep ImportReport) {
	t.Helper()
	if rep.Users != 2 || len(rep.SkippedUsers) != 1 || rep.SkippedUsers[0] != "Alice" {
		t.Errorf("users: %+v", rep)
	}
	if rep.Mail != 2 || rep.DuplicateMail != 1 || rep.Blocks != 3 {
		t.Errorf("mail/blocks: %+v", rep)
	}
	ctx := context.Background()
	u, err := dst.GetUser(ctx, "alice")
	if err != nil || u.CreatedAt != 100 {
		t.Errorf("earliest alice not kept: %+v %v", u, err)
	}
	m, err := dst.GetMail(ctx, "m1", "bobby")
	if err != nil || m.Timestamp != 1000 || m.EncBody != "body" {
		t.Errorf("mail m1: %+v %v", m, err)
	}
	if id, err := dst.MailIDBySignature(ctx, "sig-1"); err != nil || id != "m1" {
		t.Errorf("signature index not built: %q %v", id, err)
	}
	for _, c := range []string{"mallory@evil.example", "evil.example", "alice"} {
		if b, _ := dst.Blocked(ctx, "bobby", c); !b {
			t.Errorf("block %q not found in normalized form", c)
		}
	}
}

func TestImportD1File(t *testing.T) {
	dir := t.TempDir()
	srcPath := filepath.Join(dir, "d1.sqlite")
	src, err := sql.Open("sqlite", "file:"+srcPath)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := src.Exec(workerSchema(t) + d1Rows); err != nil {
		t.Fatal(err)
	}
	src.Close()

	dst, err := Open(context.Background(), filepath.Join(dir, "go.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer dst.Close()
	rep, err := ImportD1(context.Background(), dst, srcPath, "old.example")
	if err != nil {
		t.Fatal(err)
	}
	check(t, dst, rep)

	if _, err := ImportD1(context.Background(), dst, srcPath, "old.example"); err == nil ||
		!strings.Contains(err.Error(), "already has users") {
		t.Errorf("second import into a populated database: %v", err)
	}
}

// The form `wrangler d1 export --remote` writes: SQL text.
func TestImportD1Dump(t *testing.T) {
	dir := t.TempDir()
	dump := filepath.Join(dir, "export.sql")
	body := "PRAGMA defer_foreign_keys=TRUE;\n" + workerSchema(t) + d1Rows
	if err := os.WriteFile(dump, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	dst, err := Open(context.Background(), filepath.Join(dir, "go.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer dst.Close()
	rep, err := ImportD1(context.Background(), dst, dump, "old.example")
	if err != nil {
		t.Fatal(err)
	}
	check(t, dst, rep)
}

func TestImportRejectsOtherDatabases(t *testing.T) {
	dir := t.TempDir()
	other := filepath.Join(dir, "metadata.sqlite")
	db, _ := sql.Open("sqlite", "file:"+other)
	db.Exec("CREATE TABLE _cf_METADATA (key INTEGER PRIMARY KEY)")
	db.Close()
	dst, err := Open(context.Background(), filepath.Join(dir, "go.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer dst.Close()
	_, err = ImportD1(context.Background(), dst, other, "x.example")
	if err == nil || !strings.Contains(err.Error(), "has no users, mail and blocks tables") {
		t.Errorf("err = %v", err)
	}
}
