// Entry point. Half Sword Lab ships as a proxy version.dll placed next to the game exe:
// the game loads it like the real one, every export is forwarded to the system version.dll,
// and a background thread brings up the Lab once the engine is running.
#include <windows.h>

#include <atomic>
#include <mutex>

#include "MinHook.h"
#include "common.hpp"
#include "lab.hpp"
#include "overlay.hpp"
#include "ue.hpp"

// ------------------------------------------------------------------ version.dll forwarding

extern "C" {
FARPROC g_procs[17];
}

static const char* EXPORTS[17] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA", "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW", "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
    "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW",
};

#define STUB(i, name) \
    __asm__(".text\n.globl " #name "_proxy\n" #name "_proxy:\n\tjmp *g_procs+" #i "*8(%rip)\n");
STUB(0, GetFileVersionInfoA)
STUB(1, GetFileVersionInfoByHandle)
STUB(2, GetFileVersionInfoExA)
STUB(3, GetFileVersionInfoExW)
STUB(4, GetFileVersionInfoSizeA)
STUB(5, GetFileVersionInfoSizeExA)
STUB(6, GetFileVersionInfoSizeExW)
STUB(7, GetFileVersionInfoSizeW)
STUB(8, GetFileVersionInfoW)
STUB(9, VerFindFileA)
STUB(10, VerFindFileW)
STUB(11, VerInstallFileA)
STUB(12, VerInstallFileW)
STUB(13, VerLanguageNameA)
STUB(14, VerLanguageNameW)
STUB(15, VerQueryValueA)
STUB(16, VerQueryValueW)

static void loadRealVersionDll() {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\version.dll");
    HMODULE real = LoadLibraryW(path);
    for (int i = 0; i < 17; i++) g_procs[i] = real ? GetProcAddress(real, EXPORTS[i]) : nullptr;
}

// ------------------------------------------------------------------ game thread hook

using PEFn = void (*)(uint8_t*, uint8_t*, void*);
static PEFn oProcessEvent = nullptr;
static std::atomic<DWORD> g_gameThread{0};
static std::atomic<DWORD> g_lastDrain{0};
static thread_local bool t_inLab = false;

// ProcessEvent runs almost entirely on the game thread; pick the thread that calls it most.
static void pickGameThread(DWORD tid) {
    static std::mutex mx;
    static DWORD ids[8] = {}, firstCall = 0;
    static int counts[8] = {}, total = 0;
    std::lock_guard<std::mutex> l(mx);
    if (g_gameThread) return;
    int slot = -1;
    for (int i = 0; i < 8 && slot < 0; i++)
        if (ids[i] == tid) slot = i;
    for (int i = 0; i < 8 && slot < 0; i++)
        if (!ids[i]) { ids[i] = tid; slot = i; }
    if (slot >= 0) counts[slot]++;
    if (!firstCall) firstCall = GetTickCount();
    if (++total < 2000 && GetTickCount() - firstCall < 2000) return;
    int best = 0;
    for (int i = 1; i < 8; i++)
        if (counts[i] > counts[best]) best = i;
    g_gameThread = ids[best];
    lab::log("game thread is %lu", ids[best]);
}

static void hkProcessEvent(uint8_t* self, uint8_t* fn, void* parms) {
    if (!t_inLab) {
        DWORD tid = GetCurrentThreadId();
        if (!g_gameThread) pickGameThread(tid);
        DWORD now = GetTickCount();
        if (tid == g_gameThread && now - g_lastDrain >= 15) {
            g_lastDrain = now;
            t_inLab = true;
            lab::onGameThread();
            t_inLab = false;
        }
    }
    oProcessEvent(self, fn, parms);
}

// ------------------------------------------------------------------ startup

static DWORD WINAPI loader(LPVOID) {
    CreateDirectoryW(lab::g_home.c_str(), nullptr);
    CreateDirectoryW(lab::g_bridge.c_str(), nullptr);
    std::string ini;
    lab::readFile(lab::g_home + L"lab.ini", ini);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    lab::log("---- Half Sword Lab loading in %s", lab::narrow(exe).c_str());

    if (lab::iniGet(ini, "overlay", "1") == "1") {
        unsigned key = (unsigned)strtoul(lab::iniGet(ini, "toggle_key", "0xDC").c_str(), nullptr, 0);
        overlay::start(key);
    }

    std::string why;
    for (int tries = 0;; tries++) {
        why.clear();
        if (ue::init(why)) break;
        lab::setStatus("waiting for the engine: " + why);
        if (tries % 15 == 0) lab::log("waiting: %s", why.c_str());
        if (tries > 900) {
            lab::log("giving up: %s", why.c_str());
            lab::setStatus("could not attach: " + why);
            return 0;
        }
        Sleep(1000);
    }
    if (!why.empty()) lab::log("note: %s", why.c_str());

    lab::start();
    MH_Initialize();
    void* target = ue::processEventTarget();
    if (target && MH_CreateHook(target, (void*)hkProcessEvent, (void**)&oProcessEvent) == MH_OK) {
        ue::setProcessEventOriginal((void*)oProcessEvent);
        MH_EnableHook(target);
        lab::log("ProcessEvent hooked");
    } else {
        lab::setStatus("attached, but ProcessEvent was not found: actions are disabled");
        lab::log("ProcessEvent hook failed; the Lab cannot run on the game thread");
    }
    return 0;
}

static bool shouldActivate() {
    // Only run inside the game itself, and only when the installer put lab.ini there.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring e = exe;
    if (e.find(L"CrashReport") != std::wstring::npos) return false;
    return GetFileAttributesW((lab::g_home + L"lab.ini").c_str()) != INVALID_FILE_ATTRIBUTES;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    loadRealVersionDll();
    wchar_t me[MAX_PATH];
    GetModuleFileNameW(self, me, MAX_PATH);
    std::wstring dir = me;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    lab::g_home = dir + L"HalfSwordLab\\";
    lab::g_bridge = lab::g_home + L"bridge\\";
    if (shouldActivate()) CloseHandle(CreateThread(nullptr, 0, loader, nullptr, 0, nullptr));
    return TRUE;
}
