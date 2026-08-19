package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"regexp"
	"strings"
	"time"
)

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
// XMP subscription
// ---------------------------------------------------------------------------

// applyInboundXMP processes an inbound XMP update received from gossipsub or a
// direct HTTP push.  from is a short identifier used only for logging.
func (d *daemon) applyInboundXMP(x xmpMsg, from string) {
	d.xmpMu.Lock()
	last := d.xmpSeen[x.Path]
	t := time.UnixMilli(x.Mtime)
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
	// If the incoming XMP has no darktable history (thin mobile push) but the
	// local file does, merge only the rating/colorlabels so the processing
	// pipeline is not overwritten.
	contentToWrite := x.Content
	if !strings.Contains(x.Content, "darktable:history") {
		if existing, err := os.ReadFile(localPath + ".xmp"); err == nil &&
			strings.Contains(string(existing), "darktable:history") {
			contentToWrite = mergeXMPMeta(string(existing), x.Content)
		}
	}
	if err := writeXMP(localPath, contentToWrite); err != nil {
		log.Printf("[xmp] write '%s': %v", localPath, err)
	} else {
		// Invalidate preview caches so the next fetch regenerates them from
		// the updated XMP rather than serving the pre-edit stale files.
		invalidatePreviewCache(localPath)
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
	var targets []string
	for _, u := range d.allPeerURLs() {
		if u != "" && u != myURL && u != myLANURL && u != excludeURL {
			targets = append(targets, u)
		}
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
// XMP HTTP endpoint
// ---------------------------------------------------------------------------

func (d *daemon) serveXMP(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		// GET /xmp?path=<canonical> — serve the XMP sidecar for an announced path.
		canonicalPath := r.URL.Query().Get("path")
		if canonicalPath == "" || strings.Contains(canonicalPath, "..") {
			http.Error(w, "bad request", 400)
			return
		}
		if !d.isAnnouncedPath(canonicalPath) {
			http.Error(w, "not found", 404)
			return
		}
		xmpPath := canonicalPath + ".xmp"
		data, err := os.ReadFile(xmpPath)
		if err != nil {
			http.Error(w, "not found", 404)
			return
		}
		w.Header().Set("Content-Type", "application/xml")
		w.Write(data)
		return
	}
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

// ---------------------------------------------------------------------------
// XMP merge helpers
// ---------------------------------------------------------------------------

// mergeXMPMeta returns dst with xmp:Rating and darktable:colorlabels patched
// from src. Used when src is a thin mobile-edit push (no darktable history)
// and dst is the full desktop XMP so the history stack is not overwritten.
func mergeXMPMeta(dst, src string) string {
	reAttr := regexp.MustCompile(`\bxmp:Rating\s*=\s*"[^"]*"`)
	reElem := regexp.MustCompile(`<xmp:Rating>\s*(\d+)\s*</xmp:Rating>`)
	reColor := regexp.MustCompile(`(?s)<darktable:colorlabels>.*?</darktable:colorlabels>`)

	result := dst

	// Extract the rating value from src — prefer attribute form, fall back to element.
	ratingVal := ""
	if m := reAttr.FindString(src); m != "" {
		// m is e.g. `xmp:Rating="3"` — extract just the number
		inner := regexp.MustCompile(`"([^"]*)"`)
		if sub := inner.FindStringSubmatch(m); len(sub) == 2 {
			ratingVal = sub[1]
		}
	} else if m := reElem.FindStringSubmatch(src); len(m) == 2 {
		ratingVal = m[1]
	}

	if ratingVal != "" {
		// Apply to dst: update attribute form if present, else element form, else insert.
		if reAttr.MatchString(result) {
			result = reAttr.ReplaceAllLiteralString(result, `xmp:Rating="`+ratingVal+`"`)
		} else if reElem.MatchString(result) {
			result = reElem.ReplaceAllLiteralString(result, "<xmp:Rating>"+ratingVal+"</xmp:Rating>")
		} else {
			pos := strings.Index(result, "</rdf:Description>")
			if pos >= 0 {
				result = result[:pos] + "  <xmp:Rating>" + ratingVal + "</xmp:Rating>\n" + result[pos:]
			}
		}
	}

	// Patch colorlabels block.
	if colorSrc := reColor.FindString(src); colorSrc != "" {
		if reColor.MatchString(result) {
			result = reColor.ReplaceAllLiteralString(result, colorSrc)
		}
	}
	return result
}

// extractTagValue returns the text content of an XML element like <Tag>value</Tag>.
func extractTagValue(elem string) string {
	re := regexp.MustCompile(`>([^<]*)<`)
	if m := re.FindStringSubmatch(elem); len(m) == 2 {
		return strings.TrimSpace(m[1])
	}
	return ""
}
