// Offline render through the APO's own DSP core (dsp.cpp) - for listening tests and A/B files.
//
//   keq_render in.wav out.wav "<cfg text>"   cfg as in CONTRACT §1; ';' or a literal \n separates lines
//                                            in: 16/24/32-bit PCM or 32/64-bit float WAV; out: 32-bit float WAV
//   keq_render --demo <dir>                  synthesises <dir>\source.wav (15 s stereo: panned instruments, drums,
//                                            a little ambience) and renders <dir>\spatial_0/50/100.wav from it
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../src/dsp.h"

using namespace keq;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Audio {
    int rate = 48000, channels = 2;
    std::vector<float> s;  // interleaved
    size_t Frames() const { return s.size() / channels; }
};

uint32_t U32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

bool ReadWav(const char* path, Audio* a) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return false;
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) b.insert(b.end(), buf, buf + n);
    fclose(f);
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    int tag = 0, bits = 0;
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    for (size_t at = 12; at + 8 <= b.size();) {
        const uint32_t len = U32(&b[at + 4]);
        const uint8_t* body = &b[at + 8];
        const size_t avail = std::min<size_t>(len, b.size() - at - 8);
        if (!std::memcmp(&b[at], "fmt ", 4) && avail >= 16) {
            tag = U16(body);
            a->channels = U16(body + 2);
            a->rate = (int)U32(body + 4);
            bits = U16(body + 14);
            if (tag == 0xFFFE && avail >= 26) tag = U16(body + 24);  // WAVE_FORMAT_EXTENSIBLE: sub-format GUID
        } else if (!std::memcmp(&b[at], "data", 4)) {
            data = body;
            dataLen = avail;
        }
        at += 8 + (size_t)len + (len & 1);
    }
    if (!data || a->channels < 1 || a->rate < 8000) return false;
    const int bytes = bits / 8;
    if (!((tag == 1 && (bits == 16 || bits == 24 || bits == 32)) || (tag == 3 && (bits == 32 || bits == 64)))) {
        std::fprintf(stderr, "unsupported WAV format (tag %d, %d bits)\n", tag, bits);
        return false;
    }
    const size_t n = dataLen / bytes;
    a->s.resize(n - n % a->channels);
    for (size_t i = 0; i < a->s.size(); ++i) {
        const uint8_t* p = data + i * bytes;
        double v;
        if (tag == 3) {
            if (bits == 32) { float x; std::memcpy(&x, p, 4); v = x; }
            else { double x; std::memcpy(&x, p, 8); v = x; }
        } else if (bits == 16) v = (int16_t)U16(p) / 32768.0;
        else if (bits == 24) v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0;
        else v = (int32_t)U32(p) / 2147483648.0;
        a->s[i] = (float)v;
    }
    return true;
}

bool WriteWav(const char* path, const Audio& a) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") || !f) return false;
    const uint32_t data = (uint32_t)(a.s.size() * 4), frames = (uint32_t)a.Frames();
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(4 + 26 + 12 + 8 + data); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); u32(18);
    u16(3); u16((uint16_t)a.channels); u32((uint32_t)a.rate); u32((uint32_t)a.rate * a.channels * 4);
    u16((uint16_t)(a.channels * 4)); u16(32); u16(0);
    fwrite("fact", 1, 4, f); u32(4); u32(frames);
    fwrite("data", 1, 4, f); u32(data);
    fwrite(a.s.data(), 4, a.s.size(), f);
    return fclose(f) == 0;
}

// Same path as the APO: parse, design for this rate, process in 10 ms blocks. Adds room for a reverb tail.
Audio Render(const Audio& in, std::string cfg) {
    for (size_t i; (i = cfg.find("\\n")) != std::string::npos;) cfg.replace(i, 2, "\n");
    for (auto& ch : cfg) if (ch == ';') ch = '\n';
    const Config c = ParseConfig(cfg.data(), cfg.size());
    const Params p = Design(c, in.rate);
    Audio out = in;
    if (p.active) {
        const double tail = (p.wet > 0 ? std::min(1.5 * c.decaySec, 10.0) : 0) + (p.spatial > 0 ? 0.05 : 0);
        out.s.resize(out.s.size() + (size_t)(tail * in.rate) * in.channels, 0.f);
    }
    Processor proc;
    if (!proc.Init(in.rate, in.channels)) { std::fprintf(stderr, "out of memory\n"); return out; }
    proc.Reset(p);
    const size_t block = (size_t)in.rate / 100, frames = out.Frames();
    for (size_t at = 0; at < frames; at += block)
        proc.Process(out.s.data() + at * in.channels, (uint32_t)std::min(block, frames - at), false);
    return out;
}

