package main

import (
	"crypto/sha256"
	"crypto/tls"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// ---------------------------------------------------------------------------
// Outbound HTTP helpers — always include auth header and use TLS client
// ---------------------------------------------------------------------------

func (d *daemon) httpGet(url string) (*http.Response, error) {
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set(passphraseHeader, d.authToken)
	return d.tlsClient.Do(req)
}

func (d *daemon) httpPost(url, contentType string, body io.Reader) (*http.Response, error) {
	req, err := http.NewRequest("POST", url, body)
	if err != nil {
		return nil, err
	}
	req.Header.Set(passphraseHeader, d.authToken)
	req.Header.Set("Content-Type", contentType)
	return d.tlsClient.Do(req)
}

// ---------------------------------------------------------------------------
// HTTPS server
// ---------------------------------------------------------------------------

func (d *daemon) startProxyHTTP() error {
	tlsCfg := newServerTLSConfig(d.tlsCert)
	ln, err := tls.Listen("tcp", fmt.Sprintf(":%d", proxyHTTPPort), tlsCfg)
	if err != nil {
		ln, err = tls.Listen("tcp", ":0", tlsCfg)
		if err != nil {
			return fmt.Errorf("proxy https listen: %w", err)
		}
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/proxy", d.serveProxy)
	mux.HandleFunc("/manifest", d.serveManifest)
	mux.HandleFunc("/xmp", d.serveXMP)
	mux.HandleFunc("/preview", d.servePreview)
	d.httpSrv = &http.Server{Handler: requireAuth(mux, d.authToken)}
	go d.httpSrv.Serve(ln)
	log.Printf("[https] proxy server on %s", ln.Addr())
	return nil
}

// ---------------------------------------------------------------------------
// Network address helpers
// ---------------------------------------------------------------------------

// lanPriority returns a sort key for IPv4 addresses: lower is better.
// 1 = 192.168.x.x, 2 = 172.16-31.x.x, 3 = 10.x.x.x, 0 = not RFC-1918.
func lanPriority(ip net.IP) int {
	ip4 := ip.To4()
	if ip4 == nil {
		return 0
	}
	switch {
	case ip4[0] == 192 && ip4[1] == 168:
		return 1
	case ip4[0] == 172 && ip4[1] >= 16 && ip4[1] <= 31:
		return 2
	case ip4[0] == 10:
		return 3
	}
	return 0
}

func (d *daemon) localIP() string {
	if d.localIPOverride != "" {
		return d.localIPOverride
	}

	// Prefer RFC-1918 LAN addresses from OS interfaces (192.168 > 172.16 > 10.x).
	// Using net.Interfaces() instead of d.host.Addrs() avoids picking a VPN
	// address (e.g. Tailscale CGNAT 100.64+, or WireGuard on a p2p interface)
	// when a real LAN interface is also present.
	best := ""
	bestPri := 0
	ifaces, _ := net.Interfaces()
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		// Point-to-point flag indicates a tunnel (OpenVPN tun, some WireGuard).
		if iface.Flags&net.FlagPointToPoint != 0 {
			continue
		}
		addrs, _ := iface.Addrs()
		for _, addr := range addrs {
			var ip net.IP
			switch v := addr.(type) {
			case *net.IPNet:
				ip = v.IP
			case *net.IPAddr:
				ip = v.IP
			}
			if ip == nil || ip.IsLoopback() {
				continue
			}
			if pri := lanPriority(ip); pri > 0 && (best == "" || pri < bestPri) {
				best = ip.To4().String()
				bestPri = pri
			}
		}
	}
	if best != "" {
		return best
	}

	// Fallback: first non-loopback IPv4 from libp2p's address list.
	for _, ma := range d.host.Addrs() {
		s := ma.String()
		if strings.HasPrefix(s, "/ip4/127.") {
			continue
		}
		if strings.Contains(s, "/ip4/") {
			parts := strings.Split(s, "/")
			for i, p := range parts {
				if p == "ip4" && i+1 < len(parts) {
					return parts[i+1]
				}
			}
		}
	}
	return "127.0.0.1"
}

func (d *daemon) localLibP2PPort() int {
	for _, ma := range d.host.Addrs() {
		s := ma.String()
		if strings.Contains(s, "/tcp/") {
			parts := strings.Split(s, "/")
			for i, p := range parts {
				if p == "tcp" && i+1 < len(parts) {
					var port int
					fmt.Sscanf(parts[i+1], "%d", &port)
					if port > 0 {
						return port
					}
				}
			}
		}
	}
	return 0
}

func (d *daemon) localProxyURL() string {
	if d.externalURL != "" {
		return d.externalURL
	}
	return fmt.Sprintf("https://%s:%d", d.localIP(), proxyHTTPPort)
}

// allPeerURLs returns base URLs of peers that have recently responded to a
// manifest sync (the periodic "hello").  Only entries in d.syncedPeers are
// returned — peers that failed their last sync, or that have been discovered
// via gossipsub but not yet HTTP-verified, are excluded.  This keeps preview
// and proxy fetch attempts from going to stale or unreachable addresses.
func (d *daemon) allPeerURLs() []string {
	d.syncedPeersMu.Lock()
	defer d.syncedPeersMu.Unlock()
	urls := make([]string, 0, len(d.syncedPeers))
	for u, ok := range d.syncedPeers {
		if ok {
			urls = append(urls, u)
		}
	}
	return urls
}

// localDestination returns the local path to use for a remote canonical path.
// If the remote directory exists here, keep the same path.
// Otherwise fall back to importDir, then proxyDir, then the original path.
func (d *daemon) localDestination(remotePath string) string {
	remoteDir := filepath.Dir(remotePath)
	if _, err := os.Stat(remoteDir); err == nil {
		return remotePath // same directory structure exists locally
	}
	if d.importDir != "" {
		return filepath.Join(d.importDir, filepath.Base(remotePath))
	}
	if d.proxyDir != "" {
		return filepath.Join(d.proxyDir, filepath.Base(remotePath))
	}
	return remotePath // last resort: will attempt to create dirs
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

// isAnnouncedPath reports whether rawPath is a darktable-managed file that
// may be served to peers.  Two sources are accepted:
//
//  1. d.announcedProxies — explicitly announced by darktable via the socket.
//  2. A .proxy.avif sidecar on disk — a proxy AVIF is only ever created by
//     darktable's export process, so its presence is proof of ownership.
//     This fallback handles the window after a daemon restart before darktable
//     has finished re-announcing all paths via the socket.
func (d *daemon) isAnnouncedPath(rawPath string) bool {
	d.announcedProxiesMu.RLock()
	_, ok := d.announcedProxies[rawPath]
	d.announcedProxiesMu.RUnlock()
	if ok {
		return true
	}
	_, err := os.Stat(rawPath + ".proxy.avif")
	return err == nil
}

// rebuildManifestLocked rebuilds d.manifestPaths and d.manifestHash.
// Must be called with d.manifestMu held.
func (d *daemon) rebuildManifestLocked() {
	d.announcedProxiesMu.RLock()
	paths := make([]string, 0, len(d.announcedProxies))
	for raw := range d.announcedProxies {
		if _, err := os.Stat(raw + ".proxy.avif"); err == nil {
			paths = append(paths, raw)
		}
	}
	d.announcedProxiesMu.RUnlock()
	sort.Strings(paths)
	h := sha256.Sum256([]byte(strings.Join(paths, "\n")))
	d.manifestPaths = paths
	d.manifestHash = hex.EncodeToString(h[:8]) // 16-char hex, sufficient for change detection
}

func (d *daemon) serveManifest(w http.ResponseWriter, r *http.Request) {
	// If the caller passes ?from=URL, record them and trigger a reverse connection.
	if fromURL := r.URL.Query().Get("from"); fromURL != "" {
		d.pdb.touch(fromURL, "") // they reached us — they're alive
		d.syncedPeersMu.Lock()
		alreadySeen := d.syncedPeers[fromURL]
		d.syncedPeersMu.Unlock()
		if !alreadySeen {
			go d.syncWithPeer(fromURL)
		}
	}

	// Use cached manifest path list (rebuilt on first call and whenever
	// announcedProxies changes); avoids O(N) os.Stat per request.
	d.manifestMu.Lock()
	if d.manifestHash == "" {
		d.rebuildManifestLocked()
	}
	paths := d.manifestPaths
	hash := d.manifestHash
	d.manifestMu.Unlock()

	// Collect all HTTP base URLs we know about so the caller can gossip.
	d.peersMu.RLock()
	knownPeers := make([]string, 0, len(d.peers))
	for _, u := range d.peers {
		knownPeers = append(knownPeers, u)
	}
	d.peersMu.RUnlock()

	resp := manifestResp{
		PeerID:      d.host.ID().String(),
		LibP2PIP:    d.localIP(),
		LibP2PPort:  d.localLibP2PPort(),
		BaseURL:     d.localProxyURL(),
		ExternalURL: d.externalURL,
		Paths:       paths,
		Hash:        hash,
		KnownPeers:  knownPeers,
	}
	if resp.Paths == nil {
		resp.Paths = []string{}
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}
