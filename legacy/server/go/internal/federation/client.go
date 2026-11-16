// Package federation talks to other instances: it fetches remote users'
// public keys and relays outbound mail.
//
// Every URL here is built from a domain that some client or remote
// instance chose. The HTTP client therefore refuses to connect to
// loopback, private, link-local, and other non-public addresses. The
// check runs on the address actually dialed, after DNS, so a hostname
// that re-resolves to an internal address (DNS rebinding) still fails.
package federation

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/netip"
	"strings"
	"syscall"
	"time"
)

var ErrForbiddenAddress = errors.New("federation: refusing non-public address")

// Client is an HTTP client for instance-to-instance requests.
type Client struct {
	http   *http.Client
	scheme string
	ua     string
}

// NewClient returns a guarded client. insecure permits plain HTTP and
// non-public addresses; it exists only to federate instances on one
// machine in tests.
func NewClient(insecure bool, userAgent string) *Client {
	dialer := &net.Dialer{Timeout: 5 * time.Second}
	if !insecure {
		dialer.Control = func(_, address string, _ syscall.RawConn) error {
			host, _, err := net.SplitHostPort(address)
			if err != nil {
				return err
			}
			ip, err := netip.ParseAddr(host)
			if err != nil || !publicAddr(ip) {
				return ErrForbiddenAddress
			}
			return nil
		}
	}
	transport := &http.Transport{
		DialContext:           dialer.DialContext,
		Proxy:                 nil, // a proxy would dial for us, past the guard
		TLSHandshakeTimeout:   5 * time.Second,
		ResponseHeaderTimeout: 10 * time.Second,
		MaxIdleConnsPerHost:   4,
		IdleConnTimeout:       60 * time.Second,
	}
	scheme := "https"
	if insecure {
		scheme = "http"
	}
	return &Client{
		http: &http.Client{
			Transport: transport,
			Timeout:   15 * time.Second,
			// a redirect could point anywhere; instances do not redirect
			CheckRedirect: func(*http.Request, []*http.Request) error {
				return http.ErrUseLastResponse
			},
		},
		scheme: scheme,
		ua:     userAgent,
	}
}

// URL builds the address of path on an instance. domain must already
// have passed protocol.ValidDomain.
func (c *Client) URL(domain, path string) string {
	return c.scheme + "://" + domain + path
}

// Do sends req and returns the status and at most limit bytes of body.
func (c *Client) Do(req *http.Request, limit int64) (int, []byte, error) {
	req.Header.Set("User-Agent", c.ua)
	resp, err := c.http.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, limit+1))
	if err != nil {
		return resp.StatusCode, nil, err
	}
	if int64(len(body)) > limit {
		return resp.StatusCode, nil, fmt.Errorf("federation: response over %d bytes", limit)
	}
	return resp.StatusCode, body, nil
}

func (c *Client) get(ctx context.Context, url string, limit int64) (int, []byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return 0, nil, err
	}
	return c.Do(req, limit)
}

func (c *Client) postJSON(ctx context.Context, url, body string) (int, []byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, strings.NewReader(body))
	if err != nil {
		return 0, nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	return c.Do(req, 64<<10)
}

// publicAddr reports whether ip is globally routable.
func publicAddr(ip netip.Addr) bool {
	ip = ip.Unmap()
	if !ip.IsGlobalUnicast() || ip.IsPrivate() || ip.IsLoopback() ||
		ip.IsLinkLocalUnicast() || ip.IsMulticast() || ip.IsUnspecified() {
		return false
	}
	for _, p := range blockedPrefixes {
		if p.Contains(ip) {
			return false
		}
	}
	return true
}

// Special-purpose ranges that IsGlobalUnicast still accepts.
var blockedPrefixes = func() []netip.Prefix {
	var out []netip.Prefix
	for _, s := range []string{
		"0.0.0.0/8",       // "this network"
		"100.64.0.0/10",   // carrier-grade NAT
		"192.0.0.0/24",    // IETF protocol assignments
		"192.0.2.0/24",    // documentation
		"198.18.0.0/15",   // benchmarking
		"198.51.100.0/24", // documentation
		"203.0.113.0/24",  // documentation
		"240.0.0.0/4",     // reserved
		"64:ff9b::/96",    // NAT64: would reach IPv4 space unchecked
		"64:ff9b:1::/48",  // local-use NAT64
		"2001:db8::/32",   // documentation
		"2002::/16",       // 6to4: embeds an arbitrary IPv4 address
	} {
		out = append(out, netip.MustParsePrefix(s))
	}
	return out
}()
