// Kaleidoscope EQ for macOS: one executable, two modes (CONTRACT.md §5).
//
//   keq-mac bridge   화면 ↔ 엔진 다리. CONTRACT §4의 JSON 한 줄 프로토콜을 표준 입출력으로 말한다.
//   keq-mac engine   엔진. 사용자 LaunchAgent로 돌며 시스템 소리 전체를 Core Audio 프로세스 탭으로 받아
//                    engine/src/dsp.cpp(Windows APO와 같은 코드)로 EQ를 걸고 기본 출력 장치로 내보낸다.
//   keq-mac --list   (개발용) 출력 장치 목록. --version: 판.
//
// 엔진의 길: 이 프로세스를 뺀 전역 스테레오 탭(muteBehavior = MutedWhenTapped: 탭을 읽는 동안 원래 소리는 묵음) +
// 기본 출력 장치를 주 장치로 하는 비공개 집합 장치(aggregate). 집합 장치의 IOProc이 탭(마지막 입력 버퍼)을 읽어
// keq::Processor에 통과시키고 출력에 쓴다. 프로세스가 끝나면 탭과 집합 장치도 사라져 소리는 원래대로 돌아온다.
//
// TCC(시스템 오디오 녹음): 허용 창은 AudioDeviceCreateIOProcIDWithBlock에서 뜬다. 답이 없으면 HAL 호출이 오류 없이
// 수십 초씩 막히고 IOProc이 불리지 않는다. 그래서 HAL 설정·해제는 모두 HAL 스레드에서 하고, 주 스레드는 HAL을
// 부르지 않고 파일(설정 감시, 상태 파일)만 다룬다. 시작 뒤 5초 안에 IOProc이 불리지 않으면 permission_pending.
//
// 크기: 앱스토어 자산 한도(build.py의 ENGINE_BUDGET) 때문에 실시간 부분 밖은 Foundation으로 짧게 쓴다(std::string을
// 이어 붙이는 C++ 코드는 이 파일에서 두 배 가까이 컸다).
//
// 시험용 환경 변수: KEQ_HOME (있으면 $HOME 대신 이 폴더 아래 Library/...를 쓰고, install/uninstall이 launchctl을 부르지 않는다).
#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>

#include <dlfcn.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "dsp.h"
#include "version.h"

namespace {

NSString* const kLabel = @"me.senhyo.kaleidoscope-eq";
const UInt32 kBufferFrames = 128;     // ~2.7 ms at 48 kHz; 64 overloaded on the test machine
const double kPendingAfter = 5.0;     // s without an IOProc call after start -> permission_pending
// An IOProc created while the TCC prompt was unanswered never starts, even after "Allow" (seen on the test machine:
// CreateIOProcID returned after ~60 s, Start after ~30 s more, then silence until a rebuild). So once Start has
// returned without a single IOProc call, build again: soon if TCC already says granted, else every 20 s.
const double kRetryNoAudio = 20.0;
const double kRetryGranted = 3.0;
const double kStallAfter = 3.0;       // s without IOProc calls after it ran -> rebuild

double Now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

FILE* gLog = stderr;
// One line with a UTC time stamp. Lifecycle events only; never from the IOProc.
void Log(NSString* fmt, ...) NS_FORMAT_FUNCTION(1, 2);
void Log(NSString* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    NSString* s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSISO8601DateFormatter* f = [NSISO8601DateFormatter new];
    f.formatOptions |= NSISO8601DateFormatWithFractionalSeconds;
    fprintf(gLog, "%s pid %d %s\n", [f stringFromDate:NSDate.date].UTF8String, getpid(), s.UTF8String);
    fflush(gLog);
}

NSString* FourCC(OSStatus s) {
    char c[5] = {(char)(s >> 24), (char)(s >> 16), (char)(s >> 8), (char)s, 0};
    for (int i = 0; i < 4; ++i)
        if (c[i] < 32 || c[i] > 126) return @(s).stringValue;
    return [NSString stringWithFormat:@"'%s'", c];
}

// ── paths (CONTRACT §5) ─────────────────────────────────────────────────

bool TestHome() { return NSProcessInfo.processInfo.environment[@"KEQ_HOME"].length > 0; }
NSString* Home() { return TestHome() ? NSProcessInfo.processInfo.environment[@"KEQ_HOME"] : NSHomeDirectory(); }
NSString* Support() { return [Home() stringByAppendingPathComponent:@"Library/Application Support/KaleidoscopeEQ"]; }
NSString* Endpoints() { return [Support() stringByAppendingPathComponent:@"endpoints"]; }
NSString* InstalledBin() { return [Support() stringByAppendingPathComponent:@"keq-mac"]; }
NSString* VersionFile() { return [Support() stringByAppendingPathComponent:@"version"]; }
NSString* StateFile() { return [Support() stringByAppendingPathComponent:@"engine.state"]; }
NSString* PlistPath() {
    return [Home() stringByAppendingFormat:@"/Library/LaunchAgents/%@.plist", kLabel];
}
NSString* LogDir() { return [Home() stringByAppendingPathComponent:@"Library/Logs/KaleidoscopeEQ"]; }

// File name of a device: its UID with every byte outside [A-Za-z0-9._-] replaced by '_'.
NSString* Safe(NSString* uid) {
    const char* u = uid.UTF8String ?: "";
    NSMutableString* s = [NSMutableString string];
    for (const unsigned char* p = (const unsigned char*)u; *p; ++p) {
        const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' ||
                        *p == '_' || *p == '-';
        [s appendFormat:@"%c", ok ? *p : '_'];
    }
    if (!s.length || [s isEqualToString:@"."] || [s isEqualToString:@".."]) [s insertString:@"_" atIndex:0];
    return s;
}
NSString* CfgPath(NSString* uid) { return [Endpoints() stringByAppendingFormat:@"/%@.cfg", Safe(uid)]; }
NSString* LivePath(NSString* uid) { return [Endpoints() stringByAppendingFormat:@"/%@.live", Safe(uid)]; }

NSFileManager* FM() { return NSFileManager.defaultManager; }
bool MkdirP(NSString* dir) { return [FM() createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil]; }
NSString* ReadText(NSString* path) {
    NSData* d = [NSData dataWithContentsOfFile:path];
    return d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] : nil;
}
// Temp file in the same folder, then rename (NSDataWritingAtomic): readers see the old or the new file, never half.
bool WriteAtomic(NSString* path, NSString* text) {
    return [[text dataUsingEncoding:NSUTF8StringEncoding] writeToFile:path options:NSDataWritingAtomic error:nil];
}

