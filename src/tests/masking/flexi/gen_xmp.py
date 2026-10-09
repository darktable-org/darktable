#!/usr/bin/env python3
"""Generate classic-format (pre-flexi) test XMPs for the darktable
classic->flexi mask migration, covering drawn/parametric/combined
configurations with varying combine operators and polarity inversions.
"""
import struct
import os

OUTDIR = os.path.join(os.path.dirname(__file__), "xmps")
os.makedirs(OUTDIR, exist_ok=True)

# ---------------------------------------------------------------------------
# dt_develop_blend_params_t (blend version 14, 420 bytes, confirmed against a
# real darktable-generated default blob: identical layout to current v15/v7).
# ---------------------------------------------------------------------------
DEVELOP_MASK_ENABLED = 1
DEVELOP_MASK_MASK = 1 << 1
DEVELOP_MASK_CONDITIONAL = 1 << 2
DEVELOP_MASK_RASTER = 1 << 3
DEVELOP_MASK_MASK_CONDITIONAL = DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL

DEVELOP_COMBINE_NORM = 0x00
DEVELOP_COMBINE_INV = 0x01
DEVELOP_COMBINE_INCL = 0x02
DEVELOP_COMBINE_MASKS_POS = 0x04

DEVELOP_BLEND_CS_RGB_SCENE = 4
DEVELOP_BLEND_NORMAL2 = 0x18

# RGB-scene blendif channel indices (see dt_develop_blendif_channels_t)
CH_GRAY_in = 0
CH_RED_in = 1
CH_GREEN_in = 2
CH_BLUE_in = 3
DEVELOP_BLENDIF_active = 31
DEVELOP_BLENDIF_SIZE = 16

DT_MASKS_STATE_USE = 1 << 0
DT_MASKS_STATE_SHOW = 1 << 1
DT_MASKS_STATE_INVERSE = 1 << 2
DT_MASKS_STATE_UNION = 1 << 3
DT_MASKS_STATE_INTERSECTION = 1 << 4
DT_MASKS_STATE_DIFFERENCE = 1 << 5
DT_MASKS_STATE_EXCLUSION = 1 << 6
DT_MASKS_STATE_SUM = 1 << 7

DT_MASKS_CIRCLE = 1
DT_MASKS_PATH = 1 << 1
DT_MASKS_GROUP = 1 << 2
DEVELOP_MASKS_VERSION = 7
DT_MASKS_POINT_STATE_USER = 2

# dt_masks_refine_scope_t (src/develop/masks.h)
DT_MASKS_REFINE_OFF = 0
DT_MASKS_REFINE_ELEMENT = 1
DT_MASKS_REFINE_GROUP = 2

BLEND_FMT = "<3i2f2ifIfIffffI2I" + "64f" + "16f" + "20siii"
# mask_mode(I) blend_cst(i) blend_mode(I) blend_parameter(f) opacity(f)
# mask_combine(I) mask_id(i) blendif(I) feathering_radius(f)
# feathering_guide(I) blur_radius(f) contrast(f) brightness(f) details(f)
# feather_version(I) reserved[2](II) blendif_parameters[64](f)
# blendif_boost_factors[16](f) raster_mask_source[20](s)
# raster_mask_instance(i) raster_mask_id(i) raster_mask_invert(i)
BLEND_FMT = "<IiIffIiIfIffffI2I64f16f20siii"


def pack_blend_params(mask_mode=0, blend_cst=0,
                       blend_mode=DEVELOP_BLEND_NORMAL2, blend_parameter=0.0,
                       opacity=100.0, mask_combine=DEVELOP_COMBINE_NORM,
                       mask_id=0, blendif=0, blendif_parameters=None,
                       blendif_boost_factors=None,
                       feathering_radius=0.0, blur_radius=0.0,
                       contrast=0.0, brightness=0.0, details=0.0,
                       raster_mask_source=b"", raster_mask_instance=-1,
                       raster_mask_id=0, raster_mask_invert=0):
    """feathering_radius/blur_radius/contrast/brightness/details are the
    module-wide ("global") mask refinements, applied once to the finished
    group mask -- as opposed to the per-member ones in pack_group_member.

    raster_mask_*: the classic DEVELOP_MASK_RASTER configuration, which lives
    entirely in these scalars outside the form tree -- migration synthesizes a
    DT_MASKS_RASTER form element from them (see the K series)."""
    if blendif_parameters is None:
        blendif_parameters = [0.0, 0.0, 1.0, 1.0] * DEVELOP_BLENDIF_SIZE
    if blendif_boost_factors is None:
        blendif_boost_factors = [0.0] * DEVELOP_BLENDIF_SIZE
    assert len(blendif_parameters) == 4 * DEVELOP_BLENDIF_SIZE
    assert len(blendif_boost_factors) == DEVELOP_BLENDIF_SIZE
    data = struct.pack(
        BLEND_FMT,
        mask_mode, blend_cst, blend_mode, blend_parameter, opacity,
        mask_combine, mask_id, blendif,
        feathering_radius,
        1,    # feathering_guide (DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR)
        blur_radius, contrast, brightness, details,
        0,    # feather_version
        0, 0,  # reserved[2]
        *blendif_parameters,
        *blendif_boost_factors,
        raster_mask_source,
        raster_mask_instance,
        raster_mask_id,
        raster_mask_invert,
    )
    assert len(data) == 420, len(data)
    return data


def channel_curve(channels, taper_in=(0.0, 0.3), taper_off=(0.5, 0.8),
                   invert_channels=()):
    """Set a 0-30% taper-in / 50-80% taper-off curve on the given channel
    indices; all other channels stay at the neutral/disabled (0,0,1,1)."""
    params = [0.0, 0.0, 1.0, 1.0] * DEVELOP_BLENDIF_SIZE
    blendif = 1 << DEVELOP_BLENDIF_active
    for ch in channels:
        params[ch * 4:ch * 4 + 4] = [taper_in[0], taper_in[1], taper_off[0], taper_off[1]]
        blendif |= 1 << ch
        if ch in invert_channels:
            blendif |= 1 << (ch + 16)
    return blendif, params


# ---------------------------------------------------------------------------
# mask forms
# ---------------------------------------------------------------------------
CIRCLE_FMT = "<4f"          # center[2], radius, border
PATH_PT_FMT = "<8fI"        # corner[2], ctrl1[2], ctrl2[2], border[2], state
GROUP_MEMBER_FMT = "<iiif" + "i6f" + "128s" + "f" + "64s"
# formid(i) parentid(i) state(i) opacity(f) refinement{enabled(i) details(f)
# feathering_radius(f) feathering_guide(I->i) blur_radius(f) contrast(f)
# brightness(f)} name[128] group_opacity(f) preset_note[64]


