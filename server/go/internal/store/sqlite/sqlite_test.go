package sqlite

import (
	"context"
	"path/filepath"
	"sync"
	"testing"

	"github.com/salmonization/shyake/server/go/internal/store"
	"github.com/salmonization/shyake/server/go/internal/store/storetest"
)

func open(t *testing.T) store.Store {
	db, err := Open(context.Background(), filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db
}

func TestConformance(t *testing.T) { storetest.Run(t, open) }

// Reopening must not re-run applied migrations.
func TestMigrateIdempotent(t *testing.T) {
	path := filepath.Join(t.TempDir(), "m.db")
	for i := 0; i < 2; i++ {
		db, err := Open(context.Background(), path)
		if err != nil {
			t.Fatalf("open #%d: %v", i+1, err)
		}
		db.Close()
	}
}

// Concurrent writers must queue on the single writer connection, not
// fail with SQLITE_BUSY.
func TestConcurrentWrites(t *testing.T) {
	db := open(t)
	ctx := context.Background()
	var wg sync.WaitGroup
	errs := make(chan error, 200)
	for i := 0; i < 200; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			_, _, err := db.InsertMail(ctx, store.Mail{Sender: "a", Recipient: "b",
				Signature: string(rune('A'+i%26)) + string(rune(i)), Timestamp: int64(i)}, nil)
			errs <- err
		}(i)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}
	box, _ := db.ListMail(ctx, "b", false)
	if len(box) != 200 {
		t.Errorf("stored %d of 200", len(box))
	}
}
