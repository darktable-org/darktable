package main

import (
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
)

const (
	xmpTopic       = "darktable/xmp/v1"
	proxyTopic     = "darktable/proxy-announce/v1"
	mdnsServiceTag = "darktable-p2p"
	pbkdf2Iter     = 100000
	keyLen         = 32
	proxyHTTPPort  = 17842
)

// eventSub is a darktable client that subscribed to push events.
type eventSub struct {
	ch chan socketMsg
}

// daemon holds all P2P state.
type daemon struct {
	ctx        context.Context
	cancel     context.CancelFunc
	socketPath string
	proxyDir   string
	staticPeers []string // http base URLs from --peers flag

	host    host.Host
	ps      *pubsub.PubSub
	xmpSub  *pubsub.Subscription
	xmpTop  *pubsub.Topic
	proxySub *pubsub.Subscription
	proxyTop *pubsub.Topic

	// known peers and their proxy HTTP base URLs
	peersMu  sync.RWMutex
	peers    map[peer.ID]string // peerID → "http://addr:port"

	// xmp file store: canonical_path → last_mtime we synced
	xmpMu   sync.Mutex
	xmpSeen map[string]time.Time

	// darktable clients subscribed to push events
	subsMu sync.Mutex
	subs   []*eventSub

	httpSrv *http.Server
	ln      net.Listener
}

// Message types sent over the socket to/from darktable.
type socketMsg struct {
	Type string          `json:"type"`
	Data json.RawMessage `json:"data,omitempty"`
}

// XMP sync envelope published on xmpTopic.
type xmpMsg struct {
	SenderID string `json:"sender"`
	Path     string `json:"path"`    // canonical raw path
	Content  string `json:"content"` // full XMP text
	Mtime    int64  `json:"mtime"`   // unix ns
}

// Proxy announce published on proxyTopic.
type proxyAnnounce struct {
	SenderID string   `json:"sender"`
	BaseURL  string   `json:"base_url"`
	Paths    []string `json:"paths"`
}

// manifestResp is returned by GET /manifest.
type manifestResp struct {
	PeerID     string   `json:"peer_id"`
	LibP2PIP   string   `json:"libp2p_ip"`
	LibP2PPort int      `json:"libp2p_port"`
	BaseURL    string   `json:"base_url"`
	Paths      []string `json:"paths"`
}

func deriveKey(passphrase string) (crypto.PrivKey, error) {
	salt := sha256.Sum256([]byte("darktable-p2p-v1"))
	kb := pbkdf2.Key([]byte(passphrase), salt[:], pbkdf2Iter, keyLen, gocrypto.SHA256.New)
	priv, _, err := crypto.GenerateEd25519Key(strings.NewReader(string(kb)))
	return priv, err
}

func newDaemon(ctx context.Context, socketPath, passphrase, proxyDir string, staticPeers []string) (*daemon, error) {
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
		ctx:         dctx,
		cancel:      cancel,
		socketPath:  socketPath,
		proxyDir:    proxyDir,
		staticPeers: staticPeers,
		host:        h,
		ps:          ps,
		xmpSub:      xmpSub,
		xmpTop:      xmpTop,
		proxySub:    proxySub,
		proxyTop:    proxyTop,
		peers:       make(map[peer.ID]string),
		xmpSeen:     make(map[string]time.Time),
	}

	// mDNS discovery (best-effort; fails gracefully on APs that block multicast)
	svc := mdns.NewMdnsService(h, mdnsServiceTag, d)
	if err := svc.Start(); err != nil {
		log.Printf("mDNS start warning: %v", err)
	}

	return d, nil
}

// HandlePeerFound implements mdns.Notifee.
func (d *daemon) HandlePeerFound(pi peer.AddrInfo) {
	if pi.ID == d.host.ID() {
		return
	}
	log.Printf("mDNS discovered peer %s", pi.ID)
	if err := d.host.Connect(d.ctx, pi); err != nil {
		log.Printf("connect to %s: %v", pi.ID, err)
	}
}

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

	log.Printf("P2P daemon ready. PeerID=%s socket=%s", d.host.ID(), d.socketPath)

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

