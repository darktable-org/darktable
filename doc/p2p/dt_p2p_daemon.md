# dt-p2p-daemon

`dt-p2p-daemon` is a Go process that synchronises darktable metadata and media between a desktop darktable installation and one or more mobile phones running the companion app. It runs as a background process started by darktable at launch (when a sync passphrase is configured) and exits when darktable exits.

---

## Table of contents

1. [Purpose and scope](#purpose-and-scope)
2. [Architecture overview](#architecture-overview)
3. [Startup and configuration](#startup-and-configuration)
4. [Identity and security](#identity-and-security)
5. [Peer discovery](#peer-discovery)
6. [Peer reachability tracking](#peer-reachability-tracking)
7. [XMP sync](#xmp-sync)
8. [Proxy AVIF media](#proxy-avif-media)
9. [JPEG preview serving](#jpeg-preview-serving)
10. [Unix domain socket protocol](#unix-domain-socket-protocol)
11. [HTTPS endpoints](#https-endpoints)
12. [NAT traversal](#nat-traversal)
13. [Mobile-specific behaviour](#mobile-specific-behaviour)
14. [Persistent state](#persistent-state)

---

## Purpose and scope

The daemon provides three synchronisation services:

| Service | Transport | Description |
|---------|-----------|-------------|
| XMP metadata sync | libp2p GossipSub + HTTPS POST | Rating, colour label, and processing history edits propagate to all peers in real time |
| Proxy AVIF fetch | HTTPS GET | Half-resolution `.proxy.avif` sidecar files are downloaded to phones for offline editing |
| Preview JPEG serve | HTTPS GET | JPEG thumbnails (`thumb` 480 px, `full` 1920 px) are generated and served to phones for gallery display |

The daemon does **not** move raw camera files. Raws stay on the desktop machine at all times.

---

## Architecture overview

```
┌─────────────────────────────────────────────────────────────────────┐
│  Desktop machine                                                    │
│                                                                     │
│  darktable (C/GTK) ──── Unix socket (JSON-lines) ──── dt-p2p-daemon│
│                                                         │           │
│                                                    libp2p + GossipSub
│                                                         │           │
└─────────────────────────────────────────────────────────────────────┘
           │ HTTPS / TLS (port 17842)
           │
┌──────────▼──────────────────────────────────────────────────────────┐
│  Phone / tablet                                                     │
│                                                                     │
│  Mobile Qt/QML app ──── Unix socket (JSON-lines) ──── dt-p2p-daemon│
│                                (embedded Go daemon)                 │
└─────────────────────────────────────────────────────────────────────┘
```

Both sides run the same `dt-p2p-daemon` binary. On the desktop the daemon is started by darktable and connects to the darktable process over a Unix socket. On the phone the daemon is embedded in the Qt app and communicates with the QML layer through an identical socket protocol.

---

## Startup and configuration

### Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--passphrase` | (required) | Shared secret; derives the Ed25519 identity and the HTTP auth token |
| `--socket` | `$XDG_RUNTIME_DIR/darktable-p2p.sock` | Unix socket path for darktable/app communication |
| `--proxy-dir` | `` | Directory to scan for `.proxy.avif` files to announce and serve (desktop only) |
| `--import-dir` | `` | Destination directory for proxy files downloaded from peers (mobile only) |
| `--peers` | `` | Comma-separated HTTPS base URLs of known peers, e.g. `https://192.168.1.108:17842` |
| `--local-ip` | `` | Override auto-detected LAN IP (required on Android where SELinux blocks netlink) |
| `--display-icc` | `` | Path to desktop display ICC profile; auto-detected via colord when omitted |

### Static peer file

In addition to `--peers`, the daemon reads `~/.config/darktable/peers.txt` (one URL per line, `#` comments ignored) at startup.

### Peer URL normalisation

All peer URLs are normalised to `https://host:17842` regardless of how they were supplied (bare IP, IP:port, or `http://` URL).

### Startup sequence

1. Derive Ed25519 key from passphrase via PBKDF2 (100,000 iterations, SHA-256).
2. Build a self-signed TLS certificate from the derived key.
3. Compute own fingerprint (SHA-256 of the DER public key, hex) and write it to `~/.config/darktable/peer.fingerprint`.
4. Load the allowed fingerprint list from `~/.config/darktable/peer.keys`.
5. Start libp2p host with GossipSub on two topics: `darktable/xmp/v1` and `darktable/proxy-announce/v1`.
6. Start mDNS service for LAN peer discovery.
7. Start HTTPS server on port 17842 (fallback to any free port).
8. Start Unix socket listener for darktable/app connections.
9. Write own LAN URL to `~/.config/darktable/peer.localurl`.
10. Restore persisted `local→remote` path map from disk.
11. Open SQLite peer database and seed with static peers.
12. Begin background goroutines: XMP subscriber, proxy-announce subscriber, proxy announcer, static-peer syncer, path-map flusher.

---

## Identity and security

### Passphrase-derived identity

The shared passphrase is the root secret. From it two values are derived deterministically:

- **Ed25519 key** — used as the libp2p identity and as the TLS certificate key. Because the key is derived (not randomly generated) two peers on the same passphrase can verify each other before any out-of-band exchange.
- **HTTP auth token** — a short SHA-256 hash of the passphrase sent as the `X-DT-Auth` header on every HTTP request. All HTTPS endpoints reject requests without it.

### TLS fingerprint pinning

The TLS server presents a self-signed certificate whose public key is the passphrase-derived Ed25519 key. The TLS client rejects connections whose peer certificate fingerprint is not in the allowed list (`~/.config/darktable/peer.keys`) **or** matches the daemon's own fingerprint.

When a connection is rejected because the fingerprint is unknown, the daemon probes the peer out-of-band to retrieve its fingerprint and records it as a *candidate peer* (stored in `~/.config/darktable/peer.candidates`). The darktable preferences UI presents candidates to the user with an Accept button; accepting adds the fingerprint to `peer.keys` and to the live in-memory allowed set.

### Path security gate

The `/preview` and `/proxy` endpoints only serve files that darktable has explicitly registered via `announce_proxy`, **or** that have a `.proxy.avif` sidecar on disk (proof of darktable ownership). This prevents the daemon from acting as a general-purpose file server even for callers with the correct auth token. Paths containing `..` are unconditionally rejected.

---

## Peer discovery

Four mechanisms feed into the set of known peers. All successful contacts are persisted in `peers.db` (SQLite) so they survive daemon restarts.

### 1. mDNS

At startup the daemon registers an mDNS service tagged `darktable-p2p` on the LAN. When another daemon announces itself via mDNS, `HandlePeerFound` is called with the libp2p peer info. The daemon records the peer and triggers `syncWithPeer`.

mDNS is best-effort; many access points block multicast. It works reliably on wired LANs and home Wi-Fi networks.

### 2. Static peers / peers.txt

URLs provided via `--peers` or `peers.txt` are seeded into `peers.db` at startup and passed to `syncStaticPeers`, which runs initial syncs after a 2-second delay then re-syncs every 60 seconds.

### 3. GossipSub proxy announce

Every daemon broadcasts its library manifest (list of raw file paths + own HTTPS URL) on the `darktable/proxy-announce/v1` topic every 30 seconds. When a daemon receives an announce from a new peer it:

1. Records the peer's URL in `d.peers` (in-memory).
2. Touches the peer in `peers.db`.
3. Triggers `syncWithPeer` immediately if the peer has not been HTTP-verified yet, so it enters the reachable set quickly without waiting for the 60-second sync tick.

### 4. Manifest indirect peers (`KnownPeers`)

Each `/manifest` response includes a `known_peers` list — the URLs the remote peer has discovered. On first contact, the receiving daemon syncs with all indirect peers it hasn't already seen. This allows transitive discovery (A discovers B discovers C) without A having C's address in advance.

---

## Peer reachability tracking

Only peers that have successfully responded to a manifest sync are used as sources for preview and XMP delivery. This prevents fetch attempts from going to stale or unreachable addresses (e.g. a VPN address from a previous session).

### syncWithPeer

`syncWithPeer(baseURL)` is the "hello" handshake:

1. Fetch `/manifest` from the peer (`?from=<ownURL>` on first visit so the remote can trigger a reverse sync).
2. On failure: call `pdb.markFailure`, remove from `d.syncedPeers`, return.
3. On success: call `pdb.markSuccess`, add to `d.syncedPeers`.
4. Compare the manifest hash against the last-seen hash. If unchanged, skip the per-path work to avoid redundant downloads.
5. On first successful contact: establish the libp2p connection for GossipSub, download any missing proxies, and sync with indirect peers from `known_peers`.

### d.syncedPeers

`d.syncedPeers` is an in-memory `map[string]bool` of HTTPS base URLs. It is the canonical set of reachable peers:

- Added when `syncWithPeer` succeeds.
- Removed when `syncWithPeer` fails.
- `allPeerURLs()` returns only entries in this map. All preview, proxy, and XMP fetch operations use `allPeerURLs()`.

---

## XMP sync

XMP sidecars carry darktable's per-image metadata: star rating, colour label, and full processing history (pipeline module stack). The daemon syncs them bidirectionally in real time.

### Outbound (darktable → peers)

1. darktable writes the XMP sidecar and sends `xmp_push` on the Unix socket.
2. The daemon deduplicates by `(path, mtime)` — if the same mtime was seen before, the message is dropped.
3. The XMP content is also written locally (covering the case where darktable is running on the same machine as the daemon).
4. The daemon publishes the XMP on the GossipSub `darktable/xmp/v1` topic.
5. Simultaneously, `pushXMPToPeers` sends the XMP to every peer in `d.syncedPeers` via direct HTTPS POST to `/xmp`. This ensures delivery even when the GossipSub mesh has not yet formed.

### Inbound (peer → darktable)

XMP updates arrive via GossipSub or HTTPS POST to `/xmp`. `applyInboundXMP` handles both:

1. Deduplicate by `(path, mtime)`.
2. Resolve the remote path to a local file using `resolveLocalPath`. If no local file is found, trigger `fetchAndImport` to download the proxy AVIF first.
3. **Merge strategy**: if the incoming XMP has no `darktable:history` element (mobile thin-push containing only rating/colour label) but the local file does have one, only the rating and colour label fields are grafted into the existing local XMP. The processing pipeline is never overwritten by a mobile edit.
4. Write the merged XMP to disk.
5. Invalidate the local preview cache so the next preview request regenerates from the updated XMP.
6. Broadcast `xmp_updated` on the Unix socket so darktable (or the mobile app) reloads the file immediately.

### Echo suppression

To prevent edit-bounce loops, two suppression mechanisms are in place:

- **Daemon level** (`xmpSuppressFrom`, 15 s): when an XMP update is received from a specific peer URL, that peer is excluded from the next outbound push for that path. This stops A → B → A → B cycles.
- **Desktop level** (`_p2p_push_echo_path`, 10 s): after darktable sends `xmp_push`, the daemon's echo of `xmp_updated` for that path is suppressed on the darktable side for 10 seconds.
- **Mobile level** (`m_xmpReceivedAt`, 5 s): after the mobile app receives `xmp_updated` for a path, it skips pushing XMP back to the daemon for 5 seconds.

---

## Proxy AVIF media

`.proxy.avif` sidecars are half-resolution (or smaller) re-encodings of raw files created by darktable's export pipeline. They let the mobile app display and edit images without transferring multi-megabyte raw files.

### Announcement

- When darktable creates a proxy AVIF it immediately sends `announce_proxy` on the socket, adding the path to `d.announcedProxies` and invalidating the manifest cache.
- When darktable reconnects to a (re)started daemon it receives `request_announce` and re-announces all proxies whose `.proxy.avif` files exist on disk.
- Every 30 seconds, `publishProxyAnnounce` broadcasts the full path list on the GossipSub `darktable/proxy-announce/v1` topic.

### Downloading (mobile)

`autoFetchProxy(remotePath, baseURL)`:

1. Record `localToRemote[localPath] = remotePath` so future preview fetches know what path to request from the desktop.
2. Fetch the thumbnail JPEG first (small, fast) so the gallery shows something immediately.
3. Download the full `.proxy.avif` via `/proxy?path=…`.
4. Download the `.xmp` sidecar via `/xmp?path=…` and merge it with any existing local XMP.
5. Add the path to the local index.
6. Broadcast `image_imported` on the socket so the mobile UI adds the image to the gallery.

Up to 8 proxy downloads run concurrently (controlled by `downloadSem`).

---

## JPEG preview serving

Previews are JPEG files cached beside the raw/proxy:

| Suffix | Max dimension | Source |
|--------|---------------|--------|
| `.preview-thumb.jpg` | 480 px | Darktable mipmap cache (levels 1 or 2) |
| `.preview-full.jpg` | 1920 px | Darktable-cli export (always) |

### Generation pipeline (`servePreview`)

1. Reject the request if the raw path is not darktable-owned (not in `announcedProxies` and no `.proxy.avif` sidecar on disk).
2. If a cached JPEG exists but lacks an ICC profile, delete it so it is regenerated with the correct colour metadata.
3. If no cached JPEG exists:
   - **Thumbnails**: read the mipmap JPEG from darktable's mipmap cache (levels 1 or 2 for thumb), inject the sRGB ICC profile, and write the cache file.
   - **Full-size**: skip mipmap data entirely and use `darktable-cli` for guaranteed ICC + full Exif metadata. If darktable is currently running (to avoid SQLite conflicts), ask it to generate the mipmap instead and return `503 Service Unavailable` so the phone retries in a few seconds.
4. Serve the cache file with `http.ServeContent` (handles `If-Modified-Since` / `304 Not Modified`).

### Display ICC

JPEG thumbnails are injected with the desktop display ICC profile so the mobile renders colours consistently with the desktop. The profile is loaded from `--display-icc` or auto-detected via `colord` (D-Bus `GetProfilesForDevice`). If detection fails a compact sRGB profile is used as fallback.

### Fetching (mobile)

`fetchPreviewFromPeers(canonicalPath, size)`:

1. Resolve `canonicalPath` (local import-dir path) to the desktop's original path via `localToRemote`.
2. Try every peer in `d.syncedPeers`.
3. For each peer, `fetchPreviewJPEG` sends a conditional GET with `If-Modified-Since: <local-file-mtime>`. The desktop returns `304 Not Modified` if nothing has changed, or `200` with a fresh JPEG if the desktop re-rendered.
4. The old cached JPEG is **not** deleted before the fetch — it stays visible in the gallery until the new download atomically overwrites it.
5. Retry up to 8 times with 4-second delays (accommodates the time darktable needs to generate the mipmap after an edit).

### Stale preview handling

When the local XMP sidecar is newer than the cached preview JPEG, the model marks the image as stale (`previewIsStale = true`). The periodic `syncMissingPreviews` call (every 45 s) re-emits `previewStale` for stale entries, which triggers `forceRefreshPreview` again until a fresh preview arrives and clears the flag.

---

## Unix domain socket protocol

All messages are JSON-lines (one JSON object per line). The daemon listens on the socket and darktable / the mobile app connect to it.

### Connection modes

**Event subscription** — a persistent connection opened by sending `subscribe_events`. The daemon pushes events (below) on this connection for as long as it is open.

**Command connections** — a short-lived connection per command. Either fire-and-forget (no response expected) or single-response.

### Commands (app → daemon)

| `type` | Description |
|--------|-------------|
| `subscribe_events` | Begin receiving push events. Daemon immediately sends `request_announce`. |
| `xmp_push` | Send an XMP update to all peers. Fields: `path`, `content`, `mtime`. |
| `announce_proxy` | Register a raw file path as darktable-owned and serveable. |
| `fetch_proxy` | Download the proxy AVIF for `path` from any reachable peer. Responds with `proxy_fetched`. |
| `fetch_preview` | Download a preview JPEG for `path` at `size` (`thumb`\|`full`) from peers. |
| `push_preview` | Re-fetch preview from peers (re-download even if cached; stale cache stays until replaced). |
| `request_sync` | Re-sync with all known peers (discover new images, refresh previews). |
| `list_peers` | Returns `peers` with libp2p peer IDs. |
| `list_peer_status` | Returns `peer_status` with URL, last-seen, failure count, synced flag for every peer in the DB. |
| `list_candidates` | Returns `candidates` — peers seen whose fingerprint is not yet trusted. |
| `accept_peer` | Trust a candidate's fingerprint, persist it to `peer.keys`, trigger sync. |
| `get_pairing_info` | Returns `pairing_info` with passphrase, own fingerprint, and known peer URLs (used to generate the QR pairing code). |
| `ping` | Returns `pong`. |

### Events (daemon → app)

| `type` | Description |
|--------|-------------|
| `request_announce` | Ask darktable to call `announce_proxy` for all known proxies. |
| `xmp_updated` | XMP for `path` was written to disk (from peer or local push). |
| `image_imported` | A new image path was discovered (triggers gallery update). |
| `preview_updated` | A new or refreshed preview JPEG was written for `path`. |
| `generate_preview` | Ask darktable to regenerate its mipmap cache for `path`. |

---

## HTTPS endpoints

All endpoints require the `X-DT-Auth` header with the passphrase-derived token. Port is 17842 by default; TLS is always used.

### `GET /manifest`

Returns a JSON object describing the daemon's library:

```json
{
  "peer_id":    "12D3KooW…",
  "hash":       "a1b2c3d4e5f6a7b8",
  "paths":      ["/home/user/Pictures/…/IMG_0001.CR3", …],
  "known_peers":["https://192.168.1.50:17842"],
  "external_url":"https://203.0.113.5:17842",
  "libp2p_ip":  "192.168.1.108",
  "libp2p_port": 54321
}
```

`hash` is a 16-character hex digest of the sorted path list. Callers cache it and skip per-path work when it has not changed. If `?from=<url>` is provided the remote is recorded as a peer and a reverse sync is triggered.

### `GET /proxy?path=<raw-path>`

Serves the `.proxy.avif` sidecar for a darktable-owned path.

### `GET /preview?path=<raw-path>&size=thumb|full`

Generates (if needed) and serves the JPEG preview for a darktable-owned path. Returns `503` while darktable is generating the mipmap.

### `GET /xmp?path=<raw-path>`

Serves the XMP sidecar for a darktable-owned path.

### `POST /xmp`

Receives an XMP push from another daemon. Body is a JSON-encoded `xmpMsg`. Applies the XMP locally, broadcasts `xmp_updated` on the socket, and records echo-suppress state.

---

## NAT traversal

At startup `initExternalAccess` attempts to open a port mapping so the daemon is reachable from outside the LAN (e.g. when desktop and phone are on different networks):

1. **UPnP IGD2** (`WANIPConnection2`) — tried first.
2. **UPnP IGD1** (`WANIPConnection1`) — fallback.
3. **NAT-PMP** — second fallback via the default gateway.

On success, `d.externalURL` is set to `https://<external-ip>:17842` and included in `/manifest` responses so that syncing peers learn the external address. The UPnP lease is renewed every 50 minutes (lease duration is 3600 s).

---

## Mobile-specific behaviour

### local→remote path map

On desktop, raw files live at their original paths (e.g. `/home/user/Pictures/…/IMG_0001.CR3`). On the phone, downloaded proxies land in `--import-dir` (e.g. `/data/…/import/20260615_import1/IMG_0001.CR3`). The daemon maintains a `localToRemote` map so that when the mobile app issues a `fetch_preview` with its local path, the daemon knows which desktop path to request from the peer.

The map is populated by `autoFetchProxy` at download time and is persisted to `~/.config/darktable/path_map.json` (flushed at most every 5 seconds via `runPathMapFlusher`) so it survives daemon restarts.

### LAN IP detection

On Android, SELinux blocks the netlink socket used by the libp2p address-detection code, which would cause the daemon to advertise an incorrect IP. The `--local-ip` flag overrides auto-detection. Without the flag, `localIP()` enumerates OS interfaces via `net.Interfaces()` and selects the best RFC-1918 address in priority order: `192.168.x.x` → `172.16-31.x.x` → `10.x.x.x`. Point-to-point tunnel interfaces (OpenVPN, some WireGuard configurations) and Tailscale CGNAT addresses (`100.64/10`) are naturally excluded.

### Lighttable auto-fetch

In the mobile gallery (ThumbnailGrid), each visible thumbnail cell runs a `Timer` (30-second interval) that calls `fetchPreview("thumb")` while `model.previewKey <= 0` and the daemon is connected. The timer stops automatically when the preview arrives (`running` re-evaluates to `false`) and is destroyed when the cell scrolls out of the viewport.

---

## Persistent state

| File | Location | Contents |
|------|----------|----------|
| `peer.fingerprint` | `~/.config/darktable/` | Own TLS fingerprint (SHA-256 hex), written at startup |
| `peer.localurl` | `~/.config/darktable/` | Own HTTPS base URL for the pairing QR |
| `peer.keys` | `~/.config/darktable/` | One fingerprint per line; peers whose cert matches are trusted |
| `peer.candidates` | `~/.config/darktable/` | JSON array of untrusted-but-seen candidate peers |
| `peers.db` | `~/.config/darktable/` | SQLite peer registry: URL, peer ID, created/last-seen timestamps, failure count |
| `path_map.json` | `~/.config/darktable/` | JSON object mapping mobile-local paths to desktop canonical paths |
| `peers.txt` | `~/.config/darktable/` | User-editable static peer list (one URL per line) |

### peers.db schema

```sql
CREATE TABLE peers (
    url           TEXT    PRIMARY KEY,
    peer_id       TEXT    NOT NULL DEFAULT '',
    created_at    INTEGER NOT NULL,   -- Unix timestamp
    last_seen     INTEGER NOT NULL,   -- Unix timestamp
    failure_count INTEGER NOT NULL DEFAULT 0
);
```

`touch` updates `last_seen` without resetting `failure_count`. `markSuccess` resets `failure_count` to 0. `markFailure` increments it. The daemon orders candidates by `last_seen DESC` when selecting peers to sync with.
