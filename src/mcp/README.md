# Darktable MCP

A headless [Model Context Protocol](https://modelcontextprotocol.io) server that
exposes darktable's raw-development engine to AI agents over stdio (JSON-RPC 2.0).
It is a sibling command-line binary to `darktable-cli`, linking `libdarktable`
directly, so it gets real module introspection, the in-process pixelpipe, and
native styles/history — no XMP hand-editing or SQL injection.

## Building

`darktable-mcp` is built by default (CMake option `USE_MCP`, default `ON`,
disable with `-DUSE_MCP=OFF` or `./build.sh --disable-mcp`). It depends on
`json-glib-1.0` (already a darktable dependency).

```sh
# built as part of a normal build
./build.sh

# or just the target
cmake -B build .
cmake --build build --target darktable-mcp -j
```

The binary lands at `build/bin/darktable-mcp`.

## Running

```sh
darktable-mcp [--core <darktable core options...>]
```

Everything after `--core` is forwarded verbatim to `dt_init`, so all darktable
core options work exactly as they do for `darktable`/`darktable-cli`:
`--configdir`, `--cachedir`, `--library`, `--conf key=value`, `-d <domain>`, etc.

Unless you override them, two defaults are injected:

- `--library :memory:` — a throwaway catalog (loose files are imported ad-hoc).
- `--conf write_sidecar_files=never` — never touch `.xmp` sidecars.

**stdout carries only JSON-RPC** (darktable's own logging is redirected to
stderr), so a client can speak the protocol cleanly.

### Library modes

- **Ad-hoc (default `:memory:`)** — render/inspect loose raw files by path.
- **Real catalog** — `--core --library /path/to/library.db` (optionally with an
  isolated `--configdir`) lets the library tools see existing images, their
  edits, and saved styles.

> darktable takes a PID lock on `library.db`/`data.db`, so catalog mode must run
> while the GUI is **not** holding that library (or against a copy).

## Connecting to Claude

`darktable-mcp` is a standard stdio MCP server. Give it a dedicated config/cache
directory so it never contends with the darktable GUI's `data.db` lock (with the
default `:memory:` library it is then safe to run even while darktable is open):

```sh
mkdir -p ~/.config/darktable-mcp
```

### Claude Code

```sh
claude mcp add darktable -- \
  /path/to/build/bin/darktable-mcp \
  --core --configdir ~/.config/darktable-mcp \
         --cachedir  ~/.config/darktable-mcp/cache
```

Default scope is local (this project); add `-s user` for all projects or
`-s project` to write a shared `.mcp.json`. Check with `claude mcp list` (or
`/mcp` in a session); remove with `claude mcp remove darktable`.

### Claude Desktop

Add the same server to `claude_desktop_config.json` (Settings → Developer → Edit
Config) and restart the app:

```json
{
  "mcpServers": {
    "darktable": {
      "command": "/path/to/build/bin/darktable-mcp",
      "args": ["--core",
               "--configdir", "/home/you/.config/darktable-mcp",
               "--cachedir",  "/home/you/.config/darktable-mcp/cache"]
    }
  }
}
```

Then ask in natural language, e.g. *"Using darktable, render this raw and show
it"* or *"measure image_stats with agx target_black at 0.008 vs 0.0008"*. For a
real catalog, swap the args to `--core --library /path/to/library.db` (GUI must
be closed for that library).

## Protocol

Standard MCP handshake over stdio, both newline-delimited JSON (default) and
`Content-Length:` framing are accepted. Methods: `initialize`,
`notifications/initialized`, `tools/list`, `tools/call`, `ping`, `shutdown`.

```jsonc
// → request
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
// ← reply
{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18",
  "capabilities":{"tools":{"listChanged":false}},
  "serverInfo":{"name":"darktable-mcp","version":"0.1.0"}}}
```

## Tools

### Introspection

| Tool | Input | Output |
|------|-------|--------|
| `list_modules` | – | `[{operation, version, have_introspection, doc_url}]` |
| `module_schema` | `{operation}` | fields with `{name, type, offset, min, max, default}` (enum values listed) + `doc_url` |
| `decode_params` | `{operation, blob_hex}` | `{operation, version, fields:{…}}` — named values from a hex `op_params` blob |
| `encode_params` | `{operation, fields:{…}}` | `{operation, blob_hex}` — seeds defaults, applies fields (enums accept symbolic names) |

