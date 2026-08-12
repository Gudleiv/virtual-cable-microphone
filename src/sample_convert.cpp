#include "sample_convert.h"

#include <algorithm>
#include <cstring>

namespace vcmic {
namespace {

constexpr float kMinus3dB = 0.70710678f;

struct SpeakerFold {
    std::uint32_t bit;
    float left;
    float right;
};

// Standard stereo fold. LFE is deliberately dropped: it is rumble, and folding
// it in only eats headroom.
constexpr SpeakerFold kSpeakerFolds[] = {
    {0x00001, 1.0f, 0.0f},              // front left
    {0x00002, 0.0f, 1.0f},              // front right
    {0x00004, kMinus3dB, kMinus3dB},    // front center
    {0x00008, 0.0f, 0.0f},              // LFE
    {0x00010, kMinus3dB, 0.0f},         // back left
    {0x00020, 0.0f, kMinus3dB},         // back right
    {0x00040, kMinus3dB, 0.0f},         // front left of center
    {0x00080, 0.0f, kMinus3dB},         // front right of center
    {0x00100, 0.5f, 0.5f},              // back center
    {0x00200, kMinus3dB, 0.0f},         // side left
    {0x00400, 0.0f, kMinus3dB},         // side right
    {0x00800, 0.5f, 0.5f},              // top center
    {0x01000, 0.5f, 0.0f},              // top front left
    {0x02000, 0.5f, 0.5f},              // top front center
    {0x04000, 0.0f, 0.5f},              // top front right
    {0x08000, 0.5f, 0.0f},              // top back left
    {0x10000, 0.5f, 0.5f},              // top back center
    {0x20000, 0.0f, 0.5f},              // top back right
};

std::size_t BytesPerSample(SampleFormat format) {
    switch (format) {
        case SampleFormat::Float32: return 4;
        case SampleFormat::Int16: return 2;
        case SampleFormat::Int24: return 3;
        case SampleFormat::Int32: return 4;
        case SampleFormat::Unsupported: return 0;
    }
    return 0;
}

inline float ReadSample(SampleFormat format, const std::uint8_t* p) noexcept {
    switch (format) {
        case SampleFormat::Float32: {
            float value = 0.0f;
            std::memcpy(&value, p, sizeof(value));
            return value;
        }
        case SampleFormat::Int16: {
            std::int16_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            return static_cast<float>(value) * (1.0f / 32768.0f);
        }
        case SampleFormat::Int24: {
            std::int32_t value = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) |
                                                           (static_cast<std::uint32_t>(p[1]) << 8) |
                                                           (static_cast<std::uint32_t>(p[2]) << 16));
            if ((value & 0x800000) != 0) {
                value |= static_cast<std::int32_t>(0xFF000000u);
            }
            return static_cast<float>(value) * (1.0f / 8388608.0f);
        }
        case SampleFormat::Int32: {
            std::int32_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            return static_cast<float>(static_cast<double>(value) * (1.0 / 2147483648.0));
        }
        case SampleFormat::Unsupported:
            return 0.0f;
    }
    return 0.0f;
}

inline float Clamp(float value) noexcept {
    if (value > 1.0f) return 1.0f;
    if (value < -1.0f) return -1.0f;
    return value;
}

inline void WriteSample(SampleFormat format, std::uint8_t* p, float value) noexcept {
    switch (format) {
        case SampleFormat::Float32: {
            std::memcpy(p, &value, sizeof(value));
            return;
        }
        case SampleFormat::Int16: {
            const auto sample = static_cast<std::int16_t>(Clamp(value) * 32767.0f);
            std::memcpy(p, &sample, sizeof(sample));
            return;
        }
        case SampleFormat::Int24: {
            const auto sample = static_cast<std::int32_t>(Clamp(value) * 8388607.0f);
            p[0] = static_cast<std::uint8_t>(sample & 0xFF);
            p[1] = static_cast<std::uint8_t>((sample >> 8) & 0xFF);
            p[2] = static_cast<std::uint8_t>((sample >> 16) & 0xFF);
            return;
        }
        case SampleFormat::Int32: {
            const auto sample =
                static_cast<std::int32_t>(static_cast<double>(Clamp(value)) * 2147483647.0);
            std::memcpy(p, &sample, sizeof(sample));
            return;
        }
        case SampleFormat::Unsupported:
            return;
    }
}

}  // namespace

SampleFormat ResolveSampleFormat(const FormatInfo& format) {
    if (format.sample_type == SampleType::Float) {
        return format.container_bits == 32 ? SampleFormat::Float32 : SampleFormat::Unsupported;
    }
    if (format.sample_type == SampleType::PcmInt) {
        switch (format.container_bits) {
            case 16: return SampleFormat::Int16;
            case 24: return SampleFormat::Int24;
            case 32: return SampleFormat::Int32;
            default: return SampleFormat::Unsupported;
        }
    }
    return SampleFormat::Unsupported;
}

