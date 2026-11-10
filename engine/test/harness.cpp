// Offline tests for the engine's DSP core and config plumbing. Runs the exact code the APO runs
// (dsp.cpp, files.cpp) on generated signals — no audio device, no registry, no admin.
#include <windows.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <chrono>
#include <complex>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "../src/dsp.h"
#include "../src/files.h"

using namespace keq;

namespace {

int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what, const char* fmt = "", double a = 0, double b = 0) {
    std::printf("  [%s] %s ", ok ? "PASS" : "FAIL", what);
    std::printf(fmt, a, b);
    std::printf("\n");
    (ok ? g_passed : g_failed)++;
}

constexpr double kPi = 3.14159265358979323846;

Params FromText(const char* text, double fs) { return Design(ParseConfig(text, std::strlen(text)), fs); }

std::vector<float> Sine(double fs, double hz, double amp, size_t frames, int ch, double phase = 0) {
    std::vector<float> v(frames * ch);
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c) v[i * ch + c] = (float)(amp * std::sin(2 * kPi * hz * i / fs + phase + 0.3 * c));
    return v;
}

std::vector<float> Noise(size_t frames, int ch, double amp, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    std::vector<float> v(frames * ch);
    for (auto& x : v) x = (float)(amp * u(rng));
    return v;
}

// Runs a whole buffer through the processor in engine-sized blocks (10 ms).
void Run(Processor& p, std::vector<float>& buf, int ch, double fs, std::function<void(size_t)> beforeBlock = nullptr) {
    const size_t block = (size_t)(fs / 100), frames = buf.size() / ch;
    for (size_t at = 0, k = 0; at < frames; at += block, ++k) {
        if (beforeBlock) beforeBlock(k);
        p.Process(buf.data() + at * ch, (uint32_t)std::min(block, frames - at), false);
    }
}

double RmsDb(const std::vector<float>& v, int ch, int c, size_t from, size_t to) {
    double e = 0;
    for (size_t i = from; i < to; ++i) e += (double)v[i * ch + c] * v[i * ch + c];
    return 10 * std::log10(e / (to - from));
}

// Exact magnitude of a biquad cascade, as client.js magDb() computes it.
double MagDb(const Params& p, double f, double fs) {
    const double w = 2 * kPi * f / fs, c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    double db = 20 * std::log10(p.preamp);
    for (int i = 0; i < p.nPeaks; ++i) {
        const Biquad& h = p.peak[i];
        const double nr = h.b0 + h.b1 * c1 + h.b2 * c2, ni = -(h.b1 * s1 + h.b2 * s2);
        const double dr = 1 + h.a1 * c1 + h.a2 * c2, di = -(h.a1 * s1 + h.a2 * s2);
        db += 10 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    return db;
}

// ── 1. bypass is bit-exact ───────────────────────────────────────────────
void TestPassthrough() {
    std::printf("\n1. disabled / missing config = bit-exact passthrough\n");
    const double fs = 48000;
    const int ch = 2;
    struct Case { const char* name; const char* cfg; } cases[] = {
        {"no config (initial)", nullptr},
        {"enabled 0 with peaks/reverb/spatial", "# kaleidoscope-eq 1\nenabled 0\npreamp -3\npeak 1000 1.41 6\nreverb 2.4 28 -2\nspatial 0.6\n"},
        {"enabled 1 but all neutral", "enabled 1\npreamp 0\npeak 1000 1.41 0\n"},
    };
    for (auto& k : cases) {
        Processor p;
        p.Init(fs, ch);
        if (k.cfg) p.Reset(FromText(k.cfg, fs));
        auto in = Noise(48000, ch, 0.9, 1);
        in[100] = 1e-40f;  // a denormal must survive untouched too
        in[101] = -0.f;
        auto out = in;
        bool touched = false;
        const size_t block = 480;
        for (size_t at = 0; at < 48000; at += block) touched |= p.Process(out.data() + at * ch, (uint32_t)block, false);
        Check(!touched && std::memcmp(in.data(), out.data(), in.size() * 4) == 0, k.name, "(48000 frames, memcmp equal)");
    }
    {   // active -> disabled: once the 30 ms fade is over, every bit is the input again
        Processor p;
        p.Init(fs, ch);
        p.Reset(FromText("preamp -3\npeak 1000 1.41 6\nreverb 2.4 28 -2\nspatial 0.6\n", fs));
        auto in = Noise(48000, ch, 0.5, 2);
        auto out = in;
        Run(p, out, ch, fs, [&](size_t k) {
            if (k == 10) { p.Inbox().Back() = FromText("enabled 0\n", fs); p.Inbox().Publish(); }
        });
        const size_t from = (10 * 480 + 1440) * ch;  // fade start + 30 ms
        const bool exact = std::memcmp(in.data() + from, out.data() + from, (in.size() - from) * 4) == 0;
        Check(exact, "active -> enabled 0: bit-exact after the 30 ms fade");
    }
    {
        Config c;
        const Load r = ReadConfigFile(L"C:\\nonexistent-keq-dir\\{00000000-0000-0000-0000-000000000000}.cfg", &c);
        Check(r == Load::Missing && !Design(c, fs).active, "missing .cfg file -> bypass params");
    }
}

// ── 2. EQ accuracy ───────────────────────────────────────────────────────
void TestPeak() {
    std::printf("\n2. +6 dB peak at 1 kHz measures +6 dB at 1 kHz\n");
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        Processor p;
        p.Init(fs, 2);
        p.Reset(FromText("peak 1000 1.41 6\n", fs));
        const size_t n = (size_t)fs;
        auto in = Sine(fs, 1000, 0.25, n, 2);
        auto out = in;
        Run(p, out, 2, fs);
        const double gain = RmsDb(out, 2, 0, n / 2, n) - RmsDb(in, 2, 0, n / 2, n);
        char what[64];
        std::snprintf(what, sizeof what, "fs %.0f: gain at 1 kHz", fs);
        Check(std::fabs(gain - 6) < 0.05, what, "= %+.3f dB", gain);
    }
    // A 10-band curve like the app's presets: measured response matches the analytic curve client.js draws.
    const double fs = 48000;
    const char* cfg = "preamp -4.5\npeak 32 1.41 3\npeak 64 1.41 2.5\npeak 125 1.41 1\npeak 250 1.41 -1\npeak 500 1.41 -2\n"
                      "peak 1000 1.41 0\npeak 2000 1.41 1.5\npeak 4000 1.41 3\npeak 8000 1.41 4\npeak 16000 1.41 2\n";
    const Params prm = FromText(cfg, fs);
    double worst = 0;
    for (double f : {40.0, 100.0, 300.0, 1000.0, 3000.0, 6000.0, 12000.0}) {
        Processor p;
        p.Init(fs, 2);
        p.Reset(prm);
        const size_t n = 96000;
        auto in = Sine(fs, f, 0.1, n, 2);
        auto out = in;
        Run(p, out, 2, fs);
        const double err = std::fabs((RmsDb(out, 2, 1, n / 2, n) - RmsDb(in, 2, 1, n / 2, n)) - MagDb(prm, f, fs));
        worst = std::max(worst, err);
    }
    Check(worst < 0.05, "10-band curve + preamp vs analytic (client.js) response", "worst error %.4f dB over 7 freqs", worst);
}

