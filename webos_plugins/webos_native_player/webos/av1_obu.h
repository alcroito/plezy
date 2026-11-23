// SPDX-License-Identifier: BSD-3-Clause
// AV1 for LG's buffer-feed pipeline. The pipeline is given no codec configuration for AV1, so it
// travels in-band: each fed access unit is a Section 5 (low overhead) temporal unit that starts
// with a temporal delimiter, carries the av1C record's configuration OBUs (sequence header, HDR
// metadata) in front of keyframes that lack a sequence header, and gives every OBU a size field.
#ifndef WEBOS_NATIVE_PLAYER_AV1_OBU_H_
#define WEBOS_NATIVE_PLAYER_AV1_OBU_H_

#include <cstddef>
#include <cstdint>
#include <vector>

// The configuration OBUs of an av1C record (or of a bare OBU run): every sequence header and
// metadata OBU, each with a size field. Empty when there is no sequence header.
std::vector<uint8_t> Av1ConfigObus(const uint8_t* extradata, int size);

// One demuxed AV1 packet as a temporal unit for the pipeline (see above). False when the packet
// is not a valid OBU run.
bool Av1TemporalUnit(const uint8_t* d, int size, bool keyframe, const std::vector<uint8_t>& config,
                     std::vector<uint8_t>* out);

#endif  // WEBOS_NATIVE_PLAYER_AV1_OBU_H_
