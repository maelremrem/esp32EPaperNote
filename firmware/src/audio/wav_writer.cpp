#include "audio/wav_writer.h"

#include <cstring>

namespace audio {

#pragma pack(push, 1)
struct WavHeader {
    char riff[4];
    uint32_t chunk_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
};
#pragma pack(pop)

bool writeWavHeader(FILE *file, uint32_t data_bytes, uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample) {
    if (!file) {
        return false;
    }

    WavHeader h{};
    std::memcpy(h.riff, "RIFF", 4);
    std::memcpy(h.wave, "WAVE", 4);
    std::memcpy(h.fmt, "fmt ", 4);
    std::memcpy(h.data, "data", 4);
    h.chunk_size = 36 + data_bytes;
    h.fmt_size = 16;
    h.audio_format = 1;
    h.channels = channels;
    h.sample_rate = sample_rate;
    h.bits_per_sample = bits_per_sample;
    h.block_align = channels * bits_per_sample / 8;
    h.byte_rate = sample_rate * h.block_align;
    h.data_size = data_bytes;

    if (std::fseek(file, 0, SEEK_SET) != 0) {
        return false;
    }
    return std::fwrite(&h, 1, sizeof(h), file) == sizeof(h);
}

} // namespace audio
