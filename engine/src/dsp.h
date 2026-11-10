// DSP core of the Kaleidoscope EQ APO. No Windows/COM here, so the offline test harness runs the exact same code.
//
//   Config  — what the .cfg file says (CONTRACT §1), range-checked, independent of sample rate.
//   Params  — a Config designed for one sample rate: plain numbers, fixed size, no heap. Built off the realtime thread.
//   Processor — the realtime part. Takes new Params through a wait-free mailbox and crossfades old -> new.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace keq {

constexpr int kMaxPeaks = 32;     // contract says 0..20; extra lines are ignored
constexpr int kMaxChannels = 16;  // channels beyond this pass untouched
constexpr int kFdnLines = 8;

struct Config {
    bool enabled = false;
    double preampDb = 0;
    int nPeaks = 0;
    struct Peak { double f, q, gainDb; } peaks[kMaxPeaks] = {};
    bool reverb = false;
    double decaySec = 0, predelayMs = 0, wetDb = 0;
    double spatial = 0;
};

// Parses CONTRACT §1 text. Unknown or malformed lines are ignored. No `enabled` line means enabled.
Config ParseConfig(const char* text, size_t len);

struct Biquad { double b0, b1, b2, a1, a2; };
// RBJ cookbook peaking EQ, identical to client.js peaking().
Biquad Peaking(double fs, double f0, double q, double gainDb);

struct Params {
    bool active;       // false: the stream passes through bit-exact
    double preamp;     // linear
    int nPeaks;
    Biquad peak[kMaxPeaks];
    double wet;        // linear gain on the reverb output, calibrated so reverb/direct energy == wetDb; 0 = no reverb
    int predelay;      // samples
    double feedback[kFdnLines];
    double spatial;    // 0..1
};
Params Bypass();
Params Design(const Config& c, double fs);
bool SameParams(const Params& a, const Params& b);

// Wait-free single-producer/single-consumer handoff of the latest value (classic triple buffer).
template <class T>
class Mailbox {
public:
    T& Back() { return slot_[back_]; }                       // writer fills this...
    void Publish() { back_ = middle_.exchange(back_ | kNew, std::memory_order_acq_rel) & 3; }  // ...then publishes
    const T* Take() {                                        // reader: newest unseen value or nullptr
        if (!(middle_.load(std::memory_order_acquire) & kNew)) return nullptr;
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & 3;
        return &slot_[front_];
    }
private:
    static constexpr uint32_t kNew = 4;
    T slot_[3] = {};
    std::atomic<uint32_t> middle_{1};
    uint32_t back_ = 0, front_ = 2;
};

// One zeroed float block sized at Init (not realtime). Owns its memory; not copyable.
class Floats {
public:
    Floats() = default;
    Floats(const Floats&) = delete;
    Floats& operator=(const Floats&) = delete;
    ~Floats();
    bool Resize(size_t n);  // contents are zeroed; false (and empty) if out of memory
    void Zero();
    float* data() const { return p_; }
    size_t size() const { return n_; }
private:
    float* p_ = nullptr;
    size_t n_ = 0;
};

// Feedback delay network: mono in, decorrelated stereo out. Buffers are sized once in Init.
class Reverb {
public:
    bool Init(double fs, int maxPredelay);  // false if out of memory (Tick must not run then)
    void Clear();
    // predelay crossfades from tap a to tap b by t; fb = per-line feedback gains
    void Tick(double in, int preA, int preB, double t, const double* fb, double& l, double& r);
    static void LineLengths(double fs, int* len);
private:
    struct Delay { float* buf = nullptr; int len = 0, pos = 0; };
    Floats mem_;  // predelay, then the two allpasses, then the FDN lines
    float* pre_ = nullptr;
    int preMask_ = 0, preW_ = 0;
    Delay ap_[2];
    double apGain_ = 0.6;
    Delay line_[kFdnLines];
    double lp_[kFdnLines] = {};
    double damp_ = 1;
};

