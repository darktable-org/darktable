// darktable P2P sync daemon.
//
// Launched by darktable on startup when a sync passphrase is configured.
// Communicates with darktable via a Unix domain socket (JSON-lines protocol).
// Provides:
//   - XMP sync: push/pull edits across peers on the same passphrase
//   - Proxy media fetch: serve and retrieve *.proxy.avif sidecars
//   - mDNS peer discovery on the local network
//
// Build:
//   cd src/p2p && go build -o dt-p2p-daemon .
//
// Run:
//   dt-p2p-daemon -socket /run/user/$UID/darktable-p2p.sock -passphrase "my secret"

package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"
)

func main() {
	socketPath := flag.String("socket", defaultSocketPath(), "Unix socket path")
	passphrase := flag.String("passphrase", "", "Shared passphrase (derives Ed25519 identity)")
	proxyDir   := flag.String("proxy-dir", "", "Directory to serve proxy files from (defaults to each film folder)")
	flag.Parse()

	if *passphrase == "" {
		log.Fatal("--passphrase is required")
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	d, err := newDaemon(ctx, *socketPath, *passphrase, *proxyDir)
	if err != nil {
		log.Fatalf("daemon init: %v", err)
	}

	go func() {
		if err := d.run(); err != nil && err != context.Canceled {
			log.Printf("daemon error: %v", err)
			cancel()
		}
	}()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGTERM, syscall.SIGINT)
	select {
	case <-sig:
	case <-ctx.Done():
	}
	log.Println("shutting down")
	d.close()
}

func defaultSocketPath() string {
	uid := os.Getenv("XDG_RUNTIME_DIR")
	if uid != "" {
		return uid + "/darktable-p2p.sock"
	}
	return "/tmp/darktable-p2p.sock"
}
