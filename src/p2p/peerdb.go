package main

import (
	"database/sql"
	"log"
	"os"
	"path/filepath"
	"time"

	_ "modernc.org/sqlite"
)

// peerDB wraps the persistent SQLite peer registry.
type peerDB struct {
	db *sql.DB
}

// peerRecord mirrors one row of the peers table.
type peerRecord struct {
	URL          string
	PeerID       string
	CreatedAt    time.Time
	LastSeen     time.Time
	FailureCount int
}

// peerDBPath returns the canonical path for peers.db, respecting XDG.
func peerDBPath() string {
	if dir := os.Getenv("XDG_CONFIG_HOME"); dir != "" {
		return filepath.Join(dir, "darktable", "peers.db")
	}
	if home, err := os.UserHomeDir(); err == nil {
		return filepath.Join(home, ".config", "darktable", "peers.db")
	}
	return ""
}

// openPeerDB opens (and creates if necessary) the peer database.
func openPeerDB() (*peerDB, error) {
	path := peerDBPath()
	if path == "" {
		return nil, nil
	}
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return nil, err
	}
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, err
	}
	// Single connection serialises all writes; no concurrent SQLITE_BUSY.
	db.SetMaxOpenConns(1)
	for _, pragma := range []string{
		`PRAGMA journal_mode=WAL`,
		`PRAGMA busy_timeout=5000`,
	} {
		if _, err := db.Exec(pragma); err != nil {
			db.Close()
			return nil, err
		}
	}
	_, err = db.Exec(`CREATE TABLE IF NOT EXISTS peers (
		url           TEXT    PRIMARY KEY,
		peer_id       TEXT    NOT NULL DEFAULT '',
		created_at    INTEGER NOT NULL,
		last_seen     INTEGER NOT NULL,
		failure_count INTEGER NOT NULL DEFAULT 0
	)`)
	if err != nil {
		db.Close()
		return nil, err
	}
	return &peerDB{db: db}, nil
}

// touch inserts the peer if unknown; if already known, updates last_seen only.
// Does NOT reset failure_count — use markSuccess for that.
func (p *peerDB) touch(peerURL, peerID string) {
	if p == nil || peerURL == "" {
		return
	}
	now := time.Now().Unix()
	_, err := p.db.Exec(`
		INSERT INTO peers (url, peer_id, created_at, last_seen, failure_count)
		VALUES (?, ?, ?, ?, 0)
		ON CONFLICT(url) DO UPDATE SET
			peer_id   = CASE WHEN excluded.peer_id != '' THEN excluded.peer_id ELSE peer_id END,
			last_seen = excluded.last_seen`,
		peerURL, peerID, now, now)
	if err != nil {
		log.Printf("[peerdb] touch %s: %v", peerURL, err)
	}
}

// markSuccess records a successful connection: updates peer_id, last_seen,
// and resets failure_count to 0.
func (p *peerDB) markSuccess(peerURL, peerID string) {
	if p == nil || peerURL == "" {
		return
	}
	now := time.Now().Unix()
	_, err := p.db.Exec(`
		INSERT INTO peers (url, peer_id, created_at, last_seen, failure_count)
		VALUES (?, ?, ?, ?, 0)
		ON CONFLICT(url) DO UPDATE SET
			peer_id       = CASE WHEN excluded.peer_id != '' THEN excluded.peer_id ELSE peer_id END,
			last_seen     = excluded.last_seen,
			failure_count = 0`,
		peerURL, peerID, now, now)
	if err != nil {
		log.Printf("[peerdb] markSuccess %s: %v", peerURL, err)
	}
}

// markFailure increments failure_count for an existing peer.
// If the peer isn't in the DB yet it is inserted with failure_count=1.
func (p *peerDB) markFailure(peerURL string) {
	if p == nil || peerURL == "" {
		return
	}
	now := time.Now().Unix()
	_, err := p.db.Exec(`
		INSERT INTO peers (url, peer_id, created_at, last_seen, failure_count)
		VALUES (?, '', ?, ?, 1)
		ON CONFLICT(url) DO UPDATE SET
			failure_count = failure_count + 1`,
		peerURL, now, now)
	if err != nil {
		log.Printf("[peerdb] markFailure %s: %v", peerURL, err)
	}
}

// allURLs returns every known peer URL ordered by most-recently-seen first.
func (p *peerDB) allURLs() []string {
	if p == nil {
		return nil
	}
	rows, err := p.db.Query(`SELECT url FROM peers ORDER BY last_seen DESC`)
	if err != nil {
		log.Printf("[peerdb] allURLs: %v", err)
		return nil
	}
	defer rows.Close()
	var urls []string
	for rows.Next() {
		var u string
		if err := rows.Scan(&u); err == nil {
			urls = append(urls, u)
		}
	}
	return urls
}

func (p *peerDB) close() {
	if p != nil {
		p.db.Close()
	}
}