// ── 3. numerical safety ─────────────────────────────────────────────────
bool AllFiniteNormal(const std::vector<float>& v, size_t* subnormals) {
    *subnormals = 0;
    for (float x : v) {
        if (!std::isfinite(x)) return false;
        if (x != 0 && std::fpclassify(x) == FP_SUBNORMAL) ++*subnormals;
    }
    return true;
}

void TestSafety() {
    std::printf("\n3. no NaN / denormal blowups\n");
    const char* extreme = "preamp 0\npeak 10 30 30\npeak 20000 0.1 -30\npeak 60 0.1 30\npeak 23900 30 30\n"
                          "reverb 15 200 12\nspatial 1\n";
    for (double fs : {44100.0, 48000.0, 192000.0}) {
        Processor p;
        p.Init(fs, 2);
        p.Reset(FromText(extreme, fs));
        const size_t burst = (size_t)(2 * fs), tail = (size_t)(20 * fs);
        auto buf = Noise(burst, 2, 0.01, 3);
        buf.resize((burst + tail) * 2, 0.f);  // 2 s noise then 20 s of digital silence: the reverb rings into denormal range
        const size_t block = (size_t)(fs / 100);
        std::vector<double> cost;
        for (size_t at = 0; at < burst + tail; at += block) {
            const auto t0 = std::chrono::steady_clock::now();
            p.Process(buf.data() + at * 2, (uint32_t)std::min(block, burst + tail - at), false);
            cost.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        size_t sub = 0;
        const bool finite = AllFiniteNormal(buf, &sub);
        double early = 0, late = 0;  // CPU per block: first second vs last second (denormal stalls would show here)
        const size_t perSec = cost.size() / 22;
        for (size_t i = 0; i < perSec; ++i) { early += cost[i]; late += cost[cost.size() - 1 - i]; }
        char what[96];
        std::snprintf(what, sizeof what, "fs %.0f extreme cfg, 2 s noise + 20 s silence: finite, 0 subnormal", fs);
        Check(finite && sub == 0, what, "(subnormals: %.0f)", (double)sub);
        std::snprintf(what, sizeof what, "fs %.0f CPU late/early ratio (denormal stall check)", fs);
        Check(late / early < 2.0, what, "%.2f", late / early);
    }
    {   // NaN from upstream: muted for one block, then clean again
        const double fs = 48000;
        Processor p;
        p.Init(fs, 2);
        p.Reset(FromText("peak 100 1 6\nreverb 2 20 -6\nspatial 0.5\n", fs));
        auto buf = Sine(fs, 440, 0.3, 48000, 2);
        buf[4800 * 2 + 7] = NAN;
        Run(p, buf, 2, fs);
        size_t sub = 0;
        const bool finite = AllFiniteNormal(buf, &sub);
        const double after = RmsDb(buf, 2, 0, 24000, 48000);
        Check(finite && after > -20, "NaN injected mid-stream: output stays finite and recovers", "(rms after %.1f dBFS)", after);
    }
    {   // silent-flagged buffers: the reverb tail is played out, then the APO stops touching them
        const double fs = 48000;
        Processor p;
        p.Init(fs, 2);
        p.Reset(FromText("reverb 1 0 -3\n", fs));
        auto buf = Noise(4800, 2, 0.5, 4);
        p.Process(buf.data(), 4800, false);
        std::vector<float> s(480 * 2);
        int wroteBlocks = 0, firstUntouched = -1;
        double firstTail = 0;
        for (int k = 0; k < 1000 && firstUntouched < 0; ++k) {
            std::fill(s.begin(), s.end(), 123.f);  // "undefined" content of a silent buffer
            if (p.Process(s.data(), 480, true)) {
                ++wroteBlocks;
                if (k == 0) firstTail = RmsDb(s, 2, 0, 0, 480);
            } else {
                firstUntouched = k;
            }
        }
        const bool garbageIgnored = s[0] == 123.f;
        Check(firstTail > -40 && firstUntouched > 0 && garbageIgnored, "silent buffers: tail rendered, then left untouched",
              "(tail %.1f dBFS first block; untouched after %.0f blocks)", firstTail, (double)firstUntouched);
    }
}

// ── 4. parameter swaps are click-free ───────────────────────────────────
// Biggest second difference |y[n] - 2y[n-1] + y[n-2]| right after a swap, relative to the steady signal around it.
// A 200 Hz sine has a tiny second difference (A*w^2); a step or a kink (a click) makes it jump by orders of magnitude.
double SwapSpike(const char* from, const char* to, double fadeMs, int swapAtFrameOffset) {
    const double fs = 48000;
    Processor p;
    p.Init(fs, 2, fadeMs);
    p.Reset(FromText(from, fs));
    const size_t n = 96000, block = 480, swapBlock = 100;  // swap at t = 1.0 s (+ offset into the sine phase)
    auto buf = Sine(fs, 200, 0.5, n, 2, 2 * kPi * 200 * swapAtFrameOffset / fs);
    Run(p, buf, 2, fs, [&](size_t k) {
        if (k == swapBlock) { p.Inbox().Back() = FromText(to, fs); p.Inbox().Publish(); }
    });
    auto maxJump = [&](size_t a, size_t b) {
        double m = 0;
        for (size_t i = a + 2; i < b; ++i)
            for (int c = 0; c < 2; ++c)
                m = std::max(m, std::fabs((double)buf[i * 2 + c] - 2.0 * buf[(i - 1) * 2 + c] + buf[(i - 2) * 2 + c]));
        return m;
    };
    const size_t at = swapBlock * block;
    const double before = maxJump(at - 9600, at), after = maxJump(n - 9600, n);
    const double window = maxJump(at - 2, at + (size_t)(0.2 * fs));
    return window / std::max(before, after);
}

void TestSwap() {
    std::printf("\n4. parameter swap: no discontinuity spike (max |d2y| in 200 ms after swap / steady max |d2y|; a click scores >> 10)\n");
    struct Case { const char* name; const char* from; const char* to; } cases[] = {
        {"bypass -> EQ +12 dB @200 Hz", "enabled 0\n", "peak 200 1.41 12\n"},
        {"EQ +6 -> -6 dB @200 Hz", "peak 200 1.41 6\n", "peak 200 1.41 -6\n"},
        {"preamp 0 -> -12 dB", "peak 1000 1 3\n", "preamp -12\npeak 1000 1 3\n"},
        {"reverb off -> on", "peak 1000 1 3\n", "peak 1000 1 3\nreverb 2.4 28 -2\n"},
        {"reverb predelay 10 -> 120 ms", "reverb 2.4 10 -2\n", "reverb 2.4 120 -2\n"},
        {"spatial 0 -> 1", "peak 1000 1 3\n", "peak 1000 1 3\nspatial 1\n"},
        {"everything -> enabled 0", "preamp -3\npeak 200 1.41 9\nreverb 2.4 28 -2\nspatial 0.6\n", "enabled 0\n"},
    };
    for (auto& k : cases) {
        double faded = 0, instant = 0;
        for (int off : {0, 37, 61, 90}) {  // several phases of the sine at the swap instant
            faded = std::max(faded, SwapSpike(k.from, k.to, 30, off));
            instant = std::max(instant, SwapSpike(k.from, k.to, 0.0001, off));
        }
        Check(faded < 1.3, k.name, "30 ms fade: %.3f   (instant swap would be %.2f)", faded, instant);
    }
}

// ── 5. config file plumbing ─────────────────────────────────────────────
void TestConfig() {
    std::printf("\n5. config parsing and the file watcher\n");
    const char* text = "\xEF\xBB\xBF# kaleidoscope-eq 1\r\nenabled 1\r\npreamp -3.5\r\npeak 32 1.41 3\r\npeak 64 1.41 2.5\r\n"
                       "future-directive 1 2 3\r\npeak 1,5 1 1\r\nreverb 2.4 28 -2\r\nspatial 0.6\r\n";
    const Config c = ParseConfig(text, std::strlen(text));
    Check(c.enabled && c.preampDb == -3.5 && c.nPeaks == 2 && c.peaks[1].f == 64 && c.peaks[1].gainDb == 2.5 &&
              c.reverb && c.decaySec == 2.4 && c.predelayMs == 28 && c.wetDb == -2 && c.spatial == 0.6,
          "CONTRACT §1 sample parses (BOM, CRLF, unknown + malformed lines ignored)");

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"keq-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring cfg = dir + L"\\{6f716e85-6fb5-4258-b265-b0fbbe9be74e}.cfg";
    auto writeAtomic = [&](const char* body) {  // what the bridge does: temp file, then rename over
        const std::wstring t = cfg + L".new";
        HANDLE h = CreateFileW(t.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w;
        WriteFile(h, body, (DWORD)std::strlen(body), &w, nullptr);
        CloseHandle(h);
        MoveFileExW(t.c_str(), cfg.c_str(), MOVEFILE_REPLACE_EXISTING);
    };
    const double fs = 48000;
    Mailbox<Params> box;
    ConfigWatcher watcher;
    auto waitFor = [&](const Params& want, double* ms) {
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) {
            if (const Params* got = box.Take())
                if (SameParams(*got, want)) {
                    *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    return true;
                }
            Sleep(1);
        }
        return false;
    };
    double ms = 0;
    watcher.Start(cfg.c_str(), fs, &box);
    Check(waitFor(Bypass(), &ms), "watcher: no file -> publishes bypass", "(%.0f ms)", ms);
    const char* a = "enabled 1\npeak 1000 1.41 6\nreverb 1.2 20 -6\n";
    writeAtomic(a);
    Check(waitFor(FromText(a, fs), &ms), "watcher: new file (atomic rename) -> designed params published", "(%.0f ms)", ms);
    const char* b = "enabled 1\npeak 1000 1.41 -6\n";
    writeAtomic(b);
    Check(waitFor(FromText(b, fs), &ms), "watcher: replaced file -> new params published", "(%.0f ms)", ms);
    DeleteFileW(cfg.c_str());
    Check(waitFor(Bypass(), &ms), "watcher: file deleted -> bypass", "(%.0f ms)", ms);
    watcher.Stop();

    const std::wstring live = dir + L"\\{6f716e85-6fb5-4258-b265-b0fbbe9be74e}.live";
    char got[128] = {};
    bool ok = WriteLiveFile(live.c_str());
    HANDLE h = CreateFileW(live.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD r = 0;
    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, got, sizeof got - 1, &r, nullptr); CloseHandle(h); }
    char want[32];
    std::snprintf(want, sizeof want, "pid %lu\r\n", GetCurrentProcessId());
    ok = ok && std::strncmp(got, want, std::strlen(want)) == 0 && std::strchr(got + std::strlen(want), 'Z');
    std::string shown(got);
    for (auto& ch : shown) if (ch == '\r' || ch == '\n') ch = ' ';
    std::printf("  .live content: \"%s\"\n", shown.c_str());
    Check(ok, ".live file: \"pid <pid>\" line + UTC time line");
    DeleteFileW(live.c_str());
    RemoveDirectoryW(dir.c_str());
}

