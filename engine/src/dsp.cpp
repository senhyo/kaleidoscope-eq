#include "dsp.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <xmmintrin.h>

namespace keq {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Denormals in recirculating state cost 100x CPU on x86. Flush-to-zero + denormals-are-zero for the scope.
struct FtzGuard {
    unsigned saved = _mm_getcsr();
    FtzGuard() { _mm_setcsr(saved | 0x8040); }
    ~FtzGuard() { _mm_setcsr(saved); }
};

double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
double DbToGain(double db) { return std::pow(10.0, db / 20.0); }
double OnePole(double fs, double hz) { return 1.0 - std::exp(-2.0 * kPi * hz / fs); }
int Pow2AtLeast(int n) { int p = 1; while (p < n) p <<= 1; return p; }

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Splits one line into whitespace-separated tokens; numbers parse locale-independently.
struct Line {
    const char* tok[8];
    size_t len[8];
    int n = 0;
    bool Word(int i, const char* w) const {
        size_t wl = std::strlen(w);
        if (i >= n || len[i] != wl) return false;
        for (size_t k = 0; k < wl; ++k)
            if ((tok[i][k] | 0x20) != w[k]) return false;
        return true;
    }
    // The whole token is a finite decimal: [-] digits [. digits] [e [+-] digits], std::from_chars' grammar without
    // inf/nan, and like from_chars it rejects a nonzero value that underflows to 0. The grammar is checked here; the
    // correctly rounded conversion is the CRT's, in the "C" locale.
    bool Num(int i, double* v, _locale_t c) const {
        if (i >= n || len[i] >= 64 || !c) return false;
        const char* s = tok[i];
        const size_t m = len[i];
        size_t k = s[0] == '-' ? 1 : 0, digits = 0;
        bool nonzero = false;
        for (; k < m && IsDigit(s[k]); ++k) ++digits, nonzero |= s[k] != '0';
        if (k < m && s[k] == '.')
            for (++k; k < m && IsDigit(s[k]); ++k) ++digits, nonzero |= s[k] != '0';
        if (digits == 0) return false;
        if (k < m && (s[k] | 0x20) == 'e') {
            if (++k < m && (s[k] == '+' || s[k] == '-')) ++k;
            const size_t e = k;
            while (k < m && IsDigit(s[k])) ++k;
            if (k == e) return false;
        }
        if (k != m) return false;
        char z[64];
        std::memcpy(z, s, m);
        z[m] = 0;
        *v = _strtod_l(z, nullptr, c);
        return std::isfinite(*v) && (*v != 0 || !nonzero);
    }
};

}  // namespace

// ── config ───────────────────────────────────────────────────────────────

Config ParseConfig(const char* text, size_t len) {
    Config c;
    c.enabled = true;
    const _locale_t C = _create_locale(LC_NUMERIC, "C");  // whatever locale another component in audiodg has set
    const char* p = text;
    const char* end = text + len;
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (p < end) {
        const char* eol = static_cast<const char*>(std::memchr(p, '\n', end - p));
        if (!eol) eol = end;
        Line ln;
        for (const char* q = p; q < eol && ln.n < 8;) {
            while (q < eol && IsSpace(*q)) ++q;
            if (q >= eol || *q == '#') break;
            const char* s = q;
            while (q < eol && !IsSpace(*q)) ++q;
            ln.tok[ln.n] = s;
            ln.len[ln.n++] = q - s;
        }
        p = eol + 1;
        double a, b, d;
        if (ln.Word(0, "enabled") && ln.Num(1, &a, C)) {
            c.enabled = a != 0;
        } else if (ln.Word(0, "preamp") && ln.Num(1, &a, C)) {
            c.preampDb = Clamp(a, -60, 30);
        } else if (ln.Word(0, "peak") && ln.Num(1, &a, C) && ln.Num(2, &b, C) && ln.Num(3, &d, C)) {
            if (c.nPeaks < kMaxPeaks && a > 0) c.peaks[c.nPeaks++] = {Clamp(a, 10, 30000), Clamp(b, 0.1, 30), Clamp(d, -30, 30)};
        } else if (ln.Word(0, "reverb") && ln.Num(1, &a, C) && ln.Num(2, &b, C) && ln.Num(3, &d, C)) {
            c.reverb = true;
            c.decaySec = Clamp(a, 0.1, 15);
            c.predelayMs = Clamp(b, 0, 200);
            c.wetDb = Clamp(d, -80, 12);
        } else if (ln.Word(0, "spatial") && ln.Num(1, &a, C)) {
            c.spatial = Clamp(a, 0, 1);
        }
    }
    if (C) _free_locale(C);
    return c;
}