// A symmetric pair of virtual loudspeakers at +-azimuth (0 = front, 90 = side, 180 = behind), heard through
// Brown & Duda's structural head model (IEEE Trans. Speech Audio Proc. 6(5), 1998) - no measured HRIR data:
//  - interaural delay: Woodworth's spherical head, a/c * (theta + sin theta) on the lateral angle, fractional;
//  - head shadow: B&D's one-pole/one-zero per ear, alpha(incidence) with the ears at +-100 degrees. Each pair is
//    normalised to its near ear at high frequencies (there the head isolates the ears, so headphones and speakers
//    agree) and to `gain` per ear at low frequencies (there the head is transparent: both ears hear each speaker);
//  - pinna: B&D's first pinna echo with its azimuth-dependent delay, plus the pinna's shadow on sound from behind
//    (a high shelf). Both scale with (1 - cos azimuth) / 2, i.e. vanish straight ahead, so a frontal pair is
//    nearly comb-free and a rear pair is clearly darker with a moved notch - the front/back cue.
class SpeakerPair {
public:
    bool Init(double fs, double azimuthDeg, double gain);  // false if out of memory
    void Clear();
    // One sample from each speaker -> what reaches each ear.
    void Tick(double left, double right, double& earL, double& earR);
    double ItdSamples() const { return itd_; }
private:
    struct First { double b0 = 1, b1 = 0, a1 = 0; };  // first-order IIR, transposed direct form II
    Floats hist_;  // interleaved left/right speaker feed after the back shelf
    int mask_ = 0, w_ = 0;
    double itd_ = 0, echo_ = 0;       // far-ear delay and pinna echo delay, samples (fractional)
    double direct_ = 1, reflect_ = 0;  // pinna: direct and echo gains (sum 1: flat at low frequencies)
    double backA_ = 0, backG_ = 1;     // pinna back shelf: one-pole coefficient and high-frequency gain
    First near_, far_;
    double back_[2] = {}, zNear_[2] = {}, zFar_[2] = {};
    double Read(int c, double d) const;
};

// Headphone virtual surround from stereo, one strength (0..1). Mid M = (L+R)/2 and side S = (L-R)/2:
//  1. front stage: M is a real centre speaker (exactly the dry centre: no comb, no level change), S drives a
//     +-30 degree speaker pair. Equivalent to a speaker triangle whose phantom centre has been equalised flat;
//  2. surround: ambience (+-S plus 0.6 M, so even near-mono music wraps) through velvet-noise decorrelators
//     (sparse random-sign taps, 7-43 ms: diffuse, no recirculation, so no metallic ring) to a +-110 degree pair;
//  3. room: eight early reflections per ear (3-29 ms, different per ear, from both front speakers), low-passed.
// Ambience and room are high-passed (bass stays dry and tight); the dry path is trimmed by (1 + s^2 E)^-1/4 so
// a centred source neither swells nor sinks (half the added room energy is compensated: mids +0.8, bass -0.8 dB).
class Spatial {
public:
    bool Init(double fs);  // false if out of memory
    void Clear();
    // feed scales what enters the delay history (ramped from 0 after a Clear)
    void Tick(double& l, double& r, double strength, double feed);
private:
    static constexpr int kEr = 8, kAmb = 16;
    struct Tap { int d; int c; double g; };  // delay (samples), history column, gain
    SpeakerPair front_, rear_;
    Floats hist_;  // 4 interleaved columns: high-passed L, R (reflections) and the two surround feeds
    int mask_ = 0, w_ = 0;
    Tap er_[2][kEr] = {}, amb_[2][kAmb] = {};
    double hpA_ = 0, erA_ = 0, ambA_ = 0, energy_ = 0;
    double hpL_ = 0, hpR_ = 0, erL_ = 0, erR_ = 0, ambL_ = 0, ambR_ = 0;
    double lastS_ = -1, trim_ = 1;
    // 감싸는 울림: 서라운드 스피커에서 나오는 짧고 좌우가 다른 잔향(같은 FDN, 0.7초). 한 단위 입력의 잔향 에너지가
    // 1이 되게 envNorm_으로 맞춘 뒤 kEnvelop만큼 섞는다.
    Reverb env_;
    double envFb_[kFdnLines] = {}, envNorm_ = 0, envA_ = 0, envL_ = 0, envR_ = 0;  // envA_: 잔향의 고음을 덜어 내는 저역 통과
};

class Processor {
public:
    // Not realtime: allocates. fadeMs is the crossfade on every parameter change. False if out of memory.
    bool Init(double fs, int channels, double fadeMs = 30);
    void Reset(const Params& p);        // not realtime: set params without fade, clear state
    void Clear() { Reset(cur_); }        // keep params, empty all delay lines and filter state
    Mailbox<Params>& Inbox() { return inbox_; }

    // Realtime: in-place on interleaved float32. `silent`: the engine flagged the input silent (content undefined).
    // Returns true if it wrote valid audio; false if it left the buffer untouched (caller keeps the input flags).
    bool Process(float* buf, uint32_t frames, bool silent);

private:
    void BeginSwap(const Params& next);
    void ClearState();
    double RunBank(const Params& p, int bank, int ch, double x);

    double fs_ = 48000;
    int ch_ = 2, fadeLen_ = 1, fadePos_ = 0;
    bool fading_ = false, quiet_ = true, reverbRun_ = false, spatialRun_ = false;
    double reverbIn_ = 1, spatialIn_ = 1;  // input ramps of the delay-based stages
    Params cur_ = Bypass(), prev_ = Bypass();
    int bank_ = 0;
    double z_[2][kMaxChannels][kMaxPeaks][2] = {};
    Reverb reverb_;
    Spatial spatial_;
    Mailbox<Params> inbox_;
};

}  // namespace keq