// ── 6. cost ──────────────────────────────────────────────────────────────
void TestCost() {
    std::printf("\n6. CPU cost (single core)\n");
    const double fs = 48000;
    Processor p;
    p.Init(fs, 2);
    p.Reset(FromText("preamp -4\npeak 32 1.41 3\npeak 64 1.41 2.5\npeak 125 1.41 1\npeak 250 1.41 -1\npeak 500 1.41 -2\n"
                     "peak 1000 1.41 1\npeak 2000 1.41 1.5\npeak 4000 1.41 3\npeak 8000 1.41 4\npeak 16000 1.41 2\n"
                     "reverb 2.4 28 -2\nspatial 0.6\n", fs));
    auto buf = Noise((size_t)(10 * fs), 2, 0.2, 5);
    const auto t0 = std::chrono::steady_clock::now();
    Run(p, buf, 2, fs);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Check(sec < 1.0, "10 peaks + reverb + spatial, 48 kHz stereo", "%.2f%% of one core", 100 * sec / 10);
}


// ── 7. the built DLL, end to end through COM (no registration) ──────────
// A fake endpoint device stands in for audiodg's APOInitSystemEffects2.pDeviceCollection.
struct FakeDevice : IMMDevice, IMMDeviceCollection {
    const wchar_t* id;
    explicit FakeDevice(const wchar_t* i) : id(i) {}
    STDMETHODIMP QueryInterface(REFIID, void** ppv) override { *ppv = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP Activate(REFIID, DWORD, PROPVARIANT*, void**) override { return E_NOTIMPL; }
    STDMETHODIMP OpenPropertyStore(DWORD, IPropertyStore**) override { return E_NOTIMPL; }
    STDMETHODIMP GetId(LPWSTR* out) override {
        const size_t n = (wcslen(id) + 1) * sizeof(wchar_t);
        *out = (LPWSTR)CoTaskMemAlloc(n);
        memcpy(*out, id, n);
        return S_OK;
    }
    STDMETHODIMP GetState(DWORD* s) override { *s = DEVICE_STATE_ACTIVE; return S_OK; }
    STDMETHODIMP GetCount(UINT* n) override { *n = 1; return S_OK; }
    STDMETHODIMP Item(UINT, IMMDevice** d) override { *d = this; return S_OK; }
};

void TestDll() {
    std::printf("\n7. built KaleidoscopeEQ.dll through COM (LoadLibrary, no registration)\n");
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dllPath(exe);
    dllPath = dllPath.substr(0, dllPath.rfind(L'\\')) + L"\\KaleidoscopeEQ.dll";
    HMODULE dll = LoadLibraryW(dllPath.c_str());
    auto getClass = dll ? (HRESULT(WINAPI*)(REFCLSID, REFIID, void**))GetProcAddress(dll, "DllGetClassObject") : nullptr;
    const CLSID clsid = {0x3cdcc045, 0xa1a2, 0x4d8a, {0x93, 0x69, 0xfe, 0xdf, 0x2b, 0xd6, 0xc4, 0xc6}};
    IClassFactory* factory = nullptr;
    IAudioProcessingObject* apo = nullptr;
    if (getClass && SUCCEEDED(getClass(clsid, __uuidof(IClassFactory), (void**)&factory)))
        factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject), (void**)&apo);

    // audiodg creates APOs aggregated: CreateInstance(outer, IID_IUnknown). Interfaces handed out by the inner
    // object must count references on the outer object, and only IUnknown may be asked for when aggregating.
    if (factory) {
        struct Outer final : IUnknown {
            IUnknown* inner = nullptr;
            ULONG refs = 1;
            STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
                if (riid == __uuidof(IUnknown)) { *ppv = this; ++refs; return S_OK; }
                return inner->QueryInterface(riid, ppv);
            }
            STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
            STDMETHODIMP_(ULONG) Release() override { return --refs; }
        } outer;
        IUnknown* bad = nullptr;
        const HRESULT refused = factory->CreateInstance(&outer, __uuidof(IAudioProcessingObject), (void**)&bad);
        const HRESULT made = factory->CreateInstance(&outer, __uuidof(IUnknown), (void**)&outer.inner);
        IAudioProcessingObject* viaInner = nullptr;
        bool delegates = false, identity = false;
        if (SUCCEEDED(made) && outer.inner) {
            const ULONG before = outer.refs;
            outer.inner->QueryInterface(__uuidof(IAudioProcessingObject), (void**)&viaInner);
            delegates = viaInner && outer.refs == before + 1;  // AddRef went to the outer object
            IUnknown* back = nullptr;
            if (viaInner) viaInner->QueryInterface(__uuidof(IUnknown), (void**)&back);  // delegates to outer
            identity = back == &outer;
            if (back) back->Release();
            if (viaInner) viaInner->Release();
            outer.inner->Release();  // destroys the aggregate
        }
        Check(refused == CLASS_E_NOAGGREGATION && SUCCEEDED(made) && delegates && identity,
              "aggregation: refuses non-IUnknown, inner hands out interfaces that delegate to the outer object");
    }
    IAudioProcessingObjectRT* rt = nullptr;
    IAudioProcessingObjectConfiguration* conf = nullptr;
    IAudioSystemEffects2* fx2 = nullptr;
    if (apo) {
        apo->QueryInterface(__uuidof(IAudioProcessingObjectRT), (void**)&rt);
        apo->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration), (void**)&conf);
        apo->QueryInterface(__uuidof(IAudioSystemEffects2), (void**)&fx2);
    }
    Check(apo && rt && conf && fx2, "class factory creates the APO; RT / Configuration / SystemEffects2 interfaces");
    if (!(apo && rt && conf && fx2)) return;

    APO_REG_PROPERTIES* reg = nullptr;
    apo->GetRegistrationProperties(&reg);
    Check(reg && reg->clsid == clsid && reg->Flags == 0xF && reg->u32NumAPOInterfaces == 1, "registration properties (in-place, formats must match)");
    CoTaskMemFree(reg);

    // A private endpoint id so nothing real is touched; its folder is created only if missing and removed after.
    const wchar_t* guid = L"{6b1d2a3c-0000-4000-8000-00000000c0de}";
    const std::wstring id = std::wstring(L"{0.0.0.00000000}.") + L"{6B1D2A3C-0000-4000-8000-00000000C0DE}";
    wchar_t dirBuf[kPathMax];
    EndpointDir(dirBuf);
    const std::wstring dir = dirBuf, root = dir.substr(0, dir.rfind(L'\\'));
    const bool madeRoot = CreateDirectoryW(root.c_str(), nullptr) != 0, madeDir = CreateDirectoryW(dir.c_str(), nullptr) != 0;
    const std::wstring cfg = dir + L"\\" + guid + L".cfg", live = dir + L"\\" + guid + L".live";
    {
        HANDLE h = CreateFileW(cfg.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        const char body[] = "# kaleidoscope-eq 1\nenabled 1\npeak 1000 1.41 6\n";
        DWORD w;
        WriteFile(h, body, sizeof body - 1, &w, nullptr);
        CloseHandle(h);
    }
    FakeDevice dev(id.c_str());
    APOInitSystemEffects2 init = {};
    init.APOInit.cbSize = sizeof init;
    init.APOInit.clsid = clsid;
    init.pDeviceCollection = &dev;
    Check(SUCCEEDED(apo->Initialize(sizeof init, (BYTE*)&init)), "Initialize(APOInitSystemEffects2)");

    WAVEFORMATEX pcm = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
    IAudioMediaType *req = nullptr, *sup = nullptr;
    CreateAudioMediaType(&pcm, sizeof pcm, &req);
    const HRESULT hr = apo->IsInputFormatSupported(nullptr, req, &sup);
    const WAVEFORMATEX* s = sup ? sup->GetAudioFormat() : nullptr;
    Check(hr == S_FALSE && s && s->wBitsPerSample == 32 && s->nChannels == 2 && s->nSamplesPerSec == 48000,
          "16-bit PCM request -> proposes float32, same rate/channels");
    if (req) req->Release();

    APO_CONNECTION_DESCRIPTOR d = {};
    d.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
    d.u32MaxFrameCount = 480;
    d.pFormat = sup;
    APO_CONNECTION_DESCRIPTOR* dp = &d;
    Check(SUCCEEDED(conf->LockForProcess(1, &dp, 1, &dp)), "LockForProcess (float32 48 kHz stereo)");

    char text[128] = {};
    HANDLE h = CreateFileW(live.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD got = 0;
    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, text, sizeof text - 1, &got, nullptr); CloseHandle(h); }
    unsigned long pid = 0;
    sscanf_s(text, "pid %lu", &pid);
    Check(pid == GetCurrentProcessId(), ".live written on LockForProcess with this process's pid", "(pid %.0f)", (double)pid);

    const size_t n = 48000;
    auto in = Sine(48000, 1000, 0.25, n, 2);
    auto buf = in;
    for (size_t at = 0; at < n; at += 480) {
        APO_CONNECTION_PROPERTY prop = {(UINT_PTR)(buf.data() + at * 2), 480, BUFFER_VALID, APO_CONNECTION_PROPERTY_SIGNATURE};
        APO_CONNECTION_PROPERTY* pp = &prop;
        rt->APOProcess(1, &pp, 1, &pp);
    }
    const double gain = RmsDb(buf, 2, 0, n / 2, n) - RmsDb(in, 2, 0, n / 2, n);
    Check(std::fabs(gain - 6) < 0.05, "APOProcess applies the endpoint's .cfg (peak 1000 1.41 6)", "= %+.3f dB", gain);

    conf->UnlockForProcess();
    DeleteFileW(cfg.c_str());
    DeleteFileW(live.c_str());
    if (madeDir) RemoveDirectoryW(dir.c_str());
    if (madeRoot) RemoveDirectoryW(root.c_str());
    if (sup) sup->Release();
    fx2->Release(); conf->Release(); rt->Release(); apo->Release();
    factory->Release();
    using CanUnload = HRESULT(WINAPI*)();
    Check(((CanUnload)GetProcAddress(dll, "DllCanUnloadNow"))() == S_OK, "DllCanUnloadNow after release");
}

}  // namespace