// ── design ───────────────────────────────────────────────────────────────

Biquad Peaking(double fs, double f0, double q, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0), w0 = 2 * kPi * f0 / fs;
    const double alpha = std::sin(w0) / (2 * q), c = std::cos(w0);
    const double a0 = 1 + alpha / A;
    return {(1 + alpha * A) / a0, -2 * c / a0, (1 - alpha * A) / a0, -2 * c / a0, (1 - alpha / A) / a0};
}

Params Bypass() {
    Params p;
    std::memset(&p, 0, sizeof p);
    p.preamp = 1;
    return p;
}

Params Design(const Config& c, double fs) {
    FtzGuard ftz;
    Params p = Bypass();
    if (!c.enabled) return p;
    p.preamp = DbToGain(c.preampDb);
    for (int i = 0; i < c.nPeaks; ++i) {
        const auto& k = c.peaks[i];
        if (k.gainDb == 0 || k.f >= 0.49 * fs) continue;  // identity, or above Nyquist for this rate
        p.peak[p.nPeaks++] = Peaking(fs, k.f, k.q, k.gainDb);
    }
    if (c.reverb && c.wetDb > -80) {
        int len[kFdnLines];
        Reverb::LineLengths(fs, len);
        for (int i = 0; i < kFdnLines; ++i) p.feedback[i] = std::pow(10.0, -3.0 * len[i] / (fs * c.decaySec));
        p.predelay = (int)std::lround(c.predelayMs * fs / 1000);
        // Calibrate with the same reverb the realtime path runs: reverb/direct energy ratio = wetDb,
        // the meaning client.js gave its impulse responses.
        Reverb scratch;
        const int n = scratch.Init(fs, 0) ? (int)(fs * std::min(1.5 * c.decaySec, 20.0)) : 0;
        double e = 0, l, r;
        for (int i = 0; i < n; ++i) {
            scratch.Tick(i == 0 ? 1.0 : 0.0, 0, 0, 0, p.feedback, l, r);
            e += 0.5 * (l * l + r * r);
        }
        p.wet = e > 0 ? std::sqrt(std::pow(10.0, c.wetDb / 10) / e) : 0;
    }
    p.spatial = c.spatial;
    p.active = p.preamp != 1 || p.nPeaks > 0 || p.wet > 0 || p.spatial > 0;
    return p.active ? p : Bypass();
}

bool SameParams(const Params& a, const Params& b) {
    if (a.active != b.active || a.preamp != b.preamp || a.nPeaks != b.nPeaks || a.wet != b.wet ||
        a.predelay != b.predelay || a.spatial != b.spatial)
        return false;
    for (int i = 0; i < a.nPeaks; ++i) {
        const Biquad &x = a.peak[i], &y = b.peak[i];
        if (x.b0 != y.b0 || x.b1 != y.b1 || x.b2 != y.b2 || x.a1 != y.a1 || x.a2 != y.a2) return false;
    }
    for (int i = 0; i < kFdnLines; ++i)
        if (a.feedback[i] != b.feedback[i]) return false;
    return true;
}

// ── buffers ──────────────────────────────────────────────────────────────

Floats::~Floats() { std::free(p_); }

bool Floats::Resize(size_t n) {
    std::free(p_);
    p_ = static_cast<float*>(std::calloc(n, sizeof(float)));
    n_ = p_ ? n : 0;
    return p_ != nullptr;
}

void Floats::Zero() { std::memset(p_, 0, n_ * sizeof(float)); }

// ── reverb ───────────────────────────────────────────────────────────────

