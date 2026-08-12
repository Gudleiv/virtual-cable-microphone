#pragma once

#include "audio_format.h"
#include "com.h"
#include "config.h"
#include "win_headers.h"

#include <audioclient.h>  // REFERENCE_TIME, and every caller needs it eventually
#include <mmdeviceapi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vcmic {

struct EndpointInfo {
    std::wstring id;             // IMMDevice::GetId - the only stable identity (spec 4.3)
    std::wstring friendly_name;  // "Headset (Sound Blaster GC7)"
    std::wstring adapter_name;   // "Sound Blaster GC7"
    std::wstring description;    // "Headset"
    EDataFlow flow = eRender;
    DWORD state = DEVICE_STATE_NOTPRESENT;
    std::uint32_t form_factor = 10;  // UnknownFormFactor

    bool default_console = false;
    bool default_multimedia = false;
    bool default_communications = false;

    // Filled in only when the endpoint is active and IAudioClient can be
    // activated; probing is best-effort and never fatal.
    std::vector<std::uint8_t> mix_format;
    std::wstring probe_error;
    REFERENCE_TIME default_period = 0;  // 100 ns units
    REFERENCE_TIME min_period = 0;

    const WAVEFORMATEX* Format() const { return AsWaveFormat(mix_format); }
    bool IsActive() const { return state == DEVICE_STATE_ACTIVE; }
};

const wchar_t* DeviceStateName(DWORD state);
const wchar_t* FormFactorName(std::uint32_t form_factor);
const wchar_t* DataFlowName(EDataFlow flow);

HRESULT CreateDeviceEnumerator(ComPtr<IMMDeviceEnumerator>& out);

// state_mask takes DEVICE_STATEMASK_ALL or a subset of DEVICE_STATE_*.
HRESULT CollectEndpoints(IMMDeviceEnumerator* enumerator, EDataFlow flow, DWORD state_mask,
                         bool probe_format, std::vector<EndpointInfo>& out);

HRESULT ReadEndpoint(IMMDeviceEnumerator* enumerator, IMMDevice* device, bool probe_format,
                     EndpointInfo& out);

enum class MatchKind {
    None,
    ById,
    ByName,
};

struct ResolvedDevice {
    ComPtr<IMMDevice> device;
    EndpointInfo info;
    MatchKind matched = MatchKind::None;
};

// Prefers the endpoint id; falls back to a case-insensitive substring match on
// the friendly name (reported through MatchKind so the caller can warn).
// `error` gets a human-readable reason when this returns a failure HRESULT.
HRESULT ResolveDevice(IMMDeviceEnumerator* enumerator, EDataFlow flow,
                      const DeviceSelector& selector, bool probe_format, ResolvedDevice& out,
                      std::wstring& error);

}  // namespace vcmic