NSString* SelfPath() {
    char buf[PATH_MAX * 2];
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) != 0) return nil;
    return [@(buf) stringByResolvingSymlinksInPath];
}

// pid is alive and runs a keq-mac executable (guards against pid reuse after the engine died).
bool IsEngineProcess(pid_t pid) {
    if (pid <= 0 || (kill(pid, 0) != 0 && errno != EPERM)) return false;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    return proc_pidpath(pid, path, sizeof path) > 0 && [@(path).lastPathComponent isEqualToString:@"keq-mac"];
}

bool Supported() { return [NSProcessInfo.processInfo isOperatingSystemAtLeastVersion:{14, 2, 0}]; }

// ── Core Audio property helpers ─────────────────────────────────────────

AudioObjectPropertyAddress A(AudioObjectPropertySelector s, AudioObjectPropertyScope sc = kAudioObjectPropertyScopeGlobal) {
    return {s, sc, kAudioObjectPropertyElementMain};
}
template <class T>
OSStatus Get(AudioObjectID o, AudioObjectPropertyAddress a, T& v, UInt32 qs = 0, const void* q = nullptr) {
    UInt32 sz = sizeof(T);
    return AudioObjectGetPropertyData(o, &a, qs, q, &sz, &v);
}
NSString* GetStr(AudioObjectID o, AudioObjectPropertySelector s) {
    CFStringRef r = nullptr;
    if (Get(o, A(s), r) || !r) return nil;
    return (__bridge_transfer NSString*)r;
}
UInt32 GetU32(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope sc = kAudioObjectPropertyScopeGlobal,
              OSStatus* err = nullptr) {
    UInt32 v = 0;
    OSStatus e = Get(o, A(s, sc), v);
    if (err) *err = e;
    return e ? 0 : v;
}
UInt32 OutputChannels(AudioObjectID dev) {
    auto a = A(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) || !sz) return 0;
    NSMutableData* buf = [NSMutableData dataWithLength:sz];
    auto* bl = (AudioBufferList*)buf.mutableBytes;
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, bl)) return 0;
    UInt32 n = 0;
    for (UInt32 i = 0; i < bl->mNumberBuffers; ++i) n += bl->mBuffers[i].mNumberChannels;
    return n;
}
AudioObjectID DefaultOutput() {
    AudioObjectID d = 0;
    Get(kAudioObjectSystemObject, A(kAudioHardwarePropertyDefaultOutputDevice), d);
    return d;
}
double NominalRate(AudioObjectID d) {
    Float64 sr = 0;
    Get(d, A(kAudioDevicePropertyNominalSampleRate), sr);
    return sr;
}

// Bluetooth -> headphones (most Bluetooth outputs are; a Bluetooth speaker also lands here). Built-in: by data source.
NSString* Kind(AudioObjectID d) {
    UInt32 t = GetU32(d, kAudioDevicePropertyTransportType);
    if (t == kAudioDeviceTransportTypeBluetooth || t == kAudioDeviceTransportTypeBluetoothLE) return @"headphones";
    if (t == kAudioDeviceTransportTypeBuiltIn) {
        OSStatus e;
        UInt32 ds = GetU32(d, kAudioDevicePropertyDataSource, kAudioObjectPropertyScopeOutput, &e);
        if (!e && ds == 'hdpn') return @"headphones";
        if (!e && ds == 'ispk') return @"speakers";
    }
    return @"other";
}

// An output device as the bridge reports it, without the engine fields. nil if d is not a usable output.
NSDictionary* DevOf(AudioObjectID d) {
    NSString* uid = d && OutputChannels(d) ? GetStr(d, kAudioDevicePropertyDeviceUID) : nil;
    if (!uid.length) return nil;
    return @{@"id": uid, @"name": GetStr(d, kAudioObjectPropertyName) ?: uid, @"kind": Kind(d)};
}
NSArray<NSDictionary*>* OutputDevices() {
    auto a = A(kAudioHardwarePropertyDevices);
    UInt32 sz = 0;
    NSMutableArray* r = [NSMutableArray array];
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz)) return r;
    NSMutableData* buf = [NSMutableData dataWithLength:sz];
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, buf.mutableBytes)) return r;
    const AudioObjectID* ids = (const AudioObjectID*)buf.bytes;
    for (UInt32 i = 0; i < sz / sizeof(AudioObjectID); ++i) {
        if (GetU32(ids[i], kAudioDevicePropertyIsHidden)) continue;
        if (NSDictionary* x = DevOf(ids[i])) [r addObject:x];
    }
    return r;
}

// Read-only TCC diagnostic (private SPI, may be missing): 0 granted, 1 denied, 2 not determined, <0 unknown.
// Only "denied" is trusted: right after a grant it can still say 2 for a while.
int TccPreflight() {
    static auto f = []() -> int (*)(CFStringRef, CFDictionaryRef) {
        void* h = dlopen("/System/Library/PrivateFrameworks/TCC.framework/TCC", RTLD_LAZY);
        return h ? (int (*)(CFStringRef, CFDictionaryRef))dlsym(h, "TCCAccessPreflight") : nullptr;
    }();
    return f ? f(CFSTR("kTCCServiceAudioCapture"), nullptr) : -1;
}

// ── engine.state (engine writes, bridge reads) ──────────────────────────
// First line `<state> [arg]`, second line `pid <pid>`. States: starting, running <uid>, permission_pending <uid>,
// permission_denied, unsupported, error <message>, stopped.

