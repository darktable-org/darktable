#!/usr/bin/env python3
"""
make_proxy_avif.py — create proxy AVIF sidecars from camera RAW files.

For each RAW file (e.g. IMG_1234.CR2) this script produces a companion
IMG_1234.CR2.proxy.avif that darktable will load transparently when the
original raw is absent (e.g. on a laptop with limited storage).

The proxy is:
  • Half the raw sensor resolution (one output pixel per 2×2 Bayer block).
  • Camera-native linear colorspace: no white balance, no colour matrix,
    no highlight reconstruction — just black-level subtraction and
    white-point normalisation followed by Bayer averaging.
  • Stored as 12-bit AVIF with identity YUV matrix (RGB stored directly),
    unspecified colour primaries, and linear transfer characteristics so
    that any reader knows it is not a display-ready image.

Requirements:
  pip install rawpy numpy Pillow
  avifenc  (from libavif-bin / libavif-tools package)

Usage:
  make_proxy_avif.py [options] RAW_FILE [RAW_FILE …]

Options:
  -q / --quality  AVIF qcolor value (0-63, lower = better, default 22)
  -j / --jobs     encoder threads  (default: all available)
  --overwrite     replace existing .proxy.avif files
  --dry-run       print what would be done without writing any files
"""

import argparse
import os
import subprocess
import sys
import tempfile

try:
    import numpy as np
except ImportError:
    sys.exit("Error: numpy not installed.  Run: pip install numpy")

try:
    import rawpy
except ImportError:
    sys.exit("Error: rawpy not installed.  Run: pip install rawpy")

try:
    from PIL import Image
except ImportError:
    sys.exit("Error: Pillow not installed.  Run: pip install Pillow")


# ---------------------------------------------------------------------------
# Bayer helpers
# ---------------------------------------------------------------------------

def _bayer_channel_slices(pattern):
    """
    Return (r_row, r_col, g_rows_cols, b_row, b_col) for the given
    2×2 Bayer pattern array.

    rawpy encodes colours as: 0=R  1=G  2=B  3=G2
    """
    r_pos = g_pos = b_pos = None
    g_positions = []
    for row in range(2):
        for col in range(2):
            c = int(pattern[row, col])
            if c == 0:
                r_pos = (row, col)
            elif c == 2:
                b_pos = (row, col)
            else:  # 1 or 3 → green
                g_positions.append((row, col))
    if r_pos is None or b_pos is None or len(g_positions) < 2:
        raise ValueError(f"Unexpected Bayer pattern: {pattern}")
    return r_pos, g_positions, b_pos


def bayer_to_halfres_rgb(raw):
    """
    Demosaic by simple 2×2 block averaging:
      R_out = R_sensor
      G_out = (G1_sensor + G2_sensor) / 2
      B_out = B_sensor

    Then normalise each channel independently using its own black level
    and the shared white level so that the output is in [0, 1] (float32).

    Returns (r, g, b) as float32 arrays of shape (H/2, W/2).
    """
    bayer = raw.raw_image_visible  # uint16, shape (H, W)
    pattern = raw.raw_pattern       # 2×2 uint8
    white = float(raw.white_level)

    # black levels: rawpy gives one value per CFA colour in order R,G1,B,G2
    black = [float(b) for b in raw.black_level_per_channel]
    # Map pattern positions to per-channel black levels
    # rawpy's black_level_per_channel follows the order of colours as they
    # appear scanning left-to-right, top-to-bottom in the pattern, so index
    # into the pattern raster order.
    pattern_flat = [int(pattern[r, c]) for r in range(2) for c in range(2)]
    # build colour → black level map (take first occurrence for each colour)
    colour_black = {}
    for idx, col_id in enumerate(pattern_flat):
        if col_id not in colour_black and idx < len(black):
            colour_black[col_id] = black[idx]
    r_black  = colour_black.get(0, black[0])
    g1_black = colour_black.get(1, black[1] if len(black) > 1 else black[0])
    g2_black = colour_black.get(3, black[3] if len(black) > 3 else g1_black)
    b_black  = colour_black.get(2, black[2] if len(black) > 2 else black[0])

    r_pos, g_positions, b_pos = _bayer_channel_slices(pattern)

    # Extract 2×2 sub-grids (each is H/2 × W/2)
    r_raw  = bayer[r_pos[0]::2,          r_pos[1]::2         ].astype(np.float32)
    g1_raw = bayer[g_positions[0][0]::2, g_positions[0][1]::2].astype(np.float32)
    g2_raw = bayer[g_positions[1][0]::2, g_positions[1][1]::2].astype(np.float32)
    b_raw  = bayer[b_pos[0]::2,          b_pos[1]::2         ].astype(np.float32)

    # Normalise: subtract black, divide by (white − black), clip to [0, 1]
    def norm(arr, bl):
        return np.clip((arr - bl) / (white - bl), 0.0, 1.0)

    r = norm(r_raw, r_black)
    g = norm((g1_raw + g2_raw) * 0.5, (g1_black + g2_black) * 0.5)
    b = norm(b_raw, b_black)
    return r, g, b


