package ttlset

import (
	"testing"
	"time"
)

func key(b byte) [32]byte { return [32]byte{b} }

func TestRemembersForTTL(t *testing.T) {
	s := New(time.Minute, 100)
	t0 := time.Unix(1000, 0)
	if !s.Add(key(1), t0) {
		t.Fatal("first Add not new")
	}
	if s.Add(key(1), t0.Add(30*time.Second)) {
		t.Error("forgot within ttl")
	}
	if s.Add(key(1), t0.Add(90*time.Second)) {
		t.Error("forgot within 2*ttl after one rotation")
	}
	if !s.Add(key(1), t0.Add(5*time.Minute)) {
		t.Error("still remembered after 2*ttl")
	}
}

func TestBoundedMemory(t *testing.T) {
	s := New(time.Hour, 10)
	now := time.Unix(0, 0)
	for i := 0; i < 1000; i++ {
		s.Add(key(byte(i)), now)
	}
	if len(s.cur)+len(s.prev) > 20 {
		t.Errorf("holds %d keys, cap is 2*10", len(s.cur)+len(s.prev))
	}
}
