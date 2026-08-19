package main

import (
	"encoding/json"
	"regexp"
	"time"
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

// candidatePeer is a peer whose TLS fingerprint was seen but is not yet in the
// allowed list.  Stored so the preferences UI can offer the user an Accept button.
type candidatePeer struct {
	URL         string    `json:"url"`
	Fingerprint string    `json:"fingerprint"`
	SeenAt      time.Time `json:"seen_at"`
}

// eventSub is a darktable client subscribed to push events.
type eventSub struct {
	ch chan socketMsg
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
