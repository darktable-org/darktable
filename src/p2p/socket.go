package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"path/filepath"
	"time"
)

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
