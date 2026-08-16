// INITGUID has to come first: it turns the DEFINE_PROPERTYKEY entries in
// mmdeviceapi.h and functiondiscoverykeys_devpkey.h into real definitions, so
// no extra import library is needed for PKEY_Device_FriendlyName and friends.
#include <initguid.h>

#include "device_registry.h"

#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <format>

#include "hresult.h"
#include "strings.h"

namespace vcmic {
namespace {

struct DefaultEndpointIds {
    std::wstring console;
    std::wstring multimedia;
    std::wstring communications;
};

std::wstring DeviceIdOf(IMMDevice* device) {
    if (device == nullptr) {
        return std::wstring();
    }
    WCHAR* raw = nullptr;
    if (FAILED(device->GetId(&raw)) || raw == nullptr) {
        return std::wstring();
    }
    CoTaskMemPtr<WCHAR> owned(raw);
    return std::wstring(owned.get());
}

std::wstring DefaultIdFor(IMMDeviceEnumerator* enumerator, EDataFlow flow, ERole role) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, device.Put()))) {
        return std::wstring();
    }
    return DeviceIdOf(device.Get());
}

DefaultEndpointIds QueryDefaults(IMMDeviceEnumerator* enumerator, EDataFlow flow) {
    DefaultEndpointIds ids;
    ids.console = DefaultIdFor(enumerator, flow, eConsole);
    ids.multimedia = DefaultIdFor(enumerator, flow, eMultimedia);
    ids.communications = DefaultIdFor(enumerator, flow, eCommunications);
    return ids;
}

std::wstring ReadStringProperty(IPropertyStore* store, const PROPERTYKEY& key) {
    if (store == nullptr) {
        return std::wstring();
    }
    PropVariantHolder value;
    if (FAILED(store->GetValue(key, value.Put()))) {
        return std::wstring();
    }
    return value.AsString();
}

std::uint32_t ReadUInt32Property(IPropertyStore* store, const PROPERTYKEY& key,
                                 std::uint32_t fallback) {
    if (store == nullptr) {
        return fallback;
    }
    PropVariantHolder value;
    if (FAILED(store->GetValue(key, value.Put()))) {
        return fallback;
    }
    return value.AsUInt32(fallback);
}

void ProbeFormat(IMMDevice* device, EndpointInfo& info) {
    ComPtr<IAudioClient> client;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.PutVoid());
    if (FAILED(hr)) {
        info.probe_error = std::format(L"Activate(IAudioClient) failed: {}", FormatHresult(hr));
        return;
    }

    WAVEFORMATEX* format = nullptr;
    hr = client->GetMixFormat(&format);
    if (FAILED(hr) || format == nullptr) {
        info.probe_error = std::format(L"GetMixFormat failed: {}", FormatHresult(hr));
        return;
    }
    CoTaskMemPtr<WAVEFORMATEX> owned(format);
    info.mix_format = CloneFormat(owned.get());

    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME min_period = 0;
    if (SUCCEEDED(client->GetDevicePeriod(&default_period, &min_period))) {
        info.default_period = default_period;
        info.min_period = min_period;
    }
}

}  // namespace

const wchar_t* DeviceStateName(DWORD state) {
    switch (state) {
        case DEVICE_STATE_ACTIVE: return L"ACTIVE";
        case DEVICE_STATE_DISABLED: return L"DISABLED";
        case DEVICE_STATE_NOTPRESENT: return L"NOTPRESENT";
        case DEVICE_STATE_UNPLUGGED: return L"UNPLUGGED";
        default: return L"UNKNOWN";
    }
}

const wchar_t* FormFactorName(std::uint32_t form_factor) {
    static const wchar_t* kNames[] = {
        L"RemoteNetworkDevice", L"Speakers",       L"LineLevel",
        L"Headphones",          L"Microphone",     L"Headset",
        L"Handset",             L"UnknownDigitalPassthrough", L"SPDIF",
        L"DigitalAudioDisplayDevice",             L"Unknown",
    };
    if (form_factor < (sizeof(kNames) / sizeof(kNames[0]))) {
        return kNames[form_factor];
    }
    return L"Unknown";
}

const wchar_t* DataFlowName(EDataFlow flow) {
    switch (flow) {
        case eRender: return L"render";
        case eCapture: return L"capture";
        case eAll: return L"all";
        default: return L"?";
    }
}

