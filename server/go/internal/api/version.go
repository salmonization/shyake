package api

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"

	"golang.org/x/sync/singleflight"

	"github.com/salmonization/shyake/server/go/internal/protocol"
)

// GET /api/version names this server's release and protocol level
// (SPEC §5.4). Clients read the level to know which request formats the
// instance accepts.
func (s *Server) serverVersion(w http.ResponseWriter, r *http.Request) {
	v := s.cfg.Version
	if v == "" {
		v = "dev"
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"version": v, "implementation": "go", "protocol": protocol.Level})
}

// GET /api/client/version relays the newest stable and preview tags,
// with per-asset SHA-256 digests, from the GitHub Releases API (SPEC §12).
// The answer is cached for an hour; if GitHub fails after that, the last
// good answer is served rather than an error.

const (
	releasesURL = "https://api.github.com/repos/salmonization/shyake/releases"
	versionTTL  = time.Hour
)

type versionCache struct {
	client *http.Client
	url    string
	group  singleflight.Group

	mu   sync.Mutex
	body []byte
	at   time.Time
}

func newVersionCache() *versionCache {
	return &versionCache{client: &http.Client{Timeout: 15 * time.Second}, url: releasesURL}
}

func (s *Server) clientVersion(w http.ResponseWriter, r *http.Request) {
	v := s.version
	v.mu.Lock()
	body, at := v.body, v.at
	v.mu.Unlock()

	if body == nil || time.Since(at) >= versionTTL {
		fresh, err, _ := v.group.Do("", func() (any, error) {
			return v.fetch(context.WithoutCancel(r.Context()))
		})
		switch {
		case err == nil:
			body = fresh.([]byte)
			v.mu.Lock()
			v.body, v.at = body, time.Now()
			v.mu.Unlock()
		case body == nil:
			s.log.Warn("client version: GitHub", "err", err)
			fail(w, http.StatusBadGateway, "Failed to fetch releases")
			return
		default:
			s.log.Warn("client version: serving stale", "err", err)
		}
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write(body)
}

type ghRelease struct {
	Tag        string `json:"tag_name"`
	Draft      bool   `json:"draft"`
	Prerelease bool   `json:"prerelease"`
	Assets     []struct {
		Name   string `json:"name"`
		Digest string `json:"digest"`
	} `json:"assets"`
}

func (v *versionCache) fetch(ctx context.Context) ([]byte, error) {
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, v.url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "shyake-server/1.0")
	req.Header.Set("Accept", "application/vnd.github+json")
	resp, err := v.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, &httpError{resp.StatusCode}
	}
	var releases []ghRelease
	if err := json.NewDecoder(io.LimitReader(resp.Body, 8<<20)).Decode(&releases); err != nil {
		return nil, err
	}
	return json.Marshal(summarize(releases))
}

// summarize picks the newest non-draft release of each channel that
// carries client builds. A release with only server builds is skipped:
// clients would find no asset for their platform in it. GitHub lists
// releases newest first.
func summarize(releases []ghRelease) map[string]any {
	digests := func(r ghRelease) map[string]string {
		out := map[string]string{}
		for _, a := range r.Assets {
			if d, ok := strings.CutPrefix(a.Digest, "sha256:"); ok {
				out[a.Name] = d
			}
		}
		return out
	}
	payload := map[string]any{}
	for _, r := range releases {
		if r.Draft || !hasClientAsset(r) {
			continue
		}
		if !r.Prerelease && payload["release"] == nil {
			payload["release"] = r.Tag
			payload["release_digests"] = digests(r)
		}
		if r.Prerelease && payload["pre_release"] == nil {
			payload["pre_release"] = r.Tag
			payload["pre_release_digests"] = digests(r)
		}
		if payload["release"] != nil && payload["pre_release"] != nil {
			break
		}
	}
	return payload
}

// hasClientAsset reports whether r carries a client build, named
// shyake-<os>-<arch>.tar.gz; server builds are shyake-server-*.
func hasClientAsset(r ghRelease) bool {
	for _, a := range r.Assets {
		if strings.HasPrefix(a.Name, "shyake-") && !strings.HasPrefix(a.Name, "shyake-server-") &&
			strings.HasSuffix(a.Name, ".tar.gz") {
			return true
		}
	}
	return false
}

type httpError struct{ status int }

func (e *httpError) Error() string { return "HTTP " + http.StatusText(e.status) }