void Reverb::LineLengths(double fs, int* len) {
    static const double ms[kFdnLines] = {29.7, 33.1, 37.9, 41.3, 45.7, 51.1, 56.3, 61.9};
    for (int i = 0; i < kFdnLines; ++i) len[i] = (int)std::lround(ms[i] * fs / 1000);
}

bool Reverb::Init(double fs, int maxPredelay) {
    const int preSize = Pow2AtLeast(maxPredelay + 1);
    preMask_ = preSize - 1;
    static const double apMs[2] = {4.77, 3.59};
    for (int i = 0; i < 2; ++i) ap_[i].len = (int)std::lround(apMs[i] * fs / 1000);
    int len[kFdnLines];
    LineLengths(fs, len);
    for (int i = 0; i < kFdnLines; ++i) line_[i].len = len[i];
    size_t total = preSize;
    for (const auto& d : ap_) total += d.len;
    for (const auto& d : line_) total += d.len;
    if (!mem_.Resize(total)) return false;
    float* at = mem_.data();
    pre_ = at;
    at += preSize;
    for (auto& d : ap_) { d.buf = at; at += d.len; }
    for (auto& d : line_) { d.buf = at; at += d.len; }
    damp_ = OnePole(fs, 7000);  // high frequencies die sooner: the tail darkens like a real room
    Clear();
    return true;
}

void Reverb::Clear() {
    mem_.Zero();
    preW_ = 0;
    for (auto& d : ap_) d.pos = 0;
    for (auto& d : line_) d.pos = 0;
    std::memset(lp_, 0, sizeof lp_);
}

void Reverb::Tick(double in, int preA, int preB, double t, const double* fb, double& l, double& r) {
    pre_[preW_] = (float)in;
    const double a = pre_[(preW_ - preA) & preMask_], b = pre_[(preW_ - preB) & preMask_];
    preW_ = (preW_ + 1) & preMask_;
    double x = a + t * (b - a);
    for (auto& d : ap_) {  // Schroeder allpasses thicken the onset
        const double z = d.buf[d.pos], v = x + apGain_ * z;
        d.buf[d.pos] = (float)v;
        x = z - apGain_ * v;
        if (++d.pos == d.len) d.pos = 0;
    }
    double o[kFdnLines], m[kFdnLines];
    for (int i = 0; i < kFdnLines; ++i) {
        o[i] = line_[i].buf[line_[i].pos];
        lp_[i] += damp_ * (o[i] - lp_[i]);
        m[i] = lp_[i] * fb[i];
    }
    for (int h = 1; h < kFdnLines; h <<= 1)  // orthonormal Hadamard mix (lossless; the loss is all in fb)
        for (int i = 0; i < kFdnLines; i += h << 1)
            for (int j = i; j < i + h; ++j) { const double p = m[j], q = m[j + h]; m[j] = p + q; m[j + h] = p - q; }
    const double norm = 0.35355339059327373;  // 1/sqrt(8)
    for (int i = 0; i < kFdnLines; ++i) {
        line_[i].buf[line_[i].pos] = (float)(m[i] * norm + x);
        if (++line_[i].pos == line_[i].len) line_[i].pos = 0;
    }
    l = (o[0] - o[1] + o[2] - o[3] + o[4] - o[5] + o[6] - o[7]) * norm;
    r = (o[0] + o[1] - o[2] - o[3] + o[4] + o[5] - o[6] - o[7]) * norm;
}

// ── spatial ──────────────────────────────────────────────────────────────

namespace {
constexpr double kHeadRadius = 0.0875, kSound = 343.0;  // m, m/s
constexpr double kRoom = 0.5;     // early reflections: -6 dB re the dry sound at strength 1
constexpr double kSurround = 0.7; // surround speakers
constexpr double kWiden = 1.0;    // stage width: the side signal is raised by up to (1 + kWiden) = +6 dB
constexpr double kEnvelop = 0.6;  // short room around the head from the surround speakers: about -4.4 dB re dry
constexpr double kEnvelopSec = 0.7;
constexpr double kEnvMid = 0.5;   // share of the centre that goes into that room (so vocals sit in a room too)
constexpr double kAmbMid = 0.6;   // how much of the centre feeds the surround speakers (with all of the side)

// Brown & Duda's head-shadow parameter for the angle between the source and an ear (degrees): 2 facing the ear
// (+6 dB at high frequencies), 0.1 at the far side (-20 dB), alpha_min 0.1 at theta_min 150 degrees.
double ShadowAlpha(double incidenceDeg) { return 1.05 + 0.95 * std::cos(incidenceDeg / 150.0 * kPi); }
double AngleBetween(double a, double b) {
    const double d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180 ? 360 - d : d;
}
}  // namespace

