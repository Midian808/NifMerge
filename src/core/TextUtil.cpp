#include "TextUtil.hpp"

#include <windows.h>

#include <cstdio>

namespace nm {

std::string ToUtf8(const std::wstring& w) {
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring FromUtf8(const std::string& s) {
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
	return w;
}

std::string JsonQuote(const std::string& s) {
	std::string r = "\"";
	for (unsigned char c : s) {
		switch (c) {
			case '"': r += "\\\""; break;
			case '\\': r += "\\\\"; break;
			case '\n': r += "\\n"; break;
			case '\r': r += "\\r"; break;
			case '\t': r += "\\t"; break;
			default:
				if (c < 0x20) {
					char buf[8];
					snprintf(buf, sizeof buf, "\\u%04x", c);
					r += buf;
				}
				else
					r += static_cast<char>(c);
		}
	}
	return r + "\"";
}

std::string JsonNum(double v) {
	char buf[32];
	snprintf(buf, sizeof buf, "%.6g", v);
	return buf;
}

} // namespace nm
