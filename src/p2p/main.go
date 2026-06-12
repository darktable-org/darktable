// darktable P2P sync daemon.
//
// Launched by darktable on startup when a sync passphrase is configured.
// Communicates with darktable via a Unix domain socket (JSON-lines protocol).
// Provides:
//   - XMP sync: push/pull edits across peers on the same passphrase
//   - Proxy media fetch: serve and retrieve *.proxy.avif sidecars
//   - mDNS peer discovery on the local network (best-effort)
//   - Static peer configuration via --peers for networks where mDNS is blocked
//
// Build:
//   cd src/p2p && go build -o dt-p2p-daemon .
//
// Run:
//   dt-p2p-daemon --socket /run/user/$UID/darktable-p2p.sock \
//                 --passphrase "my secret" \
//                 --proxy-dir /home/user/Pictures \
//                 --peers http://192.168.1.108:17842,http://192.168.1.50:17842

package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"
)

func main() {
	socketPath := flag.String("socket", defaultSocketPath(), "Unix socket path")
	passphrase := flag.String("passphrase", "", "Shared passphrase (derives Ed25519 identity)")
	proxyDir   := flag.String("proxy-dir", "", "Directory to walk for proxy files to announce and serve")
	peersFlag  := flag.String("peers", "", "Comma-separated HTTP base URLs of static peers, e.g. http://192.168.1.108:17842")
	flag.Parse()

	if *passphrase == "" {
		log.Fatal("--passphrase is required")
	}

	var staticPeers []string
	if *peersFlag != "" {
		for _, p := range strings.Split(*peersFlag, ",") {
			p = strings.TrimSpace(p)
			if p != "" {
				staticPeers = append(staticPeers, p)
			}
		}
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	d, err := newDaemon(ctx, *socketPath, *passphrase, *proxyDir, staticPeers)
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
