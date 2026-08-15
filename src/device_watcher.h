#pragma once

#include "com.h"
#include "win_headers.h"

#include <mmdeviceapi.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <string>

namespace vcmic {

// Spec 4.8: WASAPI announces endpoint arrivals, departures, state changes and
// role changes on a thread of its own.
//
// Recovery is deliberately not driven from these callbacks. They repeat, they
// can be missed entirely, and an endpoint that has just been announced is not
// necessarily ready to be opened yet - a USB DAC coming out of sleep reports
// itself active well before its streams can be initialized. Each stream rebuilds
// itself on its own backoff instead, and all this class does is pulse the event
// that stream is waiting on. That turns the worst case after a replug from "up
// to five seconds" into "the next few milliseconds", without making correctness
// depend on a notification arriving at all.
//
// Every waiter is pulsed for every change rather than only the one whose id
// matched. An endpoint id usually survives re-enumeration, but it is not
// promised to, and a stream that is not currently broken ignores the pulse
// anyway; the cost of being wrong in this direction is one wasted retry.
// Final because Release() deletes through the static type: this is the most
// derived class by construction, and COM gives interfaces no virtual destructor
// to delete through.
class DeviceWatcher final : public IMMNotificationClient {
public:
    static constexpr std::size_t kMaxWaiters = 3;

    DeviceWatcher() = default;
    ~DeviceWatcher();

    DeviceWatcher(const DeviceWatcher&) = delete;
    DeviceWatcher& operator=(const DeviceWatcher&) = delete;

    // Both of these must be called before Register(); afterwards the waiter
    // table is read-only, which is what makes it safe to read from the
    // notification thread without a lock.
    void Watch(HANDLE wake_event, std::wstring endpoint_id, const wchar_t* label);

    HRESULT Register(IMMDeviceEnumerator* enumerator);
    void Unregister();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR device_id, DWORD new_state) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR device_id) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR device_id) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR device_id) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR device_id,
                                                     const PROPERTYKEY key) override;

private:
    struct Waiter {
        HANDLE event = nullptr;
        std::wstring id;
        const wchar_t* label = L"";
    };

    // Which of our three streams an id belongs to, or nullptr for any of the
    // dozens of other endpoints on the machine.
    const wchar_t* LabelFor(LPCWSTR device_id) const noexcept;

    // Logs at Info for an endpoint we are using and at Debug for everything
    // else, then releases whichever streams are sitting in a backoff.
    void Notify(LPCWSTR device_id, const std::wstring& what);

    ComPtr<IMMDeviceEnumerator> enumerator_;  // held so Unregister always has one
    std::array<Waiter, kMaxWaiters> waiters_{};
    std::size_t waiter_count_ = 0;
    bool registered_ = false;
    std::atomic<ULONG> refs_{1};
};

}  // namespace vcmic