struct EngineState {
    NSString* state = @"";
    NSString* arg = @"";
    pid_t pid = 0;
};
EngineState ReadState() {
    EngineState s;
    NSArray<NSString*>* lines = [ReadText(StateFile()) componentsSeparatedByString:@"\n"];
    if (lines.count < 2) return s;
    NSRange sp = [lines[0] rangeOfString:@" "];
    s.state = sp.location == NSNotFound ? lines[0] : [lines[0] substringToIndex:sp.location];
    s.arg = sp.location == NSNotFound ? @"" : [lines[0] substringFromIndex:sp.location + 1];
    if ([lines[1] hasPrefix:@"pid "]) s.pid = (pid_t)[lines[1] substringFromIndex:4].intValue;
    return s;
}
pid_t LivePid(NSString* uid) {
    NSString* t = ReadText(LivePath(uid));
    return [t hasPrefix:@"pid "] ? (pid_t)[t substringFromIndex:4].intValue : 0;
}

// ═════════════════════════════════════════════════════════════════════════
// Engine
// ═════════════════════════════════════════════════════════════════════════

struct Stats {
    std::atomic<uint64_t> calls{0}, overloads{0}, n{0};
    std::atomic<double> inSq{0}, outSq{0};
};
Stats gS;
bool gMeasure = false;  // --log: per-second level ratio for tests (off in production)

void AtomicAdd(std::atomic<double>& a, double v) {
    double o = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(o, o + v, std::memory_order_relaxed)) {}
}

constexpr UInt32 kChunk = 1024;
float gStereo[2 * kChunk];                 // only the IOProc touches it
std::atomic<keq::Processor*> gRt{nullptr};  // processor the IOProc runs

// Realtime. No locks, no allocation, no Objective-C.
void IOProc(const AudioBufferList* in, AudioBufferList* out) {
    gS.calls.fetch_add(1, std::memory_order_relaxed);
    if (!out || !out->mNumberBuffers) return;
    // Output channels as (pointer, stride) in order; L/R go to the first two, everything else is silenced.
    float* oc[2] = {nullptr, nullptr};
    UInt32 os[2] = {0, 0}, nOut = 0, frames = UINT32_MAX;
    for (UInt32 b = 0; b < out->mNumberBuffers; ++b) {
        AudioBuffer& ob = out->mBuffers[b];
        const UInt32 ch = ob.mNumberChannels ? ob.mNumberChannels : 1;
        if (!ob.mData) continue;
        memset(ob.mData, 0, ob.mDataByteSize);
        if (ob.mDataByteSize / (4 * ch) < frames) frames = ob.mDataByteSize / (4 * ch);
        for (UInt32 c = 0; c < ch && nOut < 2; ++c, ++nOut) {
            oc[nOut] = (float*)ob.mData + c;
            os[nOut] = ch;
        }
    }
    // Tap = the last input buffer (the aggregate lists sub-device inputs first, taps after).
    const float* ic[2] = {nullptr, nullptr};
    UInt32 is = 0, iframes = 0;
    if (in && in->mNumberBuffers) {
        const AudioBuffer& last = in->mBuffers[in->mNumberBuffers - 1];
        const AudioBuffer* prev = in->mNumberBuffers >= 2 ? &in->mBuffers[in->mNumberBuffers - 2] : nullptr;
        const UInt32 ch = last.mNumberChannels ? last.mNumberChannels : 1;
        if (last.mData) {
            ic[0] = (const float*)last.mData;
            ic[1] = ch >= 2 ? ic[0] + 1 : ic[0];  // mono tap: both sides
            is = ch;
            iframes = last.mDataByteSize / (4 * ch);
            if (ch == 1 && prev && prev->mData && prev->mNumberChannels == 1 && prev->mDataByteSize == last.mDataByteSize)
                ic[0] = (const float*)prev->mData;  // non-interleaved stereo tap
        }
    }
    if (!ic[0] || !nOut || frames == UINT32_MAX) return;  // outputs stay silent
    const UInt32 total = iframes < frames ? iframes : frames;
    keq::Processor* proc = gRt.load(std::memory_order_acquire);
    double si = 0, so = 0;
    for (UInt32 base = 0; base < total; base += kChunk) {
        const UInt32 n = total - base < kChunk ? total - base : kChunk;
        for (UInt32 f = 0; f < n; ++f) {
            gStereo[2 * f] = ic[0][(base + f) * is];
            gStereo[2 * f + 1] = ic[1][(base + f) * is];
        }
        if (gMeasure)
            for (UInt32 i = 0; i < 2 * n; ++i) si += (double)gStereo[i] * gStereo[i];
        if (proc) proc->Process(gStereo, n, false);  // inactive params: buffer untouched (bit-exact)
        if (gMeasure)
            for (UInt32 i = 0; i < 2 * n; ++i) so += (double)gStereo[i] * gStereo[i];
        for (UInt32 f = 0; f < n; ++f) {
            if (nOut >= 2) {
                oc[0][(base + f) * os[0]] = gStereo[2 * f];
                oc[1][(base + f) * os[1]] = gStereo[2 * f + 1];
            } else {
                oc[0][(base + f) * os[0]] = 0.5f * (gStereo[2 * f] + gStereo[2 * f + 1]);
            }
        }
    }
    if (gMeasure) {
        AtomicAdd(gS.inSq, si);
        AtomicAdd(gS.outSq, so);
        gS.n.fetch_add(2 * total, std::memory_order_relaxed);
    }
}

