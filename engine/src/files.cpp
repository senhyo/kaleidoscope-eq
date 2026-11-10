#include "files.h"

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace keq {

bool EndpointDir(wchar_t* out) {
    PWSTR base = nullptr;
    const bool known = SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &base));
    const int n = _snwprintf_s(out, kPathMax, _TRUNCATE, L"%s\\KaleidoscopeEQ\\endpoints",
                                 known ? base : L"C:\\ProgramData");
    CoTaskMemFree(base);
    return n > 0;
}

Load ReadConfigFile(const wchar_t* path, Config* out) {
    // Share everything so the bridge can rename its new file over this one while we read.
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            *out = Config{};
            return Load::Missing;
        }
        return Load::Busy;
    }
    constexpr DWORD kMax = 64 * 1024;
    char* text = static_cast<char*>(std::malloc(kMax));
    DWORD got = 0;
    const BOOL ok = text && ReadFile(h, text, kMax, &got, nullptr);
    CloseHandle(h);
    if (ok) *out = ParseConfig(text, got);
    std::free(text);
    return ok ? Load::Ok : Load::Busy;
}

Params LoadParams(const wchar_t* path, double fs) {
    Config c;
    return ReadConfigFile(path, &c) == Load::Ok ? Design(c, fs) : Bypass();
}

bool WriteLiveFile(const wchar_t* path) {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char text[96];
    const int n = std::snprintf(text, sizeof text, "pid %lu\r\n%04u-%02u-%02uT%02u:%02u:%02uZ\r\n", GetCurrentProcessId(),
                                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    wchar_t tmp[kPathMax];
    if (n <= 0 || _snwprintf_s(tmp, kPathMax, _TRUNCATE, L"%s.%lu.tmp", path, GetCurrentProcessId()) <= 0) return false;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, text, (DWORD)n, &wrote, nullptr);
    CloseHandle(h);
    if (ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(tmp);
    return false;
}

void Trace(const char* fmt, ...) {
    wchar_t dir[kPathMax], path[kPathMax];
    // Next to the .live files: register.ps1 gives audiodg (LocalService) write access to this folder only.
    if (!EndpointDir(dir) || _snwprintf_s(path, kPathMax, _TRUNCATE, L"%s\\engine.log", dir) <= 0) return;
    SYSTEMTIME t;
    GetSystemTime(&t);
    char line[512];
    int n = std::snprintf(line, sizeof line, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid %lu ", t.wYear, t.wMonth, t.wDay,
                          t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    if (n <= 0) return;
    va_list args;
    va_start(args, fmt);
    const int m = std::vsnprintf(line + n, sizeof line - n - 2, fmt, args);
    va_end(args);
    if (m < 0) return;
    n = (int)std::strlen(line);
    line[n++] = '\r';
    line[n++] = '\n';
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 64 * 1024) {
        CloseHandle(h);
        h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    DWORD wrote = 0;
    WriteFile(h, line, (DWORD)n, &wrote, nullptr);
    CloseHandle(h);
}

// ── watcher ──────────────────────────────────────────────────────────────

namespace {
struct Stamp {
    bool exists = false;
    FILETIME write = {}, create = {};
    DWORD sizeLow = 0, sizeHigh = 0;
    bool operator==(const Stamp& o) const {
        return exists == o.exists && CompareFileTime(&write, &o.write) == 0 &&
               CompareFileTime(&create, &o.create) == 0 && sizeLow == o.sizeLow && sizeHigh == o.sizeHigh;
    }
};
Stamp StampOf(const wchar_t* path) {
    Stamp s;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a)) {
        s.exists = true;
        s.write = a.ftLastWriteTime;
        s.create = a.ftCreationTime;
        s.sizeLow = a.nFileSizeLow;
        s.sizeHigh = a.nFileSizeHigh;
    }
    return s;
}
}  // namespace

bool ConfigWatcher::Start(const wchar_t* path, double fs, Mailbox<Params>* box, DWORD pollMs) {
    Stop();
    if (wcsncpy_s(path_, path, _TRUNCATE) != 0) return false;  // STRUNCATE: too long
    fs_ = fs;
    box_ = box;
    pollMs_ = pollMs;
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = stop_ ? CreateThread(nullptr, 0, &ConfigWatcher::Main, this, 0, nullptr) : nullptr;
    return thread_ != nullptr;
}

void ConfigWatcher::Stop() {
    if (thread_) {
        SetEvent(stop_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stop_) {
        CloseHandle(stop_);
        stop_ = nullptr;
    }
}

DWORD WINAPI ConfigWatcher::Main(void* self) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    static_cast<ConfigWatcher*>(self)->Run();
    return 0;
}

void ConfigWatcher::Run() {
    Stamp seen;
    bool haveSeen = false;  // the first pass always publishes; identical params cause no fade
    do {
        const Stamp now = StampOf(path_);
        if (haveSeen && now == seen) continue;
        Config c;
        if (ReadConfigFile(path_, &c) == Load::Busy) continue;  // retry next poll
        box_->Back() = Design(c, fs_);
        box_->Publish();
        seen = now;
        haveSeen = true;
    } while (WaitForSingleObject(stop_, pollMs_) == WAIT_TIMEOUT);
}

}  // namespace keq
