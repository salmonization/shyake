package federation

import (
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/salmonization/shyake/server/go/internal/protocol"
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

func TestRelay(t *testing.T) {
	var got atomic.Value
	status := http.StatusCreated
	reply := `{"message":"Mail sent","id":"x"}`
	remote := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		got.Store(string(b))
		w.WriteHeader(status)
		io.WriteString(w, reply)
	}))
	defer remote.Close()
	c := NewClient(true, "test")
	ctx := context.Background()
	domain := strings.TrimPrefix(remote.URL, "http://")

	st, text, err := c.Relay(ctx, domain, []byte(`{"exact":"bytes"}`))
	if err != nil || st != http.StatusCreated || text != "" {
		t.Fatalf("delivered: %d %q %v", st, text, err)
	}
	if got.Load() != `{"exact":"bytes"}` {
		t.Errorf("payload changed on the way: %q", got.Load())
	}

	status, reply = http.StatusForbidden, "{\"error\":\"Recipient has blocked\\u0007 this sender\"}"
	st, text, err = c.Relay(ctx, domain, []byte(`{}`))
	if err != nil || st != http.StatusForbidden || text != "Recipient has blocked this sender" {
		t.Errorf("refusal: %d %q %v", st, text, err)
	}

	if _, _, err := c.Relay(ctx, "127.0.0.1:1", []byte(`{}`)); err == nil {
		t.Error("dead instance: no error")
	}
}
