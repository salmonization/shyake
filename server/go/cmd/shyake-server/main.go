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

// version is set at build time: -ldflags "-X main.version=v0.3.0".
var version = "dev"

func main() {
	showVersion := flag.Bool("version", false, "print the version and exit")
	flag.Parse()
	if *showVersion {
		fmt.Println("shyake-server", version)
		return
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

	client := federation.NewClient(cfg.FederationInsecure, "shyake-server/"+version)
	outbox := federation.NewOutbox(db, client, log)
	outbox.Start(ctx)
	defer outbox.Stop()

	srv := &http.Server{
		Addr:              cfg.Listen,
		Handler:           api.New(cfg, db, federation.NewKeys(client), outbox, log).Handler(),
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