// ── 8. surround (spatial) ───────────────────────────────────────────────
namespace {

// Pink-ish noise (Paul Kellet's three-pole economy filter, about -3 dB/octave from 40 Hz to 15 kHz), unit RMS.
std::vector<double> Pink(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0, 1);
    std::vector<double> v(n);
    double b0 = 0, b1 = 0, b2 = 0, e = 0;
    for (size_t i = 0; i < n; ++i) {
        const double w = g(rng);
        b0 = 0.99765 * b0 + w * 0.0990460;
        b1 = 0.96300 * b1 + w * 0.2965164;
        b2 = 0.57000 * b2 + w * 1.0526913;
        v[i] = b0 + b1 + b2 + w * 0.1848;
        e += v[i] * v[i];
    }
    for (auto& x : v) x /= std::sqrt(e / n);
    return v;
}

std::vector<float> RunSpatial(const std::vector<float>& in, double s, double fs) {
    Processor p;
    p.Init(fs, 2);
    char cfg[64];
    std::snprintf(cfg, sizeof cfg, "spatial %.3f\n", s);
    p.Reset(FromText(s > 0 ? cfg : "", fs));
    auto buf = in;
    Run(p, buf, 2, fs);
    return buf;
}

// One channel of an interleaved stereo buffer through `passes` one-pole low-passes (or the complementary high-pass).
std::vector<double> Band(const std::vector<float>& v, int c, double hz, double fs, bool high, int passes = 2) {
    std::vector<double> x(v.size() / 2);
    for (size_t i = 0; i < x.size(); ++i) x[i] = v[2 * i + c];
    const double a = 1 - std::exp(-2 * kPi * hz / fs);
    for (int k = 0; k < passes; ++k) {
        double z = 0;
        for (auto& s : x) { z += a * (s - z); s = high ? s - z : z; }
    }
    return x;
}