const wchar_t* SampleFormatName(SampleFormat format) {
    switch (format) {
        case SampleFormat::Float32: return L"float32";
        case SampleFormat::Int16: return L"int16";
        case SampleFormat::Int24: return L"int24";
        case SampleFormat::Int32: return L"int32";
        case SampleFormat::Unsupported: return L"unsupported";
    }
    return L"unsupported";
}

DownmixMap BuildDownmix(const FormatInfo& format) {
    DownmixMap map;
    map.channels = (std::min)(format.channels, static_cast<std::uint32_t>(kMaxChannels));
    if (map.channels == 0) {
        return map;
    }

    // Mono goes to both sides at unity, whatever the mask claims: a mono
    // microphone should not lose 3 dB on the way to a stereo mix.
    if (map.channels == 1) {
        map.to_left[0] = 1.0f;
        map.to_right[0] = 1.0f;
        map.from_channel_mask = false;
        return map;
    }

    if (format.channel_mask != 0) {
        std::uint32_t index = 0;
        for (const SpeakerFold& fold : kSpeakerFolds) {
            if ((format.channel_mask & fold.bit) == 0) {
                continue;
            }
            if (index >= map.channels) {
                break;
            }
            map.to_left[index] = fold.left;
            map.to_right[index] = fold.right;
            ++index;
        }
        if (index > 0) {
            map.from_channel_mask = true;
            return map;
        }
    }

    // No usable mask: assume the first two channels are front left and right.
    map.to_left[0] = 1.0f;
    map.to_right[1] = 1.0f;
    map.from_channel_mask = false;
    return map;
}

UpmixMap BuildUpmix(const FormatInfo& format) {
    UpmixMap map;
    map.channels = (std::min)(format.channels, static_cast<std::uint32_t>(kMaxChannels));
    if (map.channels == 0) {
        return map;
    }
    if (map.channels == 1) {
        map.left_index = 0;  // both sides are summed into the single channel
        map.right_index = 0;
        return map;
    }

    if (format.channel_mask != 0) {
        std::uint32_t index = 0;
        for (const SpeakerFold& fold : kSpeakerFolds) {
            if ((format.channel_mask & fold.bit) == 0) {
                continue;
            }
            if (index >= map.channels) {
                break;
            }
            if (fold.bit == 0x1) {
                map.left_index = static_cast<int>(index);
            } else if (fold.bit == 0x2) {
                map.right_index = static_cast<int>(index);
            }
            ++index;
        }
    }
    if (map.left_index < 0 || map.right_index < 0) {
        map.left_index = 0;
        map.right_index = 1;
    }
    return map;
}

void CaptureToStereo(SampleFormat sample_format, const DownmixMap& map, std::uint32_t block_align,
                     const std::uint8_t* source, std::size_t frames, float* left,
                     float* right) noexcept {
    const std::size_t sample_bytes = BytesPerSample(sample_format);
    if (sample_bytes == 0 || map.channels == 0 || block_align == 0) {
        std::memset(left, 0, frames * sizeof(float));
        std::memset(right, 0, frames * sizeof(float));
        return;
    }

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::uint8_t* base = source + frame * block_align;
        float sum_left = 0.0f;
        float sum_right = 0.0f;
        for (std::uint32_t channel = 0; channel < map.channels; ++channel) {
            const float sample = ReadSample(sample_format, base + channel * sample_bytes);
            sum_left += sample * map.to_left[channel];
            sum_right += sample * map.to_right[channel];
        }
        left[frame] = sum_left;
        right[frame] = sum_right;
    }
}

void StereoToRender(SampleFormat sample_format, const UpmixMap& map, std::uint32_t block_align,
                    const float* left, const float* right, std::size_t frames,
                    std::uint8_t* destination) noexcept {
    const std::size_t sample_bytes = BytesPerSample(sample_format);
    if (sample_bytes == 0 || map.channels == 0 || block_align == 0) {
        std::memset(destination, 0, frames * block_align);
        return;
    }

    // Channels the mix does not drive stay silent.
    std::memset(destination, 0, frames * block_align);

    if (map.left_index == map.right_index) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            std::uint8_t* base = destination + frame * block_align;
            const float mono = (left[frame] + right[frame]) * 0.5f;
            WriteSample(sample_format, base + static_cast<std::size_t>(map.left_index) * sample_bytes,
                        mono);
        }
        return;
    }

    for (std::size_t frame = 0; frame < frames; ++frame) {
        std::uint8_t* base = destination + frame * block_align;
        WriteSample(sample_format, base + static_cast<std::size_t>(map.left_index) * sample_bytes,
                    left[frame]);
        WriteSample(sample_format, base + static_cast<std::size_t>(map.right_index) * sample_bytes,
                    right[frame]);
    }
}

}  // namespace vcmic
