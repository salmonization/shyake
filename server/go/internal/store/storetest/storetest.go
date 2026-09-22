// Package storetest is the contract every store.Store backend must
// meet. A backend's own test calls Run with a constructor for a fresh,
// empty database; SQLite does so today, PostgreSQL must before it ships.
package storetest

import (
	"context"
	"errors"
	"testing"

	"github.com/salmonization/shyake/server/go/internal/store"
)

func Run(t *testing.T, open func(t *testing.T) store.Store) {
	for _, c := range []struct {
		name string
		fn   func(*testing.T, store.Store)
	}{
		{"Users", testUsers},
		{"Mail", testMail},
		{"IdempotentMail", testIdempotentMail},
		{"Blocks", testBlocks},
		{"RotateAndDestroy", testRotateAndDestroy},
		{"Relays", testRelays},
	} {
		t.Run(c.name, func(t *testing.T) { c.fn(t, open(t)) })
	}
}

var ctx = context.Background()

func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
}

func user(name string) store.User {
	return store.User{Username: name, KEMPubkey: "kem-" + name, SigPubkey: "sig-" + name, CreatedAt: 1}
}

func mail(sender, recipient, sig string, ts int64) store.Mail {
	return store.Mail{Sender: sender, Recipient: recipient, EncKeySender: "ks",
		EncKeyRecipient: "kr", EncSubject: "subj", EncBody: "body", Size: 4,
		Signature: sig, Timestamp: ts}
}

func testUsers(t *testing.T, s store.Store) {
	must(t, s.CreateUser(ctx, user("alice")))
	for _, dup := range []string{"alice", "Alice", "ALICE"} {
		if err := s.CreateUser(ctx, user(dup)); !errors.Is(err, store.ErrConflict) {
			t.Errorf("CreateUser(%q) after alice: %v, want ErrConflict", dup, err)
		}
	}
	u, err := s.GetUser(ctx, "alice")
	must(t, err)
	if u.KEMPubkey != "kem-alice" || u.SigPubkey != "sig-alice" {
		t.Errorf("GetUser: %+v", u)
	}
	// lookups are exact: SPEC §4
	if _, err := s.GetUser(ctx, "Alice"); !errors.Is(err, store.ErrNotFound) {
		t.Errorf("GetUser(Alice): %v, want ErrNotFound", err)
	}
	must(t, s.Ping(ctx))
}

func testMail(t *testing.T, s store.Store) {
	id1, _, err := s.InsertMail(ctx, mail("alice", "bobby", "s1", 100), nil)
	must(t, err)
	id2, _, err := s.InsertMail(ctx, mail("carol", "bobby", "s2", 200), nil)
	must(t, err)
	if len(id1) != 10 || id1 == id2 {
		t.Fatalf("ids %q %q", id1, id2)
	}

	inbox, err := s.ListMail(ctx, "bobby", false)
	must(t, err)
	if len(inbox) != 2 || inbox[0].ID != id2 || inbox[1].ID != id1 {
		t.Errorf("inbox not newest first: %+v", inbox)
	}
	if inbox[0].EncBody != "" {
		t.Error("ListMail returned a body")
	}
	sent, err := s.ListMail(ctx, "alice", true)
	must(t, err)
	if len(sent) != 1 || sent[0].ID != id1 {
		t.Errorf("sent: %+v", sent)
	}
	empty, err := s.ListMail(ctx, "nobody", false)
	must(t, err)
	if empty == nil || len(empty) != 0 {
		t.Error("empty mailbox must be an empty, non-nil list")
	}

	m, err := s.GetMail(ctx, id1, "alice")
	must(t, err)
	if m.EncBody != "body" || m.Signature != "s1" || m.Timestamp != 100 {
		t.Errorf("GetMail: %+v", m)
	}
	if _, err := s.GetMail(ctx, id1, "carol"); !errors.Is(err, store.ErrNotFound) {
		t.Error("third party read a mail")
	}
	if err := s.DeleteMail(ctx, id1, "carol"); !errors.Is(err, store.ErrNotFound) {
		t.Error("third party burned a mail")
	}
	must(t, s.DeleteMail(ctx, id1, "bobby"))
	if _, err := s.GetMail(ctx, id1, "alice"); !errors.Is(err, store.ErrNotFound) {
		t.Error("burned mail still readable by its sender")
	}
}

func testIdempotentMail(t *testing.T, s store.Store) {
	id, dup, err := s.InsertMail(ctx, mail("alice", "bobby", "same-sig", 1), nil)
	must(t, err)
	if dup {
		t.Fatal("first insert reported dup")
	}
	again, dup, err := s.InsertMail(ctx, mail("alice", "bobby", "same-sig", 2), nil)
	must(t, err)
	if !dup || again != id {
		t.Errorf("resubmission: id %q dup %v, want %q true", again, dup, id)
	}
	found, err := s.MailIDBySignature(ctx, "same-sig")
	must(t, err)
	if found != id {
		t.Errorf("MailIDBySignature: %q", found)
	}
	if _, err := s.MailIDBySignature(ctx, "other"); !errors.Is(err, store.ErrNotFound) {
		t.Errorf("unknown signature: %v", err)
	}
	box, _ := s.ListMail(ctx, "bobby", false)
	if len(box) != 1 {
		t.Errorf("resubmission stored twice: %d rows", len(box))
	}
}

