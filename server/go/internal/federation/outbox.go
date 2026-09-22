package federation

import (
	"context"
	"fmt"
	"log/slog"
	"net/http"
	"sync"
	"time"

	"github.com/salmonization/shyake/server/go/internal/store"
)

// A relayed payload carries the sender's signed timestamp, and the
// receiving instance rejects it 300 s after that (SPEC §3.4). Retries
// past that point are pointless, so each relay has a deadline. The
// margin absorbs clock skew between the sender and the remote.
const (
	signatureWindow = 300 * time.Second
	deadlineMargin  = 20 * time.Second
)

var backoff = []time.Duration{5 * time.Second, 15 * time.Second, 30 * time.Second, 60 * time.Second}

// Outbox delivers queued relays to remote instances.
//
// The mail and its relay row are written in one transaction, so a crash
// between storing and forwarding no longer loses the delivery: the relay
// resumes on restart, within its deadline. Transient failures (network
// errors, 408, 429, 5xx) are retried with backoff. A remote refusal
// (any other 4xx) is final.
type Outbox struct {
	db     store.Store
	c      *Client
	log    *slog.Logger
	kick   chan struct{}
	now    func() time.Time
	wg     sync.WaitGroup
	cancel context.CancelFunc
}

func NewOutbox(db store.Store, c *Client, log *slog.Logger) *Outbox {
	return &Outbox{db: db, c: c, log: log, kick: make(chan struct{}, 1), now: time.Now}
}

// Start runs the delivery loop until Stop.
func (o *Outbox) Start(ctx context.Context) {
	ctx, o.cancel = context.WithCancel(ctx)
	o.wg.Add(1)
	go func() {
		defer o.wg.Done()
		tick := time.NewTicker(time.Second)
		defer tick.Stop()
		for {
			o.drain(ctx)
			select {
			case <-ctx.Done():
				return
			case <-o.kick:
			case <-tick.C:
			}
		}
	}()
}

// Stop waits for deliveries in flight to finish.
func (o *Outbox) Stop() {
	if o.cancel != nil {
		o.cancel()
	}
	o.wg.Wait()
}

// Kick asks for an immediate pass, so a new relay goes out at once.
func (o *Outbox) Kick() {
	select {
	case o.kick <- struct{}{}:
	default:
	}
}

// Deadline is the last moment a relay signed at signedAt is worth trying.
func Deadline(signedAt int64) time.Time {
	return time.Unix(signedAt, 0).Add(signatureWindow - deadlineMargin)
}

func (o *Outbox) drain(ctx context.Context) {
	// bounded, so a queue update that keeps failing cannot spin
	for pass := 0; pass < 8 && ctx.Err() == nil; pass++ {
		due, err := o.db.DueRelays(ctx, o.now().Unix(), 32)
		if err != nil {
			o.log.Error("relay: list due", "err", err)
			return
		}
		if len(due) == 0 {
			return
		}
		var wg sync.WaitGroup
		for _, r := range due {
			wg.Add(1)
			go func(r store.Relay) {
				defer wg.Done()
				// a delivery already under way finishes even during Stop
				o.deliver(context.WithoutCancel(ctx), r)
			}(r)
		}
		wg.Wait()
	}
}

func (o *Outbox) deliver(ctx context.Context, r store.Relay) {
	status, err := o.post(ctx, r)
	now := o.now()
	switch {
	case err == nil && status >= 200 && status < 300:
		o.settle(ctx, o.db.RelayDone(ctx, r.ID))
		o.log.Info("relay delivered", "mail_id", r.MailID, "domain", r.Domain, "attempts", r.Attempts+1)
		return
	case err == nil && status >= 400 && status < 500 &&
		status != http.StatusRequestTimeout && status != http.StatusTooManyRequests:
		reason := fmt.Sprintf("refused by remote: HTTP %d", status)
		o.settle(ctx, o.db.RelayDead(ctx, r.ID, reason))
		o.log.Warn("relay refused", "mail_id", r.MailID, "domain", r.Domain, "status", status)
		return
	}

	reason := fmt.Sprintf("HTTP %d", status)
	if err != nil {
		reason = err.Error()
	}
	step := backoff[min(r.Attempts, len(backoff)-1)]
	next := now.Add(step)
	if next.After(Deadline(r.SignedAt)) {
		o.settle(ctx, o.db.RelayDead(ctx, r.ID, "signature window expired; last error: "+reason))
		o.log.Warn("relay gave up", "mail_id", r.MailID, "domain", r.Domain,
			"attempts", r.Attempts+1, "err", reason)
		return
	}
	o.settle(ctx, o.db.RelayRetry(ctx, r.ID, next.Unix(), reason))
	o.log.Info("relay retry scheduled", "mail_id", r.MailID, "domain", r.Domain,
		"in", step, "err", reason)
}

func (o *Outbox) post(ctx context.Context, r store.Relay) (int, error) {
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	status, _, err := o.c.postJSON(ctx, o.c.URL(r.Domain, "/api/mail"), r.Payload)
	return status, err
}

func (o *Outbox) settle(ctx context.Context, err error) {
	if err != nil && ctx.Err() == nil {
		o.log.Error("relay: update queue", "err", err)
	}
}
