// Shared helpers: logging, safe memory reads, string utils.
#pragma once
#include <windows.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace lab {

extern std::wstring g_home;    // ...\Binaries\Win64\HalfSwordLab\  (trailing backslash)
extern std::wstring g_bridge;  // g_home + L"bridge\\"

void log(const char* fmt, ...);
bool readable(const void* p, size_t n = 8);
void resetReadCache();

template <class T>
T rd(const void* p, T def = T{}) {
    return readable(p, sizeof(T)) ? *(const T*)p : def;
}

std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);
std::string lower(std::string s);
bool hasWord(const std::string& s, std::initializer_list<const char*> words);
bool readFile(const std::wstring& path, std::string& out);
bool writeFileAtomic(const std::wstring& path, const std::string& data);
uint64_t fileStamp(const std::wstring& path);  // last-write time, 0 if missing
std::string iniGet(const std::string& ini, const std::string& key, const std::string& def);

}  // namespace lab
