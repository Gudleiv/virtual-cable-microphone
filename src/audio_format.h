#pragma once

#include "win_headers.h"

#include <mmreg.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vcmic {

enum class SampleType {
    Unknown,
    PcmInt,
    Float,
};

struct FormatInfo {
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t container_bits = 0;  // wBitsPerSample
    std::uint32_t valid_bits = 0;      // Samples.wValidBitsPerSample, or container_bits
    std::uint32_t channel_mask = 0;    // 0 when the format is not extensible
    std::uint32_t block_align = 0;
    SampleType sample_type = SampleType::Unknown;
    bool extensible = false;
};

FormatInfo InspectFormat(const WAVEFORMATEX* format);

// One line for --list-devices and for the startup summary in the log.
std::wstring DescribeFormat(const WAVEFORMATEX* format);
std::wstring DescribeChannelMask(std::uint32_t mask);
const wchar_t* SampleTypeName(SampleType type);

// WAVEFORMATEX is variable-length (cbSize tail), so it is stored as a blob.
std::vector<std::uint8_t> CloneFormat(const WAVEFORMATEX* format);
const WAVEFORMATEX* AsWaveFormat(const std::vector<std::uint8_t>& blob);

}  // namespace vcmic
