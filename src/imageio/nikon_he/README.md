# Nikon HE codec

This directory contains the Nikon High Efficiency / High Efficiency* codec
compiled directly into darktable. It uses the C++ standard library and
darktable's existing RawSpeed dependency. It does not link to LibRaw or
require another package.

The codec sources were imported from
[pdcgomes/redlamp-libraw at 6c5f4637a73e69074119ed0c38662027bca84982](https://github.com/pdcgomes/redlamp-libraw/tree/6c5f4637a73e69074119ed0c38662027bca84982/src/decoders/nikon_he).
That version incorporates the decoder by Dmitri Sotnikov, Yogthos and
Jiang Yaogeng from [LibRaw PR 826](https://github.com/LibRaw/LibRaw/pull/826),
RdWing's decoding fixes, and Redlamp's fixes for stream layout, input
validation and concurrent decoding. Original copyright and license notices
are preserved. Darktable uses the LGPL-2.1 option; copies of both licenses
offered by the original sources are included here.

Changes to the imported codec:

- Use owned vectors for per-band prediction and precinct scratch storage.
- Keep only three tiles of coefficients and reconstructed rows, and write
  directly into RawSpeed's strided image buffer.
- Validate the component table, supported transform and vendor parameters,
  slice sequence, precinct sizes and end marker before decoding.
- Map the second LL line to its component's prediction-mode and weight entries.
- Pass each picture's weights explicitly, removing thread-local state and
  the unused empirical GTLI table.
- Reject entropy substreams that run out of bits instead of padding them
  into a partial image.
- Remove unused local variables rejected by darktable's compiler warnings.
- Omit the unused approximate transfer-curve table. The integration uses
  the exact transfer parameters from each picture header.

The exact transfer curve in `nikon_he_picture_header.cpp` is adapted from
the same revision's `nikon_he_decoder.cpp`. `../imageio_nikon_he.cc` selects
a RawDecoder implementation for Z5II HE/HE* files. Its normal `decodeRaw()`
wrapper applies pixel-aspect hints, bad-pixel correction and initialization
checks. The original NEF decoder handles camera support and metadata,
including white balance, crop, sensor levels and the color matrix. Neither
RawSpeed nor LibRaw source files or dependency revisions are modified.

See `dev-doc/Nikon_Z5II.md` for build and regression checks, including the
independent Adobe DNG Converter comparison. This is a fixed Nikon profile,
not a general JPEG XS implementation. The supported profile and its vendor
differences are described there.
