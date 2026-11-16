// Package ttlset remembers keys for a while, in bounded memory.
//
// Keys go into the current generation. Each ttl, the current generation
// becomes the previous one and the previous one is dropped, so a key is
// remembered for at least ttl and at most 2*ttl. A generation that fills
// up rotates early: under a flood the set forgets sooner rather than
// grow without bound.
package ttlset

import (
	"sync"
	"time"
)

type Set struct {
	mu        sync.Mutex
	ttl       time.Duration
	max       int
	cur, prev map[[32]byte]struct{}
	rotatedAt time.Time
}

func New(ttl time.Duration, maxPerGeneration int) *Set {
	return &Set{
		ttl:  ttl,
		max:  maxPerGeneration,
		cur:  make(map[[32]byte]struct{}),
		prev: make(map[[32]byte]struct{}),
	}
}

// Add records k and reports whether it was new.
func (s *Set) Add(k [32]byte, now time.Time) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.rotatedAt.IsZero() {
		s.rotatedAt = now
	}
	switch elapsed := now.Sub(s.rotatedAt); {
	case elapsed >= 2*s.ttl:
		s.prev, s.cur = make(map[[32]byte]struct{}), make(map[[32]byte]struct{})
		s.rotatedAt = now
	case elapsed >= s.ttl || len(s.cur) >= s.max:
		s.prev, s.cur = s.cur, make(map[[32]byte]struct{}, len(s.cur))
		s.rotatedAt = now
	}
	if _, ok := s.cur[k]; ok {
		return false
	}
	if _, ok := s.prev[k]; ok {
		return false
	}
	s.cur[k] = struct{}{}
	return true
}
