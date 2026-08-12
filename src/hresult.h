#pragma once

#include "win_headers.h"

#include <string>

namespace vcmic {

// "AUDCLNT_E_DEVICE_INVALIDATED (0x88890004)" plus the system message when one
// exists. WASAPI failures are unreadable as bare hex, and 4.8 asks us to log
// exactly which failure triggered a stream rebuild.
std::wstring FormatHresult(HRESULT hr);

}  // namespace vcmic