// ── demo passage ─────────────────────────────────────────────────────────
// 96 bpm, six bars of Am F C G Am F. Placed like a typical mix: kick, bass, snare and the lead in the centre,
// a plucked guitar left, an electric piano right, hi-hat right, shaker left, a wide pad and a little room.
Audio MakeDemo() {
    Audio a;
    const int fs = a.rate = 48000;
    a.channels = 2;
    const double beat = 60.0 / 96, len = 15.0;
    const size_t n = (size_t)(len * fs);
    std::vector<double> L(n), R(n);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> uni(-1, 1);
    auto hz = [](double midi) { return 440 * std::pow(2.0, (midi - 69) / 12); };
    auto add = [&](size_t i, double v, double pan) {  // pan -1 (left) .. 1 (right), constant power
        if (i >= n) return;
        const double t = (pan + 1) * kPi / 4;
        L[i] += v * std::cos(t);
        R[i] += v * std::sin(t);
    };
    const int roots[6] = {45, 41, 48, 43, 45, 41};
    const int chords[6][3] = {{57, 60, 64}, {53, 57, 60}, {60, 64, 67}, {55, 59, 62}, {57, 60, 64}, {53, 57, 60}};

    for (int bar = 0; bar < 6; ++bar) {
        const double t0 = bar * 4 * beat;
        for (int b = 0; b < 4; ++b) {  // kick on 1 and 3 (+ the "and" of 3), snare on 2 and 4
            const double tb = t0 + b * beat;
            auto kick = [&](double at, double amp) {
                double ph = 0;
                for (size_t i = 0; i < (size_t)(0.4 * fs); ++i) {
                    const double t = i / (double)fs;
                    ph += 2 * kPi * (45 + 75 * std::exp(-t * 30)) / fs;
                    add((size_t)(at * fs) + i, amp * std::sin(ph) * std::exp(-t * 9), 0);
                }
            };
            if (b == 0 || b == 2) kick(tb, 0.9);
            if (b == 2) kick(tb + 0.5 * beat, 0.5);
            if (b == 1 || b == 3) {
                double hp = 0;
                for (size_t i = 0; i < (size_t)(0.3 * fs); ++i) {
                    const double t = i / (double)fs, w = uni(rng);
                    hp += 0.15 * (w - hp);
                    add((size_t)(tb * fs) + i, 0.45 * (w - hp) * std::exp(-t * 18) + 0.3 * std::sin(2 * kPi * 190 * t) * std::exp(-t * 35), 0.1);
                }
            }
        }
        for (int e = 0; e < 8; ++e) {  // eighths: hi-hat right, bass, guitar arpeggio left
            const double te = t0 + e * beat / 2;
            double prev = 0;
            const double hatDecay = e % 2 ? 14 : 45;
            for (size_t i = 0; i < (size_t)(0.25 * fs); ++i) {
                const double t = i / (double)fs, w = uni(rng);
                add((size_t)(te * fs) + i, 0.16 * (w - prev) * std::exp(-t * hatDecay), 0.45);
                prev = w;
            }
            const double fb = hz(roots[bar] + (e == 3 || e == 7 ? 12 : 0));
            for (size_t i = 0; i < (size_t)(0.9 * beat / 2 * fs); ++i) {
                const double t = i / (double)fs, env = std::min(1.0, t * 200) * std::exp(-t * 4);
                add((size_t)(te * fs) + i, 0.35 * env * (std::sin(2 * kPi * fb * t) + 0.3 * std::sin(4 * kPi * fb * t)), 0);
            }
            static const int pattern[8] = {0, 1, 2, 3, 2, 1, 3, 2};
            const int k = pattern[e];
            const double fg = hz(k == 3 ? chords[bar][0] + 12 : chords[bar][k]);
            std::vector<double> ks((size_t)(fs / fg));  // Karplus-Strong pluck
            for (auto& x : ks) x = uni(rng);
            for (size_t i = 0, j = 0; i < (size_t)(1.2 * fs); ++i, j = (j + 1) % ks.size()) {
                const double y = ks[j];
                ks[j] = 0.996 * 0.5 * (ks[j] + ks[(j + 1) % ks.size()]);
                add((size_t)(te * fs) + i, 0.18 * y, -0.7);
            }
        }
        for (int s = 0; s < 16; ++s) {  // sixteenth shaker, left
            const double ts = t0 + s * beat / 4;
            double hp = 0;
            for (size_t i = 0; i < (size_t)(0.08 * fs); ++i) {
                const double t = i / (double)fs, w = uni(rng);
                hp += 0.5 * (w - hp);
                add((size_t)(ts * fs) + i, (s % 4 == 2 ? 0.07 : 0.04) * (w - hp) * std::min(1.0, t * 400) * std::exp(-t * 60), -0.55);
            }
        }
        for (double off : {1.5, 3.5}) {  // electric piano (FM) stabs on the off-beats, right
            for (int v = 0; v < 3; ++v) {
                const double f = hz(chords[bar][v] + 12);
                for (size_t i = 0; i < (size_t)(1.5 * fs); ++i) {
                    const double t = i / (double)fs, I = 0.3 + 1.7 * std::exp(-t * 6);
                    add((size_t)((t0 + off * beat) * fs) + i,
                        0.07 * std::exp(-t * 2.5) * std::sin(2 * kPi * f * t + I * std::sin(2 * kPi * f * t)), 0.65);
                }
            }
        }
        for (int v = 0; v < 3; ++v) {  // pad: detuned saws, different per side (wide), low-passed
            const double f = hz(chords[bar][v]), dur = 4 * beat + 0.6;
            double lpL = 0, lpR = 0, lpL2 = 0, lpR2 = 0;
            const double c = 1 - std::exp(-2 * kPi * 900 / fs);
            for (size_t i = 0; i < (size_t)(dur * fs); ++i) {
                const double t = i / (double)fs;
                const double env = std::min(1.0, t / 0.5) * std::min(1.0, (dur - t) / 0.6);
                auto saw = [&](double ff) { const double x = ff * t; return 2 * (x - std::floor(x)) - 1; };
                lpL += c * (saw(f * 1.003) + saw(f * 0.9985) - lpL); lpL2 += c * (lpL - lpL2);
                lpR += c * (saw(f * 0.997) + saw(f * 1.0015) - lpR); lpR2 += c * (lpR - lpR2);
                const size_t at = (size_t)(t0 * fs) + i;
                if (at < n) { L[at] += 0.05 * env * lpL2; R[at] += 0.05 * env * lpR2; }
            }
        }
    }
    // lead melody (bars 3-6), centre: a voice-like tone with vibrato
    static const double mel[][3] = {{8, 1, 72}, {9, .5, 74}, {9.5, .5, 76}, {10, 2, 79}, {12, 1, 79}, {13, 1, 77},
                                    {14, 2, 74}, {16, 1.5, 76}, {17.5, .5, 74}, {18, 2, 72}, {20, 1, 69}, {21, 1, 72}, {22, 2, 72}};
    for (const auto& m : mel) {
        const double f = hz(m[2]), dur = m[1] * beat;
        double ph = 0;
        for (size_t i = 0; i < (size_t)((dur + 0.1) * fs); ++i) {
            const double t = i / (double)fs;
            const double env = std::min(1.0, t / 0.03) * std::min(1.0, std::max(0.0, (dur + 0.1 - t) / 0.1));
            ph += 2 * kPi * f * (1 + 0.004 * std::min(1.0, t / 0.3) * std::sin(2 * kPi * 5.2 * t)) / fs;
            double v = 0;
            for (int k = 1; k <= 6; ++k) v += std::sin(k * ph) / (k * k);
            add((size_t)(m[0] * beat * fs) + i, 0.16 * env * v, 0);
        }
    }
    // a little room on the whole mix: the engine's own FDN reverb (decay 1.2 s), about -16 dB
    Reverb room;
    if (room.Init(fs, 1)) {
        int lens[kFdnLines];
        double fb[kFdnLines];
        Reverb::LineLengths(fs, lens);
        for (int i = 0; i < kFdnLines; ++i) fb[i] = std::pow(10.0, -3.0 * lens[i] / (fs * 1.2));
        for (size_t i = 0; i < n; ++i) {
            double l, r;
            room.Tick(0.5 * (L[i] + R[i]), 0, 0, 0, fb, l, r);
            L[i] += 0.06 * l;
            R[i] += 0.06 * r;
        }
    }
    double peak = 0;
    for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
    const double g = 0.5 / peak;  // -6 dBFS peak: headroom for the renders
    a.s.resize(2 * n);
    for (size_t i = 0; i < n; ++i) { a.s[2 * i] = (float)(g * L[i]); a.s[2 * i + 1] = (float)(g * R[i]); }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--demo")) {
        const std::string dir = argv[2];
        const Audio src = MakeDemo();
        bool ok = WriteWav((dir + "\\source.wav").c_str(), src);
        for (int pct : {0, 50, 100}) {
            char cfg[32], name[32];
            std::snprintf(cfg, sizeof cfg, "spatial %.2f", pct / 100.0);
            std::snprintf(name, sizeof name, "\\spatial_%d.wav", pct);
            ok = ok && WriteWav((dir + name).c_str(), Render(src, cfg));
        }
        if (!ok) { std::fprintf(stderr, "could not write into %s\n", dir.c_str()); return 1; }
        std::printf("wrote %s\\source.wav, spatial_0.wav, spatial_50.wav, spatial_100.wav\n", dir.c_str());
        return 0;
    }
    if (argc != 4) {
        std::fprintf(stderr, "usage: keq_render in.wav out.wav \"<cfg text>\"   (e.g. \"spatial 1;peak 100 1 3\")\n"
                             "       keq_render --demo <dir>\n");
        return 2;
    }
    Audio in;
    if (!ReadWav(argv[1], &in)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    if (!WriteWav(argv[2], Render(in, argv[3]))) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    std::printf("%s: %d Hz, %d ch, %.2f s -> %s\n", argv[1], in.rate, in.channels, in.Frames() / (double)in.rate, argv[2]);
    return 0;
}
