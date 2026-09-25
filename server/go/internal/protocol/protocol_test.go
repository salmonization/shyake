package protocol

import (
	"crypto/sha1"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"
)

// liboqs_vectors.json is made by testdata/gen_vectors.c with the real
// liboqs and the client's vendored cJSON. Passing here means circl
// accepts liboqs signatures and our rebuild of each signed message is
// byte-exact with what the C client signs.
func TestLiboqsVectors(t *testing.T) {
	raw, err := os.ReadFile("testdata/liboqs_vectors.json")
	if err != nil {
		t.Fatal(err)
	}
	var file struct {
		SigPubkey string `json:"sig_pubkey"`
		Vectors   []struct {
			Kind      string         `json:"kind"`
			Fields    map[string]any `json:"fields"`
			Signed    string         `json:"signed"`
			Signature string         `json:"signature"`
		} `json:"vectors"`
	}
	if err := json.Unmarshal(raw, &file); err != nil {
		t.Fatal(err)
	}
	pk, err := ParsePublicKey(file.SigPubkey)
	if err != nil {
		t.Fatal(err)
	}
	if len(file.Vectors) == 0 {
		t.Fatal("no vectors")
	}
	for i, v := range file.Vectors {
		s := func(k string) string { return v.Fields[k].(string) }
		var msg []byte
		switch v.Kind {
		case "register":
			msg = RegisterMessage(s("username"), s("kem_pubkey"), s("sig_pubkey"), s("timestamp"))
		case "mail":
			msg = MailMessage(s("sender"), s("recipient"), s("recipient_kem_fingerprint"),
				s("enc_subject"), s("enc_body"), s("timestamp"), int64(v.Fields["size"].(float64)))
		case "header":
			msg = HeaderMessage(s("method"), s("path"), s("username"), s("timestamp"))
		default:
			t.Fatalf("vector %d: unknown kind %q", i, v.Kind)
		}
		if string(msg) != v.Signed {
			t.Errorf("vector %d (%s): rebuild differs from cJSON\n got %q\nwant %q", i, v.Kind, msg, v.Signed)
			continue
		}
		if !pk.Verify(msg, v.Signature) {
			t.Errorf("vector %d (%s): circl rejected the liboqs signature", i, v.Kind)
		}
		if pk.Verify(append(msg, ' '), v.Signature) {
			t.Errorf("vector %d (%s): verified a modified message", i, v.Kind)
		}
	}
}

func TestSignatureBase64Forms(t *testing.T) {
	raw, _ := os.ReadFile("testdata/liboqs_vectors.json")
	var f struct {
		SigPubkey string `json:"sig_pubkey"`
		Vectors   []struct {
			Signed, Signature string
		} `json:"vectors"`
	}
	json.Unmarshal(raw, &f)
	v := f.Vectors[len(f.Vectors)-1]
	url := strings.NewReplacer("+", "-", "/", "_", "=", "").Replace(v.Signature)
	pkURL := strings.NewReplacer("+", "-", "/", "_", "=", "").Replace(f.SigPubkey)
	pk, err := ParsePublicKey(pkURL)
	if err != nil {
		t.Fatal("base64url public key:", err)
	}
	if !pk.Verify([]byte(v.Signed), url) {
		t.Error("unpadded base64url signature rejected")
	}
}

func TestValidUsername(t *testing.T) {
	for s, want := range map[string]bool{
		"salmon": true, "al_1": true, "A_B_": true, "abcdefghijklmnop": true,
		"abc": false, "abcdefghijklmnopq": false, "1234": false, "____": false,
		"al-ice": false, "al.ice": false, "al@ice": false, "": false, "ålice": false,
	} {
		if ValidUsername(s) != want {
			t.Errorf("ValidUsername(%q) = %v", s, !want)
		}
	}
}

