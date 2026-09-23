package federation

import (
	"context"
	"encoding/json"
	"strings"
	"time"
	"unicode"
)

// relayTimeout bounds one relay, so a slow remote cannot hold the
// sender's request open.
const relayTimeout = 15 * time.Second

// Relay posts a mail submission to the recipient's instance, byte for
// byte: the remote re-verifies the sender's signature over it. It
// returns the remote's status and, when the remote refuses, the error
// text of its reply. A transport failure is an error.
//
// There is no queue. The sender's client learns the outcome at once
// and keeps the mail as a draft on failure.
func (c *Client) Relay(ctx context.Context, domain string, payload []byte) (int, string, error) {
	ctx, cancel := context.WithTimeout(ctx, relayTimeout)
	defer cancel()
	status, body, err := c.postJSON(ctx, c.URL(domain, "/api/mail"), string(payload))
	if err != nil {
		return 0, "", err
	}
	var reply struct {
		Error string `json:"error"`
	}
	json.Unmarshal(body, &reply)
	return status, cleanRemoteText(reply.Error), nil
}

// cleanRemoteText makes a remote's error text safe to hand to our
// client: no control or format characters, at most 200 characters.
func cleanRemoteText(s string) string {
	s = strings.Map(func(r rune) rune {
		if unicode.Is(unicode.C, r) {
			return -1
		}
		return r
	}, s)
	if r := []rune(s); len(r) > 200 {
		s = string(r[:200])
	}
	return s
}