# ---------------------------------------------------------------------------
# AVIF encoding
# ---------------------------------------------------------------------------

def _check_avifenc():
    try:
        r = subprocess.run(['avifenc', '--version'],
                           capture_output=True, timeout=5)
        return r.returncode == 0
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return False


def write_proxy_avif(r, g, b, output_path, quality=22, jobs=None, dry_run=False):
    """
    Write a 12-bit AVIF at *output_path* with:
      • YUV 4:4:4 + identity matrix  → RGB stored without colour rotation
      • Unspecified primaries         → camera-native, not a standard space
      • Linear transfer               → linear light values
      • Full range
    """
    if dry_run:
        h, w = r.shape
        print(f"    would write {w}×{h} proxy → {output_path}")
        return True

    # Scale float [0,1] → uint16 [0,65535]; avifenc --depth 12 will truncate
    # the bottom 4 bits, giving us effectively 12-bit precision.
    scale = 65535.0
    r16 = np.clip(r * scale, 0, 65535).astype(np.uint16)
    g16 = np.clip(g * scale, 0, 65535).astype(np.uint16)
    b16 = np.clip(b * scale, 0, 65535).astype(np.uint16)
    rgb16 = np.stack([r16, g16, b16], axis=2)  # (H, W, 3)

    pil_img = Image.fromarray(rgb16, mode='RGB')

    with tempfile.NamedTemporaryFile(suffix='.tiff', delete=False) as f:
        tmp = f.name
    try:
        pil_img.save(tmp, format='TIFF', compression='raw')

        cmd = [
            'avifenc',
            '--depth',              '12',
            '--yuv',                '444',
            '--matrix-coefficients','identity',
            '--color-primaries',    'unspecified',
            '--transfer-characteristics', 'linear',
            '--range',              'full',
            '--qcolor',             str(quality),
            '--jobs',               str(jobs) if jobs else 'all',
            tmp, output_path,
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"    avifenc error:\n{result.stderr.strip()}", file=sys.stderr)
            return False
        return True
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Per-file processing
# ---------------------------------------------------------------------------

def process(raw_path, quality, jobs, overwrite, dry_run):
    out_path = raw_path + '.proxy.avif'

    if not overwrite and os.path.exists(out_path):
        print(f"  skip (exists): {out_path}")
        return True

    try:
        with rawpy.imread(raw_path) as raw:
            r, g, b = bayer_to_halfres_rgb(raw)
    except Exception as exc:
        print(f"  read error: {exc}", file=sys.stderr)
        return False

    h, w = r.shape
    orig_size = os.path.getsize(raw_path)
    print(f"  {os.path.basename(raw_path)}  →  {w}×{h} half-res", end='')
    if dry_run:
        print()

    ok = write_proxy_avif(r, g, b, out_path,
                          quality=quality, jobs=jobs, dry_run=dry_run)
    if ok and not dry_run and os.path.exists(out_path):
        proxy_size = os.path.getsize(out_path)
        pct = proxy_size * 100 // orig_size
        print(f"  ({proxy_size // 1024} KB, {pct}% of raw)")
    return ok


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+', metavar='RAW_FILE')
    ap.add_argument('-q', '--quality',   type=int, default=22,
                    help='AVIF qcolor (0-63, lower=better, default 22)')
    ap.add_argument('-j', '--jobs',      type=int, default=None,
                    help='encoder threads (default: all)')
    ap.add_argument('--overwrite',       action='store_true',
                    help='replace existing .proxy.avif files')
    ap.add_argument('--dry-run',         action='store_true',
                    help='show what would be done without writing files')
    args = ap.parse_args()

    if not args.dry_run and not _check_avifenc():
        sys.exit(
            "Error: avifenc not found.\n"
            "  Ubuntu/Debian : sudo apt install libavif-bin\n"
            "  Fedora/RHEL   : sudo dnf install libavif-tools\n"
            "  Arch Linux    : sudo pacman -S libavif\n"
        )

    ok = err = 0
    for path in args.files:
        print(f"Processing: {path}")
        if process(path, args.quality, args.jobs, args.overwrite, args.dry_run):
            ok += 1
        else:
            err += 1

    print(f"\n{ok} created, {err} failed.")
    return 1 if err else 0


if __name__ == '__main__':
    sys.exit(main())