OSStatus OnOverload(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*) {
    gS.overloads.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// What the HAL thread has built, shared with the main thread under gLock.
enum Phase { kIdle, kStarting, kStarted, kFailed, kDenied };
struct Shared {
    Phase phase = kIdle;
    NSString* uid = nil;
    NSString* error = nil;
    double sr = 0;         // the output device's nominal rate when built (a change means rebuild)
    double fs = 0;         // the rate the processor runs at (the aggregate's)
    double startedAt = 0;  // just before CreateIOProcID (where the TCC prompt fires)
    double runningAt = 0;  // when AudioDeviceStart returned
    uint64_t callsAtStart = 0;
    keq::Processor* proc = nullptr;
    int generation = 0;  // bumps on every build: the main thread re-publishes the config and re-writes .live
};
pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
Shared gShared;
struct Locked {
    Locked() { pthread_mutex_lock(&gLock); }
    ~Locked() { pthread_mutex_unlock(&gLock); }
};

std::atomic<bool> gQuit{false}, gHalDone{false};
void OnSignal(int) { gQuit.store(true); }

keq::Params Design(NSString* text, double fs) {
    if (!text) return keq::Bypass();  // missing -> pass-through (CONTRACT §1)
    const char* t = text.UTF8String;
    return keq::Design(keq::ParseConfig(t, strlen(t)), fs);
}

class API_AVAILABLE(macos(14.2)) Hal {
public:
    // The HAL thread's whole life: follow the default output device until gQuit, then tear down.
    void Loop() {
        double retryAt = 0, lastProgress = Now();
        uint64_t lastCalls = 0;
        while (!gQuit.load()) {
            @autoreleasepool { Tick(retryAt, lastProgress, lastCalls); }
            for (int i = 0; i < 5 && !gQuit.load(); ++i) usleep(50 * 1000);
        }
        Teardown();
    }

private:
    void Tick(double& retryAt, double& lastProgress, uint64_t& lastCalls) {
        const AudioObjectID out = DefaultOutput();
        NSDictionary* d = DevOf(out);
        const double sr = d ? NominalRate(out) : 0, now = Now();
        const uint64_t calls = gS.calls.load();
        if (calls != lastCalls) {
            lastCalls = calls;
            lastProgress = now;
        }
        Shared s;
        {
            Locked g;
            s = gShared;
        }
        NSString* why = nil;
        if (!d) {
            if (agg_) {
                Log(@"no output device");
                Teardown();
            }
        } else if (s.phase == kIdle) {
            why = @"start";
        } else if ((s.phase == kFailed || s.phase == kDenied) && now >= retryAt) {
            why = @"";  // quiet retry
        } else if (s.phase == kStarted && ![d[@"id"] isEqualToString:s.uid]) {
            why = @"default output changed";
        } else if (s.phase == kStarted && sr != s.sr) {
            why = @"sample rate changed";
        } else if (s.phase == kStarted && calls > s.callsAtStart && now - lastProgress > kStallAfter) {
            why = @"IOProc stalled";
        } else if (s.phase == kStarted && calls == s.callsAtStart &&
                   (now - s.runningAt > kRetryNoAudio || (now - s.runningAt > kRetryGranted && TccPreflight() == 0))) {
            why = @"no audio since start (permission?)";
        }
        if (!why) return;
        if (why.length) Log(@"build (%@): %@ \"%@\" %.0f Hz", why, d[@"id"], d[@"name"], sr);
        Teardown();
        if (gQuit.load()) return;
        const Phase r = Build(d[@"id"], out);
        if (r == kFailed || r == kDenied) retryAt = Now() + (r == kDenied ? 5 : 3);
        lastProgress = Now();
    }

    void SetPhase(Phase p, NSString* err = nil) {
        Locked g;
        gShared.phase = p;
        gShared.error = err;
    }
    Phase Fail(NSString* msg) {
        if (![msg isEqualToString:lastFail_]) Log(@"build failed: %@", msg);  // retried every 3 s: log a repeat once
        lastFail_ = msg;
        Teardown();
        SetPhase(kFailed, msg);
        return kFailed;
    }

    Phase Build(NSString* uid, AudioObjectID out) {
        if (TccPreflight() == 1) {
            if (![lastFail_ isEqualToString:@"denied"]) Log(@"TCC: system audio recording is denied for this executable");
            lastFail_ = @"denied";
            SetPhase(kDenied);
            return kDenied;
        }
        AudioObjectID self = 0;
        pid_t pid = getpid();
        Get(kAudioObjectSystemObject, A(kAudioHardwarePropertyTranslatePIDToProcessObject), self, sizeof pid, &pid);
        // Without excluding ourselves the tap would hear our own output (feedback). Try again later instead.
        if (!self) return Fail(@"no Core Audio process object for this process");
        desc_ = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[ @(self) ]];
        desc_.name = @"Kaleidoscope EQ";
        desc_.privateTap = YES;
        desc_.muteBehavior = CATapMutedWhenTapped;
        OSStatus e = AudioHardwareCreateProcessTap(desc_, &tap_);
        if (e || !tap_) {
            tap_ = 0;
            return Fail([@"AudioHardwareCreateProcessTap " stringByAppendingString:FourCC(e)]);
        }
        NSDictionary* agg = @{
            @kAudioAggregateDeviceNameKey: @"Kaleidoscope EQ",
            @kAudioAggregateDeviceUIDKey: [NSString stringWithFormat:@"%@.agg.%d.%@", kLabel, getpid(), NSUUID.UUID.UUIDString],
            @kAudioAggregateDeviceMainSubDeviceKey: uid,
            @kAudioAggregateDeviceIsPrivateKey: @YES,
            @kAudioAggregateDeviceIsStackedKey: @NO,
            @kAudioAggregateDeviceTapAutoStartKey: @YES,
            @kAudioAggregateDeviceSubDeviceListKey: @[ @{@kAudioSubDeviceUIDKey: uid} ],
            @kAudioAggregateDeviceTapListKey:
                @[ @{@kAudioSubTapUIDKey: desc_.UUID.UUIDString, @kAudioSubTapDriftCompensationKey: @YES} ],
        };
        e = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)agg, &agg_);
        if (e || !agg_) {
            agg_ = 0;
            return Fail([@"AudioHardwareCreateAggregateDevice " stringByAppendingString:FourCC(e)]);
        }
        UInt32 bf = kBufferFrames;
        auto ba = A(kAudioDevicePropertyBufferFrameSize);
        AudioObjectSetPropertyData(agg_, &ba, 0, nullptr, sizeof bf, &bf);
        const double sr = NominalRate(agg_) > 0 ? NominalRate(agg_) : NominalRate(out);

        proc_ = new keq::Processor();
        if (!proc_->Init(sr, 2)) return Fail(@"out of memory");
        proc_->Reset(Design(ReadText(CfgPath(uid)), sr));
        gRt.store(proc_, std::memory_order_release);
        {
            Locked g;
            gShared.phase = kStarting;
            gShared.uid = uid;
            gShared.sr = NominalRate(out);
            gShared.fs = sr;
            gShared.error = nil;
            gShared.proc = proc_;
            gShared.callsAtStart = gS.calls.load();
            gShared.startedAt = Now();
            gShared.generation++;
        }
        auto oa = A(kAudioDeviceProcessorOverload);
        AudioObjectAddPropertyListener(agg_, &oa, OnOverload, nullptr);
        Log(@"aggregate ready: %@ %.0f Hz, %u frames; creating the IOProc (TCC asks here the first time)", uid, sr,
            GetU32(agg_, kAudioDevicePropertyBufferFrameSize));
        e = AudioDeviceCreateIOProcIDWithBlock(&ioproc_, agg_, nullptr,
                                               ^(const AudioTimeStamp*, const AudioBufferList* i, const AudioTimeStamp*,
                                                 AudioBufferList* o, const AudioTimeStamp*) { IOProc(i, o); });
        if (e || !ioproc_) {
            ioproc_ = nullptr;
            return Fail([@"AudioDeviceCreateIOProcIDWithBlock " stringByAppendingString:FourCC(e)]);
        }
        e = AudioDeviceStart(agg_, ioproc_);
        if (e) return Fail([@"AudioDeviceStart " stringByAppendingString:FourCC(e)]);
        Log(@"started");
        lastFail_ = nil;
        {
            Locked g;
            gShared.phase = kStarted;
            gShared.runningAt = Now();
        }
        return kStarted;
    }

    void Teardown() {
        if (agg_ && ioproc_) {
            AudioDeviceStop(agg_, ioproc_);
            AudioDeviceDestroyIOProcID(agg_, ioproc_);
        }
        ioproc_ = nullptr;
        gRt.store(nullptr, std::memory_order_release);  // the IOProc can no longer run
        if (agg_) {
            auto a = A(kAudioDeviceProcessorOverload);
            AudioObjectRemovePropertyListener(agg_, &a, OnOverload, nullptr);
            AudioHardwareDestroyAggregateDevice(agg_);
            agg_ = 0;
        }
        if (tap_) AudioHardwareDestroyProcessTap(tap_);
        tap_ = 0;
        desc_ = nil;
        {
            Locked g;
            gShared.proc = nullptr;
            gShared.phase = kIdle;
            gShared.uid = nil;
        }
        delete proc_;  // the main thread only reaches it through gShared.proc, under gLock
        proc_ = nullptr;
    }

    AudioObjectID tap_ = 0, agg_ = 0;
    AudioDeviceIOProcID ioproc_ = nullptr;
    CATapDescription* desc_ = nil;
    keq::Processor* proc_ = nullptr;
    NSString* lastFail_ = nil;

};

