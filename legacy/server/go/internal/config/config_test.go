package config

import (
	"strings"
	"testing"
)

func env(kv ...string) func(string) string {
	m := map[string]string{}
	for i := 0; i < len(kv); i += 2 {
		m[kv[i]] = kv[i+1]
	}
	return func(k string) string { return m[k] }
}

func TestDefaults(t *testing.T) {
	c, err := Load(env("SHYAKE_INSTANCE_DOMAIN", "Mail.Example.org"))
	if err != nil {
		t.Fatal(err)
	}
	if c.InstanceDomain != "mail.example.org" || c.Listen != "127.0.0.1:8787" ||
		c.Database != "shyake.db" || !c.RegistrationEnabled || !c.FederationEnabled ||
		c.MaxMailSize != 196608 || c.FederationInsecure || len(c.ReservedUsernames) != 7 {
		t.Errorf("defaults: %+v", c)
	}
	if len(c.TrustedProxies) != 2 {
		t.Errorf("trusted proxies: %v", c.TrustedProxies)
	}
}

func TestErrors(t *testing.T) {
	for _, c := range []struct {
		env  []string
		want string
	}{
		{nil, "SHYAKE_INSTANCE_DOMAIN is required"},
		{[]string{"SHYAKE_INSTANCE_DOMAIN", "https://x.org/"}, "not a host name"},
		{[]string{"SHYAKE_INSTANCE_DOMAIN", "x.org", "SHYAKE_DATABASE", "postgres://u@h/db"}, "PostgreSQL is not supported yet"},
		{[]string{"SHYAKE_INSTANCE_DOMAIN", "x.org", "SHYAKE_MAX_MAIL_SIZE", "1000000"}, "must not exceed"},
		{[]string{"SHYAKE_INSTANCE_DOMAIN", "x.org", "SHYAKE_FEDERATION_ENABLED", "yes please"}, "not true or false"},
		{[]string{"SHYAKE_INSTANCE_DOMAIN", "x.org", "SHYAKE_TRUSTED_PROXIES", "not-an-ip"}, "not an address or CIDR"},
	} {
		_, err := Load(env(c.env...))
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("env %v: err %v, want %q", c.env, err, c.want)
		}
	}
}

func TestOverrides(t *testing.T) {
	c, err := Load(env("SHYAKE_INSTANCE_DOMAIN", "x.org", "SHYAKE_DATABASE", "sqlite:/var/lib/shyake/db",
		"SHYAKE_TRUSTED_PROXIES", "10.0.0.5, 192.168.0.0/16", "SHYAKE_REGISTRATION_ENABLED", "false",
		"SHYAKE_RESERVED_USERNAMES", " root , , admin "))
	if err != nil {
		t.Fatal(err)
	}
	if c.Database != "/var/lib/shyake/db" {
		t.Errorf("sqlite: prefix kept: %q", c.Database)
	}
	if c.RegistrationEnabled || len(c.ReservedUsernames) != 2 {
		t.Errorf("overrides: %+v", c)
	}
	if got := c.TrustedProxies[0].String(); got != "10.0.0.5/32" {
		t.Errorf("bare proxy address: %s", got)
	}
}