// broadcast sends a push event to all subscribed darktable clients.
func (d *daemon) broadcast(msg socketMsg) {
	d.subsMu.Lock()
	defer d.subsMu.Unlock()
	for _, s := range d.subs {
		select {
		case s.ch <- msg:
		default: // drop if client is slow
		}
	}
}

// handleEventSub handles a darktable "subscribe_events" connection.
// It blocks until the connection is broken or the daemon shuts down.
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

// handleConn handles one darktable connection (JSON-lines).
func (d *daemon) handleConn(conn net.Conn) {
	defer conn.Close()
	dec := json.NewDecoder(conn)
	enc := json.NewEncoder(conn)

	for {
		var msg socketMsg
		if err := dec.Decode(&msg); err != nil {
			if err != io.EOF {
				log.Printf("socket decode: %v", err)
			}
			return
		}

		switch msg.Type {
		case "subscribe_events":
			// This connection is now exclusively for push events.
			d.handleEventSub(enc)
			return

		case "xmp_push":
			var x xmpMsg
			if err := json.Unmarshal(msg.Data, &x); err != nil {
				continue
			}
			x.SenderID = d.host.ID().String()
			if b, err := json.Marshal(x); err == nil {
				d.xmpTop.Publish(d.ctx, b)
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

// subscribeXMP handles incoming XMP messages from peers.
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
		d.xmpMu.Lock()
		last := d.xmpSeen[x.Path]
		t := time.Unix(0, x.Mtime)
		if t.After(last) {
			d.xmpSeen[x.Path] = t
			d.xmpMu.Unlock()
			if err := writeXMP(x.Path, x.Content); err != nil {
				log.Printf("write XMP %s: %v", x.Path, err)
			} else {
				log.Printf("synced XMP %s from %s", x.Path, m.ReceivedFrom)
				result, _ := json.Marshal(map[string]string{"path": x.Path})
				d.broadcast(socketMsg{Type: "xmp_updated", Data: result})
			}
		} else {
			d.xmpMu.Unlock()
		}
	}
}

// subscribeProxy handles peer proxy announce messages and auto-fetches missing proxies.
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
		log.Printf("peer %s has %d proxies at %s", m.ReceivedFrom, len(ann.Paths), ann.BaseURL)

		// Auto-fetch proxies we don't have yet.
		for _, path := range ann.Paths {
			if _, err := os.Stat(path + ".proxy.avif"); err == nil {
				continue
			}
			go d.autoFetchProxy(path, ann.BaseURL)
		}
	}
}

// autoFetchProxy downloads a proxy from baseURL and notifies darktable.
func (d *daemon) autoFetchProxy(canonicalPath, baseURL string) {
	url := baseURL + "/proxy?path=" + canonicalPath
	resp, err := http.Get(url)
	if err != nil {
		log.Printf("[autofetch] GET %s: %v", url, err)
		return
	}
	if resp.StatusCode != 200 {
		resp.Body.Close()
		return
	}
	data, err := io.ReadAll(resp.Body)
	resp.Body.Close()
	if err != nil {
		return
	}

	dst := canonicalPath + ".proxy.avif"
	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		return
	}
	if err := os.WriteFile(dst, data, 0644); err != nil {
		log.Printf("[autofetch] write %s: %v", dst, err)
		return
	}
	log.Printf("[autofetch] fetched proxy for %s (%d KB)", canonicalPath, len(data)/1024)

	result, _ := json.Marshal(map[string]string{"path": canonicalPath})
	d.broadcast(socketMsg{Type: "image_imported", Data: result})
}

// announceProxies periodically broadcasts which proxy files we have.
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
	}
}

// syncStaticPeers polls configured static peers every 60 seconds.
func (d *daemon) syncStaticPeers() {
	// Brief delay to let the libp2p host fully start.
	time.Sleep(2 * time.Second)
	for _, u := range d.staticPeers {
		go d.syncWithPeer(u)
	}

	tick := time.NewTicker(60 * time.Second)
	defer tick.Stop()
	for {
		select {
		case <-d.ctx.Done():
			return
		case <-tick.C:
			for _, u := range d.staticPeers {
				go d.syncWithPeer(u)
			}
		}
	}
}

