package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"math/big"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/peer"
	multiaddr "github.com/multiformats/go-multiaddr"

	"github.com/huin/goupnp/dcps/internetgateway1"
	"github.com/huin/goupnp/dcps/internetgateway2"
	natpmp "github.com/jackpal/go-nat-pmp"
)

// ---------------------------------------------------------------------------
// TLS helpers
// ---------------------------------------------------------------------------

// tlsCertFromKey derives a self-signed TLS certificate from the p2p identity
// key (itself derived from the shared passphrase).  All peers with the same
// passphrase share the same key and therefore the same certificate fingerprint,
// so the fingerprint doubles as a shared secret for peer identification.
func tlsCertFromKey(priv crypto.PrivKey) (tls.Certificate, error) {
	raw, err := priv.Raw()
	if err != nil {
		return tls.Certificate{}, fmt.Errorf("key raw: %w", err)
	}
	var edPriv ed25519.PrivateKey
	switch len(raw) {
	case ed25519.PrivateKeySize: // 64 bytes — full key
		edPriv = ed25519.PrivateKey(raw)
	case ed25519.SeedSize: // 32 bytes — seed only
		edPriv = ed25519.NewKeyFromSeed(raw)
	default:
		return tls.Certificate{}, fmt.Errorf("unexpected Ed25519 key length %d", len(raw))
	}

	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	if err != nil {
		return tls.Certificate{}, err
	}
	tmpl := &x509.Certificate{
		SerialNumber: serial,
		Subject:      pkix.Name{CommonName: "darktable-p2p"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(100 * 365 * 24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth, x509.ExtKeyUsageClientAuth},
	}
	certDER, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, edPriv.Public(), edPriv)
	if err != nil {
		return tls.Certificate{}, fmt.Errorf("create cert: %w", err)
	}
	return tls.Certificate{
		Certificate: [][]byte{certDER},
		PrivateKey:  edPriv,
	}, nil
}

// pubkeyFingerprint returns the lowercase hex SHA256 of the certificate's
// SubjectPublicKeyInfo (SPKI) DER bytes — stable across certificate re-issuance.
func pubkeyFingerprint(cert *x509.Certificate) string {
	spki, err := x509.MarshalPKIXPublicKey(cert.PublicKey)
	if err != nil {
		// Fallback: hash the raw SPKI field embedded in the cert.
		h := sha256.Sum256(cert.RawSubjectPublicKeyInfo)
		return hex.EncodeToString(h[:])
	}
	h := sha256.Sum256(spki)
	return hex.EncodeToString(h[:])
}

// passphraseToken derives a fixed-length authentication token from the
// passphrase.  Sent as an HTTP header; TLS encryption protects it in transit.
func passphraseToken(passphrase string) string {
	h := sha256.Sum256([]byte("darktable-p2p-auth\x00" + passphrase))
	return hex.EncodeToString(h[:])
}

// peerKeysPath returns the path of the allowed-fingerprints file.
func peerKeysPath() string {
	if dir := os.Getenv("XDG_CONFIG_HOME"); dir != "" {
		return dir + "/darktable/peer.keys"
	}
	if home, err := os.UserHomeDir(); err == nil {
		return home + "/.config/darktable/peer.keys"
	}
	return ""
}

// loadAllowedKeyHashes returns the set of accepted server TLS public key
// fingerprints.  ownFP is always included so nodes on the same passphrase
// accept each other by default.  Additional fingerprints may be listed in
// ~/.config/darktable/peer.keys, one per line (plain hex or "sha256:<hex>").
func loadAllowedKeyHashes(ownFP string) map[string]bool {
	allowed := map[string]bool{ownFP: true}
	data, err := os.ReadFile(peerKeysPath())
	if err != nil {
		return allowed
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.ToLower(strings.TrimSpace(line))
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		line = strings.TrimPrefix(line, "sha256:")
		if len(line) == 64 { // SHA256 = 32 bytes = 64 hex chars
			allowed[line] = true
		}
	}
	return allowed
}

