#!/bin/bash
#
# Regression suite for the classic -> flexi mask migration
# (src/develop/masks/migrate_legacy.c, entry point
# dt_masks_migrate_classic_to_flexi).
#
# The XMPs under xmps/ are hand-packed classic (pre-flexi) blend_params /
# mask-point blobs -- see gen_xmp.py -- covering drawn-only, parametric-only
# and drawn+parametric masks crossed with the classic combine operators
# (union/intersection/difference/exclusion), per-shape and group-level
# polarity invert (INV/MASKS_POS), DEVELOP_COMBINE_INCL, per-scope mask
# refinement, and raster masks (the K series, which needs two modules: one
# publishing a mask and one consuming it). Rendering each
# one with darktable-cli forces the migration to run (classic mode is a
# legacy-params-only code path; a fresh XMP always loads as classic and gets
# migrated to DT_MASKS_PARAMETRIC/DT_MASKS_GROUP forms on load).
#
# Two modes:
#
#   ./run.sh                 - compare current darktable-cli output against
#                               the checked-in expected/*.png (exact pixel
#                               match). This is the normal regression mode.
#
#   ./run.sh --pristine <bin> - re-run the original validation methodology:
#                               render once with <bin> (a build with
#                               migrate_legacy.c's effects stashed out, i.e.
#                               true pre-migration classic rendering) and
#                               once with the current DARKTABLE_CLI/PATH
#                               binary, and diff the two directly. Use this
#                               only when re-validating the migration itself
#                               against a from-scratch pristine build; not
#                               needed for ordinary regression testing.

set -u
CDPATH=

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$HERE"

if [[ -z ${DARKTABLE_CLI:-} ]] && [[ -z $(command -v darktable-cli) ]]; then
    echo "Make sure darktable-cli is in the PATH, or set DARKTABLE_CLI" >&2
    exit 1
fi
CLI=${DARKTABLE_CLI:-darktable-cli}
COUNT_DIFF=../../integration/count-diff-pixels
IMAGE="$HERE/images/Sweep_sRGB_Linear_Half_Zip_01.tif"

# Per-scenario comparison tolerance, in 1/255 units: a scenario fails if any
# pixel differs from its reference by MORE than this. 0 -- exact match -- for
# everything except the two noted below.
#
# F1/F2 are the only scenarios that activate the whole RGB-scene channel set at
# once, Jz/Cz/hz included, and the JzCzhz hue computation is ill-conditioned:
# Az and Bz are formed by subtracting near-equal values, so the result depends
# on inlining and vectorisation decisions rather than on darktable's logic
# alone. Across builds of the same commit they move by at most 5/255 over
# about 1,250 of 90,000 pixels: build variance, not drift.
#
# An exact-match assertion on them therefore fails for anyone whose compiler or
# build options differ from whoever last generated the reference, which is a
# false alarm that costs a real investigation to dismiss. A mask defect moves
# thousands of pixels by far more than 5/255, so the tolerance costs no real
# coverage.
#
# K1/K1C/K4 share one pixel, (349,116), on the far tail of the circle's feather,
# where the +5EV adds well under one 8-bit step: it lands on a rounding boundary
# and moves by 1/255 between builds. The K1 == K1C control below still compares
# exactly.
tolerance()
{
    case "$1" in
        F1_content_incl_allchannels_noinv|F2_content_incl_allchannels_inv)
            echo 5 ;;
        K1_raster_from_drawn|K1C_drawn_control|K4_raster_source_missing)
            echo 1 ;;
        *)  echo 0 ;;
    esac
}

PRISTINE_BIN=""
if [[ ${1:-} == --pristine ]]; then
    PRISTINE_BIN=$2
    [[ -z $PRISTINE_BIN ]] && echo "--pristine requires a binary path" >&2 && exit 1
fi

OUTDIR=$(mktemp -d)
trap 'rm -rf "$OUTDIR"' EXIT

CORE_OPTIONS=(--core --disable-opencl --library :memory:
    --conf host_memory_limit=8192
    --conf resourcelevel=reference
    --conf worker_threads=4 -t 4
    --conf plugins/lighttable/export/pixel_interpolator=lanczos3
    --conf plugins/lighttable/export/pixel_interpolator_warp=bicubic
    --conf plugins/lighttable/export/iccintent=0)