def pack_circle(cx, cy, radius, border):
    return struct.pack(CIRCLE_FMT, cx, cy, radius, border)


def pack_path(corners, border=(0.02, 0.02)):
    out = b""
    for (x, y) in corners:
        out += struct.pack(PATH_PT_FMT, x, y, x, y, x, y,
                            border[0], border[1], DT_MASKS_POINT_STATE_USER)
    return out


def pack_group_member(formid, parentid, state, opacity=1.0,
                       refine_enabled=0, details=0.0, feathering_radius=0.0,
                       blur_radius=0.0, contrast=0.0, brightness=0.0,
                       group_opacity=1.0):
    # feathering_guide is left 0 (its 0 bit pattern is identical whether the
    # reader treats this refinement slot as int or float -- not exercised
    # here, see the FMT comment above)
    data = struct.pack(GROUP_MEMBER_FMT, formid, parentid, state, opacity,
                        refine_enabled, details, feathering_radius, 0,
                        blur_radius, contrast, brightness, b"", group_opacity, b"")
    assert len(data) == 240, len(data)
    return data


CIRCLE_CX, CIRCLE_CY, CIRCLE_R, CIRCLE_BORDER = 0.45, 0.45, 0.18, 0.21
SQUARE_CORNERS = [(0.40, 0.30), (0.70, 0.30), (0.70, 0.60), (0.40, 0.60)]
# second shape pair for the two-group scenarios (J7/J8): offset to the opposite
# corner so the two groups' masks aren't near-identical
CIRCLE2_CX, CIRCLE2_CY, CIRCLE2_R, CIRCLE2_BORDER = 0.75, 0.75, 0.15, 0.18
SQUARE2_CORNERS = [(0.05, 0.65), (0.30, 0.65), (0.30, 0.90), (0.05, 0.90)]


class MaskIds:
    def __init__(self, base):
        self.circle = base + 1
        self.path = base + 2
        self.group = base + 3


def masks_history_rows(mask_num, ids, circle_state, square_state,
                       circle_opacity=1.0, circle_refine=None):
    """Returns list of (mask_id, mask_type, mask_name, mask_points_hex, mask_nb).
    circle_refine: None, or a dict of pack_group_member's refine_* kwargs --
    exercises the classic mask manager's per-shape opacity/refinement
    (dt_masks_point_group_t.opacity/.refinement), which migration must carry
    over unchanged since it reuses the drawn group's own points verbatim."""
    rows = []
    rows.append((ids.circle, DT_MASKS_CIRCLE, "circle #1",
                 pack_circle(CIRCLE_CX, CIRCLE_CY, CIRCLE_R, CIRCLE_BORDER).hex(), 1))
    rows.append((ids.path, DT_MASKS_PATH, "square #1",
                 pack_path(SQUARE_CORNERS).hex(), len(SQUARE_CORNERS)))
    members = (pack_group_member(ids.circle, ids.group, circle_state,
                                 opacity=circle_opacity,
                                 **(circle_refine or {})) +
               pack_group_member(ids.path, ids.group, square_state))
    rows.append((ids.group, DT_MASKS_GROUP, "grp exposure", members.hex(), 2))
    return [(mask_num,) + r for r in rows]


# ---------------------------------------------------------------------------
# XMP assembly
# ---------------------------------------------------------------------------
PIPELINE = [
    # (num, operation, modversion, op_params_hex, multi_name)
    (0, "colorin", 7,
     "090000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
     ""),
    (1, "colorout", 5,
     "01000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
     ""),
    (2, "gamma", 1, "0000000000000000", ""),
    (3, "flip", 2, "ffffffff", "_builtin_auto"),
]

# dt_iop_exposure_params_t (modversion 6): mode(i) black(f) exposure(f)
# deflicker_percentile(f) deflicker_target_level(f) compensate_exposure_bias(i)
# +5EV so masked/unmasked regions are unmistakably different in the
# rendered output (a faint bump makes it hard to tell a real mask shape
# from a scenario that is accidentally a no-op).
EXPOSURE_PARAMS_HEX = struct.pack("<iffffi", 0, 0.0, 5.0, 50.0, -4.0, 0).hex()

DUMMY_HASH = "0" * 32


def _history_item(num, op, modversion, params_hex, blend_params_bytes,
                  enabled=True, multi_name=""):
    return f'''     <rdf:li
      darktable:num="{num}"
      darktable:operation="{op}"
      darktable:enabled="{1 if enabled else 0}"
      darktable:modversion="{modversion}"
      darktable:params="{params_hex}"
      darktable:multi_name="{multi_name}"
      darktable:multi_priority="0"
      darktable:blendop_version="14"
      darktable:blendop_params="{blend_params_bytes.hex()}"/>'''


# what masks v7 appended to a group member, left at its v6 meaning: refinement
# off, no name, group opacity 1, no preset note
_V7_NEUTRAL_TAIL = struct.pack("<i6f128sf64s", 0, 0, 0, 0, 0, 0, 0, b"", 1.0, b"")
_V6_MEMBER_SIZE = 16


def _as_classic(mask_type, points_hex):
    """A form in the format master writes (masks v6) whenever it carries
    nothing v7 added, so that a stock master build can render the fixture;
    otherwise the v7 form as packed. Only group points differ between the two."""
    if not mask_type & DT_MASKS_GROUP:
        return points_hex, DEVELOP_MASKS_VERSION - 1
    data = bytes.fromhex(points_hex)
    size = struct.calcsize(GROUP_MEMBER_FMT)
    members = [data[i:i + size] for i in range(0, len(data), size)]
    if any(m[_V6_MEMBER_SIZE:] != _V7_NEUTRAL_TAIL for m in members):
        return points_hex, DEVELOP_MASKS_VERSION
    return b"".join(m[:_V6_MEMBER_SIZE] for m in members).hex(), DEVELOP_MASKS_VERSION - 1


