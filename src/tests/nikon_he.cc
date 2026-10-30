/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "imageio/nikon_he/nikon_he_decode.h"
#include "imageio/nikon_he/nikon_he_picture_header.h"
#include "imageio/nikon_he/nikon_he_bit_reader.h"
#include "imageio/nikon_he/nikon_he_gcli_decode.h"
#include "imageio/nikon_he/nikon_he_idwt_horizontal.h"
#include "imageio/nikon_he/nikon_he_dequantize.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
int failures = 0;

static void _check(bool condition, const char *description)
{
  if(!condition)
  {
    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failures;
  }
}

static void _append_be(std::vector<uint8_t> &bytes, uint64_t value, int count)
{
  for(int i = count - 1; i >= 0; --i) bytes.push_back((value >> (8 * i)) & 255);
}

static void _segment(std::vector<uint8_t> &bytes, uint16_t marker,
             const std::vector<uint8_t> &body)
{
  _append_be(bytes, marker, 2);
  _append_be(bytes, body.size() + 2, 2);
  bytes.insert(bytes.end(), body.begin(), body.end());
}

static size_t _find_marker(const std::vector<uint8_t> &bytes, uint16_t marker)
{
  for(size_t i = 2; i + 4 <= bytes.size();)
  {
    if(((bytes[i] << 8) | bytes[i + 1]) == marker) return i;
    i += 2 + (bytes[i + 2] << 8) + bytes[i + 3];
  }
  return bytes.size();
}

static std::vector<uint8_t> _make_image(int width, int height, int gcli = 0, uint8_t signs = 0)
{
  // build raw-GCLI packets with uniform magnitudes and no significance bits
  std::vector<uint8_t> bytes = { 0xff, 0x10 };
  const char vendor[] = "CONTACT_INTOPIX_";
  std::vector<uint8_t> cap(vendor, vendor + 16);
  cap.resize(32);
  _segment(bytes, 0xff50, cap);

  std::vector<uint8_t> pih(8, 0);
  _append_be(pih, width, 2);
  _append_be(pih, height, 2);
  _append_be(pih, 0, 2);
  _append_be(pih, 16, 2);
  const uint8_t profile[] = { 4, 4, 8, 18, 0x44, 0x10, 0x51, 0x14 };
  pih.insert(pih.end(), std::begin(profile), std::end(profile));
  // Nikon's observed vendor tail, including B=1008, A=84429 and C=16255
  const uint8_t tail[] = {
    0x50, 0x88, 0x70, 0x83, 0xf0, 0x15, 0x23, 0xd1, 0x49, 0xcd, 0x3f, 0x7f, 7
  };
  pih.insert(pih.end(), std::begin(tail), std::end(tail));
  const size_t pih_start = bytes.size() + 4;
  _segment(bytes, 0xff12, pih);
  _segment(bytes, 0xff13, {14, 0x11, 0x51, 14, 0x11, 0x51,
                         14, 0x11, 0, 14, 0x11, 0x51});
  std::vector<uint8_t> weights;
  for(int i = 0; i < 25; ++i)
  {
    weights.push_back(0);
    weights.push_back(i);
  }
  _segment(bytes, 0xff14, weights);

  nikon_he::SubbandConfig bands[26];
  nikon_he::LayoutInfo layout;
  nikon_he::compute_subband_layout(width / 2, bands, layout);
  constexpr int limits[] = { 0, 6, 12, 13, 19, 21, 23, 24, 26 };
  std::vector<uint8_t> precinct(12, 0);
  for(int packet = 0; packet < 8; ++packet)
  {
    int groups = 0;
    for(int band = limits[packet]; band < limits[packet + 1]; ++band)
      groups += bands[band].ng;
    const size_t gcli_bytes = (groups + 1) / 2;
    const size_t data_bytes = (groups * gcli * 4 + 7) / 8;
    const size_t sign_bytes = gcli ? (groups * 4 + 7) / 8 : 0;
    _append_be(precinct, (uint64_t(1) << 55) | (uint64_t(data_bytes) << 35)
                        | (uint64_t(gcli_bytes) << 15) | sign_bytes, 7);
    precinct.resize(precinct.size() + gcli_bytes, (gcli << 4) | gcli);
    precinct.resize(precinct.size() + data_bytes, 255);
    precinct.resize(precinct.size() + sign_bytes, signs);
  }
  const size_t payload = precinct.size() - 12;
  for(int i = 0; i < 3; ++i) precinct[i] = (payload >> (16 - 8 * i)) & 255;
  for(int p = 0; p < height / 4; ++p)
  {
    if(p % 16 == 0) _segment(bytes, 0xff20, {uint8_t((p / 16) >> 8), uint8_t(p / 16)});
    bytes.insert(bytes.end(), precinct.begin(), precinct.end());
  }
  _append_be(bytes, 0xff11, 2);
  for(int i = 0; i < 4; ++i) bytes[pih_start + i] = (bytes.size() >> (24 - 8 * i)) & 255;
  return bytes;
}