// newServerTLSConfig returns a TLS config for the HTTP server.
// Client certificates are not required; authentication is via the
// passphraseHeader checked at the HTTP handler layer.
func newServerTLSConfig(cert tls.Certificate) *tls.Config {
	return &tls.Config{
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS13,
		ClientAuth:   tls.NoClientCert,
	}
}

// newClientTLSConfig returns a TLS config for outbound peer connections.
// Chain validation is skipped; instead the server's public key fingerprint is
// checked against the daemon's live allowedKeyFPs map under its mutex, so newly
// accepted peers are trusted immediately without restarting the daemon.
func (d *daemon) newClientTLS() *tls.Config {
	return &tls.Config{
		Certificates:       []tls.Certificate{d.tlsCert},
		InsecureSkipVerify: true, //nolint:gosec — deliberate; fingerprint-pinned below
		VerifyPeerCertificate: func(rawCerts [][]byte, _ [][]*x509.Certificate) error {
			for _, raw := range rawCerts {
				c, err := x509.ParseCertificate(raw)
				if err != nil {
					continue
				}
				fp := pubkeyFingerprint(c)
				d.allowedKeyFPsMu.RLock()
				ok := d.allowedKeyFPs[fp]
				d.allowedKeyFPsMu.RUnlock()
				if ok {
					return nil
				}
			}
			return fmt.Errorf("server TLS public key not in allowed list")
		},
	}
}

// probePeerFingerprint dials the peer's HTTPS endpoint without certificate
// verification and returns the server's public key fingerprint.  Used to
// identify untrusted peers so the user can choose to accept them.
func probePeerFingerprint(rawURL string) string {
	u, err := url.Parse(rawURL)
	if err != nil || u.Scheme != "https" {
		return ""
	}
	host := u.Host
	if _, _, err := net.SplitHostPort(host); err != nil {
		host = fmt.Sprintf("%s:%d", host, proxyHTTPPort)
	}
	conn, err := tls.DialWithDialer(
		&net.Dialer{Timeout: 3 * time.Second}, "tcp", host,
		&tls.Config{InsecureSkipVerify: true}, //nolint:gosec — fingerprint probe only
	)
	if err != nil {
		return ""
	}
	defer conn.Close()
	certs := conn.ConnectionState().PeerCertificates
	if len(certs) == 0 {
		return ""
	}
	return pubkeyFingerprint(certs[0])
}

// recordCandidatePeer stores a discovered-but-untrusted peer and persists the
// list to ~/.config/darktable/peer.candidates so the preferences UI can read it.
func (d *daemon) recordCandidatePeer(peerURL, fp string) {
	d.allowedKeyFPsMu.RLock()
	already := d.allowedKeyFPs[fp]
	d.allowedKeyFPsMu.RUnlock()
	if already {
		return
	}

	d.candidatesMu.Lock()
	if d.candidatePeers == nil {
		d.candidatePeers = make(map[string]candidatePeer)
	}
	if _, exists := d.candidatePeers[fp]; !exists {
		d.candidatePeers[fp] = candidatePeer{URL: peerURL, Fingerprint: fp, SeenAt: time.Now()}
		log.Printf("[peer] candidate (untrusted) peer %s  fp=%s", peerURL, fp)
	}
	snapshot := make([]candidatePeer, 0, len(d.candidatePeers))
	for _, c := range d.candidatePeers {
		snapshot = append(snapshot, c)
	}
	d.candidatesMu.Unlock()

	if cfgDir, err := os.UserConfigDir(); err == nil {
		data, _ := json.Marshal(snapshot)
		_ = os.WriteFile(filepath.Join(cfgDir, "darktable", "peer.candidates"), data, 0644)
	}
}

// requireAuth is HTTP middleware that rejects requests without the correct
// passphrase token in passphraseHeader.
func requireAuth(next http.Handler, token string) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get(passphraseHeader) != token {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		next.ServeHTTP(w, r)
	})
}

// ---------------------------------------------------------------------------
// External access via UPnP / NAT-PMP
// ---------------------------------------------------------------------------

