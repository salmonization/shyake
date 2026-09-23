// Package config reads the server settings from the environment.
//
// The names mirror the Worker's wrangler.toml [vars] with a SHYAKE_
// prefix, so an operator moving between the two recognizes them.
package config

import (
	"errors"
	"fmt"
	"net/netip"
	"os"
	"strconv"
	"strings"

	"github.com/salmonization/shyake/server/go/internal/protocol"
)

type Config struct {
	// Version is the build's release tag, set by main (not from the
	// environment); "dev" for a build without one.
	Version string

	InstanceDomain      string
	Listen              string
	Database            string // SQLite file path
	RegistrationEnabled bool
	ReservedUsernames   []string
	FederationEnabled   bool
	MaxMailSize         int64
	TrustedProxies      []netip.Prefix
	RateLimit           float64 // requests per second per client IP
	RateBurst           int
	FederationInsecure  bool
	LogFormat           string // text | json
}

// DefaultReserved matches the Worker's default RESERVED_USERNAMES.
const DefaultReserved = "admin,system,support,noreply,shyake,root,postmaster"

// MaxMailCeiling is the Worker's hard limit (D1 row size). A Go instance
// could store more, but a larger mail would be refused by any Worker
// instance it is relayed to.
const MaxMailCeiling = 786432

// Load reads the configuration. getenv is os.Getenv outside tests.
func Load(getenv func(string) string) (Config, error) {
	if getenv == nil {
		getenv = os.Getenv
	}
	var errs []error
	get := func(name, def string) string {
		if v := strings.TrimSpace(getenv("SHYAKE_" + name)); v != "" {
			return v
		}
		return def
	}
	boolean := func(name string, def bool) bool {
		v := get(name, strconv.FormatBool(def))
		b, err := strconv.ParseBool(v)
		if err != nil {
			errs = append(errs, fmt.Errorf("SHYAKE_%s: %q is not true or false", name, v))
		}
		return b
	}

	c := Config{
		InstanceDomain:      strings.ToLower(get("INSTANCE_DOMAIN", "")),
		Listen:              get("LISTEN", "127.0.0.1:8787"),
		Database:            get("DATABASE", "shyake.db"),
		RegistrationEnabled: boolean("REGISTRATION_ENABLED", true),
		FederationEnabled:   boolean("FEDERATION_ENABLED", true),
		FederationInsecure:  boolean("FEDERATION_INSECURE", false),
		LogFormat:           get("LOG_FORMAT", "text"),
	}

	if c.InstanceDomain == "" {
		errs = append(errs, errors.New("SHYAKE_INSTANCE_DOMAIN is required"))
	} else if !protocol.ValidDomain(c.InstanceDomain) {
		errs = append(errs, fmt.Errorf("SHYAKE_INSTANCE_DOMAIN: %q is not a host name", c.InstanceDomain))
	}

	// PostgreSQL is planned behind the same store interface; until
	// then, say so rather than treat the URL as a file name
	if db := strings.ToLower(c.Database); strings.HasPrefix(db, "postgres://") ||
		strings.HasPrefix(db, "postgresql://") {
		errs = append(errs, errors.New("SHYAKE_DATABASE: PostgreSQL is not supported yet; give a SQLite file path"))
	}
	c.Database = strings.TrimPrefix(c.Database, "sqlite:")

	for _, r := range strings.Split(get("RESERVED_USERNAMES", DefaultReserved), ",") {
		if r = strings.TrimSpace(r); r != "" {
			c.ReservedUsernames = append(c.ReservedUsernames, r)
		}
	}

	size, err := strconv.ParseInt(get("MAX_MAIL_SIZE", "196608"), 10, 64)
	switch {
	case err != nil || size <= 0:
		errs = append(errs, errors.New("SHYAKE_MAX_MAIL_SIZE must be a positive integer"))
	case size > MaxMailCeiling:
		errs = append(errs, fmt.Errorf("SHYAKE_MAX_MAIL_SIZE must not exceed %d, the limit of Worker instances", MaxMailCeiling))
	}
	c.MaxMailSize = size

	for _, p := range strings.Split(get("TRUSTED_PROXIES", "127.0.0.1/32,::1/128"), ",") {
		p = strings.TrimSpace(p)
		if p == "" {
			continue
		}
		prefix, err := netip.ParsePrefix(p)
		if err != nil {
			if addr, aerr := netip.ParseAddr(p); aerr == nil {
				prefix = netip.PrefixFrom(addr, addr.BitLen())
			} else {
				errs = append(errs, fmt.Errorf("SHYAKE_TRUSTED_PROXIES: %q is not an address or CIDR", p))
				continue
			}
		}
		c.TrustedProxies = append(c.TrustedProxies, prefix.Masked())
	}

	c.RateLimit, err = strconv.ParseFloat(get("RATE_LIMIT", "5"), 64)
	if err != nil || c.RateLimit <= 0 {
		errs = append(errs, errors.New("SHYAKE_RATE_LIMIT must be a positive number (requests per second)"))
	}
	c.RateBurst, err = strconv.Atoi(get("RATE_BURST", "30"))
	if err != nil || c.RateBurst <= 0 {
		errs = append(errs, errors.New("SHYAKE_RATE_BURST must be a positive integer"))
	}

	if c.LogFormat != "text" && c.LogFormat != "json" {
		errs = append(errs, errors.New("SHYAKE_LOG_FORMAT must be text or json"))
	}
	return c, errors.Join(errs...)
}
