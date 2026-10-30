# Nikon Z5II High Efficiency NEFs

Darktable decodes the Z5II's 14-bit High Efficiency and High Efficiency*
NEFs with an in-tree codec in `src/imageio/nikon_he`. It adds no external
dependencies and leaves every submodule URL and revision unchanged.
The codec works with `USE_LIBRAW=ON` or `USE_LIBRAW=OFF`, including builds
that use a system LibRaw.

## Decoding and metadata

RawSpeed parses the NEF container. `imageio_nikon_he.cc` selects a decoder
for make/model `NIKON CORPORATION` / `NIKON Z5_2` and the Nikon RawImageInfo
maker-note block (tag `0x0051`). Its short at byte offset 10 identifies HE
as `13` and HE* as `14` (`0x0e`). This is separate from TIFF Compression,
which remains `34713`.

The selected decoder shares the original NEF decoder's `RawImage` and
delegates camera support and metadata handling to it. RawSpeed's normal
`decodeRaw()` wrapper still performs initialization checks, pixel-aspect
handling and bad-pixel correction. Its existing metadata code applies
white balance, crop, black/white levels and the color matrix from
`data/cameras.xml`. There is no separate hardcoded camera matrix.

Only Z5II HE/HE* images use the native codec. Lossless NEFs and other camera
models retain their original decoders. Nikon's existing `libraw_extensions`
override behavior is unchanged.

The codec writes the full 14-bit RGGB Bayer mosaic directly into RawSpeed's
strided buffer. It retains three tiles of coefficients and three tiles of
intermediate rows for wavelet reconstruction. Scratch storage is bounded
by image width instead of image area. Each decode owns its prediction and
scratch state; only the immutable prediction lookup table is shared.

## Supported codestream

This is a decoder for the observed Nikon profile, not a general JPEG XS
implementation. Unsupported transforms or header extensions are rejected.
The container must have one 14-bit strip, a width divisible by eight in
64..6064, and a height divisible by four in 64..4040. These bounds include
the Z5II's full-frame, DX, square and 16:9 modes.

The codestream contains SOC, the Nikon CAP signature, PIH, CDT, WGT, slice
headers and precincts, then EOC. The picture length, marker order, slice
indices, packet bounds and final EOC must agree with the strip. Its profile
uses four components, five horizontal and one vertical decomposition
levels, four coefficients per group, eight groups per significance block,
18-bit reconstructed values, uniform quantization and separate sign bits.
The fixed profile bytes are `44 10 51 14`.

Nikon extends PIH with 13 bytes of vendor parameters and CDT with a third
byte per component. Every component is 14-bit and sampled at 1:1; component
2 is undecomposed and the other three use five horizontal and one vertical
levels. The 25 WGT entries supply gains and unique priorities. The internal
26-band layout includes a second line of component 2, which shares its
WGT and prediction-mode entries with the first line. Raw-GCLI packets use
four-bit absolute values; other packets use the supported zero/vertical
prediction modes. Unimplemented prediction modes are rejected.

The PIH vendor tail supplies the exact transfer curve. With integer input
`x` clamped to 0..65535 and tail bytes `t[0..12]`:

```text
B = ((t[3] & 15) << 8) | t[4]
A = ((t[7] & 3) << 16) | (t[8] << 8) | t[9]
C = (t[10] << 8) | t[11]
d = x - C
s = floor((d * A + 32768) / 65536)
q = d < 0 ? -d * d : s * s
raw = clamp(B + floor((q + 163840) / 262144), 0, 16383)
```

The arithmetic uses 64-bit intermediates. Other vendor bits must match
the supported profile. This avoids silently ignoring parameters whose
effect on reconstruction is unknown.

