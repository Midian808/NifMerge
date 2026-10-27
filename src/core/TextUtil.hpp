#pragma once
#include <string>

namespace nm {

std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

// JSON string literal (quoted, escaped) and number (%.6g) for the sidecar writers.
std::string JsonQuote(const std::string& s);
std::string JsonNum(double v);

} // namespace nm
