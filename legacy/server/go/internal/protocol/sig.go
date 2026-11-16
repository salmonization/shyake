package protocol

import (
	"encoding/base64"
	"errors"
	"strings"

	"github.com/cloudflare/circl/sign/mldsa/mldsa65"
)

var ErrBadKey = errors.New("invalid public key")

// PublicKey is a parsed ML-DSA-65 verification key.
type PublicKey struct{ k mldsa65.PublicKey }

// ParsePublicKey decodes a base64 ML-DSA-65 public key as stored in
// users.sig_pubkey.
func ParsePublicKey(b64 string) (*PublicKey, error) {
	raw, err := decodeB64(b64)
	if err != nil || len(raw) != mldsa65.PublicKeySize {
		return nil, ErrBadKey
	}
	var pk PublicKey
	if err := pk.k.UnmarshalBinary(raw); err != nil {
		return nil, ErrBadKey
	}
	return &pk, nil
}

// Verify checks a base64 signature over msg. This is pure ML-DSA-65
// with an empty context, which is what liboqs OQS_SIG_sign produces.
func (pk *PublicKey) Verify(msg []byte, sigB64 string) bool {
	sig, err := decodeB64(sigB64)
	if err != nil || len(sig) != mldsa65.SignatureSize {
		return false
	}
	return mldsa65.Verify(&pk.k, msg, nil, sig)
}

// decodeB64 accepts the standard and URL alphabets, padded or not. The
// Worker normalizes to unpadded base64url before verifying, so both
// forms are in use on the wire.
func decodeB64(s string) ([]byte, error) {
	s = strings.Map(func(r rune) rune {
		switch r {
		case ' ', '\t', '\n', '\r':
			return -1
		case '-':
			return '+'
		case '_':
			return '/'
		}
		return r
	}, s)
	return base64.RawStdEncoding.DecodeString(strings.TrimRight(s, "="))
}

// DecodeKEMKey decodes a base64 ML-KEM public key for fingerprinting.
func DecodeKEMKey(b64 string) ([]byte, error) { return decodeB64(b64) }
