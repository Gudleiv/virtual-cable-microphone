#include "strings.h"

#include "win_headers.h"

#include <limits>

namespace vcmic {
namespace {

bool FitsInInt(std::size_t n) {
    return n <= static_cast<std::size_t>((std::numeric_limits<int>::max)());
}

}  // namespace

std::string Utf8FromWide(std::wstring_view text) {
    if (text.empty() || !FitsInInt(text.size())) {
        return std::string();
    }
    const int in_len = static_cast<int>(text.size());
    const int needed =
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), in_len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written =
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), in_len, out.data(), needed, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(written > 0 ? written : 0));
    return out;
}

std::wstring WideFromUtf8(std::string_view text) {
    if (text.empty() || !FitsInInt(text.size())) {
        return std::wstring();
    }
    const int in_len = static_cast<int>(text.size());
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), in_len, nullptr, 0);
    if (needed <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), in_len, out.data(), needed);
    out.resize(static_cast<std::size_t>(written > 0 ? written : 0));
    return out;
}

std::wstring ToLowerInvariant(std::wstring_view text) {
    if (text.empty() || !FitsInInt(text.size())) {
        return std::wstring();
    }
    const int in_len = static_cast<int>(text.size());
    const int needed = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), in_len,
                                       nullptr, 0, nullptr, nullptr, 0);
    if (needed <= 0) {
        return std::wstring(text);
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), in_len,
                                        out.data(), needed, nullptr, nullptr, 0);
    out.resize(static_cast<std::size_t>(written > 0 ? written : 0));
    return out;
}

bool ContainsNoCase(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty()) {
        return true;
    }
    const std::wstring lower_haystack = ToLowerInvariant(haystack);
    const std::wstring lower_needle = ToLowerInvariant(needle);
    return lower_haystack.find(lower_needle) != std::wstring::npos;
}

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    return ToLowerInvariant(a) == ToLowerInvariant(b);
}

std::wstring_view Trim(std::wstring_view text) {
    const auto is_space = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f' || c == L'\v';
    };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace vcmic