// upnpPortMapper is the subset of goupnp WANIPConnection we need.
type upnpPortMapper interface {
	GetExternalIPAddress() (string, error)
	AddPortMapping(string, uint16, string, uint16, string, bool, string, uint32) error
}

// initExternalAccess tries UPnP then NAT-PMP to discover the public IP and
// add a port mapping for proxyHTTPPort.  Sets d.externalURL on success.
func (d *daemon) initExternalAccess() {
	time.Sleep(2 * time.Second) // wait for network to settle

	var mapper upnpPortMapper

	if clients, _, err := internetgateway2.NewWANIPConnection2Clients(); err == nil && len(clients) > 0 {
		mapper = clients[0]
		log.Printf("[upnp] found WANIPConnection2 gateway")
	} else if clients, _, err := internetgateway2.NewWANIPConnection1Clients(); err == nil && len(clients) > 0 {
		mapper = clients[0]
		log.Printf("[upnp] found WANIPConnection1 (ig2) gateway")
	} else if clients, _, err := internetgateway1.NewWANIPConnection1Clients(); err == nil && len(clients) > 0 {
		mapper = clients[0]
		log.Printf("[upnp] found WANIPConnection1 (ig1) gateway")
	}

	if mapper != nil {
		extIP, err := mapper.GetExternalIPAddress()
		if err == nil && extIP != "" && extIP != "0.0.0.0" {
			_ = mapper.AddPortMapping("", uint16(proxyHTTPPort), "TCP",
				uint16(proxyHTTPPort), d.localIP(), true, "darktable-p2p", 3600)
			d.externalURL = fmt.Sprintf("https://%s:%d", extIP, proxyHTTPPort)
			log.Printf("[upnp] external URL: %s", d.externalURL)
			go d.renewUPnP(mapper)
			return
		}
	}

	// Fall back to NAT-PMP.
	gw := d.defaultGateway()
	if gw == nil {
		return
	}
	pmp := natpmp.NewClient(gw)
	if ext, err := pmp.GetExternalAddress(); err == nil {
		extIP := net.IP(ext.ExternalIPAddress[:]).String()
		if extIP != "" && extIP != "0.0.0.0" {
			_, _ = pmp.AddPortMapping("tcp", proxyHTTPPort, proxyHTTPPort, 3600)
			d.externalURL = fmt.Sprintf("https://%s:%d", extIP, proxyHTTPPort)
			log.Printf("[nat-pmp] external URL: %s", d.externalURL)
		}
	}
}

// renewUPnP refreshes the port mapping every 50 minutes (lease is 3600s).
func (d *daemon) renewUPnP(mapper upnpPortMapper) {
	tick := time.NewTicker(50 * time.Minute)
	defer tick.Stop()
	for {
		select {
		case <-d.ctx.Done():
			return
		case <-tick.C:
			_ = mapper.AddPortMapping("", uint16(proxyHTTPPort), "TCP",
				uint16(proxyHTTPPort), d.localIP(), true, "darktable-p2p", 3600)
		}
	}
}

// defaultGateway returns the default IPv4 gateway by connecting a UDP socket
// (no data is sent) and taking the interface's first-octet-1 address.
func (d *daemon) defaultGateway() net.IP {
	conn, err := net.Dial("udp4", "8.8.8.8:53")
	if err != nil {
		return nil
	}
	defer conn.Close()
	ip := conn.LocalAddr().(*net.UDPAddr).IP.To4()
	if ip == nil {
		return nil
	}
	gw := make(net.IP, 4)
	copy(gw, ip)
	gw[3] = 1
	return gw
}

// HandlePeerFound implements mdns.Notifee.
func (d *daemon) HandlePeerFound(pi peer.AddrInfo) {
	if pi.ID == d.host.ID() {
		return
	}
	log.Printf("[mdns] discovered peer %s", pi.ID)
	if err := d.host.Connect(d.ctx, pi); err != nil {
		log.Printf("[mdns] connect to %s: %v", pi.ID, err)
	}
	// HTTP URL is unknown until the peer's proxy announce arrives on gossipsub;
	// subscribeProxy will touch the DB at that point.
}

