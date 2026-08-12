#include "hresult.h"

#include <audioclient.h>

#include <format>

#include "strings.h"

namespace vcmic {
namespace {

// `L"" #code` concatenates to a wide literal holding the macro's own spelling.
#define VCMIC_HR_NAME(code)                            \
    do {                                               \
        if (hr == static_cast<HRESULT>(code)) {        \
            return L"" #code;                          \
        }                                              \
    } while (false)

const wchar_t* KnownName(HRESULT hr) {
    VCMIC_HR_NAME(S_OK);
    VCMIC_HR_NAME(S_FALSE);
    VCMIC_HR_NAME(E_POINTER);
    VCMIC_HR_NAME(E_INVALIDARG);
    VCMIC_HR_NAME(E_OUTOFMEMORY);
    VCMIC_HR_NAME(E_NOINTERFACE);
    VCMIC_HR_NAME(E_ACCESSDENIED);
    VCMIC_HR_NAME(RPC_E_CHANGED_MODE);
    VCMIC_HR_NAME(REGDB_E_CLASSNOTREG);
    VCMIC_HR_NAME(CO_E_NOTINITIALIZED);

#ifdef AUDCLNT_E_NOT_INITIALIZED
    VCMIC_HR_NAME(AUDCLNT_E_NOT_INITIALIZED);
#endif
#ifdef AUDCLNT_E_ALREADY_INITIALIZED
    VCMIC_HR_NAME(AUDCLNT_E_ALREADY_INITIALIZED);
#endif
#ifdef AUDCLNT_E_WRONG_ENDPOINT_TYPE
    VCMIC_HR_NAME(AUDCLNT_E_WRONG_ENDPOINT_TYPE);
#endif
#ifdef AUDCLNT_E_DEVICE_INVALIDATED
    VCMIC_HR_NAME(AUDCLNT_E_DEVICE_INVALIDATED);
#endif
#ifdef AUDCLNT_E_NOT_STOPPED
    VCMIC_HR_NAME(AUDCLNT_E_NOT_STOPPED);
#endif
#ifdef AUDCLNT_E_BUFFER_TOO_LARGE
    VCMIC_HR_NAME(AUDCLNT_E_BUFFER_TOO_LARGE);
#endif
#ifdef AUDCLNT_E_OUT_OF_ORDER
    VCMIC_HR_NAME(AUDCLNT_E_OUT_OF_ORDER);
#endif
#ifdef AUDCLNT_E_UNSUPPORTED_FORMAT
    VCMIC_HR_NAME(AUDCLNT_E_UNSUPPORTED_FORMAT);
#endif
#ifdef AUDCLNT_E_INVALID_SIZE
    VCMIC_HR_NAME(AUDCLNT_E_INVALID_SIZE);
#endif
#ifdef AUDCLNT_E_DEVICE_IN_USE
    VCMIC_HR_NAME(AUDCLNT_E_DEVICE_IN_USE);
#endif
#ifdef AUDCLNT_E_BUFFER_OPERATION_PENDING
    VCMIC_HR_NAME(AUDCLNT_E_BUFFER_OPERATION_PENDING);
#endif
#ifdef AUDCLNT_E_THREAD_NOT_REGISTERED
    VCMIC_HR_NAME(AUDCLNT_E_THREAD_NOT_REGISTERED);
#endif
#ifdef AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED
    VCMIC_HR_NAME(AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED);
#endif
#ifdef AUDCLNT_E_ENDPOINT_CREATE_FAILED
    VCMIC_HR_NAME(AUDCLNT_E_ENDPOINT_CREATE_FAILED);
#endif
#ifdef AUDCLNT_E_SERVICE_NOT_RUNNING
    VCMIC_HR_NAME(AUDCLNT_E_SERVICE_NOT_RUNNING);
#endif
#ifdef AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED
    VCMIC_HR_NAME(AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED);
#endif
#ifdef AUDCLNT_E_EXCLUSIVE_MODE_ONLY
    VCMIC_HR_NAME(AUDCLNT_E_EXCLUSIVE_MODE_ONLY);
#endif
#ifdef AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL
    VCMIC_HR_NAME(AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL);
#endif
#ifdef AUDCLNT_E_EVENTHANDLE_NOT_SET
    VCMIC_HR_NAME(AUDCLNT_E_EVENTHANDLE_NOT_SET);
#endif
#ifdef AUDCLNT_E_INCORRECT_BUFFER_SIZE
    VCMIC_HR_NAME(AUDCLNT_E_INCORRECT_BUFFER_SIZE);
#endif
#ifdef AUDCLNT_E_BUFFER_SIZE_ERROR
    VCMIC_HR_NAME(AUDCLNT_E_BUFFER_SIZE_ERROR);
#endif
#ifdef AUDCLNT_E_CPUUSAGE_EXCEEDED
    VCMIC_HR_NAME(AUDCLNT_E_CPUUSAGE_EXCEEDED);
#endif
#ifdef AUDCLNT_E_BUFFER_ERROR
    VCMIC_HR_NAME(AUDCLNT_E_BUFFER_ERROR);
#endif
#ifdef AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
    VCMIC_HR_NAME(AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED);
#endif
#ifdef AUDCLNT_E_INVALID_DEVICE_PERIOD
    VCMIC_HR_NAME(AUDCLNT_E_INVALID_DEVICE_PERIOD);
#endif
#ifdef AUDCLNT_E_INVALID_STREAM_FLAG
    VCMIC_HR_NAME(AUDCLNT_E_INVALID_STREAM_FLAG);
#endif
#ifdef AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE
    VCMIC_HR_NAME(AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE);
#endif
#ifdef AUDCLNT_E_RESOURCES_INVALIDATED
    VCMIC_HR_NAME(AUDCLNT_E_RESOURCES_INVALIDATED);
#endif
#ifdef AUDCLNT_E_RAW_MODE_UNSUPPORTED
    VCMIC_HR_NAME(AUDCLNT_E_RAW_MODE_UNSUPPORTED);
#endif
#ifdef AUDCLNT_E_ENGINE_FORMAT_LOCKED
    VCMIC_HR_NAME(AUDCLNT_E_ENGINE_FORMAT_LOCKED);
#endif
#ifdef AUDCLNT_S_BUFFER_EMPTY
    VCMIC_HR_NAME(AUDCLNT_S_BUFFER_EMPTY);
#endif
#ifdef AUDCLNT_S_THREAD_ALREADY_REGISTERED
    VCMIC_HR_NAME(AUDCLNT_S_THREAD_ALREADY_REGISTERED);
#endif
#ifdef AUDCLNT_S_POSITION_STALLED
    VCMIC_HR_NAME(AUDCLNT_S_POSITION_STALLED);
#endif

    // Not macro constants, so they cannot go through VCMIC_HR_NAME.
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        return L"ERROR_NOT_FOUND";
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA)) {
        return L"ERROR_MORE_DATA";
    }

    return nullptr;
}

#undef VCMIC_HR_NAME

std::wstring SystemMessage(HRESULT hr) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message;
    if (length != 0 && buffer != nullptr) {
        message.assign(buffer, length);
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    return std::wstring(Trim(message));
}

}  // namespace

std::wstring FormatHresult(HRESULT hr) {
    const wchar_t* name = KnownName(hr);
    const std::wstring code = std::format(L"0x{:08X}", static_cast<unsigned long>(hr));
    const std::wstring message = SystemMessage(hr);

    if (name != nullptr && !message.empty()) {
        return std::format(L"{} ({}): {}", name, code, message);
    }
    if (name != nullptr) {
        return std::format(L"{} ({})", name, code);
    }
    if (!message.empty()) {
        return std::format(L"{}: {}", code, message);
    }
    return code;
}

}  // namespace vcmic