def build_xmp(name, blend_params_bytes, masks_rows, outdir=None,
              exposure_enabled=True, exposure_params_hex=None,
              extra_items=()):
    """extra_items: additional history entries appended *after* exposure, as
    (operation, modversion, params_hex, blend_params_bytes, enabled) tuples --
    used by the K series, where exposure is only the raster mask's producer and
    a later module is the masked consumer under test."""
    exposure_num = len(PIPELINE)
    history_end = exposure_num + 1 + len(extra_items)

    hist_items = []
    for (num, op, modv, params_hex, multi_name) in PIPELINE:
        hist_items.append(_history_item(num, op, modv, params_hex,
                                        pack_blend_params(), multi_name=multi_name))

    hist_items.append(_history_item(
        exposure_num, "exposure", 6,
        exposure_params_hex if exposure_params_hex is not None else EXPOSURE_PARAMS_HEX,
        blend_params_bytes, enabled=exposure_enabled))

    for i, (op, modv, params_hex, bp, enabled) in enumerate(extra_items):
        hist_items.append(_history_item(exposure_num + 1 + i, op, modv,
                                        params_hex, bp, enabled=enabled))

    masks_items = []
    for (mask_num, mask_id, mask_type, mask_name, points_hex, mask_nb) in masks_rows:
        points_hex, version = _as_classic(mask_type, points_hex)
        masks_items.append(f'''     <rdf:li
      darktable:mask_num="{mask_num}"
      darktable:mask_id="{mask_id}"
      darktable:mask_type="{mask_type}"
      darktable:mask_name="{mask_name}"
      darktable:mask_version="{version}"
      darktable:mask_points="{points_hex}"
      darktable:mask_nb="{mask_nb}"
      darktable:mask_src="0000000000000000"/>''')

    xmp = f'''<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="XMP Core 4.4.0-Exiv2">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:exif="http://ns.adobe.com/exif/1.0/"
    xmlns:xmp="http://ns.adobe.com/xap/1.0/"
    xmlns:xmpMM="http://ns.adobe.com/xap/1.0/mm/"
    xmlns:darktable="http://darktable.sf.net/"
   xmp:Rating="1"
   xmpMM:DerivedFrom="Sweep_sRGB_Linear_Half_Zip_01.tif"
   darktable:xmp_version="5"
   darktable:raw_params="0"
   darktable:auto_presets_applied="1"
   darktable:history_end="{history_end}"
   darktable:iop_order_version="2"
   darktable:history_auto_hash="{DUMMY_HASH}"
   darktable:history_current_hash="{DUMMY_HASH}">
   <darktable:masks_history>
    <rdf:Seq>
{chr(10).join(masks_items)}
    </rdf:Seq>
   </darktable:masks_history>
   <darktable:history>
    <rdf:Seq>
{chr(10).join(hist_items)}
    </rdf:Seq>
   </darktable:history>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
'''
    path = os.path.join(outdir or OUTDIR, f"{name}.xmp")
    with open(path, "w") as f:
        f.write(xmp)
    return path


# ---------------------------------------------------------------------------
# test matrix
# ---------------------------------------------------------------------------
def op_state(op_bit, invert=False):
    s = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | op_bit
    if invert:
        s |= DT_MASKS_STATE_INVERSE
    return s


SCENARIOS = []


def scenario(name, mask_mode, mask_combine=DEVELOP_COMBINE_NORM, mask_id=0,
             blendif=0, blendif_parameters=None, draw=None,
             circle_opacity=1.0, circle_refine=None):
    """draw: None, or (circle_op_bit_or_None, circle_invert, square_op_bit, square_invert)
    circle_opacity/circle_refine: see masks_history_rows"""
    SCENARIOS.append(dict(name=name, mask_mode=mask_mode, mask_combine=mask_combine,
                           mask_id=mask_id, blendif=blendif,
                           blendif_parameters=blendif_parameters, draw=draw,
                           circle_opacity=circle_opacity, circle_refine=circle_refine))


# A: drawn only, varying combine operator + invert
scenario("A1_union", DEVELOP_MASK_MASK,
          draw=(None, False, DT_MASKS_STATE_UNION, False))
scenario("A2_intersection", DEVELOP_MASK_MASK,
          draw=(None, False, DT_MASKS_STATE_INTERSECTION, False))
scenario("A3_difference", DEVELOP_MASK_MASK,
          draw=(None, False, DT_MASKS_STATE_DIFFERENCE, False))
scenario("A4_exclusion", DEVELOP_MASK_MASK,
          draw=(None, False, DT_MASKS_STATE_EXCLUSION, False))
scenario("A5_union_circle_inverted", DEVELOP_MASK_MASK,
          draw=(None, True, DT_MASKS_STATE_UNION, False))
scenario("A6_intersection_group_invert", DEVELOP_MASK_MASK,
          mask_combine=DEVELOP_COMBINE_INV,
          draw=(None, False, DT_MASKS_STATE_INTERSECTION, False))

# B: parametric only, varying channel count + polarity
_b1_blendif, _b1_params = channel_curve([CH_RED_in])
scenario("B1_1channel", DEVELOP_MASK_CONDITIONAL,
          blendif=_b1_blendif, blendif_parameters=_b1_params)

_b2_blendif, _b2_params = channel_curve([CH_RED_in, CH_GREEN_in])
scenario("B2_2channel", DEVELOP_MASK_CONDITIONAL,
          blendif=_b2_blendif, blendif_parameters=_b2_params)

_b3_blendif, _b3_params = channel_curve([CH_RED_in, CH_GREEN_in, CH_BLUE_in])
scenario("B3_3channel", DEVELOP_MASK_CONDITIONAL,
          blendif=_b3_blendif, blendif_parameters=_b3_params)

_b4_blendif, _b4_params = channel_curve([CH_RED_in], invert_channels=[CH_RED_in])
scenario("B4_1channel_inverted_polarity", DEVELOP_MASK_CONDITIONAL,
          blendif=_b4_blendif, blendif_parameters=_b4_params)

_b5_blendif, _b5_params = channel_curve([CH_GREEN_in, CH_BLUE_in])
scenario("B5_2channel_group_invert", DEVELOP_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_INV,
          blendif=_b5_blendif, blendif_parameters=_b5_params)

# C: drawn AND parametric combined
_c1_blendif, _c1_params = channel_curve([CH_RED_in, CH_GREEN_in])
scenario("C1_union_2channel", DEVELOP_MASK_MASK_CONDITIONAL,
          blendif=_c1_blendif, blendif_parameters=_c1_params,
          draw=(None, False, DT_MASKS_STATE_UNION, False))

_c2_blendif, _c2_params = channel_curve([CH_BLUE_in], invert_channels=[CH_BLUE_in])
scenario("C2_intersection_1channel_inverted", DEVELOP_MASK_MASK_CONDITIONAL,
          blendif=_c2_blendif, blendif_parameters=_c2_params,
          draw=(None, False, DT_MASKS_STATE_INTERSECTION, False))

_c3_blendif, _c3_params = channel_curve([CH_RED_in, CH_GREEN_in, CH_BLUE_in])
scenario("C3_difference_drawn_inverted_3channel", DEVELOP_MASK_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_MASKS_POS,
          blendif=_c3_blendif, blendif_parameters=_c3_params,
          draw=(None, False, DT_MASKS_STATE_DIFFERENCE, False))

