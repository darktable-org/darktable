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
//                 --import-dir /home/user/Pictures/Remote \
//                 --peers http://192.168.1.108:17842,http://192.168.1.50:17842

package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net/url"
	"os"
	"os/signal"
	"strings"
	"syscall"
)

func main() {
	socketPath  := flag.String("socket", defaultSocketPath(), "Unix socket path")
	passphrase  := flag.String("passphrase", "", "Shared passphrase (derives Ed25519 identity)")
	proxyDir    := flag.String("proxy-dir", "", "Directory to walk for proxy files to announce and serve")
	importDir   := flag.String("import-dir", "", "Directory to place remote images that don't exist locally")
	peersFlag   := flag.String("peers", "", "Comma-separated HTTP base URLs of static peers, e.g. http://192.168.1.108:17842")
	localIPFlag := flag.String("local-ip", "", "Override auto-detected LAN IP (useful on Android where netlink is blocked)")
	flag.Parse()

	if *passphrase == "" {
		log.Fatal("--passphrase is required")
	}

	seen := make(map[string]bool)
	var staticPeers []string

	addPeer := func(raw string) {
		if p := normalizePeerURL(strings.TrimSpace(raw)); p != "" && !seen[p] {
			seen[p] = true
			staticPeers = append(staticPeers, p)
		}
	}

	// Peers from --peers flag.
	for _, p := range strings.Split(*peersFlag, ",") {
		addPeer(p)
	}

	// Peers from ~/.config/darktable/peers.txt (one entry per line).
	if data, err := os.ReadFile(peersFilePath()); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			line = strings.TrimSpace(line)
			if line != "" && !strings.HasPrefix(line, "#") {
				addPeer(line)
			}
		}
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	d, err := newDaemon(ctx, *socketPath, *passphrase, *proxyDir, *importDir, staticPeers, *localIPFlag)
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

func peersFilePath() string {
	if dir := os.Getenv("XDG_CONFIG_HOME"); dir != "" {
		return dir + "/darktable/peers.txt"
	}
	if home, err := os.UserHomeDir(); err == nil {
		return home + "/.config/darktable/peers.txt"
	}
	return ""
}

func defaultSocketPath() string {
	uid := os.Getenv("XDG_RUNTIME_DIR")
	if uid != "" {
		return uid + "/darktable-p2p.sock"
	}
	return "/tmp/darktable-p2p.sock"
}

// normalizePeerURL accepts bare IPs, IP:port, or full http(s):// URLs and
// returns a canonical https://host:port string.
//
//	192.168.1.108          → https://192.168.1.108:17842
//	192.168.1.108:8080     → https://192.168.1.108:8080
//	http://192.168.1.108   → https://192.168.1.108:17842
func normalizePeerURL(s string) string {
	if s == "" {
		return ""
	}
	if !strings.Contains(s, "://") {
		s = "https://" + s
	}
	u, err := url.Parse(s)
	if err != nil || u.Host == "" {
		return ""
	}
	u.Scheme = "https"
	if u.Port() == "" {
		u.Host = fmt.Sprintf("%s:%d", u.Hostname(), proxyHTTPPort)
	}
	// Drop any path/query — we only want the base URL.
	u.Path = ""
	u.RawQuery = ""
	u.Fragment = ""
	return u.String()
}
