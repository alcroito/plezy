// SPDX-License-Identifier: BSD-3-Clause
#include "hevc_sei.h"

#include <cstddef>

namespace {

constexpr int kPrefixSei = 39;
constexpr int kSuffixSei = 40;
constexpr int kUserDataRegisteredT35 = 4;

// Position of the next 00 00 01 at or after `from`, or `n`.
size_t NextStartCode(const uint8_t* d, size_t n, size_t from) {
  for (size_t i = from; i + 2 < n; i++) {
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return i;
  }
  return n;
}

// NAL payload without emulation-prevention bytes (00 00 03 -> 00 00).
std::vector<uint8_t> Unescape(const uint8_t* d, size_t n) {
  std::vector<uint8_t> out;
  out.reserve(n);
  int zeros = 0;
  for (size_t i = 0; i < n; i++) {
    if (zeros >= 2 && d[i] == 3) {
      zeros = 0;
      continue;
    }
    zeros = d[i] == 0 ? zeros + 1 : 0;
    out.push_back(d[i]);
  }
  return out;
}

void EscapeInto(const std::vector<uint8_t>& rbsp, std::vector<uint8_t>* out) {
  int zeros = 0;
  for (uint8_t b : rbsp) {
    if (zeros >= 2 && b <= 3) {
      out->push_back(3);
      zeros = 0;
    }
    out->push_back(b);
    zeros = b == 0 ? zeros + 1 : 0;
  }
}

bool IsHdr10Plus(const uint8_t* p, size_t n) {
  // itu_t_t35_country_code 0xB5 (USA), provider 0x003C (Samsung), oriented code 0x0001,
  // application_identifier 4 (ST 2094-40).
  return n >= 6 && p[0] == 0xB5 && p[1] == 0x00 && p[2] == 0x3C && p[3] == 0x00 && p[4] == 0x01 && p[5] == 4;
}

void WriteVarint(int v, std::vector<uint8_t>* out) {
  while (v >= 255) {
    out->push_back(255);
    v -= 255;
  }
  out->push_back(static_cast<uint8_t>(v));
}

enum class Result { kUnchanged, kRebuilt, kEmpty };

// Parses the SEI RBSP; drops HDR10+ messages. `kept` receives the rebuilt RBSP when kRebuilt.
Result StripSei(const std::vector<uint8_t>& rbsp, std::vector<uint8_t>* kept) {
  size_t i = 0;
  bool removed = false;
  int messages = 0;
  std::vector<uint8_t> out;
  while (i < rbsp.size()) {
    if (rbsp.size() - i == 1 && rbsp[i] == 0x80) break;  // rbsp_trailing_bits
    int type = 0, size = 0;
    while (i < rbsp.size() && rbsp[i] == 0xFF) type += rbsp[i++];
    if (i >= rbsp.size()) return Result::kUnchanged;
    type += rbsp[i++];
    while (i < rbsp.size() && rbsp[i] == 0xFF) size += rbsp[i++];
    if (i >= rbsp.size()) return Result::kUnchanged;
    size += rbsp[i++];
    if (static_cast<size_t>(size) > rbsp.size() - i) return Result::kUnchanged;  // malformed: leave it
    if (type == kUserDataRegisteredT35 && IsHdr10Plus(&rbsp[i], size)) {
      removed = true;
    } else {
      WriteVarint(type, &out);
      WriteVarint(size, &out);
      out.insert(out.end(), rbsp.begin() + i, rbsp.begin() + i + size);
      messages++;
    }
    i += size;
  }
  if (!removed) return Result::kUnchanged;
  if (messages == 0) return Result::kEmpty;
  out.push_back(0x80);
  *kept = std::move(out);
  return Result::kRebuilt;
}

}  // namespace

bool HevcStripHdr10Plus(std::vector<uint8_t>* au) {
  const uint8_t* d = au->data();
  const size_t n = au->size();
  std::vector<uint8_t> out;
  out.reserve(n);
  bool changed = false;
  size_t sc = NextStartCode(d, n, 0);
  if (sc > 0) out.insert(out.end(), d, d + sc);  // anything before the first start code
  while (sc < n) {
    size_t header = sc + 3;
    size_t next = NextStartCode(d, n, header);
    size_t end = next;
    // A four-byte start code's leading zero belongs to the next unit.
    if (next < n && end > header && d[end - 1] == 0) end--;
    // Copy this unit's start code (with a leading zero if it had one).
    size_t begin = (sc > 0 && d[sc - 1] == 0 && out.size() > 0 && out.back() == 0) ? sc : sc;
    int type = header < n ? (d[header] >> 1) & 0x3F : -1;
    if ((type == kPrefixSei || type == kSuffixSei) && end >= header + 2) {
      std::vector<uint8_t> kept;
      Result r = StripSei(Unescape(d + header + 2, end - header - 2), &kept);
      if (r == Result::kEmpty) {
        changed = true;
        // Drop the unit and its start code (and a 4-byte code's leading zero already copied).
        if (!out.empty() && out.back() == 0 && sc > 0 && d[sc - 1] == 0) out.pop_back();
        sc = next;
        continue;
      }
      if (r == Result::kRebuilt) {
        changed = true;
        out.insert(out.end(), d + begin, d + header + 2);
        EscapeInto(kept, &out);
        out.insert(out.end(), d + end, d + next);  // trailing zero of a 4-byte start code
        sc = next;
        continue;
      }
    }
    out.insert(out.end(), d + begin, d + next);
    sc = next;
  }
  if (changed) au->swap(out);
  return changed;
}

