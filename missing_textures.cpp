/*
 * NvrMissingTextures.dll -- lets the Halloween 2017 build (Echo Arena 1.76) load its lobby.
 *
 * A texture is two files in the build's _data folder: primary\e8017b774f2b6327\<version>\<id>
 * (its description) and GPU\e2f9e022d8519ca9\<version>\<id> (its pixels). The published
 * Halloween 2017 package lacks about 160 that its first global level (0x3F9915D3001DC28E)
 * lists. The game logs each as "File not found", keeps a null resource for it, and crashes on
 * that null a moment later (echovr_openxr.exe/EchoArena.exe+0x39B0A7, +0x396B22, +0x39B677).
 *
 * This plugin hooks CreateFileW/CreateFileA. When the game fails to open one half of a texture
 * because the file isn't there, it opens the same half of a texture the package has instead
 * (the first id, in name order, whose two halves both exist), so the two halves always match.
 * Every other open is untouched, and on any other build the plugin unloads itself.
 */

#include <windows.h>
#include <MinHook.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>

#include "nevr_plugin_interface.h"

namespace {

// Halloween 2017's EchoArena.exe PE timestamp; the copy EchoXR makes keeps it.
constexpr DWORD kHalloween2017 = 0x59E8F804;

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

CreateFileWFn g_trueW = nullptr;
CreateFileAFn g_trueA = nullptr;
void* g_targetW = nullptr;
void* g_targetA = nullptr;
FILE* g_log = nullptr;
std::mutex g_logLock;
volatile LONG g_replaced = 0;

std::mutex g_fallbackLock;
std::wstring g_fallbackFor;  // the primary folder g_fallback was picked in
std::wstring g_fallback;     // the stand-in texture's id

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logLock);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02u:%02u:%02u] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* plugin_logs/NvrMissingTextures/NvrMissingTextures.log beside the game's exe, where the
 * other plugins log. */
void OpenLog() {
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\plugin_logs", dir);
    CreateDirectoryW(path, nullptr);
    _snwprintf(path, MAX_PATH, L"%ls\\plugin_logs\\NvrMissingTextures", dir);
    CreateDirectoryW(path, nullptr);
    _snwprintf(path, MAX_PATH, L"%ls\\plugin_logs\\NvrMissingTextures\\NvrMissingTextures.log", dir);
    g_log = _wfopen(path, L"w");
}

DWORD ExeTimestamp() {
    const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS64* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}

struct TexturePath {
    std::wstring primaryDir;  // ...\primary\e8017b774f2b6327\<version>
    std::wstring gpuDir;      // ...\GPU\e2f9e022d8519ca9\<version>
    bool gpu;                 // the path is the pixel half
};

// Whether path is one half of a texture, and the folders of both halves.
bool ParseTexturePath(const std::wstring& path, TexturePath& out) {
    static const wchar_t kPrimary[] = L"\\primary\\e8017b774f2b6327\\";
    static const wchar_t kGpu[] = L"\\gpu\\e2f9e022d8519ca9\\";
    std::wstring lower = path;
    for (wchar_t& c : lower) c = c == L'/' ? L'\\' : (c >= L'A' && c <= L'Z' ? c + 32 : c);
    size_t at = lower.find(kPrimary), len = wcslen(kPrimary);
    out.gpu = at == std::wstring::npos;
    if (out.gpu) at = lower.find(kGpu), len = wcslen(kGpu);
    if (at == std::wstring::npos || at == 0) return false;
    size_t slash = lower.find(L'\\', at + len);  // <version>\<id>, nothing deeper
    if (slash == std::wstring::npos || slash == at + len || slash + 1 == lower.size() ||
        lower.find(L'\\', slash + 1) != std::wstring::npos)
        return false;
    std::wstring prefix = path.substr(0, at), version = path.substr(at + len, slash - at - len);
    out.primaryDir = prefix + L"\\primary\\e8017b774f2b6327\\" + version;
    out.gpuDir = prefix + L"\\GPU\\e2f9e022d8519ca9\\" + version;
    return true;
}