_c4_blendif, _c4_params = channel_curve([CH_GRAY_in])
scenario("C4_union_1channel_group_invert", DEVELOP_MASK_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_INV,
          blendif=_c4_blendif, blendif_parameters=_c4_params,
          draw=(None, False, DT_MASKS_STATE_UNION, False))

# D: "no resolvable drawn content" combinations, which translate to a real
# classic formula (not always the
# degenerate "always zero" case D1 alone would suggest), see the derivation
# in the E-series below for the cases D-alone's naive treatment gets wrong.
_d1_blendif, _d1_params = channel_curve([CH_RED_in])
scenario("D1_maskspos_no_drawn_content", DEVELOP_MASK_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_MASKS_POS, mask_id=0,
          blendif=_d1_blendif, blendif_parameters=_d1_params, draw=None)

_d2_blendif, _d2_params = channel_curve([CH_GREEN_in])
scenario("D2_parametric_incl", DEVELOP_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_INCL,
          blendif=_d2_blendif, blendif_parameters=_d2_params)

# E: INCL x INV x MASKS_POS x content-resolvability cross-matrix.
#
# NOTE on what to expect: INCL XORs *every* channel's own polarity bit in
# the mask's colorspace before classic evaluates it (see
# migrate_legacy.c's _classify_conditional()). Since every scenario below
# activates only 1-2 of the colorspace's channels, that XOR always flags at
# least one untouched channel as "canceling", which makes classic
# wholesale-replace the whole mask with a flat constant (opaque or zero,
# picked by `incl != inv`) -- never a real shaped/curve mask. So *every*
# scenario in this block (E1-E6, plus D1/D2 above) is intentionally a hard
# constant, verified two ways: against a pristine (pre-migration) binary,
# and by the module_off/mask_disabled effect-check in verify_effect.sh,
# which confirms each one is bit-identical to one of those two baselines
# (not merely "faint"). Only a channel config that activates *every*
# channel of the colorspace simultaneously would escape this and produce a
# real (screen-like) mask -- that combination has no flexi equivalent and
# is intentionally left fail-closed (stays classic), so it isn't in this
# matrix at all.

# pure parametric, INCL set alone (no INV): constant, opaque (incl!=inv).
_e1_blendif, _e1_params = channel_curve([CH_RED_in])
scenario("E1_pure_incl_only", DEVELOP_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_INCL,
          blendif=_e1_blendif, blendif_parameters=_e1_params)

# pure parametric, INCL AND INV both set: constant, zero (incl==inv).
_e2_blendif, _e2_params = channel_curve([CH_RED_in])
scenario("E2_pure_incl_and_inv", DEVELOP_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_INCL | DEVELOP_COMBINE_INV,
          blendif=_e2_blendif, blendif_parameters=_e2_params)

# drawn (resolvable) + parametric, INCL set alone (no MASKS_POS, no INV):
# constant, opaque -- the canceling-channel constant-replace discards the
# drawn geometry too, not just the parametric curve.
_e3_blendif, _e3_params = channel_curve([CH_GREEN_in])
scenario("E3_content_incl_only", DEVELOP_MASK_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_INCL,
          blendif=_e3_blendif, blendif_parameters=_e3_params,
          draw=(None, False, DT_MASKS_STATE_UNION, False))

# drawn (resolvable) + parametric, INCL AND MASKS_POS both set: constant,
# opaque -- MASKS_POS plays no role in the canceling-channel classification
# (only INCL does), so this is the same outcome as E3.
_e4_blendif, _e4_params = channel_curve([CH_RED_in, CH_GREEN_in])
scenario("E4_content_incl_maskspos", DEVELOP_MASK_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_INCL | DEVELOP_COMBINE_MASKS_POS,
          blendif=_e4_blendif, blendif_parameters=_e4_params,
          draw=(None, False, DT_MASKS_STATE_UNION, False))

# drawn+parametric, NO resolvable drawn content, MASKS_POS AND INCL both
# set -- exactly the state reachable via one click of "invert all
# channel's polarities" followed by deleting the drawn mask. Delegates to
# the pure-parametric path (masks_pos == incl), which again lands on
# constant/opaque per the same canceling-channel rule as E1.
_e5_blendif, _e5_params = channel_curve([CH_BLUE_in])
scenario("E5_nocontent_maskspos_and_incl", DEVELOP_MASK_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_MASKS_POS | DEVELOP_COMBINE_INCL, mask_id=0,
          blendif=_e5_blendif, blendif_parameters=_e5_params, draw=None)

# drawn+parametric, NO resolvable drawn content, INCL set but MASKS_POS NOT
# set: constant, opaque (opposite of D1's constant/zero), regardless of
# the channel configuration.
_e6_blendif, _e6_params = channel_curve([CH_RED_in])
scenario("E6_nocontent_incl_only_opaque", DEVELOP_MASK_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_INCL, mask_id=0,
          blendif=_e6_blendif, blendif_parameters=_e6_params, draw=None)

# F: drawn (resolvable) + parametric, INCL set, with EVERY channel of the
# RGB-scene colorspace active at once (DEVELOP_BLENDIF_RGB_MASK's full
# channel set: GRAY/RED/GREEN/BLUE in+out, plus Jz/Cz/hz in+out) -- the one
# combination that reaches DT_COND_REAL despite INCL (no channel is left
# untouched for INCL's polarity-XOR to flag as canceling). Classic's formula
# there is 1-(1-d)*temp (INV=0, F1) or
# (1-d)*temp (INV=1, F2) -- see the DT_COND_REAL/INCL derivation in
# migrate_legacy.c's _migrate_drawn_and_parametric().
# must match DEVELOP_BLENDIF_RGB_MASK (src/develop/blend.h) exactly: bits
# 0-10, 12-14 (11 and 15 unused/reserved).
_RGB_ALL_CHANNELS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14]
assert sum(1 << c for c in _RGB_ALL_CHANNELS) == 0x77FF

_f_blendif, _f_params = channel_curve(_RGB_ALL_CHANNELS)
scenario("F1_content_incl_allchannels_noinv", DEVELOP_MASK_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_INCL,
          blendif=_f_blendif, blendif_parameters=_f_params,
          draw=(None, False, DT_MASKS_STATE_UNION, False))

scenario("F2_content_incl_allchannels_inv", DEVELOP_MASK_MASK_CONDITIONAL,
          mask_combine=DEVELOP_COMBINE_INCL | DEVELOP_COMBINE_INV,
          blendif=_f_blendif, blendif_parameters=_f_params,
          draw=(None, False, DT_MASKS_STATE_UNION, False))