bool HevcFindHdrSei(const uint8_t* d, size_t n, HevcHdrSei* out) {
  bool found = false;
  size_t sc = NextStartCode(d, n, 0);
  while (sc < n) {
    size_t header = sc + 3;
    size_t next = NextStartCode(d, n, header);
    int type = header < n ? (d[header] >> 1) & 0x3F : -1;
    if (type == kPrefixSei && next > header + 2) {
      std::vector<uint8_t> r = Unescape(d + header + 2, next - header - 2);
      size_t i = 0;
      while (i < r.size()) {
        if (r.size() - i <= 1) break;
        int ptype = 0, psize = 0;
        while (i < r.size() && r[i] == 0xFF) ptype += r[i++];
        if (i >= r.size()) break;
        ptype += r[i++];
        while (i < r.size() && r[i] == 0xFF) psize += r[i++];
        if (i >= r.size()) break;
        psize += r[i++];
        if (static_cast<size_t>(psize) > r.size() - i) break;
        const uint8_t* p = r.data() + i;
        auto u16 = [p](int o) { return (p[o] << 8) | p[o + 1]; };
        auto u32 = [p](int o) { return (static_cast<long long>(p[o]) << 24) | (p[o + 1] << 16) | (p[o + 2] << 8) | p[o + 3]; };
        if (ptype == 137 && psize >= 24) {
          for (int c = 0; c < 3; c++) {
            out->primaries[c][0] = u16(c * 4);
            out->primaries[c][1] = u16(c * 4 + 2);
          }
          out->white[0] = u16(12);
          out->white[1] = u16(14);
          out->max_luminance = u32(16);
          out->min_luminance = u32(20);
          out->has_mastering = found = true;
        } else if (ptype == 144 && psize >= 4) {
          out->max_cll = u16(0);
          out->max_fall = u16(2);
          out->has_light_level = found = true;
        }
        i += psize;
      }
    }
    sc = next;
  }
  return found;
}

bool ParameterSetsFromRecord(const uint8_t* e, int size, bool hevc, std::vector<uint8_t>* out, int* nls) {
  out->clear();
  static const uint8_t kStart[4] = {0, 0, 0, 1};
  if (!e || size < 7 || e[0] != 1) return false;  // Annex B extradata or none: nothing to add
  if (hevc) {
    if (size < 23) return false;
    *nls = (e[21] & 3) + 1;
    int arrays = e[22], i = 23;
    for (int a = 0; a < arrays; a++) {
      if (i + 3 > size) return false;
      int type = e[i] & 0x3f;
      int count = (e[i + 1] << 8) | e[i + 2];
      i += 3;
      for (int k = 0; k < count; k++) {
        if (i + 2 > size) return false;
        int len = (e[i] << 8) | e[i + 1];
        i += 2;
        if (i + len > size) return false;
        if (type == 32 || type == 33 || type == 34) {  // VPS, SPS, PPS; SEI arrays are left out
          out->insert(out->end(), kStart, kStart + 4);
          out->insert(out->end(), e + i, e + i + len);
        }
        i += len;
      }
    }
  } else {
    *nls = (e[4] & 3) + 1;
    int i = 5;
    for (int pass = 0; pass < 2; pass++) {  // SPS then PPS
      if (i >= size) return false;
      int count = pass == 0 ? (e[i] & 0x1f) : e[i];
      i++;
      for (int k = 0; k < count; k++) {
        if (i + 2 > size) return false;
        int len = (e[i] << 8) | e[i + 1];
        i += 2;
        if (i + len > size) return false;
        out->insert(out->end(), kStart, kStart + 4);
        out->insert(out->end(), e + i, e + i + len);
        i += len;
      }
    }
  }
  return true;
}

bool LengthPrefixedToAnnexB(const uint8_t* d, int size, int nls, bool hevc,
                            const std::vector<uint8_t>& params, std::vector<uint8_t>* out) {
  static const uint8_t kStart[4] = {0, 0, 0, 1};
  out->clear();
  // Pass 1: keyframe?
  bool key = false;
  for (int i = 0; i + nls <= size;) {
    int len = 0;
    for (int k = 0; k < nls; k++) len = (len << 8) | d[i + k];
    i += nls;
    if (len <= 0 || i + len > size) return false;
    int type = hevc ? (d[i] >> 1) & 0x3f : d[i] & 0x1f;
    if (hevc ? (type >= 16 && type <= 23) : type == 5) {
      key = true;
      break;
    }
    i += len;
  }
  if (key) out->insert(out->end(), params.begin(), params.end());
  // Pass 2: every NAL unit with a 4-byte start code.
  for (int i = 0; i + nls <= size;) {
    int len = 0;
    for (int k = 0; k < nls; k++) len = (len << 8) | d[i + k];
    i += nls;
    if (len <= 0 || i + len > size) return false;
    out->insert(out->end(), kStart, kStart + 4);
    out->insert(out->end(), d + i, d + i + len);
    i += len;
  }
  return true;
}