// ---------------------------------------------------------------------------
// Static peer sync
// ---------------------------------------------------------------------------

func (d *daemon) syncStaticPeers() {
	time.Sleep(2 * time.Second)
	for _, u := range d.peersToSync() {
		go d.syncWithPeer(u)
	}
	tick := time.NewTicker(60 * time.Second)
	defer tick.Stop()
	for {
		select {
		case <-d.ctx.Done():
			return
		case <-tick.C:
			for _, u := range d.peersToSync() {
				go d.syncWithPeer(u)
			}
		}
	}
}

// peersToSync returns the union of static peers and DB-persisted peers,
// deduplicated.
func (d *daemon) peersToSync() []string {
	seen := make(map[string]bool)
	var out []string
	add := func(u string) {
		if u != "" && !seen[u] {
			seen[u] = true
			out = append(out, u)
		}
	}
	for _, u := range d.staticPeers {
		add(u)
	}
	for _, u := range d.pdb.allURLs() {
		add(u)
	}
	return out
}

func (d *daemon) syncWithPeer(baseURL string) {
	baseURL = strings.TrimRight(baseURL, "/")
	if baseURL == "" {
		return
	}

	// Never connect to ourselves.
	myURL := d.localProxyURL()
	if baseURL == myURL || baseURL == fmt.Sprintf("https://%s:%d", d.localIP(), proxyHTTPPort) {
		return
	}

	// Check first-visit state but don't commit until we know the HTTP call
	// succeeds — otherwise a failed attempt marks firstVisit=false forever and
	// the libp2p connect + reverse-peer handshake never happens.
	d.syncedPeersMu.Lock()
	firstVisit := !d.syncedPeers[baseURL]
	d.syncedPeersMu.Unlock()

	manifestURL := baseURL + "/manifest"
	if firstVisit {
		manifestURL += "?from=" + url.QueryEscape(myURL)
	}

	resp, err := d.httpGet(manifestURL)
	if err != nil {
		log.Printf("[peer] manifest from %s: %v", baseURL, err)
		d.pdb.markFailure(baseURL)
		// Reset so the next successful attempt still performs the firstVisit steps.
		d.syncedPeersMu.Lock()
		delete(d.syncedPeers, baseURL)
		d.syncedPeersMu.Unlock()
		// If the failure is a fingerprint rejection, probe the peer's cert so
		// we can present it to the user for acceptance in preferences.
		if strings.Contains(err.Error(), "not in allowed list") {
			go func(u string) {
				if fp := probePeerFingerprint(u); fp != "" {
					d.recordCandidatePeer(u, fp)
				}
			}(baseURL)
		}
		return
	}
	if resp.StatusCode != 200 {
		resp.Body.Close()
		log.Printf("[peer] %s: HTTP %d (old daemon?)", baseURL, resp.StatusCode)
		d.pdb.markFailure(baseURL)
		d.syncedPeersMu.Lock()
		delete(d.syncedPeers, baseURL)
		d.syncedPeersMu.Unlock()
		return
	}

	// Commit first-visit only after confirmed success, with a CAS to handle
	// concurrent goroutines both racing to be the first successful contact.
	d.syncedPeersMu.Lock()
	firstVisit = !d.syncedPeers[baseURL]
	d.syncedPeers[baseURL] = true
	d.syncedPeersMu.Unlock()
	var m manifestResp
	if err := json.NewDecoder(resp.Body).Decode(&m); err != nil {
		resp.Body.Close()
		log.Printf("[peer] manifest decode from %s: %v", baseURL, err)
		return
	}
	resp.Body.Close()

	// Successful contact — record in DB.
	d.pdb.markSuccess(baseURL, m.PeerID)

	peerShort := m.PeerID
	if len(peerShort) > 12 {
		peerShort = peerShort[:12]
	}

	// Skip the expensive per-path broadcast + stat work when the remote's
	// manifest content hasn't changed since our last successful sync.
	// (Hash is non-empty only on daemons that support this field.)
	d.peerHashMu.Lock()
	sameHash := m.Hash != "" && m.Hash == d.peerLastHash[baseURL]
	if m.Hash != "" {
		d.peerLastHash[baseURL] = m.Hash
	}
	d.peerHashMu.Unlock()

	if sameHash && !firstVisit {
		log.Printf("[peer] %s: manifest unchanged (hash=%s), skipping per-path work (%d proxies)",
			baseURL, m.Hash, len(m.Paths))
		return
	}

	log.Printf("[peer] %s: peerID=%s  proxies=%d  indirect=%d",
		baseURL, peerShort, len(m.Paths), len(m.KnownPeers))

	// Connect via libp2p so gossipsub (XMP) works (once per peer).
	if firstVisit && m.PeerID != "" && m.LibP2PIP != "" && m.LibP2PPort > 0 {
		maStr := fmt.Sprintf("/ip4/%s/tcp/%d", m.LibP2PIP, m.LibP2PPort)
		ma, maErr := multiaddr.NewMultiaddr(maStr)
		pid, pidErr := peer.Decode(m.PeerID)
		if maErr == nil && pidErr == nil && pid != d.host.ID() {
			pinfo := peer.AddrInfo{ID: pid, Addrs: []multiaddr.Multiaddr{ma}}
			d.peersMu.Lock()
			d.peers[pid] = baseURL
			d.peersMu.Unlock()
			if err := d.host.Connect(d.ctx, pinfo); err != nil {
				log.Printf("[peer] libp2p connect to %s: %v", maStr, err)
			} else {
				log.Printf("[peer] libp2p connected to %s", peerShort)
			}
		}
	}

	// Immediately notify subscribers of every image the peer knows about so the
	// mobile gallery populates at once without waiting for AVIF downloads.
	// image_imported deduplicates on the receiver side, so re-announcing on
	// every sync is harmless.
	for _, path := range m.Paths {
		localPath := d.localDestination(path)
		result, _ := json.Marshal(map[string]string{"path": localPath})
		d.broadcast(socketMsg{Type: "image_imported", Data: result})
	}

	// Now start background downloads: thumbnail JPEG (fast) then AVIF proxy.
	// downloadSem caps concurrency so a large library doesn't spawn thousands
	// of goroutines simultaneously.
	missing := 0
	for _, path := range m.Paths {
		localPath := d.localDestination(path)
		if _, err := os.Stat(localPath + ".proxy.avif"); err == nil {
			// AVIF already present; just refresh the thumbnail if it changed.
			go func(p, lp, bu string) {
				d.downloadSem <- struct{}{}
				defer func() { <-d.downloadSem }()
				d.fetchPreviewJPEG(p, lp, bu, "thumb")
			}(path, localPath, baseURL)
			continue
		}
		missing++
		go func(p, bu string) {
			d.downloadSem <- struct{}{}
			defer func() { <-d.downloadSem }()
			d.autoFetchProxy(p, bu)
		}(path, baseURL)
	}
	if missing > 0 {
		log.Printf("[peer] fetching %d missing proxies from %s", missing, baseURL)
	} else if len(m.Paths) > 0 {
		log.Printf("[peer] all %d proxies from %s already present", len(m.Paths), baseURL)
	}

	// Gossip: connect to indirect peers the remote knows about (first visit only
	// to avoid propagation storms when the mesh is fully connected).
	if firstVisit {
		for _, peerURL := range m.KnownPeers {
			if peerURL != "" && peerURL != myURL {
				d.pdb.touch(peerURL, "") // persist before we try to reach them
				go d.syncWithPeer(peerURL)
			}
		}
		// If the remote advertises a different external URL, prefer that for future calls.
		if m.ExternalURL != "" && m.ExternalURL != baseURL {
			d.syncedPeersMu.Lock()
			d.syncedPeers[m.ExternalURL] = true // treat as same peer, avoid double-sync
			d.syncedPeersMu.Unlock()
		}
	}
}