# G: bare DEVELOP_MASK_ENABLED (classic "uniform", no MASK/CONDITIONAL/
# RASTER bit at all), which renders as an empty flexi group: migration
# normalizes it to ENABLED|FLEXI, so that no raw classic value is left in
# blend_params. Passing mask_mode=0 to scenario() below yields
# exactly DEVELOP_MASK_ENABLED once ORed with it in main().
scenario("G1_bare_uniform", 0)

# H: per-shape opacity + refinement (dt_masks_point_group_t.opacity/
# .refinement) on the drawn circle, as a classic edit carries them.
# Migration reuses the drawn group's own points verbatim (see
# _migrate_drawn_and_parametric's "MASKS_POS moves onto the wrapper entry"
# comment), so these must survive completely unchanged -- both drawn-only
# (no rebuild at all involved) and drawn+parametric (the group gets a new
# *wrapper* point referencing it, but the drawn group's own points are still
# untouched).
_h_refine = dict(refine_enabled=1, details=0.3, feathering_radius=12.0,
                 blur_radius=8.0, contrast=0.25, brightness=-0.15)
scenario("H1_drawn_opacity_refinement", DEVELOP_MASK_MASK,
          draw=(None, False, DT_MASKS_STATE_UNION, False),
          circle_opacity=0.6, circle_refine=_h_refine)

_h2_blendif, _h2_params = channel_curve([CH_RED_in, CH_GREEN_in])
scenario("H2_combined_opacity_refinement", DEVELOP_MASK_MASK_CONDITIONAL,
          blendif=_h2_blendif, blendif_parameters=_h2_params,
          draw=(None, False, DT_MASKS_STATE_UNION, False),
          circle_opacity=0.6, circle_refine=_h_refine)


# I: per-member operator application -- the property the classic sequential
# fold has and a naive run fold does not.
#
# Classic walks the member list applying each member's OWN operator to the
# accumulator, once per member. The flexi fold partitions the list into runs
# and applies the run's operator once per RUN. For union those agree (max is
# idempotent), for everything else they do not, so migration gives every
# non-union member its own group -- _normalize_group() in migrate_legacy.c,
# whose comment records a real 48-brush mask that reached 0.6202 under classic
# and 0.1723 without the split.
#
# Nothing else in this matrix pins that in pixels: the A series uses one
# operator-carrying member, where per-member and per-run application coincide.
# These two use two members over the SAME overlapping circle+square geometry:
#
#   I1  intersect: classic applies the bottom member's operator too, to an
#       empty mask, so both members leave nothing and the mask is empty. The
#       flexi fold would copy the bottom member; migration keeps both as
#       zero-opacity unions instead (_zero_empty_base_members).
#   I2  sum: the operator that compounds per application, so a merged run
#       under-composites exactly the way the brush mask above did.
#
# Both are ordinary classic-authorable configurations, so unlike the J series
# below they also pass --verify-masks (see its note there).
def build_operator_chain_scenarios():
    """Two members over the shared circle+square geometry, both carrying the
    same non-union operator, so each becomes its own run."""
    written = []
    for name, op_bit in (("I1_intersection_chain", DT_MASKS_STATE_INTERSECTION),
                         ("I2_sum_chain", DT_MASKS_STATE_SUM)):
        ids = MaskIds(999000 if op_bit == DT_MASKS_STATE_INTERSECTION else 999200)
        op = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | op_bit
        members = (pack_group_member(ids.circle, ids.group, op)
                   + pack_group_member(ids.path, ids.group, op))
        masks_rows = [
            (ids.circle, DT_MASKS_CIRCLE, "circle #1",
             pack_circle(CIRCLE_CX, CIRCLE_CY, CIRCLE_R, CIRCLE_BORDER).hex(), 1),
            (ids.path, DT_MASKS_PATH, "square #1",
             pack_path(SQUARE_CORNERS).hex(), len(SQUARE_CORNERS)),
            (ids.group, DT_MASKS_GROUP, "grp exposure", members.hex(), 2),
        ]
        exposure_num = len(PIPELINE)
        masks_rows = [(exposure_num,) + r for r in masks_rows]

        bp = pack_blend_params(
            mask_mode=DEVELOP_MASK_MASK | DEVELOP_MASK_ENABLED,
            blend_cst=DEVELOP_BLEND_CS_RGB_SCENE,
            mask_combine=DEVELOP_COMBINE_NORM,
            mask_id=ids.group,
            blendif=0,
        )

        written.append((name, build_xmp(name, bp, masks_rows)))
    return written


# ---------------------------------------------------------------------------
# J: mask refinement at each of its three scopes.
#
# The same refinement fields (details / feathering / blur / contrast /
# brightness) can be applied at three different points in the fold, and the
# whole point of the scope enum is that these are NOT interchangeable:
#
#   ELEMENT (dt_masks_point_group_t.refinement, enabled=1)
#       applied to one member's own mask, before it composites into its group
#   GROUP   (same storage, enabled=2, broadcast onto every member of the run)
#       applied once to the group's finished sub-mask, after its members fold
#   GLOBAL  (dt_develop_blend_params_t.blur_radius etc.)
#       applied once to the whole mask, after every group has composited
#
# Blur is used as the probe because it is purely spatial and needs no guide
# image or detail (scharr) buffer, so the scenarios stay deterministic and
# cheap. Order matters for it: blurring two shapes and then unioning them is
# not the same image as unioning them and then blurring, which is exactly what
# makes J4 (element on both) and J5 (group) distinguishable. If those two ever
# render identically, a scope has stopped being honoured -- see
# _group_get_mask_roi_flexi in src/develop/masks/group.c.
#
# NOTE on --verify-masks: this series is deliberately NOT classic-authorable.
# DT_MASKS_REFINE_GROUP is a flexi concept, and the whole per-shape refinement
# block arrived with masks v7 (v6 has no refinement field at all), so no
# classic edit can carry scope=2. The classic fold reads the field as a plain
# bool (`if(fpt->refinement.enabled)` in masks/group.c), applying group-scope
# refinement per element, while the flexi fold applies it once per group -- so
# harvesting these XMPs and running --verify-masks over them reports J5, J6 and
# J7 as DIFFERENT. That is the two folds disagreeing about an input classic
# cannot produce, not a migration defect; the A-I and K series are ordinary
# classic configurations and do verify clean.
#
# J2 vs J3: group-scope refinement is stored broadcast, so a renderer that
# read the run head's copy unconditionally and applied it group-wide would
# leak the head's ELEMENT refinement over its whole group and drop every
# other member's. They must differ, and neither may equal J4.
# ---------------------------------------------------------------------------
_J_BLUR = 9.0        # gaussian blur radius on the mask
_J_CONTRAST = 0.35   # mask contrast, to make the blur's effect easier to see