func testBlocks(t *testing.T, s store.Store) {
	must(t, s.AddBlock(ctx, "bobby", "alice", 10))
	must(t, s.AddBlock(ctx, "bobby", "evil.example", 20))
	must(t, s.AddBlock(ctx, "bobby", "alice", 30)) // upsert, not a duplicate

	for _, c := range []struct {
		cands []string
		want  bool
	}{
		{[]string{"alice", "my.instance"}, true},
		{[]string{"mallory@evil.example", "evil.example"}, true},
		{[]string{"carol", "my.instance"}, false},
		{nil, false},
	} {
		got, err := s.Blocked(ctx, "bobby", c.cands...)
		must(t, err)
		if got != c.want {
			t.Errorf("Blocked(%v) = %v", c.cands, got)
		}
	}
	if b, _ := s.Blocked(ctx, "carol", "alice"); b {
		t.Error("block leaked to another blocker")
	}

	list, err := s.ListBlocks(ctx, "bobby")
	must(t, err)
	if len(list) != 2 || list[0].Blocked != "alice" || list[0].CreatedAt != 30 {
		t.Errorf("ListBlocks: %+v", list)
	}
	must(t, s.RemoveBlock(ctx, "bobby", "alice", "alice@my.instance"))
	if b, _ := s.Blocked(ctx, "bobby", "alice"); b {
		t.Error("block survived RemoveBlock")
	}
}

func testRotateAndDestroy(t *testing.T, s store.Store) {
	for _, n := range []string{"alice", "bobby", "carol"} {
		must(t, s.CreateUser(ctx, user(n)))
	}
	s.InsertMail(ctx, mail("alice", "bobby", "r1", 1), nil)
	s.InsertMail(ctx, mail("bobby", "alice", "r2", 2), nil)
	s.InsertMail(ctx, mail("bobby", "carol", "r3", 3), nil)

	must(t, s.RotateKeys(ctx, "alice", "kem2", "sig2"))
	u, _ := s.GetUser(ctx, "alice")
	if u.KEMPubkey != "kem2" || u.SigPubkey != "sig2" {
		t.Errorf("keys not rotated: %+v", u)
	}
	if box, _ := s.ListMail(ctx, "bobby", true); len(box) != 1 {
		t.Errorf("rotate left alice's mail behind: %d", len(box))
	}
	if err := s.RotateKeys(ctx, "nobody", "k", "s"); !errors.Is(err, store.ErrNotFound) {
		t.Errorf("RotateKeys(nobody): %v", err)
	}

	s.AddBlock(ctx, "bobby", "carol", 1)
	s.AddBlock(ctx, "carol", "bobby", 1)
	s.AddBlock(ctx, "alice", "carol", 1)
	must(t, s.DestroyUser(ctx, "bobby"))
	u, err := s.GetUser(ctx, "bobby")
	must(t, err) // the row stays to lock the name
	if u.KEMPubkey != "" || u.SigPubkey != "" {
		t.Errorf("destroyed user keeps keys: %+v", u)
	}
	if box, _ := s.ListMail(ctx, "carol", false); len(box) != 0 {
		t.Error("destroy left mail behind")
	}
	if b, _ := s.Blocked(ctx, "carol", "bobby"); b {
		t.Error("destroy left a block naming the user")
	}
	if b, _ := s.Blocked(ctx, "alice", "carol"); !b {
		t.Error("destroy removed an unrelated block")
	}
	if err := s.CreateUser(ctx, user("bobby")); !errors.Is(err, store.ErrConflict) {
		t.Error("destroyed name can be registered again")
	}
}

func testRelays(t *testing.T, s store.Store) {
	relay := &store.Relay{Domain: "b.example", Payload: `{"raw":"body"}`, SignedAt: 1000, NextTryAt: 1000}
	id, _, err := s.InsertMail(ctx, mail("alice", "bobby@b.example", "rel", 1000), relay)
	must(t, err)

	due, err := s.DueRelays(ctx, 999, 10)
	must(t, err)
	if len(due) != 0 {
		t.Error("relay due before its time")
	}
	due, err = s.DueRelays(ctx, 1000, 10)
	must(t, err)
	if len(due) != 1 || due[0].MailID != id || due[0].Payload != `{"raw":"body"}` ||
		due[0].Domain != "b.example" || due[0].SignedAt != 1000 {
		t.Fatalf("DueRelays: %+v", due)
	}

	must(t, s.RelayRetry(ctx, due[0].ID, 1100, "connection refused"))
	if d, _ := s.DueRelays(ctx, 1050, 10); len(d) != 0 {
		t.Error("retried relay due early")
	}
	d, _ := s.DueRelays(ctx, 1100, 10)
	if len(d) != 1 || d[0].Attempts != 1 {
		t.Fatalf("after retry: %+v", d)
	}

	must(t, s.RelayDead(ctx, d[0].ID, "gave up"))
	if d, _ := s.DueRelays(ctx, 1<<40, 10); len(d) != 0 {
		t.Error("dead relay still due")
	}

	// a second relay, delivered
	s.InsertMail(ctx, mail("alice", "carol@c.example", "rel2", 5), &store.Relay{
		Domain: "c.example", Payload: "{}", SignedAt: 5, NextTryAt: 5})
	d, _ = s.DueRelays(ctx, 10, 10)
	if len(d) != 1 {
		t.Fatalf("second relay: %+v", d)
	}
	must(t, s.RelayDone(ctx, d[0].ID))
	if d, _ := s.DueRelays(ctx, 1<<40, 10); len(d) != 0 {
		t.Error("delivered relay still due")
	}
}