bool SpeakerPair::Init(double fs, double azimuthDeg, double gain) {
    const double phi = azimuthDeg * kPi / 180, tau = kHeadRadius / (2 * kSound), K = 2 * fs;
    // The left speaker sits at -azimuth; the ears at -100 (left) and +100 (right) degrees, as in B&D.
    const double near = ShadowAlpha(AngleBetween(-azimuthDeg, -100)), far = ShadowAlpha(AngleBetween(-azimuthDeg, 100));
    // H(s) = gain (1 + zero s tau) / (1 + pole s tau), bilinear. The pole at near/2 makes the near ear exactly
    // 2*gain at high frequencies and both ears `gain` at low frequencies.
    auto design = [&](double zero, double pole) {
        First f;
        const double n = 1 + pole * tau * K;
        f.b0 = gain * (1 + zero * tau * K) / n;
        f.b1 = gain * (1 - zero * tau * K) / n;
        f.a1 = (1 - pole * tau * K) / n;
        return f;
    };
    near_ = design(near, near / 2);
    far_ = design(far, near / 2);
    const double lateral = std::asin(std::fabs(std::sin(phi)));
    itd_ = kHeadRadius / kSound * (lateral + std::sin(lateral)) * fs;  // Woodworth
    const double back = 0.5 * (1 - std::cos(phi));                    // 0 straight ahead, 1 behind
    echo_ = (std::cos(phi / 2) + 2) / 44100 * fs;                     // B&D pinna echo 1 (A=1, B=2 at 44.1 kHz)
    const double rho = 0.35 * back;
    direct_ = 1 / (1 + rho);
    reflect_ = rho / (1 + rho);
    backA_ = OnePole(fs, 3000);
    backG_ = DbToGain(-8 * back);
    const int size = Pow2AtLeast((int)std::ceil(itd_ + echo_) + 3);
    if (!hist_.Resize(2 * (size_t)size)) return false;
    mask_ = size - 1;
    Clear();
    return true;
}

void SpeakerPair::Clear() {
    hist_.Zero();
    w_ = 0;
    std::memset(back_, 0, sizeof back_);
    std::memset(zNear_, 0, sizeof zNear_);
    std::memset(zFar_, 0, sizeof zFar_);
}

inline double SpeakerPair::Read(int c, double d) const {  // linear-interpolated fractional delay
    const float* h = hist_.data();
    const int i = (int)d;
    const double f = d - i, a = h[2 * ((w_ - i) & mask_) + c], b = h[2 * ((w_ - i - 1) & mask_) + c];
    return a + f * (b - a);
}

void SpeakerPair::Tick(double left, double right, double& earL, double& earR) {
    const double in[2] = {left, right};
    float* h = hist_.data() + 2 * w_;
    for (int c = 0; c < 2; ++c) {  // pinna shadow from behind: high shelf
        back_[c] += backA_ * (in[c] - back_[c]);
        h[c] = (float)(back_[c] + backG_ * (in[c] - back_[c]));
    }
    double out[2][2];  // [speaker][near, far]
    for (int c = 0; c < 2; ++c) {
        const double xn = direct_ * h[c] + reflect_ * Read(c, echo_);
        const double xf = direct_ * Read(c, itd_) + reflect_ * Read(c, itd_ + echo_);
        out[c][0] = near_.b0 * xn + zNear_[c];
        zNear_[c] = near_.b1 * xn - near_.a1 * out[c][0];
        out[c][1] = far_.b0 * xf + zFar_[c];
        zFar_[c] = far_.b1 * xf - far_.a1 * out[c][1];
    }
    w_ = (w_ + 1) & mask_;
    earL = out[0][0] + out[1][1];
    earR = out[1][0] + out[0][1];
}