// The id of a texture whose two halves are both there: the first in name order.
std::wstring Fallback(const TexturePath& t) {
    std::lock_guard<std::mutex> lock(g_fallbackLock);
    if (g_fallbackFor == t.primaryDir) return g_fallback;
    std::wstring best;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((t.primaryDir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring name = fd.cFileName;
            if ((best.empty() || name < best) &&
                GetFileAttributesW((t.gpuDir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES)
                best = name;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    g_fallbackFor = t.primaryDir;
    g_fallback = best;
    if (best.empty()) Log("no texture with both halves in %ls", t.primaryDir.c_str());
    return best;
}

// After a failed open: the stand-in's handle when name is a missing texture half, else
// INVALID_HANDLE_VALUE with the open's error kept.
HANDLE OpenStandIn(const std::wstring& name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                   DWORD disposition, DWORD flags, HANDLE tmpl) {
    DWORD err = GetLastError();
    TexturePath t;
    if (err != ERROR_FILE_NOT_FOUND || disposition != OPEN_EXISTING || !ParseTexturePath(name, t)) {
        SetLastError(err);
        return INVALID_HANDLE_VALUE;
    }
    std::wstring id = Fallback(t);
    HANDLE h = id.empty() ? INVALID_HANDLE_VALUE
                          : g_trueW(((t.gpu ? t.gpuDir : t.primaryDir) + L"\\" + id).c_str(), access,
                                    share, sa, disposition, flags, tmpl);
    if (h == INVALID_HANDLE_VALUE) {
        SetLastError(err);
        return h;
    }
    LONG n = InterlockedIncrement(&g_replaced);
    if (n <= 5 || n % 50 == 0)
        Log("missing %ls: opened %ls instead (%ld so far)", name.c_str(), id.c_str(), n);
    return h;
}

HANDLE WINAPI CreateFileWHook(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                              DWORD disposition, DWORD flags, HANDLE tmpl) {
    HANDLE h = g_trueW(name, access, share, sa, disposition, flags, tmpl);
    if (h != INVALID_HANDLE_VALUE || !name) return h;
    return OpenStandIn(name, access, share, sa, disposition, flags, tmpl);
}

HANDLE WINAPI CreateFileAHook(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                              DWORD disposition, DWORD flags, HANDLE tmpl) {
    HANDLE h = g_trueA(name, access, share, sa, disposition, flags, tmpl);
    if (h != INVALID_HANDLE_VALUE || !name) return h;
    DWORD err = GetLastError();
    wchar_t wide[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 2)) {
        SetLastError(err);
        return h;
    }
    SetLastError(err);
    return OpenStandIn(wide, access, share, sa, disposition, flags, tmpl);
}

bool Hook(const char* function, void* detour, void** original, void** target) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    void* t = kernel32 ? reinterpret_cast<void*>(GetProcAddress(kernel32, function)) : nullptr;
    if (!t) {
        Log("ERROR: kernel32!%s not found", function);
        return false;
    }
    MH_STATUS s = MH_CreateHook(t, detour, original);
    if (s == MH_OK) s = MH_EnableHook(t);
    if (s != MH_OK) {
        Log("ERROR: hooking %s: %s", function, MH_StatusToString(s));
        MH_RemoveHook(t);
        return false;
    }
    *target = t;
    return true;
}

void Unhook() {
    for (void** target : {&g_targetW, &g_targetA}) {
        if (*target) {
            MH_DisableHook(*target);
            MH_RemoveHook(*target);
            *target = nullptr;
        }
    }
}

int Install() {
    if (g_targetW) return 0;  // already installed through the other entry point
    if (ExeTimestamp() != kHalloween2017) return 1;  // not Halloween 2017: nothing to do here
    OpenLog();
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Log("ERROR: MH_Initialize: %s", MH_StatusToString(s));
        return 1;
    }
    if (!Hook("CreateFileW", reinterpret_cast<void*>(CreateFileWHook), reinterpret_cast<void**>(&g_trueW), &g_targetW) ||
        !Hook("CreateFileA", reinterpret_cast<void*>(CreateFileAHook), reinterpret_cast<void**>(&g_trueA), &g_targetA)) {
        Unhook();
        return 1;
    }
    Log("Halloween 2017: missing textures open a texture the package has instead");
    return 0;
}

}  // namespace

extern "C" {

NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "missing_textures";
    info.description = "Halloween 2017: stands in for the textures its package lacks";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }

uint32_t NvrPluginGetCapabilities(void) {
    return NEVR_PLUGIN_CAP_COSMETIC | NEVR_PLUGIN_CAP_HOOKS_ENGINE;
}

int NvrPluginInitEx(const NvrGameContext*, const char*) { return Install(); }

int NvrPluginInit(const NvrGameContext*) { return Install(); }

void NvrPluginShutdown(void) {
    Unhook();
    std::lock_guard<std::mutex> lock(g_logLock);
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