void* HalMain(void*) API_AVAILABLE(macos(14.2)) {
    @autoreleasepool {
        Hal h;
        h.Loop();
    }
    gHalDone.store(true);
    return nullptr;
}

NSString* StateText(NSString* s) { return [NSString stringWithFormat:@"%@\npid %d\n", s, getpid()]; }

int EngineMain(int seconds, const char* logPath) {
    if (logPath) {
        if (FILE* f = fopen(logPath, "a")) gLog = f;
        gMeasure = true;
    }
    signal(SIGTERM, OnSignal);
    signal(SIGINT, OnSignal);
    signal(SIGHUP, OnSignal);
    signal(SIGPIPE, SIG_IGN);
    MkdirP(Endpoints());
    Log(@"engine %s start (%@)", KEQ_VERSION_STR, NSBundle.mainBundle.bundleIdentifier);
    if (!Supported()) {
        WriteAtomic(StateFile(), StateText(@"unsupported"));
        Log(@"macOS 14.2 or later is required");
        return 0;  // success, so launchd does not restart it
    }
    pthread_t hal;
    if (@available(macOS 14.2, *)) pthread_create(&hal, nullptr, HalMain, nullptr);

    const double t0 = Now();
    NSString* lastState = nil;
    int genSeen = -1, liveGen = -1;
    struct stat seen = {};
    bool seenExists = false;
    bool waiting = false;  // reported permission_pending and no audio since: rebuilds keep that state (no flicker)
    NSString* lastUid = nil;
    double lastStats = Now();
    uint64_t lastCalls = 0;
    while (!gQuit.load() && !(seconds > 0 && Now() - t0 >= seconds)) {
        usleep(250 * 1000);
        @autoreleasepool {
            Shared s;
            {
                Locked g;
                s = gShared;
            }
            const uint64_t calls = gS.calls.load();
            const bool live = (s.phase == kStarting || s.phase == kStarted) && calls > s.callsAtStart;

            // Config: re-read when the file's stamp changes or a new pipeline was built (other device or sample rate).
            if (s.proc && s.uid) {
                NSString* cfg = CfgPath(s.uid);
                struct stat st = {};
                const bool exists = stat(cfg.fileSystemRepresentation, &st) == 0;
                const bool changed = s.generation != genSeen || exists != seenExists ||
                                     (exists && (st.st_mtimespec.tv_sec != seen.st_mtimespec.tv_sec ||
                                                 st.st_mtimespec.tv_nsec != seen.st_mtimespec.tv_nsec ||
                                                 st.st_ino != seen.st_ino || st.st_size != seen.st_size));
                NSString* text = changed && exists ? ReadText(cfg) : nil;
                if (changed && (!exists || text)) {  // an unreadable file (mid-rename) is retried next poll
                    const keq::Params p = Design(text, s.fs);
                    Locked g;
                    if (gShared.proc == s.proc) {  // the HAL thread has not torn it down meanwhile
                        s.proc->Inbox().Back() = p;
                        s.proc->Inbox().Publish();
                    }
                    genSeen = s.generation;
                    seen = st;
                    seenExists = exists;
                    if (gMeasure) Log(@"config %@: %@", exists ? @"loaded" : @"missing (pass-through)", cfg);
                }
            }
            if (live && liveGen != s.generation) {
                NSISO8601DateFormatter* f = [NSISO8601DateFormatter new];
                if (WriteAtomic(LivePath(s.uid), [NSString stringWithFormat:@"pid %d\n%@\n", getpid(), [f stringFromDate:NSDate.date]]))
                    liveGen = s.generation;
                Log(@"audio is flowing through the engine (%@)", s.uid);
            }

            if (s.uid) lastUid = s.uid;
            if (live) waiting = false;
            else if ((s.phase == kStarting || s.phase == kStarted) && Now() - s.startedAt > kPendingAfter) waiting = true;
            NSString* state = s.phase == kDenied ? @"permission_denied"
                              : s.phase == kFailed ? [@"error " stringByAppendingString:s.error ?: @""]
                              : live ? [@"running " stringByAppendingString:s.uid]
                              : waiting && lastUid ? [@"permission_pending " stringByAppendingString:lastUid]
                              : @"starting";
            if (![state isEqualToString:lastState] && WriteAtomic(StateFile(), StateText(state))) {
                Log(@"state: %@", state);
                lastState = state;
            }

            if (gMeasure && Now() - lastStats >= 1.0) {
                const uint64_t n = gS.n.exchange(0);
                const double si = gS.inSq.exchange(0), so = gS.outSq.exchange(0);
                const double ri = n ? sqrt(si / n) : 0, ro = n ? sqrt(so / n) : 0;
                Log(@"stats calls/s=%llu inRMS=%.5f outRMS=%.5f ratio=%.4f overloads=%llu",
                    (unsigned long long)(calls - lastCalls), ri, ro, ri > 1e-6 ? ro / ri : 0.0,
                    (unsigned long long)gS.overloads.load());
                lastCalls = calls;
                lastStats = Now();
            }
        }
    }
    gQuit.store(true);
    Log(@"stopping (overloads %llu)", (unsigned long long)gS.overloads.load());
    // The HAL may hang in teardown while a TCC prompt is unanswered. Give it 3 s; exiting kills the tap anyway.
    for (int i = 0; i < 60 && !gHalDone.load(); ++i) usleep(50 * 1000);
    WriteAtomic(StateFile(), StateText(@"stopped"));
    if (!gHalDone.load()) {
        Log(@"HAL teardown did not finish in 3 s; exiting anyway");
        _exit(0);
    }
    Log(@"stopped");
    return 0;
}

