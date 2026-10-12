#include "common.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace lab {

std::wstring g_home;
std::wstring g_bridge;

static std::mutex g_logMutex;

void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (FILE* f = _wfopen((g_home + L"lab.log").c_str(), L"a")) {
        fprintf(f, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, buf);
        fclose(f);
    }
    OutputDebugStringA(buf);
}

// Per-thread cache of the last committed region we checked, so hot loops stay cheap.
static thread_local uintptr_t s_lo = 0, s_hi = 0;

void resetReadCache() { s_lo = s_hi = 0; }

bool readable(const void* p, size_t n) {
    uintptr_t a = (uintptr_t)p;
    if (a < 0x10000 || a > 0x7FFFFFFFFFFFull - n) return false;
    if (a >= s_lo && a + n <= s_hi) return true;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    uintptr_t lo = (uintptr_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
    if (a + n > hi) return false;
    s_lo = lo;
    s_hi = hi;
    return true;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

bool hasWord(const std::string& s, std::initializer_list<const char*> words) {
    std::string l = lower(s);
    for (const char* w : words)
        if (l.find(w) != std::string::npos) return true;
    return false;
}

bool readFile(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool writeFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

uint64_t fileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}

std::string iniGet(const std::string& ini, const std::string& key, const std::string& def) {
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t end = ini.find('\n', pos);
        if (end == std::string::npos) end = ini.size();
        std::string line = ini.substr(pos, end - pos);
        pos = end + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        if (trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
    }
    return def;
}

}  // namespace lab