def _j_refine(scope):
    return dict(refine_enabled=scope, blur_radius=_J_BLUR, contrast=_J_CONTRAST)


def build_refinement_scenarios():
    """One union group of two overlapping shapes (the shared circle+square
    geometry), rendered with refinement applied at each scope in turn."""
    written = []
    base = 998000
    cases = [
        # (name, circle_refine_scope, square_refine_scope, global_kwargs)
        ("J1_refine_global", None, None,
         dict(blur_radius=_J_BLUR, contrast=_J_CONTRAST)),
        ("J2_refine_element_head", DT_MASKS_REFINE_ELEMENT, None, {}),
        ("J3_refine_element_tail", None, DT_MASKS_REFINE_ELEMENT, {}),
        ("J4_refine_element_both", DT_MASKS_REFINE_ELEMENT,
         DT_MASKS_REFINE_ELEMENT, {}),
        ("J5_refine_group", DT_MASKS_REFINE_GROUP, DT_MASKS_REFINE_GROUP, {}),
        # group + global stacked. NB: element and group scope cannot coexist on
        # one run -- setting group scope broadcasts one refinement onto every
        # member, overwriting their element ones, so a mixed run is not a state
        # the GUI can produce. Marking the head ELEMENT and the tail GROUP
        # would test nothing: group scope is read off the run head, so the
        # tail's marking would be inert.
        ("J6_refine_group_and_global", DT_MASKS_REFINE_GROUP,
         DT_MASKS_REFINE_GROUP,
         dict(blur_radius=_J_BLUR, contrast=_J_CONTRAST)),
    ]
    for i, (name, circle_scope, square_scope, global_kw) in enumerate(cases):
        ids = MaskIds(base + i * 10)
        # the head of the run is the first member in points order (the circle);
        # both members carry the same union operator so they form one group
        op = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION
        members = (
            pack_group_member(ids.circle, ids.group, op,
                               **(_j_refine(circle_scope) if circle_scope else {}))
            + pack_group_member(ids.path, ids.group, op,
                                    **(_j_refine(square_scope) if square_scope else {}))
        )
        masks_rows = [
            (ids.circle, DT_MASKS_CIRCLE, "circle #1",
             pack_circle(CIRCLE_CX, CIRCLE_CY, CIRCLE_R, CIRCLE_BORDER).hex(), 1),
            (ids.path, DT_MASKS_PATH, "square #1",
             pack_path(SQUARE_CORNERS).hex(), len(SQUARE_CORNERS)),
            (ids.group, DT_MASKS_GROUP, "grp exposure", members.hex(), 2),
        ]
        exposure_num = len(PIPELINE)
        masks_rows = [(exposure_num,) + r for r in masks_rows]

        bp = pack_blend_params(
            mask_mode=DEVELOP_MASK_MASK | DEVELOP_MASK_ENABLED,
            blend_cst=DEVELOP_BLEND_CS_RGB_SCENE,
            mask_combine=DEVELOP_COMBINE_NORM,
            mask_id=ids.group,
            blendif=0,
            **global_kw,
        )

        written.append((name, build_xmp(name, bp, masks_rows)))

    # Two groups, refinement on the FIRST group only, at group scope (J7) and
    # at global scope (J8) with identical values. Necessary because with a
    # single group the two scopes coincide exactly -- the base group seeds the
    # accumulator directly, so "this group's finished sub-mask" and "the whole
    # mask" are the same buffer, and J1 renders pixel-identical to J5. Only
    # once a second group composites on top does group scope become
    # observable: J7 refines group A alone and then sums B onto the result,
    # J8 refines the sum of both. If J7 and J8 ever match, group-scope
    # refinement has collapsed into the global pass.
    ids = MaskIds(base + 900)
    circleB_id, squareB_id = ids.circle + 100, ids.path + 100
    op = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION
    op_sum = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | DT_MASKS_STATE_SUM
    shape_rows = [
        (ids.circle, DT_MASKS_CIRCLE, "circle #1",
         pack_circle(CIRCLE_CX, CIRCLE_CY, CIRCLE_R, CIRCLE_BORDER).hex(), 1),
        (ids.path, DT_MASKS_PATH, "square #1",
         pack_path(SQUARE_CORNERS).hex(), len(SQUARE_CORNERS)),
        (circleB_id, DT_MASKS_CIRCLE, "circle #2",
         pack_circle(CIRCLE2_CX, CIRCLE2_CY, CIRCLE2_R, CIRCLE2_BORDER).hex(), 1),
        (squareB_id, DT_MASKS_PATH, "square #2",
         pack_path(SQUARE2_CORNERS).hex(), len(SQUARE2_CORNERS)),
    ]
    for name, group_scoped in (("J7_refine_group_of_two", True),
                               ("J8_refine_global_of_two", False)):
        # group A carries the refinement broadcast on both its members (the
        # renderer reads it off the run head); group B carries none, and sums
        # onto A, since a classic list only splits a run where the operator
        # changes.
        ref = _j_refine(DT_MASKS_REFINE_GROUP) if group_scoped else {}
        members = (
            pack_group_member(ids.circle, ids.group, op, **ref)
            + pack_group_member(ids.path, ids.group, op, **ref)
            + pack_group_member(circleB_id, ids.group, op_sum)
            + pack_group_member(squareB_id, ids.group, op_sum)
        )
        exposure_num = len(PIPELINE)
        masks_rows = [(exposure_num,) + r for r in shape_rows]
        masks_rows.append((exposure_num, ids.group, DT_MASKS_GROUP,
                            "grp exposure", members.hex(), 4))
        global_kw = ({} if group_scoped
                     else dict(blur_radius=_J_BLUR, contrast=_J_CONTRAST))
        bp = pack_blend_params(
            mask_mode=DEVELOP_MASK_MASK | DEVELOP_MASK_ENABLED,
            blend_cst=DEVELOP_BLEND_CS_RGB_SCENE,
            mask_combine=DEVELOP_COMBINE_NORM,
            mask_id=ids.group,
            blendif=0,
            **global_kw,
        )
        written.append((name, build_xmp(name, bp, masks_rows)))

    return written


