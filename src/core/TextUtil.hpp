#pragma once
#include <string>

namespace nm {

std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

} // namespace nm