// ═════════════════════════════════════════════════════════════════════════
// Bridge (CONTRACT §4)
// ═════════════════════════════════════════════════════════════════════════

// No exceptions in this binary (size): the first error of a request lands in gErr and the call returns nil.
NSString* gErr = nil;
#define CHECK(cond, msg)                  \
    do {                                  \
        if (!(cond)) {                    \
            if (!gErr) gErr = (msg);      \
            return nil;                   \
        }                                 \
    } while (0)

id Field(id d, NSString* k) {
    id v = [d isKindOfClass:NSDictionary.class] ? d[k] : nil;
    return v == NSNull.null ? nil : v;
}
// Shortest text that reads back as the same double (like .NET's "R" in bridge.ps1).
NSString* Num(id v, NSString* what) {
    if (![v isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)v) == CFBooleanGetTypeID())
        CHECK(false, [what stringByAppendingString:@": 숫자가 아님"]);
    const double d = [v doubleValue];
    CHECK(std::isfinite(d), [what stringByAppendingString:@": 유한한 수가 아님"]);
    char b[40];
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        snprintf(b, sizeof b, "%.0f", d);  // whole numbers without an exponent: 1000, not 1e+03
    } else {
        for (int p = 1; p <= 17; ++p) {
            snprintf(b, sizeof b, "%.*g", p, d);
            if (strtod(b, nullptr) == d) break;
        }
    }
    return @(b);
}

// Screen config -> §1 .cfg text (the same lines as bridge.ps1's Render).
NSString* Render(id c) {
    CHECK([c isKindOfClass:NSDictionary.class], @"config가 없음");
    id en = Field(c, @"enabled");
    NSMutableString* out = [NSMutableString stringWithFormat:@"# kaleidoscope-eq 1\nenabled %d\npreamp %@\n",
                                                             [en isKindOfClass:NSNumber.class] && [en boolValue] ? 1 : 0,
                                                             Num(Field(c, @"preamp"), @"preamp")];
    id peaks = Field(c, @"peaks");
    CHECK(!peaks || [peaks isKindOfClass:NSArray.class], @"peaks: 배열이 아님");
    CHECK([peaks count] <= 20, ([NSString stringWithFormat:@"peaks: 20개까지 (%lu개)", (unsigned long)[peaks count]]));
    for (id p in (NSArray*)peaks) {
        CHECK([p isKindOfClass:NSArray.class] && [p count] == 3, @"peaks: 항목마다 [주파수, Q, 이득]");
        NSArray* q = p;
        [out appendFormat:@"peak %@ %@ %@\n", Num(q[0], @"peak 주파수"), Num(q[1], @"peak Q"), Num(q[2], @"peak 이득")];
    }
    if (id r = Field(c, @"reverb")) {
        CHECK([r isKindOfClass:NSDictionary.class], @"reverb: 객체가 아님");
        [out appendFormat:@"reverb %@ %@ %@\n", Num(Field(r, @"decay"), @"reverb decay"),
                          Num(Field(r, @"predelay"), @"reverb predelay"), Num(Field(r, @"wet"), @"reverb wet")];
    }
    [out appendFormat:@"spatial %@\n", Num(Field(c, @"spatial"), @"spatial")];
    return gErr ? nil : out;
}

struct EngineView {
    bool installed = false, alive = false;
    EngineState st;
};
EngineView ReadEngine() {
    EngineView v;
    v.installed = [FM() fileExistsAtPath:PlistPath()] && [FM() fileExistsAtPath:InstalledBin()];
    v.st = ReadState();
    v.alive = v.installed && IsEngineProcess(v.st.pid);
    return v;
}

// Whether the installed engine is this very executable (otherwise the screen offers an update).
bool Current() {
    static NSData* me = [NSData dataWithContentsOfFile:SelfPath()];
    static NSDate* seenAt = nil;
    static bool same = false;
    NSDate* at = [FM() attributesOfItemAtPath:InstalledBin() error:nil].fileModificationDate;
    if (!at) return false;
    if (![at isEqual:seenAt]) same = me && [me isEqualToData:[NSData dataWithContentsOfFile:InstalledBin()]];
    seenAt = at;
    return same;
}

