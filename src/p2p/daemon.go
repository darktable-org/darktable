package main

import (
	"context"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"

	gocrypto "crypto"

	libp2p "github.com/libp2p/go-libp2p"
	pubsub "github.com/libp2p/go-libp2p-pubsub"
	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/p2p/discovery/mdns"
	"golang.org/x/crypto/pbkdf2"

	_ "modernc.org/sqlite"
)

const (
	xmpTopic       = "darktable/xmp/v1"
	proxyTopic     = "darktable/proxy-announce/v1"
	mdnsServiceTag = "darktable-p2p"
	pbkdf2Iter     = 100000
	keyLen         = 32
	proxyHTTPPort  = 17842
	// passphraseHeader is required on every inbound HTTP request.
	passphraseHeader = "X-DT-Auth"
)

// reDatetime matches exif:DateTimeOriginal (and similar) in XMP XML.
var reDatetime = regexp.MustCompile(
	`<(?:exif:DateTimeOriginal|exif:DateTimeDigitized|xmp:CreateDate)[^>]*>([^<]+)<`)

// eventSub is a darktable client subscribed to push events.
// candidatePeer is a peer whose TLS fingerprint was seen but is not yet in the
// allowed list.  Stored so the preferences UI can offer the user an Accept button.
type candidatePeer struct {
	URL         string    `json:"url"`
	Fingerprint string    `json:"fingerprint"`
	SeenAt      time.Time `json:"seen_at"`
}

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
	tlsCert         tls.Certificate
	authToken       string // passphraseToken(passphrase) — sent as X-DT-Auth header
	allowedKeyFPsMu sync.RWMutex
	allowedKeyFPs   map[string]bool // SHA256 SPKI fingerprints of trusted peer servers
	tlsClient       *http.Client    // TLS-configured client for all outbound peer requests

	// Candidate peers: discovered but not yet trusted (fingerprint not in allowed set).
	// Presented to the user in the preferences UI for explicit acceptance.
	candidatesMu   sync.RWMutex
	candidatePeers map[string]candidatePeer // fingerprint → candidate

	// xmp dedup: canonical_path → last mtime synced
	xmpMu   sync.Mutex
	xmpSeen map[string]time.Time
	// xmpSuppressFrom: after applying inbound XMP, suppress echo to the sender
	// for a short window so we don't bounce the edit back to whoever sent it.
	xmpSuppressFrom map[string]xmpSuppressEntry

	// local file index: basename → []canonical-path, rebuilt on start and on import
	localIndexMu sync.RWMutex
	localIndex   map[string][]string

	// announced proxies: raw paths announced by darktable via socket
	announcedProxiesMu sync.RWMutex
	announcedProxies   map[string]struct{}

	// manifest cache: rebuilt lazily when announcedProxies changes.
	// manifestHash == "" means the cache is invalid and must be rebuilt.
	manifestMu    sync.Mutex
	manifestPaths []string
	manifestHash  string

	// per-peer manifest hash seen on the last successful sync; used to skip
	// the expensive per-path broadcast+stat work when nothing changed.
	peerHashMu   sync.Mutex
	peerLastHash map[string]string

	// localToRemote maps a phone-local path (importDir/file.CR3) back to the
	// peer's canonical path (/home/.../.../file.CR3) so that explicit
	// fetch_proxy commands (which arrive with local paths) can request the
	// correct path from the peer's HTTP server.
	localToRemoteMu sync.RWMutex
	localToRemote   map[string]string

	// pathMapDirty / pathMapMu: true when localToRemote was modified but not
	// yet flushed.  Actual write happens at most once every 5 s.
	pathMapMu    sync.Mutex
	pathMapDirty bool

	// downloadSem limits concurrent proxy/preview fetches so a large library
	// does not spawn thousands of goroutines at once.
	downloadSem chan struct{}

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
	Mtime       int64  `json:"mtime"`                // unix ms
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
	Hash        string   `json:"hash,omitempty"`        // sha256 of sorted path list; clients skip sync when unchanged
	KnownPeers  []string `json:"known_peers,omitempty"` // HTTP URLs of other known peers
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
	if err != nil {
		h.Close()
		return nil, err
	}
	xmpSub, err := xmpTop.Subscribe()
	if err != nil {
		h.Close()
		return nil, err
	}

	proxyTop, err := ps.Join(proxyTopic)
	if err != nil {
		h.Close()
		return nil, err
	}
	proxySub, err := proxyTop.Subscribe()
	if err != nil {
		h.Close()
		return nil, err
	}

	dctx, cancel := context.WithCancel(ctx)

	d := &daemon{
		ctx:              dctx,
		cancel:           cancel,
		socketPath:       socketPath,
		proxyDir:         proxyDir,
		importDir:        importDir,
		staticPeers:      staticPeers,
		passphrase:       passphrase,
		ownFP:            ownFP,
		localIPOverride:  localIPOverride,
		host:             h,
		ps:               ps,
		xmpSub:           xmpSub,
		xmpTop:           xmpTop,
		proxySub:         proxySub,
		proxyTop:         proxyTop,
		peers:            make(map[peer.ID]string),
		syncedPeers:      make(map[string]bool),
		tlsCert:          tlsCert,
		authToken:        authTok,
		allowedKeyFPs:    allowed,
		xmpSeen:          make(map[string]time.Time),
		xmpSuppressFrom:  make(map[string]xmpSuppressEntry),
		localIndex:       make(map[string][]string),
		announcedProxies: make(map[string]struct{}),
		peerLastHash:     make(map[string]string),
		localToRemote:    make(map[string]string),
		downloadSem:      make(chan struct{}, 8),
		displayICC:       loadDisplayICC(displayICCPath),
	}

	// Build TLS client after d is created so newClientTLS can close over d
	// and read allowedKeyFPs dynamically (enabling live accept_peer updates).
	d.tlsClient = &http.Client{
		Timeout: 10 * time.Second,
		Transport: &http.Transport{
			TLSClientConfig: d.newClientTLS(),
		},
	}

	go d.initExternalAccess()

	// Restore local→remote path mapping from previous session so fetch_proxy
	// and fetch_preview work correctly after a daemon restart on mobile.
	d.loadPathMap()

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
	go d.runPathMapFlusher()

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

	// Ask darktable to re-announce its proxies so our localIndex is populated
	// even when the daemon was (re)started after darktable was already running.
	ch <- socketMsg{Type: "request_announce"}

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
				log.Printf("[xmp] parse error: %v", err)
				continue
			}
			x.SenderID = d.host.ID().String()
			x.SenderURL = d.localProxyURL()
			x.Filename = filepath.Base(x.Path)
			x.CaptureDate = xmpCaptureDate(x.Content)

			// Dedup: darktable may send the same XMP twice (debounce + sidecar job).
			t := time.UnixMilli(x.Mtime)
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

			// Write XMP to the local file so darktable on this machine picks it up.
			// This covers the mobile-app-on-same-machine case; remote machines get it
			// via gossipsub / HTTP push below.
			localPath := d.resolveLocalPath(x.Path, x.Filename, x.CaptureDate)
			if localPath == "" {
				log.Printf("[xmp] send: '%s' — no local path found (not yet indexed?)", x.Filename)
			} else if err := writeXMP(localPath, x.Content); err != nil {
				log.Printf("[xmp] local write '%s': %v", localPath, err)
			} else {
				invalidatePreviewCache(localPath)
				result, _ := json.Marshal(map[string]string{"path": localPath})
				d.broadcast(socketMsg{Type: "xmp_updated", Data: result})
			}

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
				// Invalidate manifest cache so the next /manifest request rebuilds it.
				d.manifestMu.Lock()
				d.manifestHash = ""
				d.manifestMu.Unlock()
				// Index by basename so resolveLocalPath can find this file when
				// an inbound XMP push from mobile arrives with the mobile local path.
				d.addToLocalIndex(req.Path)
			}

		case "fetch_proxy":
			var req struct {
				Path string `json:"path"`
			}
			if err := json.Unmarshal(msg.Data, &req); err != nil {
				continue
			}
			log.Printf("[proxy] explicit fetch request for '%s'", filepath.Base(req.Path))
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
			log.Printf("[preview] explicit fetch request for '%s' size=%s",
				filepath.Base(req.Path), req.Size)
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
				// Fetch a fresh preview from peers.  We do NOT delete the cached
				// copy first — the old JPEG stays visible in the gallery until the
				// new download atomically overwrites it.  fetchPreviewJPEG uses a
				// conditional GET (If-Modified-Since) so the desktop only returns
				// a body when it has re-rendered something newer.
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

		case "list_peer_status":
			d.syncedPeersMu.Lock()
			syncedURLs := make(map[string]bool, len(d.syncedPeers))
			for u := range d.syncedPeers {
				syncedURLs[u] = true
			}
			d.syncedPeersMu.Unlock()

			type peerStatus struct {
				URL          string `json:"url"`
				PeerID       string `json:"peer_id"`
				LastSeenUnix int64  `json:"last_seen"`
				FailureCount int    `json:"failure_count"`
				Synced       bool   `json:"synced"`
			}
			records := d.pdb.allRecords()
			statuses := make([]peerStatus, 0, len(records))
			for _, r := range records {
				statuses = append(statuses, peerStatus{
					URL:          r.URL,
					PeerID:       r.PeerID,
					LastSeenUnix: r.LastSeen.Unix(),
					FailureCount: r.FailureCount,
					Synced:       syncedURLs[r.URL],
				})
			}
			resp, _ := json.Marshal(statuses)
			enc.Encode(socketMsg{Type: "peer_status", Data: resp})

		case "list_candidates":
			d.candidatesMu.RLock()
			snapshot := make([]candidatePeer, 0, len(d.candidatePeers))
			for _, c := range d.candidatePeers {
				snapshot = append(snapshot, c)
			}
			d.candidatesMu.RUnlock()
			resp, _ := json.Marshal(snapshot)
			enc.Encode(socketMsg{Type: "candidates", Data: resp})

		case "accept_peer":
			var req struct {
				Fingerprint string `json:"fingerprint"`
			}
			if err := json.Unmarshal(msg.Data, &req); err != nil || req.Fingerprint == "" {
				continue
			}
			// Add to the live allowed set so the TLS client trusts it immediately.
			d.allowedKeyFPsMu.Lock()
			d.allowedKeyFPs[req.Fingerprint] = true
			d.allowedKeyFPsMu.Unlock()
			// Persist to peer.keys.
			if cfgDir, err := os.UserConfigDir(); err == nil {
				keysPath := filepath.Join(cfgDir, "darktable", "peer.keys")
				f, ferr := os.OpenFile(keysPath, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
				if ferr == nil {
					fmt.Fprintln(f, req.Fingerprint)
					f.Close()
				}
			}
			// Remove from candidates and update the file.
			d.candidatesMu.Lock()
			var peerURL string
			if c, ok := d.candidatePeers[req.Fingerprint]; ok {
				peerURL = c.URL
				delete(d.candidatePeers, req.Fingerprint)
			}
			remaining := make([]candidatePeer, 0, len(d.candidatePeers))
			for _, c := range d.candidatePeers {
				remaining = append(remaining, c)
			}
			d.candidatesMu.Unlock()
			if cfgDir, err := os.UserConfigDir(); err == nil {
				data, _ := json.Marshal(remaining)
				_ = os.WriteFile(filepath.Join(cfgDir, "darktable", "peer.candidates"), data, 0644)
			}
			log.Printf("[peer] accepted fp=%s  url=%s", req.Fingerprint, peerURL)
			// Immediately attempt a sync now that the peer is trusted.
			if peerURL != "" {
				go d.syncWithPeer(peerURL)
			}
			enc.Encode(socketMsg{Type: "accepted", Data: json.RawMessage(`{"ok":true}`)})

		case "request_sync":
			// Mobile app requests a full re-sync (e.g. after clearing local cache or
			// on periodic background refresh). Re-visits every known peer so any new
			// images are announced and missing previews are re-fetched.
			go func() {
				for _, u := range d.peersToSync() {
					go d.syncWithPeer(u)
				}
			}()

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

// pathMapPath returns the config-dir path for the persisted local→remote map.
func pathMapPath() string {
	if dir := os.Getenv("XDG_CONFIG_HOME"); dir != "" {
		return filepath.Join(dir, "darktable", "peer.pathmap")
	}
	if home, err := os.UserHomeDir(); err == nil {
		return filepath.Join(home, ".config", "darktable", "peer.pathmap")
	}
	return ""
}

// savePathMap persists localToRemote to disk so fetch_proxy / fetch_preview
// survive a daemon restart.
func (d *daemon) markPathMapDirty() {
	d.pathMapMu.Lock()
	d.pathMapDirty = true
	d.pathMapMu.Unlock()
}

func (d *daemon) flushPathMap() {
	p := pathMapPath()
	if p == "" {
		return
	}
	d.localToRemoteMu.RLock()
	data, err := json.Marshal(d.localToRemote)
	d.localToRemoteMu.RUnlock()
	if err == nil {
		_ = os.WriteFile(p, data, 0644)
	}
}

// runPathMapFlusher writes localToRemote to disk at most once every 5 seconds,
// collapsing the O(N²) write amplification from per-download saves.
func (d *daemon) runPathMapFlusher() {
	tick := time.NewTicker(5 * time.Second)
	defer tick.Stop()
	for {
		select {
		case <-d.ctx.Done():
			d.pathMapMu.Lock()
			dirty := d.pathMapDirty
			d.pathMapDirty = false
			d.pathMapMu.Unlock()
			if dirty {
				d.flushPathMap()
			}
			return
		case <-tick.C:
			d.pathMapMu.Lock()
			dirty := d.pathMapDirty
			d.pathMapDirty = false
			d.pathMapMu.Unlock()
			if dirty {
				d.flushPathMap()
			}
		}
	}
}

// loadPathMap restores the localToRemote map saved by a previous session.
func (d *daemon) loadPathMap() {
	p := pathMapPath()
	if p == "" {
		return
	}
	data, err := os.ReadFile(p)
	if err != nil {
		return
	}
	var m map[string]string
	if json.Unmarshal(data, &m) == nil && len(m) > 0 {
		d.localToRemoteMu.Lock()
		for k, v := range m {
			d.localToRemote[k] = v
		}
		d.localToRemoteMu.Unlock()
		log.Printf("[pathmap] restored %d local→remote path entries", len(m))
	}
}

// invalidatePreviewCache removes the JPEG preview files cached beside a raw so
// that the next /preview request regenerates them from the current XMP/mipmap
// instead of serving the pre-edit stale version.
func invalidatePreviewCache(rawPath string) {
	os.Remove(rawPath + ".preview-thumb.jpg")
	os.Remove(rawPath + ".preview-full.jpg")
}
