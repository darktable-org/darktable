package main

import (
	"encoding/json"
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"
)

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
// Path map persistence (local→remote, survives daemon restarts)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------------

func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

// invalidatePreviewCache removes the JPEG preview files cached beside a raw so
// that the next /preview request regenerates them from the current XMP/mipmap
// instead of serving the pre-edit stale version.
func invalidatePreviewCache(rawPath string) {
	os.Remove(rawPath + ".preview-thumb.jpg")
	os.Remove(rawPath + ".preview-full.jpg")
}