// §4 engine object plus macOS extras: platform, supported (macOS 14.2+), state (the engine process, from engine.state),
// permission (TCC as far as the engine can tell: granted | pending | denied | null), message (state error),
// current (installed engine == this bridge's executable; false means an update is waiting).
NSDictionary* EngineJson(const EngineView& v) {
    NSString* ver = v.installed ? [ReadText(VersionFile()) stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] : nil;
    NSString* s = v.st.state;
    NSDictionary* known = @{@"running": @"granted", @"permission_pending": @"pending", @"permission_denied": @"denied"};
    id state = !v.installed ? nil
               : !v.alive ? ([s isEqualToString:@"unsupported"] ? s : @"stopped")
               : known[s] || [s isEqualToString:@"error"] ? s : @"starting";
    NSMutableDictionary* e = [@{
        @"installed": @(v.installed),
        @"version": ver.length ? ver : NSNull.null,
        @"platform": @"macos",
        @"supported": @(Supported()),
        @"state": state ?: NSNull.null,
        @"permission": (v.alive ? known[s] : nil) ?: NSNull.null,
    } mutableCopy];
    if (v.alive && [s isEqualToString:@"error"]) e[@"message"] = v.st.arg;
    if (v.installed) e[@"current"] = @(Current());
    return e;
}

NSDictionary* DeviceJson(NSDictionary* d, const EngineView& v) {
    NSMutableDictionary* r = [d mutableCopy];
    r[@"registered"] = @(v.installed);  // one tap covers every device
    r[@"active"] = @(v.alive && [v.st.state isEqualToString:@"running"] && [v.st.arg isEqualToString:d[@"id"]] &&
                     LivePid(d[@"id"]) == v.st.pid);
    return r;
}
id DefaultDeviceJson(const EngineView& v) {
    NSDictionary* d = DevOf(DefaultOutput());
    return d ? DeviceJson(d, v) : (id)NSNull.null;
}

NSString* ToJson(id obj) {
    NSData* data = [NSJSONSerialization dataWithJSONObject:obj options:NSJSONWritingWithoutEscapingSlashes error:nil];
    return data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"{}";
}
void Send(id obj) {
    fprintf(stdout, "%s\n", ToJson(obj).UTF8String);
    fflush(stdout);
}

int Launchctl(NSArray<NSString*>* args, NSString** output) {
    NSTask* t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:@"/bin/launchctl"];
    t.arguments = args;
    NSPipe* p = [NSPipe pipe];
    t.standardOutput = p;
    t.standardError = p;
    t.standardInput = [NSFileHandle fileHandleWithNullDevice];
    if (![t launchAndReturnError:nil]) return -1;
    NSData* d = [p.fileHandleForReading readDataToEndOfFile];
    [t waitUntilExit];
    if (output) *output = [[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding]
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return t.terminationStatus;
}
NSString* Domain() { return [NSString stringWithFormat:@"gui/%d", getuid()]; }

// Stops the LaunchAgent (if loaded) and waits until the engine process is gone (its exit restores normal audio).
void StopAgent() {
    const pid_t pid = ReadState().pid;
    if (TestHome()) return;
    Launchctl(@[ @"bootout", [NSString stringWithFormat:@"%@/%@", Domain(), kLabel] ], nil);  // harmless when not loaded
    for (int i = 0; i < 60 && IsEngineProcess(pid); ++i) usleep(100 * 1000);
    if (IsEngineProcess(pid)) kill(pid, SIGKILL);
}

NSDictionary* Install() {
    CHECK(Supported(), @"macOS 14.2 이상에서만 쓸 수 있어요");
    NSString *self = SelfPath(), *dst = InstalledBin();
    CHECK(self, @"이 실행 파일의 경로를 알 수 없음");
    for (NSString* dir in @[ Endpoints(), [Home() stringByAppendingPathComponent:@"Library/LaunchAgents"], LogDir() ])
        CHECK(MkdirP(dir), [@"폴더를 만들지 못함: " stringByAppendingString:dir]);
    if (![self isEqualToString:dst]) {
        NSData* bin = [NSData dataWithContentsOfFile:self];
        CHECK(bin.length >= 4096, [@"실행 파일을 읽지 못함: " stringByAppendingString:self]);
        // Atomic replace: a running engine keeps its old file (inode) until it exits.
        CHECK([bin writeToFile:dst options:NSDataWritingAtomic error:nil] &&
                  [FM() setAttributes:@{NSFilePosixPermissions: @0755} ofItemAtPath:dst error:nil],
              [@"엔진을 복사하지 못함: " stringByAppendingString:dst]);
    }
    CHECK(WriteAtomic(VersionFile(), @KEQ_VERSION_STR "\n"), @"판 파일을 쓰지 못함");
    NSString* log = [LogDir() stringByAppendingPathComponent:@"engine.log"];
    NSMutableDictionary* plist = [@{
        @"Label": kLabel,
        @"ProgramArguments": @[ dst, @"engine" ],
        @"RunAtLoad": @YES,
        @"KeepAlive": @{@"SuccessfulExit": @NO},
        @"ProcessType": @"Interactive",
        @"StandardOutPath": log,
        @"StandardErrorPath": log,
    } mutableCopy];
    if (TestHome()) plist[@"EnvironmentVariables"] = @{@"KEQ_HOME": Home()};
    NSData* xml = [NSPropertyListSerialization dataWithPropertyList:plist format:NSPropertyListXMLFormat_v1_0 options:0 error:nil];

    StopAgent();  // update = reinstall: the old engine lets go of the tap first
    [FM() removeItemAtPath:StateFile() error:nil];
    CHECK([xml writeToFile:PlistPath() options:NSDataWritingAtomic error:nil],
          [@"LaunchAgent를 쓰지 못함: " stringByAppendingString:PlistPath()]);
    if (TestHome()) return @{@"launchd": @"skipped"};
    NSString* msg = nil;
    int code = -1;
    for (int i = 0; i < 10 && code != 0; ++i) {  // right after a bootout launchd can answer "Input/output error" briefly
        if (i) usleep(300 * 1000);
        code = Launchctl(@[ @"bootstrap", Domain(), PlistPath() ], &msg);
    }
    CHECK(code == 0, ([NSString stringWithFormat:@"launchctl bootstrap 실패(%d): %@", code, msg]));
    // Answer once the engine runs, so the screen goes straight from "installing" to the engine's own state.
    for (int i = 0; i < 50 && !IsEngineProcess(ReadState().pid); ++i) usleep(100 * 1000);
    return @{@"launchd": @"loaded"};
}

