// The engine's side of the files in %ProgramData%\KaleidoscopeEQ\endpoints (CONTRACT §1, §2).
#pragma once
#include <windows.h>

#include "dsp.h"

namespace keq {

constexpr size_t kPathMax = MAX_PATH;  // every path below fits: ProgramData + \KaleidoscopeEQ\endpoints\{guid}.ext

// %ProgramData%\KaleidoscopeEQ\endpoints into out[kPathMax]. False if it does not fit.
bool EndpointDir(wchar_t* out);

enum class Load { Ok, Missing, Busy };
// Missing file -> *out is a disabled config. Busy (e.g. mid-rename) -> try again later.
Load ReadConfigFile(const wchar_t* path, Config* out);
Params LoadParams(const wchar_t* path, double fs);

// "pid <pid>" line and a UTC time line, replaced atomically.
bool WriteLiveFile(const wchar_t* path);

// One line appended to %ProgramData%\KaleidoscopeEQ\endpoints\engine.log (printf format, ASCII). Lifecycle events only —
// never call it from the audio thread. The log restarts when it passes 64 KB so it can stay on in the field.
void Trace(const char* fmt, ...);

// Background thread: whenever the .cfg changes (mtime/ctime/size), parse + design it and publish to the mailbox.
// Everything slow (IO, parsing, coefficient design, reverb calibration) happens here, never on the audio thread.
class ConfigWatcher {
public:
    ~ConfigWatcher() { Stop(); }
    bool Start(const wchar_t* path, double fs, Mailbox<Params>* box, DWORD pollMs = 50);
    void Stop();

private:
    static DWORD WINAPI Main(void* self);
    void Run();
    wchar_t path_[kPathMax] = {};
    double fs_ = 48000;
    Mailbox<Params>* box_ = nullptr;
    DWORD pollMs_ = 50;
    HANDLE thread_ = nullptr, stop_ = nullptr;
};

}  // namespace keq
