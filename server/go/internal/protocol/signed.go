package protocol

import (
	"strconv"
	"unicode/utf8"
)

// Signed messages (SPEC §3.3).
//
// Body-signed requests are verified against a compact JSON rebuild of a
// fixed field subset, in client order. The rebuild must be byte-exact
// with what the client signed (cJSON_PrintUnformatted) and with what the
// Worker rebuilds (JSON.stringify). encoding/json cannot serve here: it
// escapes <, >, & and U+2028/U+2029, which neither of the other two do.

// RegisterMessage is the signed subset of POST /api/register.
func RegisterMessage(username, kemPub, sigPub, timestamp string) []byte {
	var b jsonObj
	b.str("username", username)
	b.str("kem_pubkey", kemPub)
	b.str("sig_pubkey", sigPub)
	b.str("timestamp", timestamp)
	return b.done()
}

// MailMessage is the signed subset of POST /api/mail.
func MailMessage(sender, recipient, fingerprint, encSubject, encBody,
	timestamp string, size int64) []byte {
	var b jsonObj
	b.str("sender", sender)
	b.str("recipient", recipient)
	b.str("recipient_kem_fingerprint", fingerprint)
	b.str("enc_subject", encSubject)
	b.str("enc_body", encBody)
	b.str("timestamp", timestamp)
	b.num("size", size)
	return b.done()
}

// HeaderMessage is the signed string of a header-authenticated request:
// METHOD:/path[?query]:username:timestamp
func HeaderMessage(method, path, username, timestamp string) []byte {
	return []byte(method + ":" + path + ":" + username + ":" + timestamp)
}

type jsonObj struct{ buf []byte }

func (o *jsonObj) key(k string) {
	if len(o.buf) == 0 {
		o.buf = append(o.buf, '{')
	} else {
		o.buf = append(o.buf, ',')
	}
	o.buf = appendJSString(o.buf, k)
	o.buf = append(o.buf, ':')
}

func (o *jsonObj) str(k, v string) {
	o.key(k)
	o.buf = appendJSString(o.buf, v)
}

func (o *jsonObj) num(k string, v int64) {
	o.key(k)
	o.buf = strconv.AppendInt(o.buf, v, 10)
}

func (o *jsonObj) done() []byte { return append(o.buf, '}') }

// appendJSString quotes s the way JSON.stringify and cJSON both do:
// short escapes for \b \t \n \f \r, \u00xx (lowercase) for the other
// control characters, everything else verbatim.
func appendJSString(b []byte, s string) []byte {
	const hex = "0123456789abcdef"
	b = append(b, '"')
	for i := 0; i < len(s); {
		c := s[i]
		if c >= utf8.RuneSelf {
			_, n := utf8.DecodeRuneInString(s[i:])
			b = append(b, s[i:i+n]...)
			i += n
			continue
		}
		switch c {
		case '"':
			b = append(b, '\\', '"')
		case '\\':
			b = append(b, '\\', '\\')
		case '\b':
			b = append(b, '\\', 'b')
		case '\f':
			b = append(b, '\\', 'f')
		case '\n':
			b = append(b, '\\', 'n')
		case '\r':
			b = append(b, '\\', 'r')
		case '\t':
			b = append(b, '\\', 't')
		default:
			if c < 0x20 {
				b = append(b, '\\', 'u', '0', '0', hex[c>>4], hex[c&0xf])
			} else {
				b = append(b, c)
			}
		}
		i++
	}
	return append(b, '"')
}
