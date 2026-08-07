package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	_ "modernc.org/sqlite"
)

// ---------------------------------------------------------------------------
// Preview JPEG fetching (mobile side)
// ---------------------------------------------------------------------------

// fetchPreviewFromPeers tries every known peer until it obtains a JPEG preview
// for canonicalPath at the requested size, then caches it locally.
// Retries up to 3 additional times with a short delay so that a preview that
// is still being generated on the desktop side (common right after an edit)
// will eventually arrive without manual intervention.
func (d *daemon) fetchPreviewFromPeers(canonicalPath, size string) {
	localPath := d.localDestination(canonicalPath)

	log.Printf("[preview] fetching '%s' size=%s from peers",
		filepath.Base(canonicalPath), size)

	// On mobile, canonicalPath is the local import-dir path which differs from
	// the desktop's original path.  Use localToRemote to get the path the
	// desktop knows about (same logic fetchProxyFromPeer uses).
	remotePath := canonicalPath
	d.localToRemoteMu.RLock()
	if r, ok := d.localToRemote[canonicalPath]; ok {
		remotePath = r
	}
	d.localToRemoteMu.RUnlock()

	dst := localPath + ".preview-" + size + ".jpg"

	const maxAttempts = 8
	const retryDelay = 4 * time.Second

	for attempt := 0; attempt < maxAttempts; attempt++ {
		if attempt > 0 {
			time.Sleep(retryDelay)
		}
		for _, baseURL := range d.allPeerURLs() {
			d.fetchPreviewJPEG(remotePath, localPath, baseURL, size)
			if fileExists(dst) {
				return
			}
		}
	}
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
		log.Printf("[preview] fetch %s from %s: %v", filepath.Base(remotePath), baseURL, err)
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusNotModified {
		return // our copy is still current
	}
	if resp.StatusCode != http.StatusOK {
		log.Printf("[preview] fetch %s from %s: HTTP %d", filepath.Base(remotePath), baseURL, resp.StatusCode)
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

// ---------------------------------------------------------------------------
// Preview JPEG endpoint (desktop side)
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
	if !d.isAnnouncedPath(rawPath) {
		http.Error(w, "not found", http.StatusNotFound)
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
			// Thumbnails: mipmap JPEG lacks an ICC marker. Inject sRGB so mobile
			// apps render consistently with the full preview (also sRGB). Using
			// the desktop display ICC here would make the gallery look vivid but
			// sharing would appear muted by comparison in non-colour-managed apps.
			mipmapData = injectICCProfile(mipmapData, compactSRGBICC())
			if err := os.WriteFile(cachePath, mipmapData, 0644); err == nil {
				log.Printf("[preview] mipmap+sRGB-ICC → cache '%s' size=%s (%d KB)",
					filepath.Base(rawPath), sizeParam, len(mipmapData)/1024)
			}
		} else {
			// Skip darktable-cli when the GUI is running — it would open its own
			// database connection and race with the GUI's writers causing
			// SQLITE_BUSY assertions.  The GUI regenerates its mipmap cache on
			// its own after an XMP reload; mobile can retry in a few seconds.
			d.subsMu.Lock()
			darktableRunning := len(d.subs) > 0
			d.subsMu.Unlock()
			if darktableRunning {
				// Ask darktable to generate the mipmap in the background.
				// It will write the file to its mipmap cache; the retry loop
				// in fetchPreviewFromPeers will pick it up a few seconds later.
				pathJSON, _ := json.Marshal(rawPath)
				d.broadcast(socketMsg{
					Type: "generate_preview",
					Data: json.RawMessage(`{"path":` + string(pathJSON) + `}`),
				})
				log.Printf("[preview] requested mipmap generation for '%s' from darktable",
					filepath.Base(rawPath))
				http.Error(w, "preview regenerating", http.StatusServiceUnavailable)
				return
			}
			if _, err := exportWithDarktableCLI(rawPath, cachePath, maxDim); err != nil {
				http.Error(w, "preview not available", http.StatusNotFound)
				return
			}
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

// ---------------------------------------------------------------------------
// Mipmap lookup
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// darktable-cli export
// ---------------------------------------------------------------------------

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
			"-Exif:All", // all Exif tags (camera make/model/lens/exposure/GPS…)
			"-IPTC:All", // keywords, caption, copyright
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

// ---------------------------------------------------------------------------
// ICC profile helpers
// ---------------------------------------------------------------------------

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