static bool _supported(const std::vector<uint8_t> &bytes)
{
  nikon_he::PictureHeader header;
  return nikon_he::parse_picture_header(bytes.data(), bytes.size(), header)
    && nikon_he::is_supported_picture_header(header, bytes.size());
}

static void _test_headers()
{
  const auto valid = _make_image(64, 68);
  _check(_supported(valid), "synthetic supported profile");
  const size_t cdt = _find_marker(valid, 0xff13) + 4;
  const size_t pih = _find_marker(valid, 0xff12) + 4;
  const size_t wgt = _find_marker(valid, 0xff14) + 4;
  const size_t slh = _find_marker(valid, 0xff20) + 4;
  for(size_t at : { cdt, cdt + 1, cdt + 2, cdt + 3, cdt + 6, cdt + 8,
                   pih + 20, pih + 21, pih + 22, pih + 23, pih + 24,
                   wgt + 1, slh })
  {
    auto broken = valid;
    broken[at] ^= 1;
    _check(!_supported(broken), "reject altered component, transform or slice profile");
  }
  auto zero_depth = valid;
  zero_depth[cdt] = 0;
  _check(!_supported(zero_depth), "reject the reported zero-bit CDT component");
  for(size_t size = 0; size < slh + 2; ++size)
  {
    nikon_he::PictureHeader header;
    _check(!nikon_he::parse_picture_header(valid.data(), size, header),
          "reject every truncated header prefix");
  }
  auto unknown = valid;
  unknown[_find_marker(unknown, 0xff13) + 1] = 0x16;
  _check(!_supported(unknown), "reject unsupported NLT extension");
  auto bad_size = valid;
  bad_size[_find_marker(bad_size, 0xff13) + 3] = 13;
  _check(!_supported(bad_size), "reject wrong component table size");
}

static void _test_transfer()
{
  const auto bytes = _make_image(64, 64);
  nikon_he::PictureHeader header;
  _check(nikon_he::parse_picture_header(bytes.data(), bytes.size(), header), "transfer header");
  _check(nikon_he::transfer_value(header, 16255) == 1008, "transfer black offset");
  _check(nikon_he::transfer_value(header, -1) == 0, "transfer lower clamp");
  _check(nikon_he::transfer_value(header, 65535) == 16383, "transfer upper clip");
  // Adobe's reported half-up vector distinguishes this from ties-to-even rounding
  header.vendor_tail[7] = 0xd1;
  header.vendor_tail[8] = 0x01;
  header.vendor_tail[9] = 0x71;
  _check(nikon_he::transfer_value(header, 49023) == 5151, "half-up gain rounding");
  _check(nikon_he::transfer_value(header, 81791)
        == nikon_he::transfer_value(header, 65535), "clamp before transfer");
  // a unit gain and centered curve make both branches independently calculable
  header.vendor_tail[3] = 0x84;
  header.vendor_tail[4] = 0;
  header.vendor_tail[8] = header.vendor_tail[9] = 0;
  header.vendor_tail[10] = 0x80;
  header.vendor_tail[11] = 0;
  _check(nikon_he::transfer_value(header, 32768) == 1024, "alternate curve center");
  _check(nikon_he::transfer_value(header, 32768 + 512) == 1025, "positive square");
  _check(nikon_he::transfer_value(header, 32768 - 512) == 1023, "negative square");
}

