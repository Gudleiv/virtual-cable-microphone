#include "audio_format.h"

#include <cstring>
#include <format>

namespace vcmic {
namespace {

// KSDATAFORMAT_SUBTYPE_PCM and KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out so
// that ksmedia.h (which drags in the whole kernel streaming stack) stays out.
constexpr GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

struct ChannelBit {
    std::uint32_t mask;
    const wchar_t* name;
};

// Standard SPEAKER_* bit order.
constexpr ChannelBit kChannelBits[] = {
    {0x00001, L"FL"},   {0x00002, L"FR"},   {0x00004, L"FC"},  {0x00008, L"LFE"},
    {0x00010, L"BL"},   {0x00020, L"BR"},   {0x00040, L"FLC"}, {0x00080, L"FRC"},
    {0x00100, L"BC"},   {0x00200, L"SL"},   {0x00400, L"SR"},  {0x00800, L"TC"},
    {0x01000, L"TFL"},  {0x02000, L"TFC"},  {0x04000, L"TFR"}, {0x08000, L"TBL"},
    {0x10000, L"TBC"},  {0x20000, L"TBR"},
};

bool IsExtensible(const WAVEFORMATEX* format) {
    return format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
           format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
}

}  // namespace

FormatInfo InspectFormat(const WAVEFORMATEX* format) {
    FormatInfo info;
    if (format == nullptr) {
        return info;
    }

    info.sample_rate = format->nSamplesPerSec;
    info.channels = format->nChannels;
    info.container_bits = format->wBitsPerSample;
    info.valid_bits = format->wBitsPerSample;
    info.block_align = format->nBlockAlign;

    if (IsExtensible(format)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        info.extensible = true;
        info.channel_mask = ext->dwChannelMask;
        if (ext->Samples.wValidBitsPerSample != 0) {
            info.valid_bits = ext->Samples.wValidBitsPerSample;
        }
        if (IsEqualGUID(ext->SubFormat, kSubtypeIeeeFloat)) {
            info.sample_type = SampleType::Float;
        } else if (IsEqualGUID(ext->SubFormat, kSubtypePcm)) {
            info.sample_type = SampleType::PcmInt;
        }
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        info.sample_type = SampleType::Float;
    } else if (format->wFormatTag == WAVE_FORMAT_PCM) {
        info.sample_type = SampleType::PcmInt;
    }

    return info;
}

const wchar_t* SampleTypeName(SampleType type) {
    switch (type) {
        case SampleType::Float: return L"float";
        case SampleType::PcmInt: return L"int";
        case SampleType::Unknown: return L"unknown";
    }
    return L"unknown";
}

std::wstring DescribeChannelMask(std::uint32_t mask) {
    if (mask == 0) {
        return L"none";
    }
    std::wstring names;
    std::uint32_t remaining = mask;
    for (const ChannelBit& bit : kChannelBits) {
        if ((mask & bit.mask) != 0) {
            if (!names.empty()) {
                names.push_back(L' ');
            }
            names.append(bit.name);
            remaining &= ~bit.mask;
        }
    }
    if (remaining != 0) {
        if (!names.empty()) {
            names.push_back(L' ');
        }
        names.append(std::format(L"+0x{:X}", remaining));
    }
    return names;
}

std::wstring DescribeFormat(const WAVEFORMATEX* format) {
    if (format == nullptr) {
        return L"<none>";
    }

    const FormatInfo info = InspectFormat(format);

    std::wstring text = std::format(L"{} Hz, {} ch, {} {}-bit", info.sample_rate, info.channels,
                                    SampleTypeName(info.sample_type), info.container_bits);
    if (info.valid_bits != info.container_bits) {
        text += std::format(L" ({} valid)", info.valid_bits);
    }
    if (info.extensible) {
        text += std::format(L", mask 0x{:X} [{}]", info.channel_mask,
                            DescribeChannelMask(info.channel_mask));
        text += L", WAVEFORMATEXTENSIBLE";
    } else {
        text += std::format(L", tag {}", format->wFormatTag);
    }
    text += std::format(L", block {} B", info.block_align);
    return text;
}

std::vector<std::uint8_t> CloneFormat(const WAVEFORMATEX* format) {
    std::vector<std::uint8_t> blob;
    if (format == nullptr) {
        return blob;
    }
    const std::size_t size = sizeof(WAVEFORMATEX) + format->cbSize;
    blob.resize(size);
    std::memcpy(blob.data(), format, size);
    return blob;
}

const WAVEFORMATEX* AsWaveFormat(const std::vector<std::uint8_t>& blob) {
    if (blob.size() < sizeof(WAVEFORMATEX)) {
        return nullptr;
    }
    return reinterpret_cast<const WAVEFORMATEX*>(blob.data());
}

}  // namespace vcmic