render()
{
    local bin=$1 xmp=$2 out=$3
    "$bin" --width 400 --height 225 --hq true --apply-custom-presets false \
        "$IMAGE" "$xmp" "$out" "${CORE_OPTIONS[@]}" \
        > "${out%.png}.log" 2>&1
}

FAIL=0
COUNT=0

for xmp in xmps/*.xmp; do
    name=$(basename "$xmp" .xmp)
    COUNT=$((COUNT + 1))
    rm -f "$OUTDIR/$name"*.png

    if [[ -n $PRISTINE_BIN ]]; then
        render "$PRISTINE_BIN" "$xmp" "$OUTDIR/${name}_pristine.png"
        render "$CLI" "$xmp" "$OUTDIR/${name}_migrated.png"
        ref="$OUTDIR/${name}_pristine.png"
        got="$OUTDIR/${name}_migrated.png"
    else
        render "$CLI" "$xmp" "$OUTDIR/${name}.png"
        ref="expected/${name}.png"
        got="$OUTDIR/${name}.png"
    fi

    if [[ ! -f $got ]]; then
        echo "FAIL $name (render failed, see $OUTDIR/${name}*.log)"
        FAIL=$((FAIL + 1))
        continue
    fi
    if [[ ! -f $ref ]]; then
        echo "FAIL $name (missing $ref)"
        FAIL=$((FAIL + 1))
        continue
    fi

    tol=$(tolerance "$name")
    diff=$(python3 "$COUNT_DIFF" --threshold "$tol" "$ref" "$got")
    if [[ $diff == 0 ]]; then
        echo "OK   $name"
    else
        if [[ $tol == 0 ]]; then
            echo "FAIL $name ($diff differing pixels)"
        else
            echo "FAIL $name ($diff pixels differ by more than $tol/255)"
        fi
        FAIL=$((FAIL + 1))
    fi
done

# Equivalence controls (K series).
#
# Everything above compares against a checked-in PNG, which locks in behaviour
# but cannot by itself say that behaviour is *right* -- the references were
# generated by the migrating binary. (Proving that is what --pristine is for,
# and it needs a second build.)
#
# These pairs need no second build: each names two scenarios that must render
# identically because they express the same mask by different routes, so the
# assertion holds whatever the reference PNGs happen to contain.
#
#   K1 vs K1C  consuming a mask as a raster mask == owning the same geometry
#              as your own drawn mask
#   K2 vs K2C  ... and the same with the mask inverted, where the two routes
#              diverge completely: K2 inverts via raster_mask_invert, which
#              migration moves onto the element's DT_MASKS_STATE_INVERSE bit,
#              while K2C inverts via classic DEVELOP_COMBINE_MASKS_POS. An
#              inversion applied at the wrong level of the fold breaks this
#              and nothing else.
CONTROL_PAIRS="K1_raster_from_drawn:K1C_drawn_control
K2_raster_inverted:K2C_drawn_inverted_control"

suffix=""
[[ -n $PRISTINE_BIN ]] && suffix="_migrated"

for pair in $CONTROL_PAIRS; do
    a=${pair%%:*}
    b=${pair##*:}
    COUNT=$((COUNT + 1))
    if [[ ! -f $OUTDIR/${a}${suffix}.png || ! -f $OUTDIR/${b}${suffix}.png ]]; then
        echo "FAIL $a == $b (a render is missing)"
        FAIL=$((FAIL + 1))
        continue
    fi
    diff=$(python3 "$COUNT_DIFF" "$OUTDIR/${a}${suffix}.png" "$OUTDIR/${b}${suffix}.png")
    if [[ $diff == 0 ]]; then
        echo "OK   $a == $b"
    else
        echo "FAIL $a == $b ($diff differing pixels)"
        FAIL=$((FAIL + 1))
    fi
done

echo
echo "$((COUNT - FAIL)) / $COUNT scenarios OK"
[[ $FAIL == 0 ]] && exit 0 || exit 1
