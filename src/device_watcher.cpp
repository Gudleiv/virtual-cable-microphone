#include "device_watcher.h"

#include <format>

#include "device_registry.h"
#include "logging.h"
#include "strings.h"

namespace vcmic {
namespace {

const wchar_t* RoleName(ERole role) {
    switch (role) {
        case eConsole:
            return L"Console";
        case eMultimedia:
            return L"Multimedia";
        case eCommunications:
            return L"Communications";
        default:
            return L"unknown";
    }
}

}  // namespace

DeviceWatcher::~DeviceWatcher() { Unregister(); }

void DeviceWatcher::Watch(HANDLE wake_event, std::wstring endpoint_id, const wchar_t* label) {
    if (registered_ || waiter_count_ >= kMaxWaiters || wake_event == nullptr) {
        return;
    }
    Waiter& waiter = waiters_[waiter_count_++];
    waiter.event = wake_event;
    waiter.id = std::move(endpoint_id);
    waiter.label = label;
}

HRESULT DeviceWatcher::Register(IMMDeviceEnumerator* enumerator) {
    if (registered_ || enumerator == nullptr) {
        return E_UNEXPECTED;
    }
    const HRESULT hr = enumerator->RegisterEndpointNotificationCallback(this);
    if (FAILED(hr)) {
        return hr;
    }
    // Registration gives WASAPI a reference of its own; this one only has to
    // keep alive the enumerator that Unregister will be called on.
    enumerator->AddRef();
    enumerator_ = ComPtr<IMMDeviceEnumerator>(enumerator);
    registered_ = true;
    return S_OK;
}

void DeviceWatcher::Unregister() {
    if (!registered_) {
        return;
    }
    registered_ = false;
    enumerator_->UnregisterEndpointNotificationCallback(this);
    enumerator_.Reset();
}

const wchar_t* DeviceWatcher::LabelFor(LPCWSTR device_id) const noexcept {
    if (device_id == nullptr) {
        return nullptr;
    }
    for (std::size_t i = 0; i < waiter_count_; ++i) {
        if (EqualsNoCase(waiters_[i].id, device_id)) {
            return waiters_[i].label;
        }
    }
    return nullptr;
}

void DeviceWatcher::Notify(LPCWSTR device_id, const std::wstring& what) {
    // This runs on WASAPI's own notification thread, so it stays short. The
    // logger takes a mutex and writes one line, which is affordable here and is
    // the only record of what a sleep/wake cycle did to the endpoints. The
    // endpoints we are not using are far more numerous and go to debug.
    if (const wchar_t* label = LabelFor(device_id)) {
        LogInfo(L"device notification: the {} endpoint {}", label, what);
    } else {
        LogDebug(L"device notification: an endpoint {} [{}]", what,
                 device_id == nullptr ? L"<none>" : device_id);
    }

    for (std::size_t i = 0; i < waiter_count_; ++i) {
        ::SetEvent(waiters_[i].event);
    }
}

HRESULT STDMETHODCALLTYPE DeviceWatcher::QueryInterface(REFIID riid, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *object = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeviceWatcher::AddRef() {
    return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE DeviceWatcher::Release() {
    const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        delete this;
    }
    return remaining;
}

HRESULT STDMETHODCALLTYPE DeviceWatcher::OnDeviceStateChanged(LPCWSTR device_id, DWORD new_state) {
    Notify(device_id, std::format(L"is now {}", DeviceStateName(new_state)));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceWatcher::OnDeviceAdded(LPCWSTR device_id) {
    Notify(device_id, L"was added");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceWatcher::OnDeviceRemoved(LPCWSTR device_id) {
    Notify(device_id, L"was removed");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceWatcher::OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                                LPCWSTR device_id) {
    // Nothing here follows the default device - the configured endpoints are
    // matched by id (spec 4.3) - but a role moving is the clearest sign in the
    // log that Windows re-shuffled the endpoints underneath us.
    Notify(device_id, std::format(L"took the {} {} role", DataFlowName(flow), RoleName(role)));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceWatcher::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) {
    // Fires constantly (volume, peak meters, format changes) and says nothing
    // about availability. Ignored on purpose, and not even logged.
    return S_OK;
}

}  // namespace vcmic