`doc_url` is the module's page in the darktable usermanual — fetch it for prose
docs on what each parameter means.

### Develop

| Tool | Input | Output |
|------|-------|--------|
| `render` | `{input:{path\|imgid}, width?, height?, stack?, disable_tone_mappers?, history_end?}` | MCP image content (base64 PNG) |
| `image_stats` | same as `render` | per-channel `{min, max, mean, p1, p50, p99, clip_lo, clip_hi}` |

A `stack` is an array of `{operation, params:{…} | blob_hex, multi_priority?, enabled?}`
applied on top of the image's base pipeline. `disable_tone_mappers:true` switches
off `sigmoid`/`filmicrgb`/`basecurve` so an added tone mapper (e.g. `agx`) owns the
tone curve. Rendering happens on a throwaway duplicate, so the source image is
never modified.

### Library (catalog)

| Tool | Input | Output |
|------|-------|--------|
| `list_images` | `{limit?}` | `[{imgid, path}]` |
| `get_history` | `{imgid}` | the edit stack, decoded per module `[{num, operation, version, enabled, multi_priority, fields}]` |
| `list_styles` | – | `[{name, description}]` |
| `apply_style` | `{name, imgid, overwrite?}` | `{ok:true}` |
| `save_style` | `{name, description?, imgid}` | `{ok:true}` |
| `import_style` | `{path}` | `{ok:true}` (`.dtstyle` → styles DB) |
| `export` | `{input, out_path, width?, height?, stack?, …}` | writes a PNG to `out_path` |

## Example

Measure the shadow-floor effect of an `agx` parameter on a raw, with the default
tone mapper disabled:

```jsonc
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{
  "name":"image_stats",
  "arguments":{
    "input":{"path":"/photos/DSCF0001.RAF"},
    "width":512,"height":512,
    "disable_tone_mappers":true,
    "stack":[{"operation":"agx",
              "params":{"curve_target_display_black_ratio":0.0008},
              "enabled":true}]}}}
```

The result's text is JSON like
`{"width":512,"height":341,"channels":{"g":{"p1":6,"p50":71,…},…}}`.

## Architecture

```text
src/mcp/
  main.c            process lifecycle: dt_init boot, --core passthrough, stdio loop
  mcp_jsonrpc.c/.h  transport: framing, JSON-RPC dispatch, error objects
  mcp_tools.c/.h    tool registry + JSON <-> C marshalling
  dt_bridge.c/.h    the ONLY unit that calls libdarktable
```

`dt_bridge.c` isolates every libdarktable call, so API churn (e.g. a bumped
module param version) stays contained. Parameters are always addressed through
introspection (`get_p` / `get_introspection_linear`), never fixed byte offsets,
so the server stays correct across module versions.

## Customizing tool descriptions & schemas

Each tool's presentation — `name`, `description`, `inputSchema` — lives in
`mcp-tools.json` in darktable's data folder (installed to
`share/darktable/mcp-tools.json`, alongside `noiseprofiles.json` etc.), loaded at
startup via `dt_loc_get_datadir()`. Only the tool *behaviour* (the handler) is
compiled into the binary, matched to a metadata entry by `name`.

So you can reword a description (to steer which tool the model picks) or tighten
an `inputSchema`, then restart the server — no rebuild. Adding a genuinely new
tool still needs a C handler in `mcp_tools.c`. A JSON entry whose `name` has no
handler is ignored with a warning on stderr.

## Notes & limitations

- **Headless rendering** goes through an in-memory export format module + plain
  cairo, not `dt_imageio_preview` (that helper builds its surface via a GUI-only
  cairo wrapper and crashes without a GUI).
- **First render** of a raw runs demosaic + the full pipe and can take a few
  seconds; give clients a generous timeout.
- **Version upgrades:** `decode_params` currently requires the blob to match the
  module's current param size. Feeding older-version blobs through
  `dt_iop_legacy_params` first is a planned addition.
- **`export`** writes PNG only for now.
- Not included: driving a live/open darktable GUI (this is a background worker).
