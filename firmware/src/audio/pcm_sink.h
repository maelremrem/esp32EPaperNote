#pragma once
#include <cstddef>
#include <cstdint>

namespace audio {
// Called only by the recorder task; implementations must copy or drop immediately.
class PcmSink {
public:
    virtual ~PcmSink() = default;
    virtual void tryPush(const uint8_t *pcm, size_t bytes) = 0;
};
}