double Db(const std::vector<double>& x, size_t from, size_t to) {
    double e = 0;
    for (size_t i = from; i < to; ++i) e += x[i] * x[i];
    return 10 * std::log10(e / (to - from) + 1e-30);
}

// Lag (in samples, fractional by parabolic interpolation) at which b best matches a delayed copy of a; positive = b later.
double XcorrLag(const std::vector<double>& a, const std::vector<double>& b, size_t from, size_t to, int maxLag) {
    double best = -1e300, at = 0;
    std::vector<double> r(2 * maxLag + 1);
    for (int L = -maxLag; L <= maxLag; ++L) {
        double s = 0;
        for (size_t i = from; i < to; ++i) s += a[i] * b[i + L];
        r[L + maxLag] = s;
        if (s > best) { best = s; at = L; }
    }
    const int k = (int)at + maxLag;
    if (k > 0 && k < 2 * maxLag) {
        const double y0 = r[k - 1], y1 = r[k], y2 = r[k + 1], den = y0 - 2 * y1 + y2;
        if (den != 0) at += 0.5 * (y0 - y2) / den;
    }
    return at;
}

// Interaural cross-correlation coefficient, the concert-hall measure (ISO 3382): the largest normalised
// cross-correlation of the two ears within +-1 ms, here on the 500 Hz - 2 kHz band (Beranek's IACC_E3 range).
// 1 - IACC tracks apparent source width / envelopment.
double Iacc(const std::vector<float>& v, double fs, size_t from, size_t to) {
    auto band = [&](int c) {
        auto x = Band(v, c, 2000, fs, false, 2);
        std::vector<float> tmp(2 * x.size());
        for (size_t i = 0; i < x.size(); ++i) tmp[2 * i] = (float)x[i];
        return Band(tmp, 0, 500, fs, true, 2);
    };
    const auto a = band(0), b = band(1);
    const int maxLag = (int)(fs / 1000);
    double ea = 0, eb = 0, best = 0;
    for (size_t i = from; i < to; ++i) ea += a[i] * a[i], eb += b[i] * b[i];
    for (int L = -maxLag; L <= maxLag; ++L) {
        double s = 0;
        for (size_t i = from; i < to; ++i) s += a[i] * b[i + L];
        best = std::max(best, std::fabs(s));
    }
    return best / std::sqrt(ea * eb + 1e-30);
}