# ---------------------------------------------------------------------------
# K: raster masks (DEVELOP_MASK_RASTER).
#
# The only classic mask mode the rest of this matrix never touches, and the one
# whose classic configuration lives *entirely* outside the form tree -- four
# scalars on blend_params (raster_mask_source / _instance / _id / _invert)
# which migration has to synthesize a DT_MASKS_RASTER form element from (see
# _migrate_raster() in migrate_legacy.c). Nothing about that synthesis is
# exercised by a single-module scenario, because a raster mask needs two
# modules: a producer and a consumer.
#
# Pipeline shape, mirroring the real-world case in
# src/tests/integration/0167-raster-mask (colorbalancergb publishes, bilat
# consumes):
#
#   exposure    at 0 EV, carrying the mask -- the *producer*. 0 EV so it is a
#               pixel no-op and the only visible effect in the frame is the
#               consumer's, gated by the raster mask. Its own mask is what gets
#               published.
#   monochrome  the *consumer*: mask_mode = RASTER|ENABLED, raster_mask_source
#               = "exposure". Full desaturation is unmistakable on the colour
#               sweep, so a wrongly-shaped mask is visible rather than subtle.
#
# monochrome sits well after exposure in the v2 iop order (exposure runs before
# colorin; monochrome between colorize and grain), which is required -- a
# raster mask can only be consumed downstream of its producer.
#
# These need their own baselines: ZBASE_module_off / ZBASE_mask_disabled
# describe a masked *exposure*, which is not what varies here. See
# build_raster_baselines() below.
# ---------------------------------------------------------------------------
# dt_iop_exposure_params_t at 0 EV: identical to EXPOSURE_PARAMS_HEX but for
# the gain, so the producer contributes no pixels of its own.
EXPOSURE_NEUTRAL_PARAMS_HEX = struct.pack("<iffffi", 0, 0.0, 0.0, 50.0, -4.0, 0).hex()

# dt_iop_monochrome_params_t (modversion 2): a(f) b(f) size(f) highlights(f)
MONOCHROME_PARAMS_HEX = struct.pack("<4f", 0.0, 0.0, 2.0, 0.0).hex()
MONOCHROME_MODVERSION = 2


def _monochrome_item(bp, enabled=True):
    return ("monochrome", MONOCHROME_MODVERSION, MONOCHROME_PARAMS_HEX, bp, enabled)


def _raster_producer_bp(mask_id, blendif=0, blendif_parameters=None,
                        mask_mode=DEVELOP_MASK_MASK,
                        mask_combine=DEVELOP_COMBINE_NORM):
    return pack_blend_params(
        mask_mode=mask_mode | DEVELOP_MASK_ENABLED,
        blend_cst=DEVELOP_BLEND_CS_RGB_SCENE,
        mask_combine=mask_combine,
        mask_id=mask_id,
        blendif=blendif,
        blendif_parameters=blendif_parameters,
    )


def _raster_consumer_bp(source=b"exposure", invert=0):
    return pack_blend_params(
        mask_mode=DEVELOP_MASK_RASTER | DEVELOP_MASK_ENABLED,
        blend_cst=DEVELOP_BLEND_CS_RGB_SCENE,
        mask_combine=DEVELOP_COMBINE_NORM,
        mask_id=0,
        blendif=0,
        raster_mask_source=source,
        raster_mask_instance=0,
        raster_mask_id=0,     # BLEND_RASTER_ID: the producer's own blend mask
        raster_mask_invert=invert,
    )


def build_raster_scenarios():
    written = []
    base = 997000
    last_num = len(PIPELINE) + 1   # the monochrome item: where mask rows live

    def drawn_rows(ids):
        op = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE
        return masks_history_rows(last_num, ids,
                                  circle_state=op,
                                  square_state=op | DT_MASKS_STATE_UNION)

    # K1: the base case -- a drawn union mask published by exposure and
    # consumed by monochrome. Migration must turn the consumer's four scalars
    # into a one-element group holding a DT_MASKS_RASTER form that resolves
    # back to the same producer.
    ids = MaskIds(base)
    written.append(("K1_raster_from_drawn", build_xmp(
        "K1_raster_from_drawn", _raster_producer_bp(ids.group), drawn_rows(ids),
        exposure_params_hex=EXPOSURE_NEUTRAL_PARAMS_HEX,
        extra_items=[_monochrome_item(_raster_consumer_bp())])))

    # K1C: the control, and the reason K1 proves equivalence rather than merely
    # locking in today's pixels. Same producer, but the consumer carries the
    # same geometry as its OWN drawn mask instead of consuming a raster one.
    # "Consume X as a raster mask" and "have X as your own drawn mask" must
    # render identically; run.sh asserts K1 == K1C. Without this, the K series
    # would only be a snapshot of current behaviour, since the regression mode
    # compares against checked-in PNGs generated by the migrating binary.
    ids_c = MaskIds(base + 100)
    rows_c = drawn_rows(ids_c)
    ids_c2 = MaskIds(base + 200)
    rows_c += masks_history_rows(last_num, ids_c2,
                                 circle_state=DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE,
                                 square_state=DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE
                                 | DT_MASKS_STATE_UNION)
    written.append(("K1C_drawn_control", build_xmp(
        "K1C_drawn_control", _raster_producer_bp(ids_c.group), rows_c,
        exposure_params_hex=EXPOSURE_NEUTRAL_PARAMS_HEX,
        extra_items=[_monochrome_item(_raster_producer_bp(ids_c2.group))])))

    # K2: raster_mask_invert. Classic stores the inversion in its own scalar;
    # the flexi element has no such field, so migration has to move it onto the
    # member's DT_MASKS_STATE_INVERSE bit. test_raster_inversion_moves_onto_the
    # _state_bit checks that structurally -- this checks it in pixels, which is
    # where an inversion that lands on the wrong side actually shows up.
    ids2 = MaskIds(base + 300)
    written.append(("K2_raster_inverted", build_xmp(
        "K2_raster_inverted", _raster_producer_bp(ids2.group), drawn_rows(ids2),
        exposure_params_hex=EXPOSURE_NEUTRAL_PARAMS_HEX,
        extra_items=[_monochrome_item(_raster_consumer_bp(invert=1))])))

    # K2C: the same control for the inverted case, and the more valuable of the
    # two -- the consumer owns the geometry and inverts it the classic drawn
    # way (DEVELOP_COMBINE_MASKS_POS, see dt_develop_blend_process). K2 arrives at the same
    # picture by a completely different route: raster_mask_invert becomes
    # DT_MASKS_STATE_INVERSE on the synthesized element. run.sh asserts
    # K2 == K2C, which is what catches an inversion applied at the wrong level
    # of the fold -- the failure mode a structural test cannot see.
    ids2c = MaskIds(base + 500)
    rows_2c = drawn_rows(ids2c)
    ids2c2 = MaskIds(base + 600)
    rows_2c += masks_history_rows(last_num, ids2c2,
                                  circle_state=DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE,
                                  square_state=DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE
                                  | DT_MASKS_STATE_UNION)
    written.append(("K2C_drawn_inverted_control", build_xmp(
        "K2C_drawn_inverted_control", _raster_producer_bp(ids2c.group), rows_2c,
        exposure_params_hex=EXPOSURE_NEUTRAL_PARAMS_HEX,
        extra_items=[_monochrome_item(_raster_producer_bp(
            ids2c2.group, mask_combine=DEVELOP_COMBINE_MASKS_POS))])))

    # K3: the producer's own mask is parametric, so BOTH ends migrate -- the
    # producer's classic blendif becomes DT_MASKS_PARAMETRIC forms, and the
    # consumer must still receive the same published mask afterwards. This is
    # the shape 0167-raster-mask uses, and the case where a migration that
    # changes what the producer publishes would go unnoticed by any
    # single-module scenario.
    _k3_blendif, _k3_params = channel_curve([CH_RED_in, CH_GREEN_in])
    written.append(("K3_raster_from_parametric", build_xmp(
        "K3_raster_from_parametric",
        _raster_producer_bp(0, blendif=_k3_blendif, blendif_parameters=_k3_params,
                            mask_mode=DEVELOP_MASK_CONDITIONAL),
        [],
        exposure_params_hex=EXPOSURE_NEUTRAL_PARAMS_HEX,
        extra_items=[_monochrome_item(_raster_consumer_bp())])))

    # K4: a consumer whose producer is not in the pipeline at all -- what a
    # user gets by deleting the source module. dt_masks_raster_is_unresolved()
    # has four structural tests; this pins the rendered result, an all-zero
    # mask (raster.c's _raster_unresolved), so the consumer does nothing.
    ids4 = MaskIds(base + 400)
    written.append(("K4_raster_source_missing", build_xmp(
        "K4_raster_source_missing", _raster_producer_bp(ids4.group), drawn_rows(ids4),
        exposure_params_hex=EXPOSURE_NEUTRAL_PARAMS_HEX,
        extra_items=[_monochrome_item(
            _raster_consumer_bp(source=b"colorbalancergb"))])))

    return written


