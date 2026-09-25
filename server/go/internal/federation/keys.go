package federation

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/url"
	"sync"
	"time"

	"golang.org/x/sync/singleflight"

	"github.com/salmonization/shyake/server/go/internal/protocol"
)

var (
	// ErrRemoteNotFound: the remote instance answered, without the user.
	ErrRemoteNotFound = errors.New("federation: remote user not found")
	// ErrUnreachable: the remote instance could not be asked.
	ErrUnreachable = errors.New("federation: remote instance unreachable")
)

// RemoteKeys is a remote user's public keys.
type RemoteKeys struct {
	KEM string `json:"kem_pubkey"`
	Sig string `json:"sig_pubkey"`
}

const keyCacheTTL = time.Minute

// Keys resolves remote users' public keys. Concurrent lookups of one
// user share a single request, and answers are kept for a minute. A
// caller that suspects a stale key (a failed signature, a fingerprint
// mismatch) asks again with fresh = true.
type Keys struct {
	c     *Client
	group singleflight.Group

	mu    sync.Mutex
	cache map[string]cached
}

type cached struct {
	keys RemoteKeys
	at   time.Time
}

func NewKeys(c *Client) *Keys {
	return &Keys{c: c, cache: make(map[string]cached)}
}

// Lookup fetches GET /api/pubkey/<user> from the user's instance.
func (k *Keys) Lookup(ctx context.Context, addr protocol.Address, fresh bool) (RemoteKeys, error) {
	id := addr.Stored()
	if !fresh {
		k.mu.Lock()
		c, ok := k.cache[id]
		k.mu.Unlock()
		if ok && time.Since(c.at) < keyCacheTTL {
			return c.keys, nil
		}
	}
	v, err, _ := k.group.Do(id, func() (any, error) {
		// shared by every waiter: one caller hanging up must not fail the rest
		keys, err := k.fetch(context.WithoutCancel(ctx), addr)
		if err == nil {
			k.mu.Lock()
			k.sweepLocked()
			k.cache[id] = cached{keys: keys, at: time.Now()}
			k.mu.Unlock()
		}
		return keys, err
	})
	if err != nil {
		return RemoteKeys{}, err
	}
	return v.(RemoteKeys), nil
}

func (k *Keys) fetch(ctx context.Context, addr protocol.Address) (RemoteKeys, error) {
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	u := k.c.URL(addr.Domain(), "/api/pubkey/"+url.PathEscape(addr.Local()))
	status, body, err := k.c.get(ctx, u, 64<<10)
	if err != nil {
		return RemoteKeys{}, ErrUnreachable
	}
	if status != http.StatusOK {
		return RemoteKeys{}, ErrRemoteNotFound
	}
	var keys RemoteKeys
	if err := json.Unmarshal(body, &keys); err != nil {
		return RemoteKeys{}, ErrUnreachable
	}
	return keys, nil
}

// sweepLocked drops expired entries once the cache gets large.
func (k *Keys) sweepLocked() {
	if len(k.cache) < 4096 {
		return
	}
	for id, c := range k.cache {
		if time.Since(c.at) >= keyCacheTTL {
			delete(k.cache, id)
		}
	}
}