double Corr(const std::vector<float>& v, size_t from, size_t to) {
    double sl = 0, sr = 0, slr = 0;
    for (size_t i = from; i < to; ++i) { const double l = v[2 * i], r = v[2 * i + 1]; sl += l * l; sr += r * r; slr += l * r; }
    return slr / std::sqrt(sl * sr + 1e-30);
}

}  // namespace

void TestSurround() {
    std::printf("\n8. surround (spatial): virtual speakers, head model, level, audibility\n");
    const double fs = 48000;
    const size_t n = 4 * 48000, from = 24000, to = n - 2000;

    {   // (a) a hard-left source reaches the right ear later (ITD) and darker (head shadow)
        auto w = Noise(n, 1, 0.3, 21);
        std::vector<float> in(2 * n, 0.f);
        for (size_t i = 0; i < n; ++i) in[2 * i] = w[i];
        const auto out = RunSpatial(in, 1, fs);
        const auto lL = Band(out, 0, 800, fs, false), lR = Band(out, 1, 800, fs, false);
        const double itdMs = XcorrLag(lL, lR, from, to, 48) / fs * 1000;
        Check(itdMs > 0.2 && itdMs < 0.6, "hard left: right ear lags the left ear below 800 Hz by a head-sized ITD",
              "(%.3f ms; a real head: 0.38 ms at low frequencies for 30 degrees, the room's taps pull it up a little)", itdMs);
        const auto hL = Band(out, 0, 4000, fs, true), hR = Band(out, 1, 4000, fs, true);
        const double ildLow = Db(lL, from, to) - Db(lR, from, to), ildHigh = Db(hL, from, to) - Db(hR, from, to);
        Check(ildHigh - ildLow > 6, "hard left: right ear is low-passed (head shadow): ILD above 4 kHz >> below 800 Hz",
              "(ILD low %.1f dB, high %.1f dB)", ildLow, ildHigh);
        Check(ildLow < 6, "hard left: right ear gets real low-frequency level (speaker-like, not headphone-isolated)",
              "(left - right below 800 Hz: %.1f dB)", ildLow);
    }

    {   // (b) the head model itself: one speaker of a front (30) and a rear (110 degree) pair, impulse responses
        struct Resp { double nearDb[6], farDb[6], itdMs; } resp[2];
        const double freqs[6] = {200, 500, 2000, 5000, 8000, 12000};
        const double az[2] = {30, 110};
        for (int k = 0; k < 2; ++k) {
            SpeakerPair p;
            p.Init(fs, az[k], 1);
            const int len = 4096;
            std::vector<double> nearIr(len), farIr(len);
            for (int i = 0; i < len; ++i) p.Tick(i == 0 ? 1 : 0, 0, nearIr[i], farIr[i]);
            std::complex<double> nearH[6], farH[6];
            for (int f = 0; f < 6; ++f) {
                for (int i = 0; i < len; ++i) {
                    const std::complex<double> e = std::polar(1.0, -2 * kPi * freqs[f] * i / fs);
                    nearH[f] += nearIr[i] * e;
                    farH[f] += farIr[i] * e;
                }
                resp[k].nearDb[f] = 20 * std::log10(std::abs(nearH[f]));
                resp[k].farDb[f] = 20 * std::log10(std::abs(farH[f]));
            }
            resp[k].itdMs = std::arg(nearH[0] / farH[0]) / (2 * kPi * freqs[0]) * 1000;  // interaural phase delay at 200 Hz
        }
        const Resp &fr = resp[0], &re = resp[1];
        Check(fr.itdMs > 0.33 && fr.itdMs < 0.43 && re.itdMs > 0.62 && re.itdMs < 0.8,
              "head model ITD at 200 Hz: 30 deg ~0.38 ms, 110 deg ~0.72 ms (Kuhn: 3a/c sin(lateral))", "(%.3f ms, %.3f ms)",
              fr.itdMs, re.itdMs);
        const double shadowF = fr.nearDb[4] - fr.farDb[4], shadowR = re.nearDb[4] - re.farDb[4];
        Check(shadowF > 10 && shadowR > 10 && std::fabs(fr.nearDb[0] - fr.farDb[0]) < 1,
              "head model: far ear is shadowed at 8 kHz, both ears equal at 200 Hz", "(near - far at 8 kHz: front %.1f, rear %.1f dB)",
              shadowF, shadowR);
        // Spectral shape of the near ear relative to 500 Hz: the rear speaker must be clearly darker up top.
        double worst = 0;
        for (int f = 3; f < 6; ++f) worst = std::max(worst, (fr.nearDb[f] - fr.nearDb[1]) - (re.nearDb[f] - re.nearDb[1]));
        char what[200];
        std::snprintf(what, sizeof what, "rear speaker differs from front (pinna echo + back shadow): near ear re 500 Hz at 5/8/12 kHz "
                      "front %+.1f %+.1f %+.1f, rear %+.1f %+.1f %+.1f dB",
                      fr.nearDb[3] - fr.nearDb[1], fr.nearDb[4] - fr.nearDb[1], fr.nearDb[5] - fr.nearDb[1],
                      re.nearDb[3] - re.nearDb[1], re.nearDb[4] - re.nearDb[1], re.nearDb[5] - re.nearDb[1]);
        Check(worst > 4, what);
    }

    {   // (c) a centred mono source keeps its level as strength goes 0 -> 0.5 -> 1, and stays mostly in front
        const auto pk = Pink(n, 22);
        std::vector<float> in(2 * n);
        for (size_t i = 0; i < n; ++i) in[2 * i] = in[2 * i + 1] = (float)(0.2 * pk[i]);
        const auto inL = Band(in, 0, 150, fs, false);
        double worst = 0, lv[3], bass[3];
        const double ss[3] = {0, 0.5, 1};
        std::vector<float> full;
        for (int k = 0; k < 3; ++k) {
            const auto out = RunSpatial(in, ss[k], fs);
            lv[k] = 0.5 * (RmsDb(out, 2, 0, from, to) + RmsDb(out, 2, 1, from, to)) - RmsDb(in, 2, 0, from, to);
            bass[k] = Db(Band(out, 0, 150, fs, false), from, to) - Db(inL, from, to);
            worst = std::max(worst, std::fabs(lv[k]));
            if (k == 2) full = out;
        }
        char what[200];
        std::snprintf(what, sizeof what, "mono pink centre: level within +-1.5 dB at strength 0/0.5/1 (%+.2f %+.2f %+.2f dB; below 150 Hz %+.2f %+.2f dB)",
                      lv[0], lv[1], lv[2], bass[1], bass[2]);
        Check(worst <= 1.5 && std::fabs(bass[2]) <= 1.5, what);
        const double cm = Corr(full, from, to);
        Check(cm > 0.5 && cm < 0.95, "mono centre at strength 1: anchored in front (direct sound identical at both ears), room around it",
              "(interaural correlation %.2f)", cm);
    }

    {   // (d) audibility on a realistic mix: pink noise, interchannel correlation 0.6
        const auto a = Pink(n, 23), b = Pink(n, 24);
        std::vector<float> in(2 * n);
        for (size_t i = 0; i < n; ++i) {
            in[2 * i] = (float)(0.2 * (std::sqrt(0.8) * a[i] + std::sqrt(0.2) * b[i]));
            in[2 * i + 1] = (float)(0.2 * (std::sqrt(0.8) * a[i] - std::sqrt(0.2) * b[i]));
        }
        const double ss[5] = {0.1, 0.25, 0.5, 0.75, 1};
        double err[5], lvl[5], corr[5];
        bool mono = true;
        for (int k = 0; k < 5; ++k) {
            const auto out = RunSpatial(in, ss[k], fs);
            corr[k] = Iacc(out, fs, from, to);
            double e = 0, x = 0;
            for (size_t i = 2 * from; i < 2 * to; ++i) { const double d = out[i] - in[i]; e += d * d; x += (double)in[i] * in[i]; }
            err[k] = 10 * std::log10(e / x);
            lvl[k] = 0.5 * (RmsDb(out, 2, 0, from, to) + RmsDb(out, 2, 1, from, to)) - RmsDb(in, 2, 0, from, to);
            if (k > 0 && err[k] <= err[k - 1]) mono = false;
        }
        char what[200];
        std::snprintf(what, sizeof what, "corr-0.6 pink: difference from dry grows with strength (0.1/0.25/0.5/0.75/1: %.1f %.1f %.1f %.1f %.1f dB)",
                      err[0], err[1], err[2], err[3], err[4]);
        Check(mono, what);
        Check(err[4] >= -12, "corr-0.6 pink at strength 1: clearly audible (difference >= -12 dB re dry)", "(%.1f dB)", err[4]);
        Check(std::fabs(lvl[4]) <= 1.5, "corr-0.6 pink at strength 1: no loudness jump", "(%+.2f dB)", lvl[4]);
        const double dryIacc = Iacc(in, fs, from, to);
        Check(corr[4] < dryIacc - 0.1 && corr[2] < dryIacc && corr[2] > corr[4],
              "corr-0.6 pink: IACC 500 Hz-2 kHz falls with strength (wider, enveloping)",
              "(dry %.2f -> strength 1: %.2f)", dryIacc, corr[4]);
        std::printf("        (IACC at 0.25/0.5/0.75: %.2f %.2f %.2f; broadband correlation dry %.2f -> %.2f: below 500 Hz a speaker\n"
                    "         pair merges the ears, as in a room, so broadband correlation is not the measure)\n",
                    corr[1], corr[2], corr[3], Corr(in, from, to), Corr(RunSpatial(in, 1, fs), from, to));
    }
}

int main() {
    TestPassthrough();
    TestPeak();
    TestSafety();
    TestSwap();
    TestConfig();
    TestCost();
    TestSurround();
    TestDll();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