func TestParseAddress(t *testing.T) {
	const own = "My.Instance"
	for _, c := range []struct {
		in, stored, domain string
		local              bool
	}{
		{"alice", "alice", "my.instance", true},
		{"alice@my.instance", "alice", "my.instance", true},
		{"alice@MY.Instance", "alice", "my.instance", true},
		{"Alice@Evil.Example", "Alice@evil.example", "evil.example", false},
		{"bobby@localhost:8787", "bobby@localhost:8787", "localhost:8787", false},
	} {
		a, err := ParseAddress(c.in, own)
		if err != nil {
			t.Errorf("%q: %v", c.in, err)
			continue
		}
		if a.Stored() != c.stored || a.Domain() != c.domain || a.IsLocal() != c.local {
			t.Errorf("%q: got (%q, %q, %v)", c.in, a.Stored(), a.Domain(), a.IsLocal())
		}
	}
	for _, bad := range []string{
		"", "ab", "alice@", "alice@@x.com", "alice@x.com/path", "alice@x.com?q",
		"alice@user@x.com", "alice@-x.com", "alice@x..com", "alice@[::1]",
		"alice@x.com:", "alice@x.com:123456", "alice@x.com:80a", "alice@ex ample.com",
	} {
		if _, err := ParseAddress(bad, own); err == nil {
			t.Errorf("ParseAddress(%q) accepted", bad)
		}
	}
}

func TestParseBlockTarget(t *testing.T) {
	for in, stored := range map[string]string{
		"Evil.EXAMPLE":        "evil.example",
		"mallory":             "mallory",
		"mallory@my.instance": "mallory",
		"mallory@Evil.com":    "mallory@evil.com",
	} {
		a, err := ParseBlockTarget(in, "my.instance")
		if err != nil || a.Stored() != stored {
			t.Errorf("%q: got %q, %v", in, a.Stored(), err)
		}
	}
	// a remote block must not collide with a local user of the same name
	r, _ := ParseBlockTarget("mallory@evil.com", "my.instance")
	l, _ := ParseBlockTarget("mallory", "my.instance")
	if r.Stored() == l.Stored() {
		t.Error("remote and local mallory store the same")
	}
}

// mint mirrors shyake_mint_pow in client/src/lib/libshyake.c.
func mint(resource string, bits int, date time.Time) string {
	for c := uint64(0); ; c++ {
		tok := fmt.Sprintf("1:%d:%s:%s::abcdefghijkl:%x", bits, date.UTC().Format("060102"), resource, c)
		if leadingZeroBits(sha1.Sum([]byte(tok)), bits) {
			return tok
		}
	}
}

func TestVerifyPoW(t *testing.T) {
	now := time.Date(2026, 9, 22, 12, 0, 0, 0, time.UTC)
	tok := mint("salmon", PoWBits, now)
	if err := VerifyPoW(tok, "salmon", now); err != nil {
		t.Fatal("valid token rejected:", err)
	}
	if VerifyPoW(tok, "mallory", now) == nil {
		t.Error("token accepted for another resource")
	}
	if VerifyPoW(tok, "salmon", now.Add(72*time.Hour)) == nil {
		t.Error("stale token accepted")
	}
	if VerifyPoW(tok, "salmon", now.Add(-25*time.Hour)) != nil {
		t.Error("token from tomorrow (clock skew) rejected")
	}
	ported := mint("alice@127.0.0.1:8787", PoWBits, now)
	if err := VerifyPoW(ported, "alice@127.0.0.1:8787", now); err != nil {
		t.Error("resource with a port rejected:", err)
	}
	if VerifyPoW(ported, "alice@127.0.0.1", now) == nil {
		t.Error("ported token accepted for the portless resource")
	}
	weak := mint("salmon", 8, now)
	if VerifyPoW(weak, "salmon", now) == nil {
		t.Error("8-bit token accepted")
	}
	// claims 20 bits but was mined for fewer
	lying := strings.Replace(weak, ":8:", ":20:", 1)
	if VerifyPoW(lying, "salmon", now) == nil && !leadingZeroBits(sha1.Sum([]byte(lying)), PoWBits) {
		t.Error("token with inflated bit claim accepted")
	}
	for _, bad := range []string{"", "1:20:260922:salmon", "2" + tok[1:], strings.Repeat("x", 300)} {
		if VerifyPoW(bad, "salmon", now) == nil {
			t.Errorf("malformed token %q accepted", bad)
		}
	}
}

func TestHeaderBodyMessage(t *testing.T) {
	got := string(HeaderBodyMessage("POST", "/api/block", "bobby", "1700000000", []byte(`{"target":"alice"}`)))
	want := "POST:/api/block:bobby:1700000000:a09842c7bd02453ac46c510bf066cabfd1e9b2220e2a95b87672adf4d6b1bcb7"
	if got != want {
		t.Errorf("got %q, want %q", got, want)
	}
}
