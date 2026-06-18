package main

import (
	"context"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
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
