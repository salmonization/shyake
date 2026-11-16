// Package protocol holds the wire-level rules of Shyake (docs/SPEC.md):
// addresses, proof of work, signatures, and the signed messages. It does
// no I/O.
package protocol

import (
	"errors"
	"strings"
)

// Username rules: ^(?=.*[a-zA-Z])[a-zA-Z0-9_]{4,16}$ (SPEC §4).
func ValidUsername(s string) bool {
	if len(s) < 4 || len(s) > 16 {
		return false
	}
	letter := false
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z':
			letter = true
		case c >= '0' && c <= '9', c == '_':
		default:
			return false
		}
	}
	return letter
}

// IsReserved compares case-insensitively against the reserved list.
func IsReserved(name string, reserved []string) bool {
	for _, r := range reserved {
		if strings.EqualFold(name, r) {
			return true
		}
	}
	return false
}

// ValidDomain accepts a lowercase hostname with an optional port. It
// guards every outbound federation URL, so it is deliberately strict:
// no userinfo, path, query, or IPv6 literal can pass.
func ValidDomain(d string) bool {
	host, port, hasPort := strings.Cut(d, ":")
	if hasPort {
		if len(port) == 0 || len(port) > 5 {
			return false
		}
		for i := 0; i < len(port); i++ {
			if port[i] < '0' || port[i] > '9' {
				return false
			}
		}
	}
	if len(host) == 0 || len(host) > 253 {
		return false
	}
	for _, label := range strings.Split(host, ".") {
		if len(label) == 0 || len(label) > 63 ||
			label[0] == '-' || label[len(label)-1] == '-' {
			return false
		}
		for i := 0; i < len(label); i++ {
			c := label[i]
			if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-') {
				return false
			}
		}
	}
	return true
}

// Address is a parsed Shyake address: a user on some instance, or a
// whole instance (a blocklist target). Only the parse functions make
// one, so holding an Address means the input was valid.
type Address struct {
	local  string // username; empty for a bare-domain target
	domain string // lowercased
	own    bool   // domain is this instance
}

var (
	ErrBadUsername = errors.New("invalid username format")
	ErrBadDomain   = errors.New("invalid domain")
)

// ParseAddress parses "user" (this instance) or "user@domain".
func ParseAddress(s, instanceDomain string) (Address, error) {
	local, domain, qualified := strings.Cut(s, "@")
	if !ValidUsername(local) {
		return Address{}, ErrBadUsername
	}
	own := strings.ToLower(instanceDomain)
	if !qualified {
		return Address{local: local, domain: own, own: true}, nil
	}
	domain = strings.ToLower(domain)
	if !ValidDomain(domain) {
		return Address{}, ErrBadDomain
	}
	return Address{local: local, domain: domain, own: domain == own}, nil
}

// ParseBlockTarget also accepts a bare domain. Usernames contain
// neither "@" nor ".", so a dotted string without "@" is a domain.
func ParseBlockTarget(s, instanceDomain string) (Address, error) {
	if !strings.Contains(s, "@") && strings.Contains(s, ".") {
		domain := strings.ToLower(s)
		if !ValidDomain(domain) {
			return Address{}, ErrBadDomain
		}
		return Address{domain: domain, own: domain == strings.ToLower(instanceDomain)}, nil
	}
	return ParseAddress(s, instanceDomain)
}

// Stored is the database form: local users bare, remote users
// "user@domain", bare domains as themselves (SPEC §4, blocks).
func (a Address) Stored() string {
	switch {
	case a.local == "":
		return a.domain
	case a.own:
		return a.local
	default:
		return a.local + "@" + a.domain
	}
}

func (a Address) Local() string  { return a.local }
func (a Address) Domain() string { return a.domain }
func (a Address) IsLocal() bool  { return a.own }
