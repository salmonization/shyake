package protocol

import (
	"crypto/sha1"
	"errors"
	"strconv"
	"strings"
	"time"
)

// PoWBits is the difficulty every authenticated request pays (SPEC §3.5).
const PoWBits = 20

// maxPoWLen bounds the hashing work an unauthenticated caller can ask for.
const maxPoWLen = 256

var ErrBadPoW = errors.New("invalid proof of work")

// VerifyPoW checks a Hashcash v1 token:
//
//	1:<bits>:<yymmdd>:<resource>::<rand>:<counter-hex>
//
// Beyond the leading zero bits, the token must name the acting user as
// its resource and carry a date within one day of now. SPEC §3.5 requires
// the resource binding. Without it one minted token pays for any request
// by anyone, forever.
func VerifyPoW(token, resource string, now time.Time) error {
	if len(token) > maxPoWLen {
		return ErrBadPoW
	}
	// The resource of a federated sender is "user@host:port" when the
	// instance runs on a port, so it can hold colons. The three trailing
	// fields (ext, rand, counter) are fixed, so cut from both ends.
	parts := strings.Split(token, ":")
	n := len(parts)
	if n < 7 || parts[0] != "1" {
		return ErrBadPoW
	}
	claimed, err := strconv.Atoi(parts[1])
	if err != nil || claimed < PoWBits {
		return ErrBadPoW
	}
	if strings.Join(parts[3:n-3], ":") != resource || !freshDate(parts[2], now) {
		return ErrBadPoW
	}
	if !leadingZeroBits(sha1.Sum([]byte(token)), PoWBits) {
		return ErrBadPoW
	}
	return nil
}

// freshDate accepts yesterday, today, or tomorrow (UTC), which covers
// clock skew around midnight.
func freshDate(yymmdd string, now time.Time) bool {
	d, err := time.Parse("060102", yymmdd)
	if err != nil {
		return false
	}
	today := now.UTC().Truncate(24 * time.Hour)
	delta := d.Sub(today)
	return delta >= -24*time.Hour && delta <= 24*time.Hour
}

func leadingZeroBits(h [sha1.Size]byte, bits int) bool {
	full, rem := bits/8, bits%8
	for i := 0; i < full; i++ {
		if h[i] != 0 {
			return false
		}
	}
	return rem == 0 || h[full]>>(8-rem) == 0
}