HRESULT CreateDeviceEnumerator(ComPtr<IMMDeviceEnumerator>& out) {
    return ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), out.PutVoid());
}

HRESULT ReadEndpoint(IMMDeviceEnumerator* enumerator, IMMDevice* device, bool probe_format,
                     EndpointInfo& out) {
    if (enumerator == nullptr || device == nullptr) {
        return E_POINTER;
    }

    out = EndpointInfo{};
    out.id = DeviceIdOf(device);

    DWORD state = DEVICE_STATE_NOTPRESENT;
    if (SUCCEEDED(device->GetState(&state))) {
        out.state = state;
    }

    ComPtr<IMMEndpoint> endpoint;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IMMEndpoint), endpoint.PutVoid()))) {
        EDataFlow flow = eRender;
        if (SUCCEEDED(endpoint->GetDataFlow(&flow))) {
            out.flow = flow;
        }
    }

    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.Put()))) {
        out.friendly_name = ReadStringProperty(store.Get(), PKEY_Device_FriendlyName);
        out.adapter_name = ReadStringProperty(store.Get(), PKEY_DeviceInterface_FriendlyName);
        out.description = ReadStringProperty(store.Get(), PKEY_Device_DeviceDesc);
        out.form_factor = ReadUInt32Property(store.Get(), PKEY_AudioEndpoint_FormFactor, 10);
    }

    const DefaultEndpointIds defaults = QueryDefaults(enumerator, out.flow);
    out.default_console = !out.id.empty() && out.id == defaults.console;
    out.default_multimedia = !out.id.empty() && out.id == defaults.multimedia;
    out.default_communications = !out.id.empty() && out.id == defaults.communications;

    if (probe_format && out.IsActive()) {
        ProbeFormat(device, out);
    }

    return S_OK;
}

HRESULT CollectEndpoints(IMMDeviceEnumerator* enumerator, EDataFlow flow, DWORD state_mask,
                         bool probe_format, std::vector<EndpointInfo>& out) {
    out.clear();
    if (enumerator == nullptr) {
        return E_POINTER;
    }

    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = enumerator->EnumAudioEndpoints(flow, state_mask, collection.Put());
    if (FAILED(hr)) {
        return hr;
    }

    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        return hr;
    }

    out.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.Put()))) {
            continue;
        }
        EndpointInfo info;
        if (SUCCEEDED(ReadEndpoint(enumerator, device.Get(), probe_format, info))) {
            out.push_back(std::move(info));
        }
    }
    return S_OK;
}

HRESULT ResolveDevice(IMMDeviceEnumerator* enumerator, EDataFlow flow,
                      const DeviceSelector& selector, bool probe_format, ResolvedDevice& out,
                      std::wstring& error) {
    error.clear();
    out = ResolvedDevice{};

    if (enumerator == nullptr) {
        return E_POINTER;
    }
    if (selector.IsEmpty()) {
        error = L"neither an endpoint id nor a name fragment is configured";
        return E_INVALIDARG;
    }

    std::wstring id_failure;

    if (!selector.id.empty()) {
        ComPtr<IMMDevice> device;
        const HRESULT hr = enumerator->GetDevice(selector.id.c_str(), device.Put());
        if (SUCCEEDED(hr)) {
            EndpointInfo info;
            if (SUCCEEDED(ReadEndpoint(enumerator, device.Get(), probe_format, info))) {
                if (info.flow != flow) {
                    id_failure = std::format(L"endpoint '{}' is a {} endpoint, expected {}",
                                             selector.id, DataFlowName(info.flow),
                                             DataFlowName(flow));
                } else if (!info.IsActive()) {
                    id_failure = std::format(L"endpoint '{}' ({}) is {}, not ACTIVE", selector.id,
                                             info.friendly_name, DeviceStateName(info.state));
                } else {
                    out.device = device;
                    out.info = std::move(info);
                    out.matched = MatchKind::ById;
                    return S_OK;
                }
            } else {
                id_failure = std::format(L"endpoint '{}' could not be inspected", selector.id);
            }
        } else {
            id_failure = std::format(L"endpoint id '{}' not found: {}", selector.id,
                                     FormatHresult(hr));
        }
    }

    if (selector.name_contains.empty()) {
        error = id_failure;
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.Put());
    if (FAILED(hr)) {
        error = std::format(L"EnumAudioEndpoints failed: {}", FormatHresult(hr));
        return hr;
    }

    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        error = std::format(L"IMMDeviceCollection::GetCount failed: {}", FormatHresult(hr));
        return hr;
    }

    std::vector<ComPtr<IMMDevice>> devices;
    std::vector<EndpointInfo> matches;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.Put()))) {
            continue;
        }
        EndpointInfo info;
        if (FAILED(ReadEndpoint(enumerator, device.Get(), probe_format, info))) {
            continue;
        }
        if (ContainsNoCase(info.friendly_name, selector.name_contains)) {
            devices.push_back(device);
            matches.push_back(std::move(info));
        }
    }

    if (matches.empty()) {
        error = std::format(L"no active {} endpoint has a name containing '{}'",
                            DataFlowName(flow), selector.name_contains);
        if (!id_failure.empty()) {
            error = id_failure + L"; " + error;
        }
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    if (matches.size() > 1) {
        error = std::format(L"'{}' matches {} endpoints, so it is ambiguous:",
                            selector.name_contains, matches.size());
        for (const EndpointInfo& info : matches) {
            error += std::format(L"\n    {}  ->  {}", info.friendly_name, info.id);
        }
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }

    out.device = devices.front();
    out.info = std::move(matches.front());
    out.matched = MatchKind::ByName;
    return S_OK;
}

