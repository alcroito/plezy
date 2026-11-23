// SPDX-License-Identifier: BSD-3-Clause
#include "av1_obu.h"

namespace {

constexpr int kObuSequenceHeader = 1;
constexpr int kObuTemporalDelimiter = 2;
constexpr int kObuMetadata = 5;

// leb128 (AV1 spec 4.10.5): value and length, or false when it runs past the end.
bool ReadLeb128(const uint8_t* d, size_t n, uint64_t* value, size_t* len) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8 && i < n; i++) {
    v |= static_cast<uint64_t>(d[i] & 0x7f) << (7 * i);
    if (!(d[i] & 0x80)) {
      *value = v;
      *len = i + 1;
      return true;
    }
  }
  return false;
}

void WriteLeb128(uint64_t v, std::vector<uint8_t>* out) {
  do {
    uint8_t b = v & 0x7f;
    v >>= 7;
    if (v) b |= 0x80;
    out->push_back(b);
  } while (v);
}

struct Obu {
  int type;
  const uint8_t* header;  // obu_header (1 byte, 2 with the extension)
  size_t header_size;
  const uint8_t* payload;
  size_t payload_size;
};

// Splits an OBU run. An OBU without a size field runs to the end of the buffer.
bool SplitObus(const uint8_t* d, size_t n, std::vector<Obu>* obus) {
  size_t i = 0;
  while (i < n) {
    const uint8_t h = d[i];
    if (h & 0x80) return false;  // forbidden bit
    const bool ext = h & 0x04, has_size = h & 0x02;
    size_t hs = ext ? 2 : 1;
    if (i + hs > n) return false;
    uint64_t ps;
    size_t ls = 0;
    if (has_size) {
      if (!ReadLeb128(d + i + hs, n - i - hs, &ps, &ls)) return false;
    } else {
      ps = n - i - hs;
    }
    if (i + hs + ls + ps > n) return false;
    obus->push_back({(h >> 3) & 0x0f, d + i, hs, d + i + hs + ls, static_cast<size_t>(ps)});
    i += hs + ls + ps;
  }
  return true;
}

// Appends an OBU with its size field set.
void AppendObu(const Obu& o, std::vector<uint8_t>* out) {
  out->push_back(o.header[0] | 0x02);
  if (o.header_size == 2) out->push_back(o.header[1]);
  WriteLeb128(o.payload_size, out);
  out->insert(out->end(), o.payload, o.payload + o.payload_size);
}

}  // namespace

std::vector<uint8_t> Av1ConfigObus(const uint8_t* extradata, int size) {
  std::vector<uint8_t> out;
  if (!extradata || size <= 0) return out;
  const uint8_t* d = extradata;
  size_t n = static_cast<size_t>(size);
  // av1C: marker (1) + version (7) = 0x81, then three bytes of fields, then configOBUs.
  if (n >= 4 && d[0] == 0x81) {
    d += 4;
    n -= 4;
  }
  std::vector<Obu> obus;
  if (!SplitObus(d, n, &obus)) return out;
  bool sequence = false;
  for (const Obu& o : obus) {
    if (o.type == kObuSequenceHeader) {
      AppendObu(o, &out);
      sequence = true;
    }
  }
  if (!sequence) return {};
  for (const Obu& o : obus) {
    if (o.type == kObuMetadata) AppendObu(o, &out);
  }
  return out;
}

bool Av1TemporalUnit(const uint8_t* d, int size, bool keyframe, const std::vector<uint8_t>& config,
                     std::vector<uint8_t>* out) {
  std::vector<Obu> obus;
  if (size <= 0 || !SplitObus(d, static_cast<size_t>(size), &obus)) return false;
  out->clear();
  out->push_back(0x12);  // temporal delimiter: type 2 with a size field, size 0
  out->push_back(0x00);
  bool has_sequence = false;
  for (const Obu& o : obus) has_sequence = has_sequence || o.type == kObuSequenceHeader;
  if (keyframe && !has_sequence) out->insert(out->end(), config.begin(), config.end());
  for (const Obu& o : obus) {
    if (o.type == kObuTemporalDelimiter) continue;  // ours is first
    AppendObu(o, out);
  }
  return true;
}