The implementation was adapted from
[Redlamp LibRaw 6c5f4637a73e69074119ed0c38662027bca84982](https://github.com/pdcgomes/redlamp-libraw/commit/6c5f4637a73e69074119ed0c38662027bca84982),
which incorporates the community decoder in
[LibRaw PR 826](https://github.com/LibRaw/LibRaw/pull/826).
That PR was not accepted into official LibRaw. RdWing's review documents
the [exact transfer curve](https://github.com/LibRaw/LibRaw/pull/826#issuecomment-5639699213),
[bottom-row reconstruction](https://github.com/LibRaw/LibRaw/pull/826#issuecomment-5639654673)
and [raw GCLI handling](https://github.com/LibRaw/LibRaw/pull/826#issuecomment-5639783552).
See `src/imageio/nikon_he/README.md` for attribution, licenses and local
changes. Those references describe the source of this implementation;
independent pixel comparisons are described below.

## Regression checks

Configure darktable with `-DBUILD_TESTING=ON`, then run:

```sh
python3 src/tests/download_nikon_z5ii_samples.py ./nikon-samples
export NIKON_Z5II_SAMPLE_DIR="$PWD/nikon-samples"
cmake --build build --target darktable-test-nikon-he darktable-test-nikon-z5ii
ctest --test-dir build -R '^imageio_nikon_' --output-on-failure
```

The downloader verifies the eleven CC0 raw.pixls.us files against
`src/tests/nikon_z5ii_samples.json`. The sample test checks every photosite
of eight HE/HE* files, dimensions, compression, camera calibration and white
balance. Hashes cover unsigned 16-bit little-endian photosites without row
padding. Three lossless NEFs exercise the original decoder. Concurrent
decodes exercise different dimensions and HE profiles. Malformed-input
checks cover truncated input, header/component mutations, empty precincts,
packet bounds, dimension mismatches and damaged EOC. A lifecycle test
checks pixel-aspect hints and bad-pixel correction.

The sample test is skipped if its environment variable is absent. The core
unit test always runs: it checks synthetic raw-GCLI streams, transform and
transfer vectors, malformed headers, coefficient bounds, output padding,
and partial final tiles (including one-precinct tails). Linux CI downloads
the samples for the existing unit-test job; the standalone core tests also
run on Windows, Linux and macOS.

Repeat the application build and tests with `-DUSE_LIBRAW=OFF`. Exercise
HE, HE*, all four image areas, and lossless NEFs through the application:

```sh
darktable-cli sample.NEF sample.ppm --width 1280 --height 1280 \
  --apply-custom-presets false --core --configdir ./test-config \
  --cachedir ./test-cache --disable-opencl
```

The PPM export is uncompressed. Use isolated configuration/cache directories;
the CLI uses an in-memory library and disables sidecar writing.

## Independent pixel reference

The eight public HE/HE* golden hashes were independently verified against
Adobe DNG Converter 18.7 on Windows, using `-u -p0` to produce uncompressed
DNGs without JPEG previews. An additional 162 private Z5II NEFs matched.
Every full Bayer mosaic matched exactly, before crop or demosaicing. The
private photographs are not distributed or required by the tests.

To reproduce a public reference, convert a manifest sample:

```text
"Adobe DNG Converter.exe" -u -p0 -d reference sample.NEF
```

Read the DNG's CFA image IFD (PhotometricInterpretation=32803), check
Compression=1 and BitsPerSample=16, and concatenate its uncompressed strips
in row order. Normalize sample endianness to little-endian before computing
SHA-256. Compare the full dimensions and hash with `raw_sha256` in the
manifest. Do not hash the DNG container, a preview, an active-area crop, or
a rendered image. Adobe's converter is an optional validation tool; it is
not used by darktable, the build or CI.

## Sanitizers and fuzzing

The codec can be built independently with CMake and a C++17 compiler. For
Clang address/undefined-behavior sanitizer tests and the production-entry
libFuzzer target:

```sh
cmake -S src/imageio/nikon_he -B build-he -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DNIKON_HE_FUZZER=ON
cmake --build build-he
ctest --test-dir build-he --output-on-failure
python3 src/tests/download_nikon_z5ii_samples.py ./nikon-samples \
  --codestreams ./he-corpus
./build-he/darktable-fuzz-nikon-he ./he-corpus \
  -max_total_time=180 -timeout=15 -rss_limit_mb=1500 -max_len=16000000
```

The fuzz input is a complete HE codestream, not a NEF container. The target
calls the same header validation, exact transfer and image decoder used by
the application. A bounded fuzz run is useful evidence, not exhaustive
coverage of possible malformed files.