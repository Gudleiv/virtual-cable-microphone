#pragma once

#include <string>
#include <string_view>

namespace vcmic {

std::string Utf8FromWide(std::wstring_view text);
std::wstring WideFromUtf8(std::string_view text);

// Invariant-locale lowercase; used for case-insensitive device name matching.
std::wstring ToLowerInvariant(std::wstring_view text);

bool ContainsNoCase(std::wstring_view haystack, std::wstring_view needle);
bool EqualsNoCase(std::wstring_view a, std::wstring_view b);

std::wstring_view Trim(std::wstring_view text);

}  // namespace vcmic
