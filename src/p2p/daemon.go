package main

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/p2p/discovery/mdns"
	pubsub "github.com/libp2p/go-libp2p-pubsub"
	multiaddr "github.com/multiformats/go-multiaddr"
	"golang.org/x/crypto/pbkdf2"
	gocrypto "crypto"
	"net/url"

	"github.com/huin/goupnp/dcps/internetgateway1"
	"github.com/huin/goupnp/dcps/internetgateway2"
	natpmp "github.com/jackpal/go-nat-pmp"
	_ "modernc.org/sqlite"
)

const (
	xmpTopic        = "darktable/xmp/v1"
	proxyTopic      = "darktable/proxy-announce/v1"
	mdnsServiceTag  = "darktable-p2p"
	pbkdf2Iter      = 100000
	keyLen          = 32
	proxyHTTPPort   = 17842
	// passphraseHeader is required on every inbound HTTP request.
	passphraseHeader = "X-DT-Auth"
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
// Chain validation is skipped; instead the server's public key fingerprint
// must be in the allowed set.
func newClientTLSConfig(cert tls.Certificate, allowed map[string]bool) *tls.Config {
	return &tls.Config{
		Certificates:       []tls.Certificate{cert},
		InsecureSkipVerify: true, //nolint:gosec — deliberate; fingerprint-pinned below
		VerifyPeerCertificate: func(rawCerts [][]byte, _ [][]*x509.Certificate) error {
			for _, raw := range rawCerts {
				c, err := x509.ParseCertificate(raw)
				if err != nil {
					continue
				}
				if allowed[pubkeyFingerprint(c)] {
					return nil
				}
			}
			return fmt.Errorf("server TLS public key not in allowed list")
		},
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

// reDatetime matches exif:DateTimeOriginal (and similar) in XMP XML.
var reDatetime = regexp.MustCompile(
	`<(?:exif:DateTimeOriginal|exif:DateTimeDigitized|xmp:CreateDate)[^>]*>([^<]+)<`)

// eventSub is a darktable client subscribed to push events.
type eventSub struct {
	ch chan socketMsg
}

// daemon holds all P2P state.
type daemon struct {
	ctx         context.Context
	cancel      context.CancelFunc
	socketPath  string
	proxyDir    string
	importDir   string   // default folder for importing remote images
	staticPeers []string // http base URLs from --peers flag
	passphrase  string
	ownFP       string

	// displayICC is the raw bytes of the desktop monitor's ICC profile,
	// injected into thumbnail JEPGs so the mobile renders them correctly.
	// Loaded from --display-icc or auto-detected via colord at startup.
	// Falls back to a compact sRGB profile when detection fails.
	displayICC []byte

	host     host.Host
	ps       *pubsub.PubSub
	xmpSub   *pubsub.Subscription
	xmpTop   *pubsub.Topic
	proxySub *pubsub.Subscription
	proxyTop *pubsub.Topic

	// externalURL is set when UPnP/NAT-PMP port mapping succeeds.
	externalURL string
	// localIPOverride, when non-empty, replaces the netlink-detected LAN IP.
	// Used on Android where SELinux blocks raw netlink route sockets.
	localIPOverride string

	// pdb is the persistent peer registry; nil if the DB could not be opened.
	pdb *peerDB

	// known peers: peerID → http base URL
	peersMu sync.RWMutex
	peers   map[peer.ID]string

	// syncedPeers tracks HTTP base URLs we have already synced with so peer
	// gossip and reverse connections don't create infinite loops.
	syncedPeersMu sync.Mutex
	syncedPeers   map[string]bool

	// TLS identity and authentication.
	tlsCert      tls.Certificate
	authToken    string          // passphraseToken(passphrase) — sent as X-DT-Auth header
	allowedKeyFPs map[string]bool // SHA256 SPKI fingerprints of trusted peer servers
	tlsClient    *http.Client    // TLS-configured client for all outbound peer requests

	// xmp dedup: canonical_path → last mtime synced
	xmpMu          sync.Mutex
	xmpSeen        map[string]time.Time
	// xmpSuppressFrom: after applying inbound XMP, suppress echo to the sender
	// for a short window so we don't bounce the edit back to whoever sent it.
	xmpSuppressFrom map[string]xmpSuppressEntry

	// local file index: basename → []canonical-path, rebuilt on start and on import
	localIndexMu sync.RWMutex
	localIndex   map[string][]string

	// announced proxies: raw paths announced by darktable via socket
	announcedProxiesMu sync.RWMutex
	announcedProxies   map[string]struct{}

	// darktable clients subscribed to push events
	subsMu sync.Mutex
	subs   []*eventSub

	httpSrv *http.Server
	ln      net.Listener
}

// Message types on the Unix socket.
type socketMsg struct {
	Type string          `json:"type"`
	Data json.RawMessage `json:"data,omitempty"`
}

// xmpSuppressEntry records the peer URL to exclude from the next push for a path.
type xmpSuppressEntry struct {
	url   string
	until time.Time
}

// XMP sync envelope published on xmpTopic.
type xmpMsg struct {
	SenderID    string `json:"sender"`
	SenderURL   string `json:"sender_url,omitempty"` // HTTP base URL of the sender
	Path        string `json:"path"`                 // sender's canonical raw path
	Filename    string `json:"filename"`             // basename for cross-machine matching
	CaptureDate string `json:"capture_date"`         // exif:DateTimeOriginal for disambiguation
	Content     string `json:"content"`              // full XMP text
	Mtime       int64  `json:"mtime"`                // unix ns
}

// Proxy announce published on proxyTopic.
type proxyAnnounce struct {
	SenderID string   `json:"sender"`
	BaseURL  string   `json:"base_url"`
	Paths    []string `json:"paths"`
}

// manifestResp is returned by GET /manifest.
type manifestResp struct {
	PeerID      string   `json:"peer_id"`
	LibP2PIP    string   `json:"libp2p_ip"`
	LibP2PPort  int      `json:"libp2p_port"`
	BaseURL     string   `json:"base_url"`
	ExternalURL string   `json:"external_url,omitempty"` // public URL if UPnP succeeded
	Paths       []string `json:"paths"`
	KnownPeers  []string `json:"known_peers,omitempty"` // HTTP URLs of other known peers
}

// ---------------------------------------------------------------------------
// XMP helpers
// ---------------------------------------------------------------------------

// xmpCaptureDate extracts the first date-time field found in XMP XML content.
func xmpCaptureDate(content string) string {
	m := reDatetime.FindStringSubmatch(content)
	if len(m) > 1 {
		return strings.TrimSpace(m[1])
	}
	return ""
}

// xmpDateFromFile reads a .xmp sidecar and returns its capture date, or "".
func xmpDateFromFile(xmpPath string) string {
	data, err := os.ReadFile(xmpPath)
	if err != nil {
		return ""
	}
	return xmpCaptureDate(string(data))
}

// ---------------------------------------------------------------------------
// Local file index
// ---------------------------------------------------------------------------

// buildLocalIndex walks proxyDir and indexes every raw-looking file by basename.
// Call this once on startup and after every new proxy download.
func (d *daemon) buildLocalIndex() {
	if d.proxyDir == "" {
		return
	}
	idx := make(map[string][]string)
	filepath.Walk(d.proxyDir, func(p string, fi os.FileInfo, err error) error {
		if err != nil || fi.IsDir() {
			return nil
		}
		name := filepath.Base(p)
		if strings.HasSuffix(name, ".xmp") {
			return nil
		}
		if strings.HasSuffix(name, ".proxy.avif") {
			// Proxy-only entry: the raw doesn't exist locally.
			// Index the canonical raw name so resolveLocalPath can find it.
			rawName := strings.TrimSuffix(name, ".proxy.avif")
			rawPath := strings.TrimSuffix(p, ".proxy.avif")
			if _, err := os.Stat(rawPath); err != nil {
				idx[rawName] = append(idx[rawName], rawPath)
			}
			return nil
		}
		idx[name] = append(idx[name], p)
		return nil
	})
	d.localIndexMu.Lock()
	d.localIndex = idx
	d.localIndexMu.Unlock()
}

// addToLocalIndex registers a new canonical path in the index.
func (d *daemon) addToLocalIndex(canonical string) {
	name := filepath.Base(canonical)
	d.localIndexMu.Lock()
	d.localIndex[name] = append(d.localIndex[name], canonical)
	d.localIndexMu.Unlock()
}

// resolveLocalPath returns the local canonical path that best matches a remote
// path + filename + captureDate.  Returns "" if no match is found.
func (d *daemon) resolveLocalPath(remotePath, filename, captureDate string) string {
	// 1. Exact path: the raw file or a proxy placeholder exists here already.
	if _, err := os.Stat(remotePath); err == nil {
		return remotePath
	}
	if _, err := os.Stat(remotePath + ".proxy.avif"); err == nil {
		return remotePath
	}

	// 2. Filename match in local index.
	if filename == "" {
		return ""
	}
	d.localIndexMu.RLock()
	paths := d.localIndex[filename]
	d.localIndexMu.RUnlock()

	if len(paths) == 0 {
		return ""
	}
	if len(paths) == 1 {
		return paths[0]
	}
	// Multiple local files share this name — use capture date to disambiguate.
	if captureDate != "" {
		for _, p := range paths {
			if xmpDateFromFile(p+".xmp") == captureDate {
				return p
			}
		}
	}
	return paths[0] // best guess when date is unavailable
}

// ---------------------------------------------------------------------------
// Daemon construction
// ---------------------------------------------------------------------------

func deriveKey(passphrase string) (crypto.PrivKey, error) {
	salt := sha256.Sum256([]byte("darktable-p2p-v1"))
	kb := pbkdf2.Key([]byte(passphrase), salt[:], pbkdf2Iter, keyLen, gocrypto.SHA256.New)
	priv, _, err := crypto.GenerateEd25519Key(strings.NewReader(string(kb)))
	return priv, err
}

func newDaemon(ctx context.Context, socketPath, passphrase, proxyDir, importDir string, staticPeers []string, localIPOverride, displayICCPath string) (*daemon, error) {
	priv, err := deriveKey(passphrase)
	if err != nil {
		return nil, fmt.Errorf("key derivation: %w", err)
	}

	// Build TLS identity from the passphrase-derived key.
	tlsCert, err := tlsCertFromKey(priv)
	if err != nil {
		return nil, fmt.Errorf("tls cert: %w", err)
	}
	parsed, err := x509.ParseCertificate(tlsCert.Certificate[0])
	if err != nil {
		return nil, fmt.Errorf("parse own cert: %w", err)
	}
	ownFP := pubkeyFingerprint(parsed)
	allowed := loadAllowedKeyHashes(ownFP)
	log.Printf("[tls] own fingerprint (sha256): %s", ownFP)
	// Write fingerprint so darktable UI can display it in preferences.
	if cfgDir, e2 := os.UserConfigDir(); e2 == nil {
		_ = os.WriteFile(filepath.Join(cfgDir, "darktable", "peer.fingerprint"),
			[]byte(ownFP+"\n"), 0644)
	}
	log.Printf("[tls] %d key(s) in allowed list (add extras to ~/.config/darktable/peer.keys)", len(allowed))

	authTok := passphraseToken(passphrase)

	tlsClient := &http.Client{
		Timeout: 10 * time.Second,
		Transport: &http.Transport{
			TLSClientConfig: newClientTLSConfig(tlsCert, allowed),
		},
	}

	h, err := libp2p.New(
		libp2p.Identity(priv),
		libp2p.ListenAddrStrings("/ip4/0.0.0.0/tcp/0"),
	)
	if err != nil {
		return nil, fmt.Errorf("libp2p host: %w", err)
	}

	ps, err := pubsub.NewGossipSub(ctx, h)
	if err != nil {
		h.Close()
		return nil, fmt.Errorf("gossipsub: %w", err)
	}

	xmpTop, err := ps.Join(xmpTopic)
	if err != nil { h.Close(); return nil, err }
	xmpSub, err := xmpTop.Subscribe()
	if err != nil { h.Close(); return nil, err }

	proxyTop, err := ps.Join(proxyTopic)
	if err != nil { h.Close(); return nil, err }
	proxySub, err := proxyTop.Subscribe()
	if err != nil { h.Close(); return nil, err }

	dctx, cancel := context.WithCancel(ctx)

	d := &daemon{
		ctx:             dctx,
		cancel:          cancel,
		socketPath:      socketPath,
		proxyDir:        proxyDir,
		importDir:       importDir,
		staticPeers:     staticPeers,
		passphrase:      passphrase,
		ownFP:           ownFP,
		localIPOverride: localIPOverride,
		host:        h,
		ps:          ps,
		xmpSub:      xmpSub,
		xmpTop:      xmpTop,
		proxySub:    proxySub,
		proxyTop:    proxyTop,
		peers:       make(map[peer.ID]string),
		syncedPeers: make(map[string]bool),
		tlsCert:         tlsCert,
		authToken:       authTok,
		allowedKeyFPs:   allowed,
		tlsClient:       tlsClient,
		xmpSeen:         make(map[string]time.Time),
		xmpSuppressFrom: make(map[string]xmpSuppressEntry),
		localIndex:       make(map[string][]string),
		announcedProxies: make(map[string]struct{}),
		displayICC:       loadDisplayICC(displayICCPath),
	}

	go d.initExternalAccess()

	// Open peer DB and seed it with the static peers passed in.
	var dbErr error
	d.pdb, dbErr = openPeerDB()
	if dbErr != nil {
		log.Printf("[peerdb] open failed: %v", dbErr)
	}
	for _, u := range staticPeers {
		d.pdb.touch(u, "")
	}

	// mDNS (best-effort; many APs block multicast).
	svc := mdns.NewMdnsService(h, mdnsServiceTag, d)
	if err := svc.Start(); err != nil {
		log.Printf("mDNS start warning: %v", err)
	}

	d.buildLocalIndex()

	return d, nil
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
// Run loop
// ---------------------------------------------------------------------------

func (d *daemon) run() error {
	if err := d.startProxyHTTP(); err != nil {
		return err
	}

	// Write our own HTTP URL so the UI can include it in the pairing QR even
	// when reading from files rather than querying the socket.
	if cfgDir, err := os.UserConfigDir(); err == nil {
		_ = os.WriteFile(filepath.Join(cfgDir, "darktable", "peer.localurl"),
			[]byte(d.localProxyURL()+"\n"), 0644)
	}

	os.Remove(d.socketPath)
	ln, err := net.Listen("unix", d.socketPath)
	if err != nil {
		return fmt.Errorf("socket listen: %w", err)
	}
	d.ln = ln
	defer ln.Close()

	go d.subscribeXMP()
	go d.subscribeProxy()
	go d.announceProxies()

	if len(d.staticPeers) > 0 {
		go d.syncStaticPeers()
	}

	log.Printf("[init] P2P daemon ready  peerID=%s  socket=%s  proxy-dir=%s  import-dir=%s",
		d.host.ID(), d.socketPath, d.proxyDir, d.importDir)

	for {
		conn, err := ln.Accept()
		if err != nil {
			select {
			case <-d.ctx.Done():
				return nil
			default:
				return fmt.Errorf("accept: %w", err)
			}
		}
		go d.handleConn(conn)
	}
}

// ---------------------------------------------------------------------------
// Push events to darktable
// ---------------------------------------------------------------------------

func (d *daemon) broadcast(msg socketMsg) {
	d.subsMu.Lock()
	defer d.subsMu.Unlock()
	for _, s := range d.subs {
		select {
		case s.ch <- msg:
		default:
		}
	}
}

func (d *daemon) handleEventSub(enc *json.Encoder) {
	ch := make(chan socketMsg, 32)
	sub := &eventSub{ch: ch}

	d.subsMu.Lock()
	d.subs = append(d.subs, sub)
	d.subsMu.Unlock()

	defer func() {
		d.subsMu.Lock()
		for i, s := range d.subs {
			if s == sub {
				d.subs = append(d.subs[:i], d.subs[i+1:]...)
				break
			}
		}
		d.subsMu.Unlock()
	}()

	for {
		select {
		case <-d.ctx.Done():
			return
		case msg, ok := <-ch:
			if !ok {
				return
			}
			if err := enc.Encode(msg); err != nil {
				return
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Socket command handler
// ---------------------------------------------------------------------------

func (d *daemon) handleConn(conn net.Conn) {
	defer conn.Close()
	dec := json.NewDecoder(conn)
	enc := json.NewEncoder(conn)

	for {
		var msg socketMsg
		if err := dec.Decode(&msg); err != nil {
			if err != io.EOF {
				log.Printf("[socket] decode error: %v", err)
			}
			return
		}

		switch msg.Type {
		case "subscribe_events":
			d.handleEventSub(enc)
			return

		case "xmp_push":
			var x xmpMsg
			if err := json.Unmarshal(msg.Data, &x); err != nil {
				continue
			}
			x.SenderID    = d.host.ID().String()
			x.SenderURL   = d.localProxyURL()
			x.Filename    = filepath.Base(x.Path)
			x.CaptureDate = xmpCaptureDate(x.Content)

			// Dedup: darktable may send the same XMP twice (debounce + sidecar job).
			t := time.Unix(0, x.Mtime)
			d.xmpMu.Lock()
			dup := !t.After(d.xmpSeen[x.Path])
			if !dup {
				d.xmpSeen[x.Path] = t
			}
			// Check if we recently received this path from a peer; if so, exclude
			// that peer from the push to avoid bouncing the edit back to them.
			var excludeURL string
			if e, ok := d.xmpSuppressFrom[x.Path]; ok && time.Now().Before(e.until) {
				excludeURL = e.url
				delete(d.xmpSuppressFrom, x.Path)
			}
			d.xmpMu.Unlock()
			if dup {
				continue
			}

			d.peersMu.RLock()
			nPeers := len(d.peers)
			d.peersMu.RUnlock()

			log.Printf("[xmp] send: '%s'  capture=%s  peers=%d",
				x.Filename, x.CaptureDate, nPeers)

			if b, err := json.Marshal(x); err == nil {
				d.xmpTop.Publish(d.ctx, b)
			}
			go d.pushXMPToPeers(x, excludeURL)

		case "announce_proxy":
			var req struct {
				Path string `json:"path"`
			}
			if err := json.Unmarshal(msg.Data, &req); err != nil {
				continue
			}
			if req.Path != "" {
				d.announcedProxiesMu.Lock()
				d.announcedProxies[req.Path] = struct{}{}
				d.announcedProxiesMu.Unlock()
			}

		case "fetch_proxy":
			var req struct {
				Path string `json:"path"`
			}
			if err := json.Unmarshal(msg.Data, &req); err != nil {
				continue
			}
			go d.fetchProxyFromPeer(req.Path, enc)

		case "fetch_preview":
			var req struct {
				Path string `json:"path"`
				Size string `json:"size"`
			}
			if err := json.Unmarshal(msg.Data, &req); err != nil {
				continue
			}
			if req.Size == "" {
				req.Size = "thumb"
			}
			go d.fetchPreviewFromPeers(req.Path, req.Size)

		// Desktop darktable calls this after finishing an edit so paired phones
		// receive an updated preview without having to request one manually.
		case "push_preview":
			var req struct {
				Path string `json:"path"`
				Size string `json:"size"`
			}
			if err := json.Unmarshal(msg.Data, &req); err != nil {
				continue
			}
			if req.Size == "" {
				req.Size = "full"
			}
			go func(path, size string) {
				// Delete the cached preview so fetchPreviewFromPeers re-downloads it.
				cached := path + ".preview-" + size + ".jpg"
				os.Remove(cached)
				d.fetchPreviewFromPeers(path, size)
			}(req.Path, req.Size)

		case "list_peers":
			d.peersMu.RLock()
			ids := make([]string, 0, len(d.peers))
			for id := range d.peers {
				ids = append(ids, id.String())
			}
			d.peersMu.RUnlock()
			resp, _ := json.Marshal(ids)
			enc.Encode(socketMsg{Type: "peers", Data: resp})

		case "ping":
			enc.Encode(socketMsg{Type: "pong"})

		case "get_pairing_info":
			// Always include this machine's own URL first so the mobile phone
			// knows where to reach us; then add discovered and static peers.
			ownURL := d.localProxyURL()
			seen := map[string]bool{ownURL: true}
			peerURLs := []string{ownURL}
			add := func(u string) {
				if u != "" && !seen[u] {
					seen[u] = true
					peerURLs = append(peerURLs, u)
				}
			}
			d.peersMu.RLock()
			for _, u := range d.peers {
				add(u)
			}
			d.peersMu.RUnlock()
			for _, u := range d.staticPeers {
				add(u)
			}
			resp, _ := json.Marshal(struct {
				V           int      `json:"v"`
				Passphrase  string   `json:"pp"`
				Fingerprint string   `json:"fpr"`
				Peers       []string `json:"peers"`
			}{
				V:           1,
				Passphrase:  d.passphrase,
				Fingerprint: d.ownFP,
				Peers:       peerURLs,
			})
			enc.Encode(socketMsg{Type: "pairing_info", Data: resp})
		}
	}
}

// ---------------------------------------------------------------------------
// XMP subscription
// ---------------------------------------------------------------------------

// applyInboundXMP processes an inbound XMP update received from gossipsub or a
// direct HTTP push.  from is a short identifier used only for logging.
func (d *daemon) applyInboundXMP(x xmpMsg, from string) {
	d.xmpMu.Lock()
	last := d.xmpSeen[x.Path]
	t := time.Unix(0, x.Mtime)
	shouldProcess := t.After(last)
	if !shouldProcess {
		if _, statErr := os.Stat(x.Path + ".xmp"); os.IsNotExist(statErr) {
			shouldProcess = true
		}
	}
	if !shouldProcess {
		d.xmpMu.Unlock()
		return
	}
	d.xmpSeen[x.Path] = t
	d.xmpMu.Unlock()

	localPath := d.resolveLocalPath(x.Path, x.Filename, x.CaptureDate)
	if localPath == "" {
		log.Printf("[xmp] recv: '%s' (capture=%s) from %s — no local file; fetching proxy",
			x.Filename, x.CaptureDate, from)
		go d.fetchAndImport(x.Path, x.Content, x.Filename, x.CaptureDate)
		return
	}
	if localPath != x.Path {
		log.Printf("[xmp] recv: '%s' from %s — path differs, applying to local '%s'",
			x.Filename, from, localPath)
	} else {
		log.Printf("[xmp] recv: '%s' from %s — applying to '%s'",
			x.Filename, from, localPath)
	}
	if err := writeXMP(localPath, x.Content); err != nil {
		log.Printf("[xmp] write '%s': %v", localPath, err)
	} else {
		result, _ := json.Marshal(map[string]string{"path": localPath})
		d.broadcast(socketMsg{Type: "xmp_updated", Data: result})
		// Record suppress entry so any immediate echo from our darktable
		// doesn't bounce the edit back to the peer who sent it.
		if x.SenderURL != "" {
			d.xmpMu.Lock()
			d.xmpSuppressFrom[localPath] = xmpSuppressEntry{
				url:   x.SenderURL,
				until: time.Now().Add(15 * time.Second),
			}
			d.xmpMu.Unlock()
		}
	}
}

func (d *daemon) subscribeXMP() {
	for {
		m, err := d.xmpSub.Next(d.ctx)
		if err != nil {
			return
		}
		// m.ReceivedFrom is the last-hop peer, not the originator.
		// Also check SenderID to drop our own messages that routed back via gossip.
		if m.ReceivedFrom == d.host.ID() {
			continue
		}
		var x xmpMsg
		if err := json.Unmarshal(m.Data, &x); err != nil {
			continue
		}
		if x.SenderID == d.host.ID().String() {
			continue // own message routed back via intermediate peer
		}
		d.applyInboundXMP(x, m.ReceivedFrom.ShortString())
	}
}

// pushXMPToPeers sends x directly via HTTP POST to every known reachable peer.
// This ensures delivery even when gossipsub has not yet established peer
// connections (peers=0).  excludeURL, if non-empty, is skipped — used to avoid
// bouncing an edit back to the peer we just received it from.
func (d *daemon) pushXMPToPeers(x xmpMsg, excludeURL string) {
	myURL := d.localProxyURL()
	myLANURL := fmt.Sprintf("https://%s:%d", d.localIP(), proxyHTTPPort)
	seen := make(map[string]bool)
	var targets []string
	collect := func(u string) {
		if u != "" && u != myURL && u != myLANURL && u != excludeURL && !seen[u] {
			seen[u] = true
			targets = append(targets, u)
		}
	}
	d.peersMu.RLock()
	for _, u := range d.peers {
		collect(u)
	}
	d.peersMu.RUnlock()
	for _, u := range d.peersToSync() {
		collect(u)
	}
	log.Printf("[xmp] push targets: myURL=%s  targets=%v", myURL, targets)
	if len(targets) == 0 {
		return
	}
	b, err := json.Marshal(x)
	if err != nil {
		return
	}
	for _, baseURL := range targets {
		baseURL := baseURL
		go func() {
			resp, err := d.httpPost(baseURL+"/xmp", "application/json", bytes.NewReader(b))
			if err != nil {
				log.Printf("[xmp] direct push to %s: %v", baseURL, err)
				return
			}
			resp.Body.Close()
			if resp.StatusCode == http.StatusNoContent || resp.StatusCode == http.StatusOK {
				log.Printf("[xmp] direct push to %s: ok", baseURL)
			} else {
				log.Printf("[xmp] direct push to %s: HTTP %d", baseURL, resp.StatusCode)
			}
		}()
	}
}

// fetchAndImport downloads a proxy for remotePath and imports it to darktable.
// Also writes any XMP content received alongside the proxy announcement.
func (d *daemon) fetchAndImport(remotePath, xmpContent, filename, captureDate string) {
	// Determine local destination.
	localPath := d.localDestination(remotePath)

	// Download proxy: prefer libp2p-known peers, fall back to peersToSync().
	d.peersMu.RLock()
	var baseURL string
	for _, u := range d.peers {
		baseURL = u
		break
	}
	d.peersMu.RUnlock()

	if baseURL == "" {
		myURL := d.localProxyURL()
		myLANURL := fmt.Sprintf("https://%s:%d", d.localIP(), proxyHTTPPort)
		for _, u := range d.peersToSync() {
			if u != myURL && u != myLANURL {
				baseURL = u
				break
			}
		}
	}

	if baseURL == "" {
		log.Printf("[import] no peer available to fetch proxy for '%s'", filename)
		return
	}

	if !d.downloadProxy(remotePath, localPath, baseURL) {
		return
	}

	// Write XMP if provided.
	if xmpContent != "" {
		if err := writeXMP(localPath, xmpContent); err != nil {
			log.Printf("[import] write XMP for '%s': %v", filename, err)
		}
	}

	log.Printf("[import] imported '%s' → '%s'", filename, localPath)
	d.addToLocalIndex(localPath)

	// Broadcast the canonical raw path so darktable imports it as the raw file
	// and uses 4F9A9030.CR3.xmp for develop settings.  _image_import_internal
	// will transparently fall back to the .proxy.avif when the raw is absent.
	result, _ := json.Marshal(map[string]string{"path": localPath})
	d.broadcast(socketMsg{Type: "image_imported", Data: result})
}

// ---------------------------------------------------------------------------
// Proxy subscription
// ---------------------------------------------------------------------------

func (d *daemon) subscribeProxy() {
	for {
		m, err := d.proxySub.Next(d.ctx)
		if err != nil {
			return
		}
		if m.ReceivedFrom == d.host.ID() {
			continue
		}
		var ann proxyAnnounce
		if err := json.Unmarshal(m.Data, &ann); err != nil {
			continue
		}
		d.peersMu.Lock()
		d.peers[m.ReceivedFrom] = ann.BaseURL
		d.peersMu.Unlock()
		d.pdb.touch(ann.BaseURL, ann.SenderID) // persist gossipsub-discovered peer
		log.Printf("[proxy] peer %s announced %d proxies at %s",
			m.ReceivedFrom.ShortString(), len(ann.Paths), ann.BaseURL)

		for _, path := range ann.Paths {
			localPath := d.localDestination(path)
			if _, err := os.Stat(localPath + ".proxy.avif"); err == nil {
				continue // already have it
			}
			log.Printf("[proxy] queuing fetch: '%s'", filepath.Base(path))
			go d.autoFetchProxy(path, ann.BaseURL)
		}
	}
}

// ---------------------------------------------------------------------------
// Proxy fetching
// ---------------------------------------------------------------------------

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

// downloadProxy fetches a proxy from baseURL, writes it to localPath+".proxy.avif",
// and returns true on success.
func (d *daemon) downloadProxy(remotePath, localPath, baseURL string) bool {
	url := baseURL + "/proxy?path=" + remotePath
	resp, err := d.httpGet(url)
	if err != nil {
		log.Printf("[proxy] fetch '%s' from %s: %v", filepath.Base(remotePath), baseURL, err)
		return false
	}
	if resp.StatusCode != 200 {
		resp.Body.Close()
		log.Printf("[proxy] fetch '%s' from %s: HTTP %d", filepath.Base(remotePath), baseURL, resp.StatusCode)
		return false
	}
	data, err := io.ReadAll(resp.Body)
	resp.Body.Close()
	if err != nil {
		return false
	}

	dst := localPath + ".proxy.avif"
	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		log.Printf("[proxy] mkdir '%s': %v", filepath.Dir(dst), err)
		return false
	}
	if err := os.WriteFile(dst, data, 0644); err != nil {
		log.Printf("[proxy] write '%s': %v", dst, err)
		return false
	}
	log.Printf("[proxy] fetched '%s' → '%s' (%d KB)", filepath.Base(remotePath), dst, len(data)/1024)
	return true
}

// fetchPreviewFromPeers tries every known peer until it obtains a JPEG preview
// for canonicalPath at the requested size, then caches it locally.
func (d *daemon) fetchPreviewFromPeers(canonicalPath, size string) {
	localPath := d.localDestination(canonicalPath)

	d.peersMu.RLock()
	urls := make([]string, 0, len(d.peers))
	for _, u := range d.peers {
		urls = append(urls, u)
	}
	d.peersMu.RUnlock()

	for _, baseURL := range urls {
		d.fetchPreviewJPEG(canonicalPath, localPath, baseURL, size)
		if fileExists(localPath + ".preview-" + size + ".jpg") {
			return
		}
	}
}

// autoFetchProxy downloads a proxy and notifies darktable to import it.
func (d *daemon) autoFetchProxy(remotePath, baseURL string) {
	localPath := d.localDestination(remotePath)

	if !d.downloadProxy(remotePath, localPath, baseURL) {
		return
	}

	// Fetch the thumbnail JPEG preview in the background so the mobile gallery
	// can display images instantly without MediaCodec decoding.
	go d.fetchPreviewJPEG(remotePath, localPath, baseURL, "thumb")

	d.addToLocalIndex(localPath)

	// Broadcast the canonical raw path so darktable imports 4F9A9030.CR3 and
	// reads 4F9A9030.CR3.xmp for develop settings.  _image_import_internal
	// falls back to the .proxy.avif transparently when the raw is absent.
	result, _ := json.Marshal(map[string]string{"path": localPath})
	d.broadcast(socketMsg{Type: "image_imported", Data: result})
}

// fetchPreviewJPEG downloads a JPEG preview from a peer and caches it beside
// the local proxy AVIF. size must be "thumb" or "full".
//
// Uses a conditional GET (If-Modified-Since) when a cached copy already exists,
// so the file is only re-downloaded when the desktop has a fresher version.
// Broadcasts preview_updated when an existing preview is replaced.
func (d *daemon) fetchPreviewJPEG(remotePath, localPath, baseURL, size string) {
	dst := localPath + ".preview-" + size + ".jpg"
	previewURL := baseURL + "/preview?path=" + url.QueryEscape(remotePath) + "&size=" + size

	req, err := http.NewRequest(http.MethodGet, previewURL, nil)
	if err != nil {
		return
	}
	req.Header.Set(passphraseHeader, d.authToken)

	// Conditional GET: ask the server to skip the body if nothing changed.
	hadCached := false
	if fi, err := os.Stat(dst); err == nil {
		hadCached = true
		req.Header.Set("If-Modified-Since", fi.ModTime().UTC().Format(http.TimeFormat))
	}

	resp, err := d.tlsClient.Do(req)
	if err != nil {
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusNotModified {
		return // our copy is still current
	}
	if resp.StatusCode != http.StatusOK {
		return
	}

	data, err := io.ReadAll(resp.Body)
	if err != nil || len(data) == 0 {
		return
	}

	if err := os.WriteFile(dst, data, 0644); err != nil {
		return
	}

	verb := "fetched"
	if hadCached {
		verb = "refreshed"
	}
	log.Printf("[preview] %s %s preview for '%s' (%d KB)", verb, size, filepath.Base(remotePath), len(data)/1024)
	result, _ := json.Marshal(map[string]string{"path": localPath})
	d.broadcast(socketMsg{Type: "preview_updated", Data: result})
}

// fetchProxyFromPeer handles an explicit "fetch_proxy" command from darktable.
func (d *daemon) fetchProxyFromPeer(canonicalPath string, enc *json.Encoder) {
	d.peersMu.RLock()
	urls := make([]string, 0, len(d.peers))
	for _, u := range d.peers {
		urls = append(urls, u)
	}
	d.peersMu.RUnlock()

	for _, baseURL := range urls {
		url := baseURL + "/proxy?path=" + canonicalPath
		resp, err := d.httpGet(url)
		if err != nil || resp.StatusCode != 200 {
			if resp != nil { resp.Body.Close() }
			continue
		}
		data, err := io.ReadAll(resp.Body)
		resp.Body.Close()
		if err != nil {
			continue
		}
		dst := canonicalPath + ".proxy.avif"
		if err := os.MkdirAll(filepath.Dir(dst), 0755); err == nil {
			if err := os.WriteFile(dst, data, 0644); err == nil {
				log.Printf("[proxy] explicit fetch '%s' from %s (%d KB)",
					filepath.Base(canonicalPath), baseURL, len(data)/1024)
				result, _ := json.Marshal(map[string]string{"path": dst, "status": "ok"})
				enc.Encode(socketMsg{Type: "proxy_fetched", Data: result})
				return
			}
		}
	}
	result, _ := json.Marshal(map[string]string{"path": canonicalPath, "status": "not_found"})
	enc.Encode(socketMsg{Type: "proxy_fetched", Data: result})
}

// ---------------------------------------------------------------------------
// Proxy announce
// ---------------------------------------------------------------------------

func (d *daemon) announceProxies() {
	tick := time.NewTicker(30 * time.Second)
	defer tick.Stop()
	d.publishProxyAnnounce()
	for {
		select {
		case <-d.ctx.Done():
			return
		case <-tick.C:
			d.publishProxyAnnounce()
		}
	}
}

func (d *daemon) publishProxyAnnounce() {
	if d.proxyDir == "" {
		return
	}
	var paths []string
	filepath.Walk(d.proxyDir, func(p string, fi os.FileInfo, err error) error {
		if err != nil || fi.IsDir() {
			return nil
		}
		if strings.HasSuffix(p, ".proxy.avif") {
			paths = append(paths, strings.TrimSuffix(p, ".proxy.avif"))
		}
		return nil
	})
	ann := proxyAnnounce{
		SenderID: d.host.ID().String(),
		BaseURL:  d.localProxyURL(),
		Paths:    paths,
	}
	if b, err := json.Marshal(ann); err == nil {
		d.proxyTop.Publish(d.ctx, b)
		if len(paths) > 0 {
			log.Printf("[announce] broadcasting %d proxies", len(paths))
		}
	}
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

	// Download any proxy the peer has that we don't.  For proxies we already
	// have, check whether the peer's JPEG preview is fresher than ours.
	missing := 0
	for _, path := range m.Paths {
		localPath := d.localDestination(path)
		if _, err := os.Stat(localPath + ".proxy.avif"); err == nil {
			go d.fetchPreviewJPEG(path, localPath, baseURL, "thumb")
			continue
		}
		missing++
		go d.autoFetchProxy(path, baseURL)
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

func (d *daemon) localIP() string {
	if d.localIPOverride != "" {
		return d.localIPOverride
	}
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

func (d *daemon) serveXMP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var x xmpMsg
	if err := json.NewDecoder(r.Body).Decode(&x); err != nil {
		http.Error(w, "bad request", http.StatusBadRequest)
		return
	}
	w.WriteHeader(http.StatusNoContent)
	go d.applyInboundXMP(x, "http:"+r.RemoteAddr)
}

func (d *daemon) serveProxy(w http.ResponseWriter, r *http.Request) {
	canonicalPath := r.URL.Query().Get("path")
	if canonicalPath == "" {
		http.Error(w, "missing path", 400)
		return
	}
	proxyPath := canonicalPath + ".proxy.avif"
	if strings.Contains(proxyPath, "..") {
		http.Error(w, "forbidden", 403)
		return
	}
	log.Printf("[http] serve proxy '%s' to %s", filepath.Base(canonicalPath), r.RemoteAddr)
	http.ServeFile(w, r, proxyPath)
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

	seen := make(map[string]struct{})
	var paths []string

	addPath := func(raw string) {
		if _, dup := seen[raw]; dup {
			return
		}
		if _, err := os.Stat(raw + ".proxy.avif"); err == nil {
			seen[raw] = struct{}{}
			paths = append(paths, raw)
		}
	}

	if d.proxyDir != "" {
		filepath.Walk(d.proxyDir, func(p string, fi os.FileInfo, err error) error {
			if err != nil || fi.IsDir() {
				return nil
			}
			if strings.HasSuffix(p, ".proxy.avif") {
				addPath(strings.TrimSuffix(p, ".proxy.avif"))
			}
			return nil
		})
	}

	d.announcedProxiesMu.RLock()
	for raw := range d.announcedProxies {
		addPath(raw)
	}
	d.announcedProxiesMu.RUnlock()

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
		KnownPeers:  knownPeers,
	}
	if resp.Paths == nil {
		resp.Paths = []string{}
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

// ---------------------------------------------------------------------------
// JPEG preview endpoint
// ---------------------------------------------------------------------------

// servePreview serves JPEG previews for raw images.
//
// GET /preview?path=<canonical-raw-path>&size=thumb|full
//
// Sources, in priority order:
//  1. Cached preview JPEG beside the proxy AVIF (.preview-thumb.jpg / .preview-full.jpg)
//  2. darktable's mipmap cache (~/.cache/darktable/mipmaps-*.d/)
//  3. darktable-cli export (desktop only; slow but guaranteed)
//
// size=thumb picks mipmap sizes [1,2] (~337–450 px wide).
// size=full  picks mipmap sizes [4,6,3] (~1800/3800/1350 px wide).
//
// Uses http.ServeContent so clients get proper Last-Modified and 304 support.
func (d *daemon) servePreview(w http.ResponseWriter, r *http.Request) {
	rawPath := r.URL.Query().Get("path")
	if rawPath == "" || strings.Contains(rawPath, "..") {
		http.Error(w, "bad request", http.StatusBadRequest)
		return
	}

	sizeParam := r.URL.Query().Get("size")
	var (
		cacheSuffix string
		mipmapSizes []int
		maxDim      int
	)
	if sizeParam == "full" {
		cacheSuffix = ".preview-full.jpg"
		mipmapSizes = []int{4, 6, 3}
		maxDim = 1920
	} else {
		cacheSuffix = ".preview-thumb.jpg"
		mipmapSizes = []int{1, 2}
		maxDim = 480
	}

	cachePath := rawPath + cacheSuffix

	// Delete a stale cache file that lacks an ICC profile so it is regenerated
	// with the correct color metadata on this request.
	if fileExists(cachePath) {
		if existing, err := os.ReadFile(cachePath); err == nil && !hasJPEGICCProfile(existing) {
			os.Remove(cachePath)
			log.Printf("[preview] removed ICC-less cache for '%s' size=%s; regenerating",
				filepath.Base(rawPath), sizeParam)
		}
	}

	// Populate the cache file if it doesn't exist yet.
	if !fileExists(cachePath) {
		mipmapData := d.findMipmapJPEG(rawPath, mipmapSizes)

		// For full-size previews, mipmap cache JEPGs must go through
		// darktable-cli to guarantee ICC profile + full Exif metadata (camera
		// make/model/lens/exposure/GPS) so sharing to other apps is complete.
		if sizeParam == "full" && mipmapData != nil {
			log.Printf("[preview] full size: skipping mipmap for '%s'; using darktable-cli for ICC+Exif",
				filepath.Base(rawPath))
			mipmapData = nil
		}

		if mipmapData != nil {
			// Thumbnails: mipmap JPEG is fine for display but lacks the sRGB
			// ICC marker. Inject it in-process so Qt/Android renders with the
			// correct colour transform.
			mipmapData = d.injectDisplayICC(mipmapData)
			if err := os.WriteFile(cachePath, mipmapData, 0644); err == nil {
				log.Printf("[preview] mipmap+ICC → cache '%s' size=%s (%d KB)",
					filepath.Base(rawPath), sizeParam, len(mipmapData)/1024)
			}
		} else if _, err := exportWithDarktableCLI(rawPath, cachePath, maxDim); err != nil {
			http.Error(w, "preview not available", http.StatusNotFound)
			return
		} else {
			log.Printf("[preview] cli export '%s' size=%s", filepath.Base(rawPath), sizeParam)
		}
	}

	// Serve the cache file. http.ServeContent handles Last-Modified and 304.
	f, err := os.Open(cachePath)
	if err != nil {
		http.Error(w, "preview not available", http.StatusNotFound)
		return
	}
	defer f.Close()
	fi, err := f.Stat()
	if err != nil {
		http.Error(w, "stat failed", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "image/jpeg")
	http.ServeContent(w, r, "", fi.ModTime(), f)
}

// findMipmapJPEG looks up the image ID in darktable's library.db and returns
// the first mipmap JPEG found for the given size preference list.
func (d *daemon) findMipmapJPEG(rawPath string, sizes []int) []byte {
	mipmapDir, err := darktableMipmapDir()
	if err != nil {
		return nil
	}
	libPath := darktableLibraryPath()
	if libPath == "" {
		return nil
	}
	imgID, err := lookupDarktableImageID(libPath, rawPath)
	if err != nil || imgID == 0 {
		return nil
	}
	for _, sz := range sizes {
		p := filepath.Join(mipmapDir, fmt.Sprintf("%d", sz), fmt.Sprintf("%d.jpg", imgID))
		if data, err := os.ReadFile(p); err == nil {
			return data
		}
	}
	return nil
}

// darktableLibraryPath returns the path to darktable's library.db, or "".
func darktableLibraryPath() string {
	cfgDir, err := os.UserConfigDir()
	if err != nil {
		return ""
	}
	p := filepath.Join(cfgDir, "darktable", "library.db")
	if _, err := os.Stat(p); err == nil {
		return p
	}
	return ""
}

// darktableMipmapDir returns the path to the darktable mipmap cache directory.
func darktableMipmapDir() (string, error) {
	cacheDir, err := os.UserCacheDir()
	if err != nil {
		return "", err
	}
	dtCache := filepath.Join(cacheDir, "darktable")
	entries, err := os.ReadDir(dtCache)
	if err != nil {
		return "", err
	}
	for _, e := range entries {
		if e.IsDir() && strings.HasPrefix(e.Name(), "mipmaps-") && strings.HasSuffix(e.Name(), ".d") {
			return filepath.Join(dtCache, e.Name()), nil
		}
	}
	return "", fmt.Errorf("no mipmaps directory in %s", dtCache)
}

// lookupDarktableImageID queries library.db for the image ID matching rawPath.
func lookupDarktableImageID(libPath, rawPath string) (int64, error) {
	// Open read-only; WAL mode so we don't interfere with a running darktable.
	db, err := sql.Open("sqlite", libPath+"?mode=ro&_journal=WAL")
	if err != nil {
		return 0, err
	}
	defer db.Close()

	dir := filepath.Dir(rawPath)
	name := filepath.Base(rawPath)
	var id int64
	err = db.QueryRow(`
		SELECT img.id
		FROM images img
		JOIN film_rolls f ON img.film_id = f.id
		WHERE img.filename = ? AND f.folder = ?`,
		name, dir).Scan(&id)
	if err != nil {
		return 0, err
	}
	return id, nil
}

// exportWithDarktableCLI renders rawPath to outPath via darktable-cli and
// returns the JPEG bytes. Applies the XMP sidecar if present.
//
// Produces sRGB output with relative-colorimetric rendering intent (the
// standard choice for photographic output), then copies the full Exif block
// from the raw file into the JPEG using exiftool so the recipient has all
// camera metadata (make, model, lens, exposure, GPS, etc.).
func exportWithDarktableCLI(rawPath, outPath string, maxDim int) ([]byte, error) {
	args := []string{rawPath}
	if xmp := rawPath + ".xmp"; fileExists(xmp) {
		args = append(args, xmp)
	}
	args = append(args, outPath,
		"--width", fmt.Sprintf("%d", maxDim),
		"--height", fmt.Sprintf("%d", maxDim),
		"--icc-type", "SRGB",
		"--icc-intent", "RELATIVE_COLORIMETRIC",
	)
	cmd := exec.Command("darktable-cli", args...)
	if out, err := cmd.CombinedOutput(); err != nil {
		return nil, fmt.Errorf("darktable-cli: %w\n%s", err, out)
	}

	// Copy all Exif tags from the raw source into the rendered JPEG.
	// darktable-cli strips Exif from exports; exiftool restores it.
	// -overwrite_original avoids leaving a _original backup file.
	// -tagsFromFile copies all tags; -m ignores minor errors.
	if exiftoolPath, err := exec.LookPath("exiftool"); err == nil {
		exif := exec.Command(exiftoolPath,
			"-overwrite_original",
			"-m",
			"-tagsFromFile", rawPath,
			"-Exif:All",        // all Exif tags (camera make/model/lens/exposure/GPS…)
			"-IPTC:All",        // keywords, caption, copyright
			outPath,
		)
		if out, err := exif.CombinedOutput(); err != nil {
			log.Printf("[preview] exiftool copy from %s: %v\n%s", filepath.Base(rawPath), err, out)
			// Non-fatal: JPEG is still usable, just lacks full Exif.
		}
	} else {
		log.Printf("[preview] exiftool not found; skipping Exif copy for %s", filepath.Base(rawPath))
	}

	return os.ReadFile(outPath)
}

// loadDisplayICC returns the bytes of the desktop monitor's ICC profile.
// Resolution order:
//  1. Explicit path from --display-icc flag.
//  2. Active display profile from colord (colormgr CLI).
//  3. First 'mntr'-class ICC file found in ~/.local/share/icc/.
//  4. Hardcoded compact sRGB profile as a universal fallback.
func loadDisplayICC(explicitPath string) []byte {
	if explicitPath != "" {
		if data, err := os.ReadFile(explicitPath); err == nil {
			log.Printf("[color] display ICC from --display-icc (%d bytes)", len(data))
			return data
		} else {
			log.Printf("[color] --display-icc %s: %v; trying auto-detect", explicitPath, err)
		}
	}

	if p := findDisplayICCViaColormgr(); p != "" {
		if data, err := os.ReadFile(p); err == nil {
			log.Printf("[color] display ICC from colord: %s (%d bytes)", filepath.Base(p), len(data))
			return data
		}
	}

	if home, err := os.UserHomeDir(); err == nil {
		iccDir := filepath.Join(home, ".local", "share", "icc")
		entries, _ := os.ReadDir(iccDir)
		for _, e := range entries {
			if !strings.HasSuffix(e.Name(), ".icc") {
				continue
			}
			p := filepath.Join(iccDir, e.Name())
			data, err := os.ReadFile(p)
			if err != nil || len(data) < 16 {
				continue
			}
			if string(data[12:16]) == "mntr" { // ICC profile class = display
				log.Printf("[color] display ICC from %s (%d bytes)", e.Name(), len(data))
				return data
			}
		}
	}

	log.Printf("[color] display ICC not detected; using compact sRGB fallback")
	return compactSRGBICC()
}

// findDisplayICCViaColormgr asks colord for the active display profile path.
func findDisplayICCViaColormgr() string {
	out, err := exec.Command("colormgr", "get-devices").Output()
	if err != nil {
		return ""
	}
	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, "Object Path:") || !strings.Contains(line, "display") {
			continue
		}
		devicePath := strings.TrimSpace(strings.TrimPrefix(line, "Object Path:"))
		prof, err := exec.Command("colormgr", "device-get-default-profile", devicePath).Output()
		if err != nil {
			continue
		}
		for _, pl := range strings.Split(string(prof), "\n") {
			pl = strings.TrimSpace(pl)
			if strings.HasPrefix(pl, "Filename:") {
				return strings.TrimSpace(strings.TrimPrefix(pl, "Filename:"))
			}
		}
	}
	return ""
}

// injectICCProfile inserts icc as an APP2 ICC_PROFILE segment immediately
// after the JPEG SOI marker. Returns data unchanged if it already has a profile.
func injectICCProfile(data, icc []byte) []byte {
	if len(data) < 2 || data[0] != 0xFF || data[1] != 0xD8 {
		return data
	}
	if hasJPEGICCProfile(data) {
		return data
	}
	const iccHeader = "ICC_PROFILE\x00\x01\x01" // single chunk
	segPayload := append([]byte(iccHeader), icc...)
	segLen := len(segPayload) + 2
	app2 := []byte{0xFF, 0xE2, byte(segLen >> 8), byte(segLen)}
	app2 = append(app2, segPayload...)
	result := make([]byte, 0, len(data)+len(app2))
	result = append(result, data[:2]...)
	result = append(result, app2...)
	result = append(result, data[2:]...)
	return result
}

// injectDisplayICC embeds the daemon's display ICC into a JPEG (for thumbnails).
func (d *daemon) injectDisplayICC(data []byte) []byte {
	return injectICCProfile(data, d.displayICC)
}

// compactSRGBICC returns a compact IEC 61966-2-1 sRGB ICC v2 profile (~212 bytes)
// used as a fallback when no system display profile can be detected.
func compactSRGBICC() []byte {
	return []byte{
		0x00, 0x00, 0x00, 0xd4, 0x6c, 0x63, 0x6d, 0x73, 0x02, 0x10, 0x00, 0x00,
		0x6d, 0x6e, 0x74, 0x72, 0x52, 0x47, 0x42, 0x20, 0x58, 0x59, 0x5a, 0x20,
		0x07, 0xe7, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x61, 0x63, 0x73, 0x70, 0x4d, 0x53, 0x46, 0x54, 0x00, 0x00, 0x00, 0x00,
		0x73, 0x61, 0x77, 0x73, 0x63, 0x74, 0x72, 0x6c, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf6, 0xd6,
		0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xd3, 0x2d, 0x6c, 0x63, 0x6d, 0x73,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,
		0x64, 0x65, 0x73, 0x63, 0x00, 0x00, 0x00, 0xf0, 0x00, 0x00, 0x00, 0x12,
		0x77, 0x74, 0x70, 0x74, 0x00, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x14,
		0x72, 0x58, 0x59, 0x5a, 0x00, 0x00, 0x01, 0x18, 0x00, 0x00, 0x00, 0x14,
		0x67, 0x58, 0x59, 0x5a, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x00, 0x00, 0x14,
		0x62, 0x58, 0x59, 0x5a, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x14,
		0x72, 0x54, 0x52, 0x43, 0x00, 0x00, 0x01, 0x54, 0x00, 0x00, 0x00, 0x28,
		0x67, 0x54, 0x52, 0x43, 0x00, 0x00, 0x01, 0x54, 0x00, 0x00, 0x00, 0x28,
		0x62, 0x54, 0x52, 0x43, 0x00, 0x00, 0x01, 0x54, 0x00, 0x00, 0x00, 0x28,
		0x63, 0x70, 0x72, 0x74, 0x00, 0x00, 0x01, 0x7c, 0x00, 0x00, 0x00, 0x58,
		// desc "sRGB"
		0x6d, 0x6c, 0x75, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x0c, 0x65, 0x6e, 0x55, 0x53, 0x00, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x00, 0x52, 0x00, 0x47, 0x00, 0x42,
		// wtpt D50
		0x58, 0x59, 0x5a, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf3, 0x52,
		0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x16, 0xcc,
		// rXYZ
		0x58, 0x59, 0x5a, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6f, 0xa0,
		0x00, 0x00, 0x38, 0xf5, 0x00, 0x00, 0x03, 0x90,
		// gXYZ
		0x58, 0x59, 0x5a, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x97,
		0x00, 0x00, 0xb7, 0x87, 0x00, 0x00, 0x18, 0xda,
		// bXYZ
		0x58, 0x59, 0x5a, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0xa0,
		0x00, 0x00, 0x0f, 0x84, 0x00, 0x00, 0xb6, 0xc4,
		// TRC (parametric curve for sRGB gamma)
		0x70, 0x61, 0x72, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
		0x00, 0x02, 0x4c, 0x1d, 0x00, 0x0a, 0x92, 0x6b, 0x00, 0x01, 0x4a, 0xb6,
		0x00, 0x00, 0x40, 0x6e, 0x00, 0x00, 0x09, 0x6c,
		// cprt
		0x74, 0x65, 0x78, 0x74, 0x00, 0x00, 0x00, 0x00, 0x43, 0x72, 0x65, 0x61,
		0x74, 0x65, 0x64, 0x20, 0x62, 0x79, 0x20, 0x6c, 0x63, 0x6d, 0x73, 0x20,
		0x32, 0x2e, 0x31, 0x30, 0x20, 0x28, 0x63, 0x29, 0x20, 0x32, 0x30, 0x31,
		0x32, 0x2e, 0x20, 0x49, 0x45, 0x43, 0x20, 0x36, 0x31, 0x39, 0x36, 0x36,
		0x2d, 0x32, 0x2d, 0x31, 0x20, 0x44, 0x65, 0x66, 0x61, 0x75, 0x6c, 0x74,
		0x20, 0x52, 0x47, 0x42, 0x20, 0x43, 0x6f, 0x6c, 0x6f, 0x75, 0x72, 0x20,
		0x53, 0x70, 0x61, 0x63, 0x65, 0x20, 0x2d, 0x20, 0x73, 0x52, 0x47, 0x42,
		0x00, 0x00, 0x00, 0x00,
	}
}

// hasJPEGICCProfile reports whether the JPEG byte slice contains an APP2
// segment whose payload begins with the "ICC_PROFILE\x00" signature.
func hasJPEGICCProfile(data []byte) bool {
	const iccSig = "ICC_PROFILE\x00"
	// Walk APP markers. JPEG starts with FF D8; APP markers are FF Ex/FF Ex.
	for i := 2; i+4 < len(data); {
		if data[i] != 0xFF {
			break
		}
		marker := data[i+1]
		segLen := int(data[i+2])<<8 | int(data[i+3]) // includes 2-byte length field
		if segLen < 2 {
			break
		}
		payloadStart := i + 4
		payloadLen := segLen - 2
		// APP2 = 0xE2
		if marker == 0xE2 && payloadLen >= len(iccSig) {
			if string(data[payloadStart:payloadStart+len(iccSig)]) == iccSig {
				return true
			}
		}
		i += 2 + segLen
	}
	return false
}

func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

func (d *daemon) close() {
	d.cancel()
	if d.httpSrv != nil {
		d.httpSrv.Close()
	}
	if d.ln != nil {
		d.ln.Close()
	}
	d.host.Close()
	d.pdb.close()
}

// ---------------------------------------------------------------------------
// XMP file write
// ---------------------------------------------------------------------------

func writeXMP(rawPath, content string) error {
	xmpPath := rawPath + ".xmp"
	tmp := xmpPath + ".tmp"
	if err := os.WriteFile(tmp, []byte(content), 0644); err != nil {
		return err
	}
	return os.Rename(tmp, xmpPath)
}
