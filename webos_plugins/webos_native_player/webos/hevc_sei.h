// SPDX-License-Identifier: BSD-3-Clause
// HEVC Annex B helpers: removing HDR10+ dynamic metadata (SMPTE ST 2094-40, carried as an ITU-T
// T.35 SEI message) from an access unit. LG's webOS HEVC decoders mis-handle it in a fed stream;
// Jellyfin's server strips it too when it repackages (hevc_metadata remove_hdr10plus).
#ifndef WEBOS_NATIVE_PLAYER_HEVC_SEI_H_
#define WEBOS_NATIVE_PLAYER_HEVC_SEI_H_

#include <cstdint>
#include <vector>

// Rewrites `au` (Annex B) without HDR10+ SEI messages. Returns true when something was removed.
bool HevcStripHdr10Plus(std::vector<uint8_t>* au);

// Static HDR metadata found in an access unit's SEI (mastering display colour volume, payload 137;
// content light level, payload 144), in the units the SEI itself uses: chromaticities in 1/50000
// (G, B, R order), luminance in 1/10000 cd/m2.
struct HevcHdrSei {
  bool has_mastering = false;
  int primaries[3][2] = {};  // G, B, R: x, y
  int white[2] = {};
  long long max_luminance = 0, min_luminance = 0;
  bool has_light_level = false;
  int max_cll = 0, max_fall = 0;
};

// Scans an Annex B access unit for those SEI messages. Returns true when either was found.
bool HevcFindHdrSei(const uint8_t* d, size_t n, HevcHdrSei* out);

// Parameter sets (VPS, SPS, PPS only) from an hvcC / avcC record, each with a 4-byte start code,
// and the record's NAL length size. Returns false when `extra` is not such a record.
bool ParameterSetsFromRecord(const uint8_t* extra, int size, bool hevc, std::vector<uint8_t>* out, int* nal_length_size);

// One length-prefixed MP4/MKV access unit as Annex B (4-byte start codes), with `params` in front
// when it is a keyframe (HEVC IRAP 16-23, H.264 IDR 5). Returns false on malformed input.
bool LengthPrefixedToAnnexB(const uint8_t* d, int size, int nal_length_size, bool hevc,
                            const std::vector<uint8_t>& params, std::vector<uint8_t>* out);

#endif  // WEBOS_NATIVE_PLAYER_HEVC_SEI_H_