DeviceSelector SelectorFor(const EndpointInfo& info) {
    DeviceSelector selector;
    selector.id = info.id;
    selector.name_contains = info.friendly_name;
    return selector;
}

DeviceSuggestion SuggestDevices(const std::vector<EndpointInfo>& render,
                                const std::vector<EndpointInfo>& capture) {
    const auto by_name = [](const std::vector<EndpointInfo>& list,
                            std::wstring_view fragment) -> const EndpointInfo* {
        for (const EndpointInfo& info : list) {
            if (info.IsActive() && ContainsNoCase(info.friendly_name, fragment)) {
                return &info;
            }
        }
        return nullptr;
    };
    const auto communications = [](const std::vector<EndpointInfo>& list) -> const EndpointInfo* {
        for (const EndpointInfo& info : list) {
            if (info.IsActive() && info.default_communications) {
                return &info;
            }
        }
        return nullptr;
    };

    // The cable first, and by name: it is the one endpoint whose identity is
    // not a matter of taste, and it must not be picked as anything else.
    const EndpointInfo* cable = by_name(render, L"cable-a input");
    if (cable == nullptr) {
        cable = by_name(render, L"cable input");
    }

    // Where the voices come from. Discord follows the communications default
    // unless it has been told otherwise, which is exactly the case this guess
    // is for; a headset is the fallback.
    const EndpointInfo* chat = communications(render);
    if (chat == cable) {
        chat = nullptr;  // rendering into the cable and recording it would be a loop
    }
    if (chat == nullptr) {
        chat = by_name(render, L"headset");
    }

    const EndpointInfo* mic = communications(capture);
    if (mic == nullptr) {
        mic = by_name(capture, L"microphone");
    }

    DeviceSuggestion suggestion;
    if (chat != nullptr) {
        suggestion.devices.chat_render = SelectorFor(*chat);
        suggestion.chat = true;
    }
    if (mic != nullptr) {
        suggestion.devices.mic_capture = SelectorFor(*mic);
        suggestion.mic = true;
    }
    if (cable != nullptr) {
        suggestion.devices.output_render = SelectorFor(*cable);
        suggestion.output = true;
    }
    return suggestion;
}

HRESULT SuggestDevices(IMMDeviceEnumerator* enumerator, DeviceSuggestion& out) {
    std::vector<EndpointInfo> render;
    std::vector<EndpointInfo> capture;
    HRESULT hr = CollectEndpoints(enumerator, eRender, DEVICE_STATE_ACTIVE, false, render);
    if (FAILED(hr)) {
        return hr;
    }
    hr = CollectEndpoints(enumerator, eCapture, DEVICE_STATE_ACTIVE, false, capture);
    if (FAILED(hr)) {
        return hr;
    }
    out = SuggestDevices(render, capture);
    return S_OK;
}

}  // namespace vcmic
