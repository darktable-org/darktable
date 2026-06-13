package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
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
	_ "crypto/sha256"
	"net/url"

	"github.com/huin/goupnp/dcps/internetgateway1"
	"github.com/huin/goupnp/dcps/internetgateway2"
	natpmp "github.com/jackpal/go-nat-pmp"
)

const (
	xmpTopic       = "darktable/xmp/v1"
	proxyTopic     = "darktable/proxy-announce/v1"
	mdnsServiceTag = "darktable-p2p"
	pbkdf2Iter     = 100000
	keyLen         = 32
	proxyHTTPPort  = 17842
)

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

	host     host.Host
	ps       *pubsub.PubSub
	xmpSub   *pubsub.Subscription
	xmpTop   *pubsub.Topic
	proxySub *pubsub.Subscription
	proxyTop *pubsub.Topic

	// externalURL is set when UPnP/NAT-PMP port mapping succeeds.
	externalURL string

	// pdb is the persistent peer registry; nil if the DB could not be opened.
	pdb *peerDB

	// known peers: peerID → http base URL
	peersMu sync.RWMutex
	peers   map[peer.ID]string

	// syncedPeers tracks HTTP base URLs we have already synced with so peer
	// gossip and reverse connections don't create infinite loops.
	syncedPeersMu sync.Mutex
	syncedPeers   map[string]bool

	// xmp dedup: canonical_path → last mtime synced
	xmpMu   sync.Mutex
	xmpSeen map[string]time.Time

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

// XMP sync envelope published on xmpTopic.
type xmpMsg struct {
	SenderID    string `json:"sender"`
	Path        string `json:"path"`         // sender's canonical raw path
	Filename    string `json:"filename"`     // basename for cross-machine matching
	CaptureDate string `json:"capture_date"` // exif:DateTimeOriginal for disambiguation
	Content     string `json:"content"`      // full XMP text
	Mtime       int64  `json:"mtime"`        // unix ns
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
		// Index raw files and their proxy placeholders.
		// A file is "known" if the raw exists OR if only the proxy sidecar exists.
		name := filepath.Base(p)
		if strings.HasSuffix(name, ".proxy.avif") || strings.HasSuffix(name, ".xmp") {
			return nil // skip sidecars
		}
		canonical := p
		idx[name] = append(idx[name], canonical)
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

func newDaemon(ctx context.Context, socketPath, passphrase, proxyDir, importDir string, staticPeers []string) (*daemon, error) {
	priv, err := deriveKey(passphrase)
	if err != nil {
		return nil, fmt.Errorf("key derivation: %w", err)
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
		ctx:         dctx,
		cancel:      cancel,
		socketPath:  socketPath,
		proxyDir:    proxyDir,
		importDir:   importDir,
		staticPeers: staticPeers,
		host:        h,
		ps:          ps,
		xmpSub:      xmpSub,
		xmpTop:      xmpTop,
		proxySub:    proxySub,
		proxyTop:    proxyTop,
		peers:       make(map[peer.ID]string),
		syncedPeers: make(map[string]bool),
		xmpSeen:     make(map[string]time.Time),
		localIndex:       make(map[string][]string),
		announcedProxies: make(map[string]struct{}),
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
			d.externalURL = fmt.Sprintf("http://%s:%d", extIP, proxyHTTPPort)
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
			d.externalURL = fmt.Sprintf("http://%s:%d", extIP, proxyHTTPPort)
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
			x.Filename    = filepath.Base(x.Path)
			x.CaptureDate = xmpCaptureDate(x.Content)

			// Dedup: darktable may send the same XMP twice (debounce + sidecar job).
			t := time.Unix(0, x.Mtime)
			d.xmpMu.Lock()
			dup := !t.After(d.xmpSeen[x.Path])
			if !dup {
				d.xmpSeen[x.Path] = t
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
			go d.pushXMPToPeers(x)

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
	}
}

func (d *daemon) subscribeXMP() {
	for {
		m, err := d.xmpSub.Next(d.ctx)
		if err != nil {
			return
		}
		if m.ReceivedFrom == d.host.ID() {
			continue
		}
		var x xmpMsg
		if err := json.Unmarshal(m.Data, &x); err != nil {
			continue
		}
		d.applyInboundXMP(x, m.ReceivedFrom.ShortString())
	}
}

var xmpPushClient = &http.Client{Timeout: 10 * time.Second}

// pushXMPToPeers sends x directly via HTTP POST to every known reachable peer.
// This ensures delivery even when gossipsub has not yet established peer
// connections (peers=0).
func (d *daemon) pushXMPToPeers(x xmpMsg) {
	myURL := d.localProxyURL()
	myLANURL := fmt.Sprintf("http://%s:%d", d.localIP(), proxyHTTPPort)
	seen := make(map[string]bool)
	var targets []string
	collect := func(u string) {
		if u != "" && u != myURL && u != myLANURL && !seen[u] {
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
			resp, err := xmpPushClient.Post(baseURL+"/xmp", "application/json", bytes.NewReader(b))
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
		myLANURL := fmt.Sprintf("http://%s:%d", d.localIP(), proxyHTTPPort)
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

	proxyPath := localPath + ".proxy.avif"
	result, _ := json.Marshal(map[string]string{"path": proxyPath})
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
	resp, err := http.Get(url)
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

// autoFetchProxy downloads a proxy and notifies darktable to import it.
func (d *daemon) autoFetchProxy(remotePath, baseURL string) {
	localPath := d.localDestination(remotePath)

	if !d.downloadProxy(remotePath, localPath, baseURL) {
		return
	}

	d.addToLocalIndex(localPath)

	// Tell darktable about the proxy file that actually exists on disk, not
	// the raw path (which may be absent or on a slow remote filesystem).
	proxyPath := localPath + ".proxy.avif"
	result, _ := json.Marshal(map[string]string{"path": proxyPath})
	d.broadcast(socketMsg{Type: "image_imported", Data: result})
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
		resp, err := http.Get(url)
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
	if baseURL == myURL || baseURL == fmt.Sprintf("http://%s:%d", d.localIP(), proxyHTTPPort) {
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

	resp, err := http.Get(manifestURL)
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

	// Download any proxy the peer has that we don't.
	missing := 0
	for _, path := range m.Paths {
		localPath := d.localDestination(path)
		if _, err := os.Stat(localPath + ".proxy.avif"); err == nil {
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
// HTTP server
// ---------------------------------------------------------------------------

func (d *daemon) startProxyHTTP() error {
	ln, err := net.Listen("tcp", fmt.Sprintf(":%d", proxyHTTPPort))
	if err != nil {
		ln, err = net.Listen("tcp", ":0")
		if err != nil {
			return fmt.Errorf("proxy http listen: %w", err)
		}
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/proxy", d.serveProxy)
	mux.HandleFunc("/manifest", d.serveManifest)
	mux.HandleFunc("/xmp", d.serveXMP)
	d.httpSrv = &http.Server{Handler: mux}
	go d.httpSrv.Serve(ln)
	log.Printf("[http] proxy server on %s", ln.Addr())
	return nil
}

func (d *daemon) localIP() string {
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
	return fmt.Sprintf("http://%s:%d", d.localIP(), proxyHTTPPort)
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