BASELINE_DIR = os.path.join(os.path.dirname(__file__), "baselines")


def build_baselines():
    """Two reference renders used by verify_effect.sh to confirm each
    scenario's mask actually has a spatial effect (as opposed to being an
    accidental no-op): the exposure module fully disabled (a scenario that
    collapses to this is a hard "always zero" mask), and the module fully
    enabled with mask_mode = DEVELOP_MASK_ENABLED / no MASK or CONDITIONAL
    bits (uniform full-frame effect -- a scenario that collapses to this is
    a hard "always opaque" mask, not a real shaped/curve one)."""
    os.makedirs(BASELINE_DIR, exist_ok=True)

    build_xmp("ZBASE_module_off", pack_blend_params(), [],
               outdir=BASELINE_DIR, exposure_enabled=False)

    bp_full = pack_blend_params(
        mask_mode=DEVELOP_MASK_ENABLED,
        blend_cst=DEVELOP_BLEND_CS_RGB_SCENE,
        mask_combine=DEVELOP_COMBINE_NORM,
        mask_id=0,
        blendif=0,
    )
    build_xmp("ZBASE_mask_disabled", bp_full, [], outdir=BASELINE_DIR)


def build_raster_baselines():
    """The K series' own pair of references. The A-J baselines vary a masked
    *exposure*; in the K series exposure is only the producer (0 EV, a pixel
    no-op) and what varies is a masked monochrome downstream of it, so those
    baselines classify nothing there. Same two poles, same pipeline: the
    consumer off ("mask is always zero") and the consumer on with a uniform
    mask ("mask is always opaque")."""
    os.makedirs(BASELINE_DIR, exist_ok=True)
    last_num = len(PIPELINE) + 1
    ids = MaskIds(996000)
    op = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE
    rows = masks_history_rows(last_num, ids, circle_state=op,
                              square_state=op | DT_MASKS_STATE_UNION)

    uniform_bp = pack_blend_params(
        mask_mode=DEVELOP_MASK_ENABLED,
        blend_cst=DEVELOP_BLEND_CS_RGB_SCENE,
        mask_combine=DEVELOP_COMBINE_NORM, mask_id=0, blendif=0)

    for name, (bp, enabled) in (
            ("ZBASE_raster_sink_off", (uniform_bp, False)),
            ("ZBASE_raster_sink_uniform", (uniform_bp, True))):
        build_xmp(name, _raster_producer_bp(ids.group), rows,
                  outdir=BASELINE_DIR,
                  exposure_params_hex=EXPOSURE_NEUTRAL_PARAMS_HEX,
                  extra_items=[_monochrome_item(bp, enabled=enabled)])


def main():
    base_id = 100000
    generated = []
    for i, sc in enumerate(SCENARIOS):
        ids = MaskIds(base_id + i * 10)
        masks_rows = []
        mask_id = sc["mask_id"]
        if sc["draw"] is not None:
            _, circle_inv, square_op, square_inv = sc["draw"]
            circle_state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE
            if circle_inv:
                circle_state |= DT_MASKS_STATE_INVERSE
            square_state = op_state(square_op, square_inv)
            exposure_num = len(PIPELINE)
            masks_rows = masks_history_rows(exposure_num, ids, circle_state, square_state,
                                            circle_opacity=sc["circle_opacity"],
                                            circle_refine=sc["circle_refine"])
            mask_id = ids.group

        bp = pack_blend_params(
            mask_mode=sc["mask_mode"] | DEVELOP_MASK_ENABLED,
            blend_cst=DEVELOP_BLEND_CS_RGB_SCENE,
            mask_combine=sc["mask_combine"],
            mask_id=mask_id,
            blendif=sc["blendif"],
            blendif_parameters=sc["blendif_parameters"],
        )
        path = build_xmp(sc["name"], bp, masks_rows)
        generated.append(sc["name"])
        print(f"wrote {path}")

    for name, path in build_operator_chain_scenarios():
        generated.append(name)
        print(f"wrote {path}")

    for name, path in build_refinement_scenarios():
        generated.append(name)
        print(f"wrote {path}")

    for name, path in build_raster_scenarios():
        generated.append(name)
        print(f"wrote {path}")

    print(f"\n{len(generated)} scenarios generated: {', '.join(generated)}")

    build_baselines()
    build_raster_baselines()
    print(f"baselines written to {BASELINE_DIR}")


if __name__ == "__main__":
    main()
