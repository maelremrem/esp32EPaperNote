#pragma once

#include <cstdio>
#include <cstdint>

namespace audio {

bool writeWavHeader(FILE *file, uint32_t data_bytes, uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample);

} // namespace audio
