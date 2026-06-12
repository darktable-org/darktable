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
	"golang.org/x/crypto/pbkdf2"
	gocrypto "crypto"
	_ "crypto/sha256"
)

const (
	xmpTopic      = "darktable/xmp/v1"
	proxyTopic    = "darktable/proxy-announce/v1"
	mdnsServiceTag = "darktable-p2p"
	pbkdf2Iter    = 100000
	keyLen        = 32
	proxyHTTPPort = 17842 // default; peers learn actual port via announce
)

// daemon holds all P2P state.
type daemon struct {
	ctx        context.Context
	cancel     context.CancelFunc
	socketPath string
	proxyDir   string

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
	Path     string `json:"path"`    // canonical raw path (relative to library root)
	Content  string `json:"content"` // full XMP text
	Mtime    int64  `json:"mtime"`   // unix ns
}

// Proxy announce published on proxyTopic.
type proxyAnnounce struct {
	SenderID string `json:"sender"`
	BaseURL  string `json:"base_url"` // e.g. "http://192.168.1.5:17842"
	Paths    []string `json:"paths"`  // canonical raw paths that have a proxy sidecar
}

func deriveKey(passphrase string) (crypto.PrivKey, error) {
	salt := sha256.Sum256([]byte("darktable-p2p-v1"))
	kb := pbkdf2.Key([]byte(passphrase), salt[:], pbkdf2Iter, keyLen, gocrypto.SHA256.New)
	priv, _, err := crypto.GenerateEd25519Key(strings.NewReader(string(kb)))
	return priv, err
}

func newDaemon(ctx context.Context, socketPath, passphrase, proxyDir string) (*daemon, error) {
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
		ctx:        dctx,
		cancel:     cancel,
		socketPath: socketPath,
		proxyDir:   proxyDir,
		host:       h,
		ps:         ps,
		xmpSub:     xmpSub,
		xmpTop:     xmpTop,
		proxySub:   proxySub,
		proxyTop:   proxyTop,
		peers:      make(map[peer.ID]string),
		xmpSeen:    make(map[string]time.Time),
	}

	// mDNS discovery
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
	log.Printf("discovered peer %s", pi.ID)
	if err := d.host.Connect(d.ctx, pi); err != nil {
		log.Printf("connect to %s: %v", pi.ID, err)
	}
}

func (d *daemon) run() error {
	// Start proxy HTTP server
	if err := d.startProxyHTTP(); err != nil {
		return err
	}

	// Start Unix socket server
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
			// Write XMP to disk alongside the raw
			if err := writeXMP(x.Path, x.Content); err != nil {
				log.Printf("write XMP %s: %v", x.Path, err)
			} else {
				log.Printf("synced XMP %s from %s", x.Path, m.ReceivedFrom)
			}
		} else {
			d.xmpMu.Unlock()
		}
	}
}

// subscribeProxy handles peer proxy announce messages.
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
	}
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
			// strip the .proxy.avif suffix to get canonical raw path
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

func (d *daemon) fetchProxyFromPeer(canonicalPath string, enc *json.Encoder) {
	d.peersMu.RLock()
	defer d.peersMu.RUnlock()
	for _, baseURL := range d.peers {
		url := baseURL + "/proxy?path=" + canonicalPath
		resp, err := http.Get(url)
		if err != nil || resp.StatusCode != 200 {
			continue
		}
		data, err := io.ReadAll(resp.Body)
		resp.Body.Close()
		if err != nil {
			continue
		}
		// Write to local path
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
		// Try any free port
		ln, err = net.Listen("tcp", ":0")
		if err != nil {
			return fmt.Errorf("proxy http listen: %w", err)
		}
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/proxy", d.serveProxy)
	d.httpSrv = &http.Server{Handler: mux}
	go d.httpSrv.Serve(ln)
	log.Printf("proxy HTTP on %s", ln.Addr())
	return nil
}

func (d *daemon) localProxyURL() string {
	if d.httpSrv == nil {
		return ""
	}
	// Announce first non-loopback address
	addrs := d.host.Addrs()
	for _, ma := range addrs {
		s := ma.String()
		if strings.HasPrefix(s, "/ip4/127.") {
			continue
		}
		if strings.Contains(s, "/ip4/") {
			// extract IP
			parts := strings.Split(s, "/")
			for i, p := range parts {
				if p == "ip4" && i+1 < len(parts) {
					return fmt.Sprintf("http://%s:%d", parts[i+1], proxyHTTPPort)
				}
			}
		}
	}
	return fmt.Sprintf("http://127.0.0.1:%d", proxyHTTPPort)
}

func (d *daemon) serveProxy(w http.ResponseWriter, r *http.Request) {
	canonicalPath := r.URL.Query().Get("path")
	if canonicalPath == "" {
		http.Error(w, "missing path", 400)
		return
	}
	proxyPath := canonicalPath + ".proxy.avif"
	// Basic path traversal guard
	if strings.Contains(proxyPath, "..") {
		http.Error(w, "forbidden", 403)
		return
	}
	http.ServeFile(w, r, proxyPath)
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