static void _test_zero_images()
{
  for(int width : {64, 72, 4000, 4048, 6064})
    for(int height : {64, 68, 128, 132})
    {
      const auto bytes = _make_image(width, height);
      const int stride = width + 8;
      std::vector<uint16_t> output(static_cast<size_t>(stride) * height, 0xffff);
      const auto result = nikon_he::decode_nikon_he_image(
        bytes.data(), bytes.size(), width, height, output.data(), stride);
      _check(result.success, "raw-GCLI image with full and partial final tiles");
      // all coefficients zero reconstruct the midpoint: d=16513, gain result=21273
      for(int y = 0; y < height; ++y)
      {
        _check(std::all_of(output.begin() + y * stride, output.begin() + y * stride + width,
                          [](uint16_t v) { return v == 2734; }), "every zero-image photosite");
        _check(std::all_of(output.begin() + y * stride + width, output.begin() + (y + 1) * stride,
                          [](uint16_t v) { return v == 0xffff; }), "output row padding preserved");
      }
      auto broken = bytes;
      broken.back() = 0;
      _check(!nikon_he::decode_nikon_he_image(broken.data(), broken.size(), width, height,
                                            output.data(), stride).success, "missing EOC");
      _check(!nikon_he::decode_nikon_he_image(bytes.data(), bytes.size(), width + 8, height,
                                            output.data(), stride).success, "dimension mismatch");
    }
}

static void _test_wavelet()
{
  // independent forward 5/3 lift followed by the production inverse
  for(int length = 1; length <= 129; ++length)
  {
    std::vector<int32_t> source(length), low((length + 1) / 2), high(length / 2), out(length);
    for(int i = 0; i < length; ++i) source[i] = ((i * 7919 + length * 13) % 65535) - 32767;
    for(int i = 0; i < length / 2; ++i)
      high[i] = source[2 * i + 1]
        - ((source[2 * i] + source[2 * i + 2 < length ? 2 * i + 2 : 2 * i]) >> 1);
    for(int i = 0; i < (length + 1) / 2; ++i)
    {
      const int32_t left = high.empty() ? 0 : high[std::max(0, i - 1)];
      const int32_t right = high.empty() ? 0 : high[std::min(i, int(high.size()) - 1)];
      low[i] = source[2 * i] + ((left + right + 2) >> 2);
    }
    nikon_he::idwt53_inverse_one_level(low.data(), low.size(), high.data(), high.size(), out.data());
    _check(out == source, "5/3 round trip at odd and even widths");
  }
}

static void _test_gcli()
{
  const uint8_t sig[] = {0};
  const uint8_t unary[] = {0xff, 0xff, 0x00};
  uint8_t output[1] = {};
  nikon_he::BitReader sig_reader(sig, sizeof(sig));
  nikon_he::BitReader gcli_reader(unary, sizeof(unary));
  _check(!nikon_he::decode_gcli_values(sig_reader, gcli_reader, 0x71, 1, 0,
                                     nullptr, nullptr, output), "reject excessive GCLI");
  _check(!nikon_he::decode_gcli_values(sig_reader, gcli_reader, 0x70, 1, 0,
                                     nullptr, nullptr, output), "reject unsupported GCLI mode");
  _check(nikon_he::dequantize_coefficient(32767, 15, 0) == 524272,
        "maximum representable coefficient stays bounded");
}

static void _test_extreme_coefficients()
{
  for(uint8_t signs : {0, 255, 0xa5})
    for(int height : {64, 68, 132})
    {
      const auto bytes = _make_image(72, height, 15, signs);
      std::vector<uint16_t> output(72 * height, 0xffff);
      _check(nikon_he::decode_nikon_he_image(bytes.data(), bytes.size(), 72, height,
                                           output.data(), 72).success,
            "maximum signed coefficients and partial tiles");
      _check(std::all_of(output.begin(), output.end(), [](uint16_t v) { return v <= 16383; }),
            "extreme coefficients remain in the raw output range");
    }
}
}

int main()
{
  _test_headers();
  _test_transfer();
  _test_zero_images();
  _test_wavelet();
  _test_gcli();
  _test_extreme_coefficients();
  std::printf("Nikon HE unit tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}

#ifdef _WIN32
// darktable's MinGW build selects the Unicode CRT entry point
int wmain() { return main(); }
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