bool Spatial::Init(double fs) {
    if (!front_.Init(fs, 30, 0.5) || !rear_.Init(fs, 110, std::sqrt(0.5))) return false;
    const int size = Pow2AtLeast((int)(0.045 * fs) + 2);
    if (!hist_.Resize(4 * (size_t)size)) return false;
    mask_ = size - 1;
    // Early reflections per ear: (ms, from the opposite front speaker?, gain). Delays differ between the ears and
    // share no common spacing, so their sum is a dense ripple rather than a comb.
    static const struct { double ms; int other; double g; } er[2][kEr] = {
        {{3.1, 0, .9}, {5.3, 1, .75}, {8.9, 0, .65}, {11.7, 1, .55}, {15.2, 0, .45}, {19.9, 1, .38}, {23.4, 0, .3}, {28.6, 1, .25}},
        {{3.7, 0, .9}, {4.6, 1, .75}, {9.8, 0, .65}, {12.9, 1, .55}, {16.4, 0, .45}, {18.7, 1, .38}, {25.1, 0, .3}, {27.3, 1, .25}},
    };
    for (int e = 0; e < 2; ++e) {
        double sum = 0;
        for (const auto& t : er[e]) sum += t.g * t.g;
        for (int k = 0; k < kEr; ++k)
            er_[e][k] = {(int)std::lround(er[e][k].ms * fs / 1000), er[e][k].other ? 1 - e : e, er[e][k].g / std::sqrt(sum)};
    }
    // Velvet-noise decorrelators: one tap per 2.25 ms slot at a pseudo-random spot, random sign, decaying.
    // A different sequence per side makes the two surround speakers uncorrelated even for mono input.
    for (int e = 0; e < 2; ++e) {
        uint32_t x = e ? 0x2545F491u : 0x9E3779B9u;
        auto next = [&]() { x = x * 1664525u + 1013904223u; return (x >> 8) / 16777216.0; };
        double sum = 0;
        for (int k = 0; k < kAmb; ++k) {
            const double ms = 7 + 36.0 * (k + next()) / kAmb, g = std::exp(-(ms - 7) / 25) * (next() < 0.5 ? -1 : 1);
            amb_[e][k] = {(int)std::lround(ms * fs / 1000), 2 + e, g};
            sum += g * g;
        }
        for (auto& t : amb_[e]) t.g /= std::sqrt(sum);
    }
    hpA_ = OnePole(fs, 120);   // bass stays dry
    erA_ = OnePole(fs, 6000);  // walls absorb the top
    ambA_ = OnePole(fs, 5000);
    envA_ = OnePole(fs, 4000);  // 방의 잔향은 벽과 공기가 고음부터 먹는다(어둡게 남는다)
    // 감싸는 울림의 잔향: 줄마다 되먹임은 울림 시간에서, 크기는 임펄스 응답 에너지로 맞춘다(Processor의 잔향과 같은 식).
    if (!env_.Init(fs, 0)) return false;
    int len[kFdnLines];
    Reverb::LineLengths(fs, len);
    for (int i = 0; i < kFdnLines; ++i) envFb_[i] = std::pow(10.0, -3.0 * len[i] / (fs * kEnvelopSec));
    {
        double e = 0, el, rr;
        const int n = (int)(fs * 1.5 * kEnvelopSec);
        for (int i = 0; i < n; ++i) { env_.Tick(i == 0 ? 1.0 : 0.0, 0, 0, 0, envFb_, el, rr); e += 0.5 * (el * el + rr * rr); }
        envNorm_ = e > 0 ? 1 / std::sqrt(e) : 0;
    }
    // what a centred source gains, re dry (reflections + surround feed + room around the head)
    energy_ = kRoom * kRoom + kSurround * kSurround * kAmbMid * kAmbMid + kEnvelop * kEnvelop * kEnvMid * kEnvMid;
    Clear();
    return true;
}

void Spatial::Clear() {
    front_.Clear();
    rear_.Clear();
    env_.Clear();
    envL_ = envR_ = 0;
    hist_.Zero();
    w_ = 0;
    hpL_ = hpR_ = erL_ = erR_ = ambL_ = ambR_ = 0;
}