// syncWithPeer fetches the manifest from a static peer, connects via libp2p,
// and downloads any missing proxy files.
func (d *daemon) syncWithPeer(baseURL string) {
	resp, err := http.Get(baseURL + "/manifest")
	if err != nil {
		log.Printf("[peer] manifest from %s: %v", baseURL, err)
		return
	}
	if resp.StatusCode != 200 {
		resp.Body.Close()
		log.Printf("[peer] manifest from %s: HTTP %d (peer may be running an older daemon)", baseURL, resp.StatusCode)
		return
	}
	var m manifestResp
	if err := json.NewDecoder(resp.Body).Decode(&m); err != nil {
		resp.Body.Close()
		log.Printf("[peer] manifest decode from %s: %v", baseURL, err)
		return
	}
	resp.Body.Close()

	log.Printf("[peer] %s: peerID=%s has %d proxies", baseURL, m.PeerID, len(m.Paths))

	// Connect via libp2p so gossipsub (XMP sync) works.
	if m.PeerID != "" && m.LibP2PIP != "" && m.LibP2PPort > 0 {
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
				log.Printf("[peer] libp2p connected to %s (%s)", m.PeerID, maStr)
			}
		}
	}

	// Auto-fetch missing proxies.
	for _, path := range m.Paths {
		if _, err := os.Stat(path + ".proxy.avif"); err == nil {
			continue
		}
		go d.autoFetchProxy(path, baseURL)
	}
}

func (d *daemon) fetchProxyFromPeer(canonicalPath string, enc *json.Encoder) {
	d.peersMu.RLock()
	defer d.peersMu.RUnlock()
	for _, baseURL := range d.peers {
		url := baseURL + "/proxy?path=" + canonicalPath
		resp, err := http.Get(url)
		if err != nil || resp.StatusCode != 200 {
			if resp != nil {
				resp.Body.Close()
			}
			continue
		}
		data, err := io.ReadAll(resp.Body)
		resp.Body.Close()
		if err != nil {
			continue
		}
		dst := canonicalPath + ".proxy.avif"
		if err := os.MkdirAll(filepath.Dir(dst), 0755); err == nil {
			os.WriteFile(dst, data, 0644)
		}
		result, _ := json.Marshal(map[string]string{"path": dst, "status": "ok"})
		enc.Encode(socketMsg{Type: "proxy_fetched", Data: result})
		log.Printf("fetched proxy for %s from %s", canonicalPath, baseURL)
		return
	}
	result, _ := json.Marshal(map[string]string{"path": canonicalPath, "status": "not_found"})
	enc.Encode(socketMsg{Type: "proxy_fetched", Data: result})
}

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
	d.httpSrv = &http.Server{Handler: mux}
	go d.httpSrv.Serve(ln)
	log.Printf("proxy HTTP on %s", ln.Addr())
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
	return fmt.Sprintf("http://%s:%d", d.localIP(), proxyHTTPPort)
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
	http.ServeFile(w, r, proxyPath)
}

func (d *daemon) serveManifest(w http.ResponseWriter, r *http.Request) {
	var paths []string
	if d.proxyDir != "" {
		filepath.Walk(d.proxyDir, func(p string, fi os.FileInfo, err error) error {
			if err != nil || fi.IsDir() {
				return nil
			}
			if strings.HasSuffix(p, ".proxy.avif") {
				paths = append(paths, strings.TrimSuffix(p, ".proxy.avif"))
			}
			return nil
		})
	}

	resp := manifestResp{
		PeerID:     d.host.ID().String(),
		LibP2PIP:   d.localIP(),
		LibP2PPort: d.localLibP2PPort(),
		BaseURL:    d.localProxyURL(),
		Paths:      paths,
	}
	if resp.Paths == nil {
		resp.Paths = []string{}
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func (d *daemon) close() {
	d.cancel()
	if d.httpSrv != nil {
		d.httpSrv.Close()
	}
	if d.ln != nil {
		d.ln.Close()
	}
	d.host.Close()
}

func writeXMP(rawPath, content string) error {
	xmpPath := rawPath + ".xmp"
	tmp := xmpPath + ".tmp"
	if err := os.WriteFile(tmp, []byte(content), 0644); err != nil {
		return err
	}
	return os.Rename(tmp, xmpPath)
}
