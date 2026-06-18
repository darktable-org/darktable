package main

import (
	"encoding/json"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"
)

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

		// If we haven't successfully synced with this peer yet, verify it
		// immediately so it enters d.syncedPeers and becomes a fetch target.
		// (Without this, a new peer would wait up to 60 s for the sync ticker.)
		d.syncedPeersMu.Lock()
		alreadySynced := d.syncedPeers[ann.BaseURL]
		d.syncedPeersMu.Unlock()
		if !alreadySynced {
			go d.syncWithPeer(ann.BaseURL)
		}

		log.Printf("[proxy] peer %s announced %d proxies at %s",
			m.ReceivedFrom.ShortString(), len(ann.Paths), ann.BaseURL)

		// Notify the gallery immediately for every announced path, then fetch.
		for _, path := range ann.Paths {
			localPath := d.localDestination(path)
			result, _ := json.Marshal(map[string]string{"path": localPath})
			d.broadcast(socketMsg{Type: "image_imported", Data: result})
		}
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
// Proxy download helpers
// ---------------------------------------------------------------------------

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

// fetchXMPSidecar downloads the XMP sidecar for remotePath from baseURL and
// writes it to localPath+".xmp". Called after proxy download so mobile starts
// edits from the full darktable XMP (preserving history and existing rating)
// rather than from an empty skeleton. Skips silently if the server has no XMP.
func (d *daemon) fetchXMPSidecar(remotePath, localPath, baseURL string) {
	xmpURL := baseURL + "/xmp?path=" + url.QueryEscape(remotePath)
	resp, err := d.httpGet(xmpURL)
	if err != nil || resp.StatusCode != 200 {
		if resp != nil {
			resp.Body.Close()
		}
		return
	}
	data, err := io.ReadAll(resp.Body)
	resp.Body.Close()
	if err != nil || len(data) == 0 {
		return
	}
	dst := localPath + ".xmp"
	if err := os.WriteFile(dst, data, 0644); err != nil {
		log.Printf("[xmp] write sidecar '%s': %v", filepath.Base(localPath), err)
	}
}

// autoFetchProxy downloads a proxy and notifies darktable to import it.
func (d *daemon) autoFetchProxy(remotePath, baseURL string) {
	localPath := d.localDestination(remotePath)

	// On the phone, localPath differs from remotePath (importDir vs desktop path).
	// Remember the mapping so fetch_proxy and fetch_preview can resolve it back.
	if localPath != remotePath {
		d.localToRemoteMu.Lock()
		d.localToRemote[localPath] = remotePath
		d.localToRemoteMu.Unlock()
		d.markPathMapDirty()
	}

	// Fetch the thumbnail JPEG first — it's small and lets the mobile gallery
	// show a preview immediately, even while the larger AVIF is still downloading.
	d.fetchPreviewJPEG(remotePath, localPath, baseURL, "thumb")

	if !d.downloadProxy(remotePath, localPath, baseURL) {
		return
	}

	// Download the XMP sidecar so mobile always starts edits from the full
	// darktable XMP (preserving history stack and existing rating).
	d.fetchXMPSidecar(remotePath, localPath, baseURL)

	d.addToLocalIndex(localPath)

	// Re-broadcast so darktable (desktop) imports the proxy path and the mobile
	// model updates hasProxy = true once the AVIF is locally present.
	result, _ := json.Marshal(map[string]string{"path": localPath})
	d.broadcast(socketMsg{Type: "image_imported", Data: result})
}

// fetchProxyFromPeer handles an explicit "fetch_proxy" command from darktable.
func (d *daemon) fetchProxyFromPeer(localPath string, enc *json.Encoder) {
	// On the phone, the command arrives with the local importDir path.
	// Resolve it back to the peer's canonical path for the HTTP request,
	// but always write the result to the local path.
	requestPath := localPath
	d.localToRemoteMu.RLock()
	if remote, ok := d.localToRemote[localPath]; ok {
		requestPath = remote
	}
	d.localToRemoteMu.RUnlock()

	for _, baseURL := range d.allPeerURLs() {
		fetchURL := baseURL + "/proxy?path=" + url.QueryEscape(requestPath)
		resp, err := d.httpGet(fetchURL)
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
		dst := localPath + ".proxy.avif"
		if err := os.MkdirAll(filepath.Dir(dst), 0755); err == nil {
			if err := os.WriteFile(dst, data, 0644); err == nil {
				log.Printf("[proxy] explicit fetch '%s' from %s (%d KB)",
					filepath.Base(localPath), baseURL, len(data)/1024)
				result, _ := json.Marshal(map[string]string{"path": dst, "status": "ok"})
				enc.Encode(socketMsg{Type: "proxy_fetched", Data: result})
				return
			}
		}
	}
	result, _ := json.Marshal(map[string]string{"path": localPath, "status": "not_found"})
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
	d.manifestMu.Lock()
	if d.manifestHash == "" {
		d.rebuildManifestLocked()
	}
	paths := d.manifestPaths
	d.manifestMu.Unlock()

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
// Proxy HTTP endpoint
// ---------------------------------------------------------------------------

func (d *daemon) serveProxy(w http.ResponseWriter, r *http.Request) {
	canonicalPath := r.URL.Query().Get("path")
	if canonicalPath == "" || strings.Contains(canonicalPath, "..") {
		http.Error(w, "bad request", 400)
		return
	}
	if !d.isAnnouncedPath(canonicalPath) {
		http.Error(w, "not found", 404)
		return
	}
	log.Printf("[http] serve proxy '%s' to %s", filepath.Base(canonicalPath), r.RemoteAddr)
	http.ServeFile(w, r, canonicalPath+".proxy.avif")
}
