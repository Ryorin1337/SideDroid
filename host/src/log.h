#pragma once
#include <string>

namespace sd {

// printf-style. Written to %LOCALAPPDATA%\SideDroid\host.log and to stderr.
// fileName is relative to dataDir().
void logInit(bool toStderr, const std::wstring& fileName = L"host.log");
void logf(const char* fmt, ...);

std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);

// %LOCALAPPDATA%\SideDroid, created on first use.
std::wstring dataDir();

}  // namespace sd