void Spatial::Tick(double& l, double& r, double strength, double feed) {
    // 들리는 정도는 세기에 대해 위로 볼록하게: 25%가 50%쯤으로 들린다(낮은 쪽에서도 분명히 바뀐다).
    const double s = std::sqrt(strength);
    const double m = 0.5 * (l + r), side = 0.5 * (l - r);
    // 1. front stage: centre speaker = M as is, side (widened) through the +-30 degree pair
    double fL, fR;
    const double wide = side * (1 + kWiden * s);
    front_.Tick(wide, -wide, fL, fR);
    // 2./3. histories for the room and the surround feeds, high-passed
    hpL_ += hpA_ * (l - hpL_);
    hpR_ += hpA_ * (r - hpR_);
    const double lh = l - hpL_, rh = r - hpR_, mh = 0.5 * (lh + rh), sh = 0.5 * (lh - rh);
    float* h = hist_.data();
    float* now = h + 4 * w_;
    now[0] = (float)(feed * lh);
    now[1] = (float)(feed * rh);
    now[2] = (float)(feed * (sh + kAmbMid * mh));
    now[3] = (float)(feed * (-sh + kAmbMid * mh));
    auto taps = [&](const Tap* t, int n) {
        double a = 0;
        for (int k = 0; k < n; ++k) a += t[k].g * h[4 * ((w_ - t[k].d) & mask_) + t[k].c];
        return a;
    };
    erL_ += erA_ * (taps(er_[0], kEr) - erL_);
    erR_ += erA_ * (taps(er_[1], kEr) - erR_);
    ambL_ += ambA_ * (taps(amb_[0], kAmb) - ambL_);
    ambR_ += ambA_ * (taps(amb_[1], kAmb) - ambR_);
    w_ = (w_ + 1) & mask_;
    // 감싸는 울림: 옆·뒤 스피커로(좌우가 다른 잔향이라 머리 둘레를 채운다)
    double eL, eR;
    env_.Tick(feed * (sh + kEnvMid * mh), 0, 0, 0, envFb_, eL, eR);
    const double env = kEnvelop / kSurround * envNorm_;
    envL_ += envA_ * (env * eL - envL_);
    envR_ += envA_ * (env * eR - envR_);
    double sL, sR;
    rear_.Tick(ambL_ + envL_, ambR_ + envR_, sL, sR);
    // 4. mix: strength 0 is the dry signal; the side crossfades from dry to the speaker pair
    if (s != lastS_) { lastS_ = s; trim_ = 1 / std::sqrt(std::sqrt(1 + s * s * energy_)); }
    const double dry = (1 - s) * side;
    l = trim_ * (m + dry + s * fL) + s * (kRoom * erL_ + kSurround * sL);
    r = trim_ * (m - dry + s * fR) + s * (kRoom * erR_ + kSurround * sR);
}

// ── processor ────────────────────────────────────────────────────────────

bool Processor::Init(double fs, int channels, double fadeMs) {
    fs_ = fs;
    ch_ = channels;
    fadeLen_ = std::max(1, (int)std::lround(fadeMs * fs / 1000));
    const bool ok = reverb_.Init(fs, (int)std::lround(0.2 * fs) + 1) && spatial_.Init(fs);
    Reset(Bypass());
    return ok;
}

void Processor::Reset(const Params& p) {
    cur_ = p;
    prev_ = p;
    fading_ = false;
    ClearState();
}

void Processor::ClearState() {
    std::memset(z_, 0, sizeof z_);
    reverbRun_ = spatialRun_ = false;  // their (large) buffers are cleared lazily when next needed
    quiet_ = true;
}

void Processor::BeginSwap(const Params& next) {
    if (SameParams(next, cur_)) return;
    prev_ = cur_;
    cur_ = next;
    bank_ ^= 1;  // the old filters keep running in the other bank for the crossfade; the new start clean
    std::memset(z_[bank_], 0, sizeof z_[bank_]);
    fadePos_ = 0;
    fading_ = true;
}

inline double Processor::RunBank(const Params& p, int bank, int ch, double x) {
    double(*z)[2] = z_[bank][ch];
    for (int i = 0; i < p.nPeaks; ++i) {  // transposed direct form II
        const Biquad& f = p.peak[i];
        const double y = f.b0 * x + z[i][0];
        z[i][0] = f.b1 * x - f.a1 * y + z[i][1];
        z[i][1] = f.b2 * x - f.a2 * y;
        x = y;
    }
    return x;
}

