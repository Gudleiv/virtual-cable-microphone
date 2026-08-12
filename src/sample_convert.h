#pragma once

#include "audio_format.h"

#include <cstddef>
#include <cstdint>

namespace vcmic {

inline constexpr std::size_t kMaxChannels = 32;

enum class SampleFormat {
    Unsupported,
    Float32,
    Int16,
    Int24,  // three bytes per sample, packed
    Int32,  // also covers 24-bit left-justified in a 32-bit container
};

SampleFormat ResolveSampleFormat(const FormatInfo& format);
const wchar_t* SampleFormatName(SampleFormat format);

// Per-channel weights folding an arbitrary endpoint layout down to stereo
// (spec 4.5: loopback may be multichannel when the GC7 runs in SURR mode, and a
// mono microphone has to land on both sides).
struct DownmixMap {
    std::uint32_t channels = 0;
    bool from_channel_mask = false;  // false means the layout was guessed
    float to_left[kMaxChannels]{};
    float to_right[kMaxChannels]{};
};

DownmixMap BuildDownmix(const FormatInfo& format);

// Where the stereo mix goes in the render endpoint's layout.
struct UpmixMap {
    std::uint32_t channels = 0;
    int left_index = -1;
    int right_index = -1;
};

UpmixMap BuildUpmix(const FormatInfo& format);

// Audio path: no allocation, no locking, no logging. Both assume the caller
// sized the buffers, which the stream setup guarantees.
void CaptureToStereo(SampleFormat sample_format, const DownmixMap& map, std::uint32_t block_align,
                     const std::uint8_t* source, std::size_t frames, float* left,
                     float* right) noexcept;

void StereoToRender(SampleFormat sample_format, const UpmixMap& map, std::uint32_t block_align,
                    const float* left, const float* right, std::size_t frames,
                    std::uint8_t* destination) noexcept;

}  // namespace vcmic
