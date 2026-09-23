package api

import (
	"encoding/json"
	"testing"
)

// A server-only release is not offered to clients.
func TestSummarizeSkipsServerOnlyReleases(t *testing.T) {
	var releases []ghRelease
	json.Unmarshal([]byte(`[
		{"tag_name":"v0.3.1","assets":[{"name":"shyake-server-linux-amd64.tar.gz","digest":"sha256:aa"}]},
		{"tag_name":"v0.3.1-rc.1","prerelease":true,"assets":[]},
		{"tag_name":"v0.3.0","assets":[{"name":"shyake-linux-x86_64.tar.gz","digest":"sha256:bb"},
			{"name":"shyake-server-linux-amd64.tar.gz","digest":"sha256:cc"}]},
		{"tag_name":"v0.3.0-rc.1","prerelease":true,"assets":[{"name":"shyake-darwin-arm64.tar.gz","digest":"sha256:dd"}]}
	]`), &releases)
	got := summarize(releases)
	if got["release"] != "v0.3.0" || got["pre_release"] != "v0.3.0-rc.1" {
		t.Errorf("picked %v / %v", got["release"], got["pre_release"])
	}
	if d := got["release_digests"].(map[string]string); d["shyake-linux-x86_64.tar.gz"] != "bb" {
		t.Errorf("digests: %v", d)
	}
}

func TestServerVersion(t *testing.T) {
	e := newEnv(t)
	c, out := e.do("GET", "/api/version", nil, nil)
	if c != 200 || out["implementation"] != "go" || out["protocol"] != float64(2) ||
		out["version"] != "dev" {
		t.Errorf("%d %v", c, out)
	}
}
