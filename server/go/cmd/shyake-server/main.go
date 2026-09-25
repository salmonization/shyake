// shyake-server is the self-hosted Shyake server. It speaks the same
// HTTP API as the Cloudflare Worker in server/cf and stores its data in
// one SQLite file. Configuration comes from SHYAKE_* environment
// variables; see docs/DEPLOY.md.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/salmonization/shyake/server/go/internal/api"
	"github.com/salmonization/shyake/server/go/internal/config"
	"github.com/salmonization/shyake/server/go/internal/federation"
	"github.com/salmonization/shyake/server/go/internal/store/sqlite"
)

// version is set at build time from server/VERSION:
// -ldflags "-X main.version=v0.3.0".
var version = "dev"

func main() {
	showVersion := flag.Bool("version", false, "print the version and exit")
	importD1 := flag.String("import-d1", "", "copy the Worker database in `file` (the D1 "+
		"SQLite file, or the SQL of wrangler d1 export) into SHYAKE_DATABASE, then exit")
	flag.Parse()
	if *showVersion {
		fmt.Println("shyake-server", version)
		return
	}
	run := run
	if *importD1 != "" {
		run = func() error { return runImport(*importD1) }
	}
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "shyake-server:", err)
		os.Exit(1)
	}
}

func run() error {
	cfg, err := config.Load(nil)
	if err != nil {
		return err
	}

	var handler slog.Handler = slog.NewTextHandler(os.Stderr, nil)
	if cfg.LogFormat == "json" {
		handler = slog.NewJSONHandler(os.Stderr, nil)
	}
	log := slog.New(handler)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	db, err := sqlite.Open(ctx, cfg.Database)
	if err != nil {
		return err
	}
	defer db.Close()

	cfg.Version = version
	client := federation.NewClient(cfg.FederationInsecure, "shyake-server/"+version)

	srv := &http.Server{
		Addr:              cfg.Listen,
		Handler:           api.New(cfg, db, federation.NewKeys(client), client, log).Handler(),
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       30 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       120 * time.Second,
		MaxHeaderBytes:    64 << 10,
	}
	ln, err := net.Listen("tcp", cfg.Listen)
	if err != nil {
		return err
	}
	if cfg.FederationInsecure {
		log.Warn("SHYAKE_FEDERATION_INSECURE is set: plain HTTP and private addresses " +
			"are allowed for federation. Use this only for local testing.")
	}
	log.Info("listening", "addr", ln.Addr().String(), "domain", cfg.InstanceDomain,
		"version", version, "database", cfg.Database)

	errc := make(chan error, 1)
	go func() { errc <- srv.Serve(ln) }()

	select {
	case err := <-errc:
		return err
	case <-ctx.Done():
	}
	log.Info("shutting down")
	shutdown, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := srv.Shutdown(shutdown); err != nil && !errors.Is(err, http.ErrServerClosed) {
		return err
	}
	return nil
}

// runImport moves a Worker instance's data into a new database. The
// instance domain must be the one the Worker used, since stored
// addresses are relative to it.
func runImport(src string) error {
	cfg, err := config.Load(nil)
	if err != nil {
		return err
	}
	ctx := context.Background()
	db, err := sqlite.Open(ctx, cfg.Database)
	if err != nil {
		return err
	}
	defer db.Close()
	rep, err := sqlite.ImportD1(ctx, db, src, cfg.InstanceDomain)
	if err != nil {
		return err
	}
	fmt.Printf("imported %d users, %d mails, %d blocks into %s\n",
		rep.Users, rep.Mail, rep.Blocks, cfg.Database)
	if rep.DuplicateMail > 0 {
		fmt.Printf("dropped %d duplicate mails (same signature stored twice)\n", rep.DuplicateMail)
	}
	for _, u := range rep.SkippedUsers {
		fmt.Printf("skipped user %q: differs from an earlier name only by case\n", u)
	}
	return nil
}
