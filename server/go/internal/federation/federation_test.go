package federation

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/salmonization/shyake/server/go/internal/protocol"
	"github.com/salmonization/shyake/server/go/internal/store"
	"github.com/salmonization/shyake/server/go/internal/store/sqlite"
)

func TestPublicAddr(t *testing.T) {
	for s, want := range map[string]bool{
		"8.8.8.8": true, "1.1.1.1": true, "2606:4700:4700::1111": true,
		"127.0.0.1": false, "10.1.2.3": false, "172.16.0.1": false, "192.168.1.1": false,
		"169.254.169.254": false, // cloud metadata
		"100.64.0.1":      false, "0.0.0.0": false, "::1": false, "fe80::1": false,
		"fc00::1": false, "::ffff:127.0.0.1": false, "::ffff:10.0.0.1": false,
		"64:ff9b::a00:1": false, "2002:7f00:1::": false, "224.0.0.1": false,
	} {
		if got := publicAddr(netip.MustParseAddr(s)); got != want {
			t.Errorf("publicAddr(%s) = %v", s, got)
		}
	}
}

// The guard must hold at dial time: a loopback server is unreachable.
func TestGuardRefusesLoopback(t *testing.T) {
	var hit atomic.Bool
	srv := httptest.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) { hit.Store(true) }))
	defer srv.Close()

	c := NewClient(false, "test")
	_, _, err := c.get(context.Background(), srv.URL, 1024)
	if !errors.Is(err, ErrForbiddenAddress) {
		t.Errorf("err = %v, want ErrForbiddenAddress", err)
	}
	if hit.Load() {
		t.Error("request reached the loopback server")
	}
}

func TestNoRedirects(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "http://169.254.169.254/", http.StatusFound)
	}))
	defer srv.Close()
	status, _, err := NewClient(true, "test").get(context.Background(), srv.URL, 1024)
	if err != nil || status != http.StatusFound {
		t.Errorf("status %d err %v: redirect was followed", status, err)
	}
}

func TestKeysLookup(t *testing.T) {
	var calls atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		if r.URL.Path != "/api/pubkey/alice" {
			http.NotFound(w, r)
			return
		}
		io.WriteString(w, `{"kem_pubkey":"K","sig_pubkey":"S"}`)
	}))
	defer srv.Close()
	domain := strings.TrimPrefix(srv.URL, "http://")
	keys := NewKeys(NewClient(true, "test"))
	ctx := context.Background()

	alice, _ := protocol.ParseAddress("alice@"+domain, "own.example")
	for i := 0; i < 3; i++ {
		k, err := keys.Lookup(ctx, alice, false)
		if err != nil || k.KEM != "K" || k.Sig != "S" {
			t.Fatalf("lookup: %+v %v", k, err)
		}
	}
	if calls.Load() != 1 {
		t.Errorf("%d fetches, want 1 (cached)", calls.Load())
	}
	keys.Lookup(ctx, alice, true)
	if calls.Load() != 2 {
		t.Error("fresh lookup served from cache")
	}

	nobody, _ := protocol.ParseAddress("nobody@"+domain, "own.example")
	if _, err := keys.Lookup(ctx, nobody, false); !errors.Is(err, ErrRemoteNotFound) {
		t.Errorf("unknown user: %v", err)
	}
	down, _ := protocol.ParseAddress("alice@127.0.0.1:1", "own.example")
	if _, err := keys.Lookup(ctx, down, false); !errors.Is(err, ErrUnreachable) {
		t.Errorf("dead instance: %v", err)
	}
}

// outboxFixture queues one relay to a fake remote whose replies the
// test scripts, and drives the outbox with a fake clock.
type outboxFixture struct {
	o       *Outbox
	db      store.Store
	clock   time.Time
	replies []int
	got     atomic.Int32
	body    atomic.Value
}

func newOutboxFixture(t *testing.T, signedAt time.Time, replies ...int) *outboxFixture {
	f := &outboxFixture{clock: signedAt, replies: replies}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		n := f.got.Add(1)
		b, _ := io.ReadAll(r.Body)
		f.body.Store(string(b))
		status := http.StatusCreated
		if int(n) <= len(f.replies) {
			status = f.replies[n-1]
		}
		w.WriteHeader(status)
	}))
	t.Cleanup(srv.Close)

	db, err := sqlite.Open(context.Background(), filepath.Join(t.TempDir(), "o.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	f.db = db
	f.o = NewOutbox(db, NewClient(true, "test"), slog.New(slog.NewTextHandler(io.Discard, nil)))
	f.o.now = func() time.Time { return f.clock }

	domain := strings.TrimPrefix(srv.URL, "http://")
	_, _, err = db.InsertMail(context.Background(), store.Mail{Sender: "alice",
		Recipient: "bobby@" + domain, Signature: "sig", Timestamp: signedAt.Unix()},
		&store.Relay{Domain: domain, Payload: `{"exact":"bytes"}`,
			SignedAt: signedAt.Unix(), NextTryAt: signedAt.Unix()})
	if err != nil {
		t.Fatal(err)
	}
	return f
}

func (f *outboxFixture) pending() int {
	d, _ := f.db.DueRelays(context.Background(), 1<<40, 10)
	return len(d)
}

func TestOutboxDelivers(t *testing.T) {
	f := newOutboxFixture(t, time.Unix(10_000, 0))
	f.o.drain(context.Background())
	if f.got.Load() != 1 || f.pending() != 0 {
		t.Fatalf("posts %d, pending %d", f.got.Load(), f.pending())
	}
	if f.body.Load() != `{"exact":"bytes"}` {
		t.Errorf("payload altered in transit: %q", f.body.Load())
	}
}

func TestOutboxRetriesTransientFailures(t *testing.T) {
	f := newOutboxFixture(t, time.Unix(10_000, 0), 503, 429)
	ctx := context.Background()
	f.o.drain(ctx) // 503
	f.o.drain(ctx) // not yet due
	if f.got.Load() != 1 || f.pending() != 1 {
		t.Fatalf("after 503: posts %d pending %d", f.got.Load(), f.pending())
	}
	f.clock = f.clock.Add(5 * time.Second)
	f.o.drain(ctx) // 429
	f.clock = f.clock.Add(15 * time.Second)
	f.o.drain(ctx) // 201
	if f.got.Load() != 3 || f.pending() != 0 {
		t.Errorf("posts %d pending %d, want 3 and 0", f.got.Load(), f.pending())
	}
}

func TestOutboxRefusalIsFinal(t *testing.T) {
	f := newOutboxFixture(t, time.Unix(10_000, 0), 403)
	f.o.drain(context.Background())
	f.clock = f.clock.Add(time.Minute)
	f.o.drain(context.Background())
	if f.got.Load() != 1 || f.pending() != 0 {
		t.Errorf("posts %d pending %d: a 403 was retried", f.got.Load(), f.pending())
	}
}

// A relay the remote can no longer accept (timestamp out of window) is
// dropped instead of retried forever.
func TestOutboxStopsAtSignatureWindow(t *testing.T) {
	signed := time.Unix(10_000, 0)
	f := newOutboxFixture(t, signed, 502, 502, 502, 502, 502, 502, 502, 502, 502)
	ctx := context.Background()
	for f.pending() > 0 && f.clock.Before(signed.Add(10*time.Minute)) {
		f.o.drain(ctx)
		f.clock = f.clock.Add(5 * time.Second)
	}
	if f.pending() != 0 {
		t.Fatal("relay still pending after the window")
	}
	if n := f.got.Load(); n < 3 || n > 8 {
		t.Errorf("%d attempts inside a 280 s budget", n)
	}
}