NSDictionary* Uninstall() {
    StopAgent();
    for (NSString* p in @[ PlistPath(), Support(), LogDir() ]) [FM() removeItemAtPath:p error:nil];  // keep nothing
    CHECK(![FM() fileExistsAtPath:PlistPath()] && ![FM() fileExistsAtPath:Support()],
          [@"다 지우지 못함: " stringByAppendingString:Support()]);
    return @{};
}

NSDictionary* Apply(NSDictionary* m) {
    id dev = Field(m, @"device_id");
    CHECK([dev isKindOfClass:NSString.class] && [dev length] && [dev length] <= 512,
          ([NSString stringWithFormat:@"잘못된 device_id: %@", dev]));
    NSString* text = Render(Field(m, @"config"));
    if (!text) return nil;
    CHECK(MkdirP(Endpoints()), [@"폴더를 만들지 못함: " stringByAppendingString:Endpoints()]);
    CHECK(WriteAtomic(CfgPath(dev), text), @"설정 파일을 쓰지 못함");
    return @{};
}

NSString* gSeen = nil;  // last device + engine the screen heard about (replies and events both update it)

NSDictionary* Handle(NSDictionary* m) {
    id op = Field(m, @"op");
    const bool status = [op isEqual:@"status"];
    NSDictionary* more = status ? @{}
                         : [op isEqual:@"apply"] ? Apply(m)
                         : [op isEqual:@"install"] ? Install()
                         : [op isEqual:@"uninstall"] ? Uninstall()
                         : nil;
    CHECK(more, ([NSString stringWithFormat:@"알 수 없는 op: %@", op ?: @""]));
    const EngineView v = ReadEngine();
    NSDictionary* engine = EngineJson(v);
    id device = DefaultDeviceJson(v);
    NSMutableDictionary* r = [@{@"id": m[@"id"] ?: NSNull.null, @"status": @"ok", @"engine": engine, @"device": device} mutableCopy];
    if (status) {
        NSMutableArray* all = [NSMutableArray array];
        for (NSDictionary* d in OutputDevices()) [all addObject:DeviceJson(d, v)];
        r[@"devices"] = all;
    }
    [r addEntriesFromDictionary:more];
    gSeen = ToJson(@[ device, engine ]);
    return r;
}

// Unsolicited {"event":"device"} when the default device or the engine state differs from what the screen last heard.
void Watch() {
    const EngineView v = ReadEngine();
    id device = DefaultDeviceJson(v);
    NSString* sig = ToJson(@[ device, EngineJson(v) ]);
    if (gSeen && ![sig isEqualToString:gSeen]) Send(@{@"event": @"device", @"device": device});
    gSeen = sig;
}

void HandleLine(NSData* line) {
    id m = [NSJSONSerialization JSONObjectWithData:line options:0 error:nil];
    gErr = nil;
    NSDictionary* r = [m isKindOfClass:NSDictionary.class] ? Handle(m) : nil;
    if (!gErr && !r) gErr = @"요청이 JSON 객체가 아님";
    // The bridge keeps running after an error (as bridge.ps1 does).
    Send(r ?: @{@"id": Field(m, @"id") ?: NSNull.null, @"status": @"error", @"message": gErr});
}

int BridgeMain() {
    signal(SIGPIPE, SIG_IGN);
    NSMutableData* buf = [NSMutableData data];
    double lastWatch = Now();
    char chunk[65536];
    for (;;) {
        struct pollfd p = {0, POLLIN, 0};
        const int r = poll(&p, 1, 1000);
        if (r < 0 && errno != EINTR) break;
        if (r > 0) {
            const ssize_t n = read(0, chunk, sizeof chunk);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) break;  // stdin closed: the screen went away
            [buf appendBytes:chunk length:(NSUInteger)n];
            for (;;) {
                const char* b = (const char*)buf.bytes;
                const char* nl = (const char*)memchr(b, '\n', buf.length);
                if (!nl) break;
                @autoreleasepool {
                    NSData* line = [buf subdataWithRange:NSMakeRange(0, nl - b)];
                    [buf replaceBytesInRange:NSMakeRange(0, nl - b + 1) withBytes:nullptr length:0];
                    NSString* t = [[NSString alloc] initWithData:line encoding:NSUTF8StringEncoding];
                    if ([t stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length) HandleLine(line);
                }
            }
        }
        if (Now() - lastWatch >= 1.0) {
            @autoreleasepool { Watch(); }
            lastWatch = Now();
        }
    }
    return 0;
}

int ListMain() {
    const EngineView v = ReadEngine();
    Send(@{@"default": DefaultDeviceJson(v), @"engine": EngineJson(v)});
    for (NSDictionary* d in OutputDevices()) {
        NSMutableDictionary* x = [d mutableCopy];
        x[@"safe"] = Safe(d[@"id"]);
        Send(x);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        const char* mode = argc > 1 ? argv[1] : "";
        int seconds = 0;
        const char* logPath = nullptr;
        for (int i = 2; i + 1 < argc; ++i) {
            if (!strcmp(argv[i], "--seconds")) seconds = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--log")) logPath = argv[++i];
        }
        if (!strcmp(mode, "bridge")) return BridgeMain();
        if (!strcmp(mode, "engine")) return EngineMain(seconds, logPath);
        if (!strcmp(mode, "--list")) return ListMain();
        if (!strcmp(mode, "--version")) return printf("%s\n", KEQ_VERSION_STR) < 0;
        fprintf(stderr, "usage: keq-mac bridge | engine [--seconds N] [--log FILE] | --list | --version\n");
        return 2;
    }
}