bool Processor::Process(float* buf, uint32_t frames, bool silent) {
    FtzGuard ftz;
    if (!fading_)
        if (const Params* next = inbox_.Take()) BeginSwap(*next);
    if (!fading_ && !cur_.active) {  // bypass: not a single bit touched
        reverbRun_ = spatialRun_ = false;  // whatever is left in their buffers is stale now
        quiet_ = true;
        return false;
    }
    if (silent) {
        if (quiet_ && !fading_) return false;     // nothing left ringing
        std::memset(buf, 0, sizeof(float) * frames * ch_);
    }

    const bool stereo = ch_ >= 2;
    const bool needReverb = cur_.wet > 0 || (fading_ && prev_.wet > 0);
    // A delay-based stage restarting from cleared buffers ramps its input in over the fade; otherwise the first
    // full-amplitude sample reaching each delay tap would be a step (a click) predelay/line-length ms later.
    if (needReverb && !reverbRun_) { reverb_.Clear(); reverbIn_ = 0; }
    reverbRun_ = needReverb;
    const bool needSpatial = stereo && (cur_.spatial > 0 || (fading_ && prev_.spatial > 0));
    if (needSpatial && !spatialRun_) { spatial_.Clear(); spatialIn_ = 0; }
    spatialRun_ = needSpatial;
    const double ramp = 1.0 / fadeLen_;

    const int nc = std::min(ch_, kMaxChannels);
    double peak = 0, sum = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        float* x = buf + (size_t)f * ch_;
        const bool fade = fading_;
        const double u = fade ? (double)(fadePos_ + 1) / fadeLen_ : 1.0;
        const double t = u * u * (3 - 2 * u);  // smoothstep: no slope corner where the fade starts or ends
        double y[kMaxChannels];
        for (int c = 0; c < nc; ++c) {
            const double in = x[c];
            y[c] = cur_.preamp * RunBank(cur_, bank_, c, in);
            if (fade) {
                const double old = prev_.preamp * RunBank(prev_, bank_ ^ 1, c, in);
                y[c] = old + t * (y[c] - old);
            }
        }
        if (needReverb) {
            double fb[kFdnLines];
            const double* fbp = cur_.feedback;
            if (fade) {
                for (int i = 0; i < kFdnLines; ++i) fb[i] = prev_.feedback[i] + t * (cur_.feedback[i] - prev_.feedback[i]);
                fbp = fb;
            }
            reverbIn_ = std::min(1.0, reverbIn_ + ramp);
            const double in = reverbIn_ * (stereo ? 0.5 * (y[0] + y[1]) : y[0]);
            double l, r;
            reverb_.Tick(in, fade ? prev_.predelay : cur_.predelay, cur_.predelay, t, fbp, l, r);
            const double wet = fade ? prev_.wet + t * (cur_.wet - prev_.wet) : cur_.wet;
            if (stereo) { y[0] += wet * l; y[1] += wet * r; }
            else y[0] += wet * 0.5 * (l + r);
        }
        if (needSpatial) {
            spatialIn_ = std::min(1.0, spatialIn_ + ramp);
            spatial_.Tick(y[0], y[1], fade ? prev_.spatial + t * (cur_.spatial - prev_.spatial) : cur_.spatial, spatialIn_);
        }
        for (int c = 0; c < nc; ++c) {
            x[c] = (float)y[c];
            sum += y[c];
            const double a = std::fabs(y[c]);
            if (a > peak) peak = a;
        }
        if (fade && ++fadePos_ >= fadeLen_) fading_ = false;
    }

    if (!std::isfinite(sum)) {  // poisoned state (e.g. NaN from upstream): mute this block and start clean
        std::memset(buf, 0, sizeof(float) * frames * ch_);
        ClearState();
        return true;
    }
    if (!silent) quiet_ = false;
    else if (peak < 1e-6) ClearState();  // tail is below -120 dBFS: stop touching silent buffers
    return true;
}

}  // namespace keq
