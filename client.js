"use strict";
// EQ 화면 로직. 원본은 셋뿐이다.
//  - 저장소 profiles: 출력 장치 id마다의 설정(고른 공간, 공간마다 손으로 고친 몫, 공간감, 켜짐, 프리앰프).
//  - 저장소 my_presets: 내 프리셋(기기 사이에서 다시 쓰는 곡선).
//  - 다리(bridge.ps1)가 알려 주는 엔진과 기본 출력 장치의 상태.
// 화면에 보이는 모든 값(이름표, 곡선, 엔진에 보낼 설정)은 이 셋에서 파생한다. 따로 들고 있는 사본은 없다.
(() => {
  // ── pure:begin ─────────────────────────────────────────────
  // 이 구역은 DOM과 kaleidoscope에 닿지 않는다. test_client.js가 이 구역만 떼어 시험한다.
  const FREQS = [32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];
  // 귀에 익은 말이 먼저, 주파수는 작게.
  const BAND_NAMES = ["깊은 저음", "저음", "저음", "중저음", "중음", "중음", "중고음", "고음", "고음", "공기감"];
  const Q = 1.41; // 한 옥타브 폭. 이웃 밴드가 자연스럽게 겹친다.
  const FS = 48000;
  const GAIN_LIMIT = 15; // 밴드 하나의 최종 이득 한계(dB).
  const TONE_LIMIT = 12; // 저음·중음·고음 손잡이 한계(dB).
  // 음악은 좁은 대역이 0 dBFS에 꽉 차는 일이 드물다. 곡선 꼭대기를 전부 깎으면 부스트가 볼륨 감소로만
  // 들리므로 이만큼은 여유로 보고 그 위만 깎는다. 잔향은 에너지를 더하므로 그만큼 더 낮춘다.
  const HEADROOM_DB = 4;
  const REVERB_TRIM_DB = 2;
  // 저음·중음·고음 손잡이가 움직이는 밴드. 손잡이 값은 그 밴드들에서 손으로 고친 몫의 평균이다(따로 저장하지 않는다).
  const TONES = [
    { key: "low", label: "저음", hz: "32–250 Hz", bands: [0, 1, 2, 3] },
    { key: "mid", label: "중음", hz: "500 Hz–2 kHz", bands: [4, 5, 6] },
    { key: "high", label: "고음", hz: "4–16 kHz", bands: [7, 8, 9] },
  ];
  const SPATIAL = [
    { value: "0", label: "끔", amount: 0 },
    { value: "0.35", label: "약하게", amount: 0.35 },
    { value: "0.65", label: "보통", amount: 0.65 },
    { value: "1", label: "강하게", amount: 1 },
  ];
  // 공간(장면). gains는 FREQS 순서(dB). reverb는 잔향: decay 60 dB 줄어드는 시간(초), predelay 직접음과 잔향 사이(ms),
  // wet 직접음 대비 잔향 에너지(dB). 첫 장면(Original, key "studio")이 원음이고 새 기기는 여기서 시작한다.
  const SCENES = [
    { key: "studio", name: "Original", desc: "더하거나 빼지 않은 원래 소리 그대로", gains: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0], reverb: null },
    { key: "concert-hall", name: "Concert Hall", desc: "넓은 홀의 긴 잔향과 따뜻한 저음", gains: [3, 3, 2, 0, -1, -1, 0, 1, 2, 1], reverb: { decay: 2.4, predelay: 28, wet: -2 } },
    { key: "jazz-club", name: "Jazz Club", desc: "가까운 무대, 짧고 포근한 울림", gains: [3, 3, 1, 2, 0, -1, 0, 2, 2, 2], reverb: { decay: 0.9, predelay: 9, wet: -9 } },
    { key: "live", name: "Live", desc: "공연장 앞줄처럼 생생하고 힘 있게", gains: [2, 2, 0, -1, 0, 2, 3, 3, 2, 1], reverb: { decay: 1.3, predelay: 15, wet: -5 } },
    { key: "club", name: "Club", desc: "몸으로 느끼는 묵직한 저음", gains: [7, 6, 3, 0, -2, -1, 0, 1, 3, 3], reverb: { decay: 0.7, predelay: 6, wet: -9 } },
    { key: "cinema", name: "Cinema", desc: "또렷한 대사와 큰 공간감", gains: [4, 3, 1, 0, 0, 1, 2, 2, 2, 1], reverb: { decay: 1.6, predelay: 20, wet: -8 } },
  ];
  // 장르. 잔향 없이 곡선만 바꾼다(예전 판의 장르 프리셋과 같은 key·곡선이라 예전 설정이 그대로 이어진다).
  const GENRES = [
    { key: "rock", name: "Rock", desc: "힘 있는 저음과 또렷한 기타", gains: [5, 4, 3, 0, -2, -1, 2, 4, 5, 5], reverb: null },
    { key: "pop", name: "Pop", desc: "보컬과 비트를 앞으로", gains: [-1, 2, 4, 5, 3, 0, -1, -1, 2, 3], reverb: null },
    { key: "jazz", name: "Jazz", desc: "따뜻한 베이스와 부드러운 고음", gains: [4, 3, 1, 2, -1, -1, 0, 2, 3, 4], reverb: null },
    { key: "classical", name: "Classical", desc: "넓은 음역을 고르게", gains: [5, 4, 3, 2, -1, -1, 0, 2, 4, 4], reverb: null },
    { key: "electronic", name: "Electronic", desc: "깊은 저음과 반짝이는 고음", gains: [7, 6, 2, 0, -2, 1, 0, 2, 5, 6], reverb: null },
    { key: "hip-hop", name: "Hip-Hop", desc: "단단한 킥과 베이스", gains: [7, 6, 3, 3, -1, -1, 1, 0, 2, 3], reverb: null },
    { key: "acoustic", name: "Acoustic", desc: "악기의 결을 자연스럽게", gains: [3, 3, 2, 1, 2, 2, 3, 4, 3, 2], reverb: null },
  ];
  // 음색. 방도 음악도 아닌 소리의 결(저음·고음·목소리·작은 볼륨·작은 스피커·게임 발소리). 곡선만 바꾼다.
  // 예전 판의 저음·고음·용도 프리셋과 같은 key·곡선이라 예전 설정이 그대로 이어진다.
  const COLORS = [
    { key: "deep-bass", name: "Deep Bass", desc: "몸으로 느끼는 깊은 저음", gains: [10, 9, 6, 2, -1, -1, 0, 0, 0, 0], reverb: null },
    { key: "bass-boost", name: "Bass Boost", desc: "저음을 조금 더", gains: [7, 6, 4, 2, 0, 0, 0, 0, 0, 0], reverb: null },
    { key: "bass-cut", name: "Bass Cut", desc: "웅웅거리는 저음을 덜어 내기", gains: [-8, -6, -4, -2, 0, 0, 0, 0, 0, 0], reverb: null },
    { key: "treble-boost", name: "Treble Boost", desc: "또렷하고 반짝이게", gains: [0, 0, 0, 0, 0, 1, 3, 5, 6, 7], reverb: null },
    { key: "treble-cut", name: "Treble Cut", desc: "쏘는 고음을 부드럽게", gains: [0, 0, 0, 0, 0, -1, -3, -5, -6, -7], reverb: null },
    { key: "vocal", name: "Vocal", desc: "목소리와 팟캐스트를 앞으로", gains: [-3, -2, -1, 1, 3, 4, 4, 3, 1, 0], reverb: null },
    { key: "spoken-word", name: "Spoken Word", desc: "말소리를 또렷하게", gains: [-6, -4, -1, 1, 3, 5, 5, 3, 1, -2], reverb: null },
    { key: "late-night", name: "Late Night", desc: "작게 틀어도 또렷하게", gains: [-4, -3, -1, 0, 0, 1, 1, 0, -1, -3], reverb: null },
    { key: "loudness", name: "Loudness", desc: "작은 볼륨에서도 저음과 고음이 살게", gains: [7, 5, 1, 0, -2, 0, -1, -1, 5, 4], reverb: null },
    { key: "small-speakers", name: "Small Speakers", desc: "노트북·작은 스피커를 크게", gains: [-5, -2, 3, 4, 2, 1, 1, 2, 3, 2], reverb: null },
    { key: "footsteps", name: "Footsteps", desc: "게임의 발소리와 효과음을 앞으로", gains: [-4, -3, -2, 0, 0, 2, 5, 6, 4, 0], reverb: null },
  ];
  // 겹쳐 쓰는 축. 공간 하나 위에 축마다 하나(또는 없음)를 얹는다. 같은 축 안에서는 하나만 고른다.
  // 공간은 방의 울림(잔향)과 그 방의 음색을 맡고, 나머지 축은 곡선만 더한다. 잔향은 공간 것을 쓴다.
  // field는 프로필에서 고른 key를 두는 자리다. 축을 늘리려면 여기에 하나 더하면 된다.
  const AXES = [
    { field: "genre", list: GENRES },
    { field: "color", list: COLORS },
  ];
  const FRESH = { scene: "studio", genre: null, color: null, edits: {}, spatial: 0, enabled: true, auto_preamp: true, preamp: 0 };

  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
  const clamp01 = v => clamp(Number.isFinite(v) ? v : 0, 0, 1);
  const round1 = v => Math.round(v * 10) / 10;
  const zeros = () => FREQS.map(() => 0);
  const num = v => (Number.isFinite(Number(v)) ? Number(v) : 0);
  const vec = a => FREQS.map((_, i) => num(a && a[i]));

  function peaking(f0, gain) {
    const A = Math.pow(10, gain / 40), w0 = 2 * Math.PI * f0 / FS;
    const alpha = Math.sin(w0) / (2 * Q), c = Math.cos(w0);
    const a0 = 1 + alpha / A;
    return { b0: (1 + alpha * A) / a0, b1: -2 * c / a0, b2: (1 - alpha * A) / a0, a1: -2 * c / a0, a2: (1 - alpha / A) / a0 };
  }
  function magDb(f, filters) {
    const w = 2 * Math.PI * f / FS, c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
    let db = 0;
    for (const h of filters) {
      const nr = h.b0 + h.b1 * c1 + h.b2 * c2, ni = -(h.b1 * s1 + h.b2 * s2);
      const dr = 1 + h.a1 * c1 + h.a2 * c2, di = -(h.a1 * s1 + h.a2 * s2);
      db += 10 * Math.log10((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    return db;
  }
  const filtersOf = gains => gains.map((g, i) => [FREQS[i], g]).filter(([, g]) => g !== 0).map(([f, g]) => peaking(f, g));
  // 20 Hz–20 kHz를 로그 간격 n점으로 본 실제 필터 응답.
  function curve(gains, n) {
    const fs = filtersOf(gains), out = [];
    for (let i = 0; i < n; i++) { const f = 20 * Math.pow(1000, i / (n - 1)); out.push([f, magDb(f, fs)]); }
    return out;
  }
  function autoPreamp(gains, hasReverb) {
    const peak = Math.max(0, ...curve(gains, 400).map(([, d]) => d));
    const trim = Math.max(0, peak - HEADROOM_DB) + (hasReverb ? REVERB_TRIM_DB : 0);
    return trim === 0 ? 0 : -Math.ceil(trim * 10) / 10;
  }

  // 장면 목록 = 기본 공간 + 내 프리셋. 내 프리셋도 같은 모양(key, name, gains, reverb)이라 똑같이 고른다.
  // 내 프리셋은 저장할 때의 곡선 전체(공간 + 장르 + 고친 몫)를 담으므로 장르를 겹치지 않는다.
  function catalog(mine) {
    return SCENES.concat((mine || []).map(p => ({ key: p.key, name: p.name, desc: p.desc || "내 프리셋", gains: vec(p.gains), reverb: p.reverb || null, mine: true })));
  }
  const sceneOf = (cat, key) => cat.find(s => s.key === key) || cat[0];
  // 축 하나에서 프로필이 고른 것(없거나 내 프리셋 위면 null). 내 프리셋은 곡선 전체라 축을 얹지 않는다.
  const pickOf = (p, axis) => (p[axis.field] && axis.list.find(x => x.key === p[axis.field])) || null;
  const picksOf = (cat, p) => (sceneOf(cat, p.scene).mine ? [] : AXES.map(a => pickOf(p, a)).filter(Boolean));
  // 프로필이 듣는 장면: 공간에 장르를 겹친 것. key는 고친 몫을 따로 기억하는 자리(공간+장르 조합마다)다.
  // 공간·장르의 정도(0~1). 고른 것마다 따로 기억한다(p.amounts[key]). 처음은 1(100%).
  const amountOf = (p, key) => clamp01(p.amounts && p.amounts[key] != null ? num(p.amounts[key]) : 1);
  // 잔향의 정도는 젖은 소리의 크기로: 50%면 -6 dB. 거의 0이면 잔향을 끈다(울림 길이는 그 방의 성격이라 그대로).
  const scaledReverb = (r, a) => (!r || a < 0.02 ? null : a >= 0.995 ? r : { ...r, wet: round1(r.wet + 20 * Math.log10(a)) });
  const pct = a => (a < 0.995 ? ` ${Math.round(a * 100)}%` : "");
  function layerOf(cat, p) {
    const space = sceneOf(cat, p.scene), picks = picksOf(cat, p);
    const sa = space.mine ? 1 : amountOf(p, space.key);
    if (!picks.length && sa >= 0.995) return space;
    const spaceName = space.name + (space.key === SCENES[0].key ? "" : pct(sa));
    const parts = picks.map(x => ({ x, a: amountOf(p, x.key) }));
    return { key: [space.key, ...picks.map(x => x.key)].join("+"), mine: space.mine,
             name: [spaceName, ...parts.map(({ x, a }) => x.name + pct(a))].join(" + "),
             desc: [space.desc, ...picks.map(x => x.desc)].join(", "),
             gains: space.gains.map((g, i) => parts.reduce((sum, { x, a }) => sum + x.gains[i] * a, g * sa)),
             reverb: scaledReverb(space.reverb, sa) };
  }
  // 기기 id의 프로필. 저장된 적이 없으면 원음(Original)에서 시작한다.
  // 기기 id의 프로필. 저장된 적이 없는 기기는 start(보통 FRESH)에서 시작한다.
  const profileOf = (profiles, id, start = FRESH) => moved({ ...start, ...((profiles && id && profiles[id]) || {}) });
  const MOVED_TO_COLOR = ["vocal", "late-night"];
  function moved(p) {
    if (!MOVED_TO_COLOR.includes(p.scene)) return p;
    const edits = { ...(p.edits || {}) };
    for (const k of Object.keys(edits)) if (k === p.scene || k.startsWith(p.scene + "+")) {
      edits[[SCENES[0].key, ...k.split("+").slice(1), p.scene].join("+")] = edits[k];
      delete edits[k];
    }
    return { ...p, scene: SCENES[0].key, color: p.color || p.scene, edits };
  }
  // 예전 EQ 앱(Equalizer APO 판)의 저장값 `eq` {enabled, presets, gains, auto_preamp, preamp}에서 처음 기기의 시작점을 파생한다.
  // 켠 프리셋 가운데 지금도 있는 공간을 고르고, 곡선과의 차이는 그 공간에서 손으로 고친 몫으로 둔다. 없으면 FRESH.
  function startOf(legacy) {
    if (!legacy || !Array.isArray(legacy.gains)) return FRESH;
    const on = legacy.presets || [];
    const space = SCENES.find(x => on.includes(x.key));
    const start = { ...FRESH, scene: space ? space.key : FRESH.scene };
    for (const a of AXES) { const x = a.list.find(y => on.includes(y.key)); if (x) start[a.field] = x.key; }
    if (!space && AXES.every(a => !start[a.field])) return FRESH;
    const scene = layerOf(SCENES, start);
    const edits = vec(legacy.gains).map((g, i) => g - scene.gains[i]);
    return { ...start, edits: edits.some(e => e !== 0) ? { [scene.key]: edits } : {},
             enabled: legacy.enabled !== false, auto_preamp: legacy.auto_preamp !== false, preamp: Number(legacy.preamp) || 0 };
  }
  // 고친 몫은 공간+장르 조합마다 따로 둔다(Concert Hall + Rock에서 고친 것은 Concert Hall만일 때와 따로).
  // 고른 조합마다(공간 + 축마다 고른 것). 내 프리셋을 고르면 축은 비운다(pick_mine).
  const editsKey = p => [p.scene, ...AXES.map(a => pickOf(p, a)).filter(Boolean).map(x => x.key)].join("+");
  const editsOf = p => vec(p.edits && p.edits[editsKey(p)]);
  // 지금 곡선 = 고른 공간 + 그 공간에서 손으로 고친 몫.
  const gainsOf = (p, scene) => scene.gains.map((g, i) => clamp(g + editsOf(p)[i], -GAIN_LIMIT, GAIN_LIMIT));
  const isCustom = p => editsOf(p).some(v => Math.abs(v) > 1e-9);
  const labelOf = (p, scene) => (isCustom(p) ? `${scene.name} (커스텀)` : scene.name);
  function withEdits(p, edits) {
    const all = { ...(p.edits || {}) };
    if (edits.some(v => Math.abs(v) > 1e-9)) all[editsKey(p)] = edits.map(round1);
    else delete all[editsKey(p)];
    return { ...p, edits: all };
  }
  // 밴드 하나를 최종 이득 g로. 손으로 고친 몫만 바뀐다.
  function setBand(p, scene, index, g) {
    const e = editsOf(p);
    e[index] = clamp(num(g), -GAIN_LIMIT, GAIN_LIMIT) - scene.gains[index];
    return withEdits(p, e);
  }
  const toneOf = (p, tone) => round1(tone.bands.reduce((s, i) => s + editsOf(p)[i], 0) / tone.bands.length);
  // 손잡이를 v로: 그 밴드들의 고친 몫을 같은 만큼 옮겨 평균이 v가 되게 한다(밴드 사이 모양은 지킨다).
  function setTone(p, tone, v) {
    const e = editsOf(p), d = clamp(num(v), -TONE_LIMIT, TONE_LIMIT) - toneOf(p, tone);
    for (const i of tone.bands) e[i] += d;
    return withEdits(p, e);
  }
  const resetScene = p => withEdits(p, zeros());
  const preampOf = (p, scene) => (p.auto_preamp === false ? num(p.preamp) : autoPreamp(gainsOf(p, scene), !!scene.reverb));
  // 다리의 apply 인자(CONTRACT §4).
  function renderConfig(p, scene) {
    const gains = gainsOf(p, scene);
    return {
      enabled: p.enabled !== false,
      preamp: preampOf(p, scene),
      peaks: gains.map((g, i) => [FREQS[i], Q, round1(g)]).filter(([, , g]) => g !== 0),
      reverb: scene.reverb ? { ...scene.reverb } : null,
      spatial: clamp01(num(p.spatial)),
    };
  }
  // 지금 곡선을 내 프리셋으로. 이름은 겹치지 않게 번호를 붙인다.
  function newPreset(mine, p, scene) {
    const base = `${scene.name.replace(/( ·)? 내 설정( \d+)?$/, "")} 내 설정`;  // 예전 판의 「… · 내 설정」도 알아본다
    const names = new Set((mine || []).map(m => m.name));
    let name = base, n = 2;
    while (names.has(name)) name = `${base} ${n++}`;
    let key = "user-" + name.toLowerCase().replace(/[^a-z0-9가-힣]+/g, "-").replace(/^-|-$/g, "");
    const keys = new Set(catalog(mine).map(s => s.key));
    while (keys.has(key)) key += "-";
    return { key, name, desc: `${scene.name}에서 만든 내 프리셋`, gains: gainsOf(p, scene).map(round1), reverb: scene.reverb || null };
  }
  // Windows의 장치 이름은 「끝점 (기기)」 꼴이다(예: "헤드폰 (AirPods Pro Stereo)", "스피커 (Realtek(R) Audio)").
  // 사람이 알아보는 것은 괄호 안의 기기라 그것을 크게, 끝점 이름을 작게 쓴다. 블루투스의 음악용·통화용 꼬리말은 뗀다.
  function splitName(name) {
    const s = String(name || "").trim(), at = s.indexOf(" (");
    if (at < 0 || !s.endsWith(")")) return { title: s, sub: "" };
    let title = s.slice(at + 2, -1).trim();
    for (let prev = ""; prev !== title;) { prev = title; title = title.replace(/\s+(stereo|hands-?free|ag audio)$/i, "").trim(); }
    return { title: title || s, sub: s.slice(0, at).trim() };
  }
  // 통화용(Hands-Free) 끝점은 따로 프로필을 두지 않는다. 같은 기기의 음악용 끝점, 없으면 마지막 음악용 장치를 쓴다.
  function musicDevice(device, devices, last) {
    if (!device) return null;
    if (device.kind !== "headset") return device;
    const same = splitName(device.name).title;
    return (devices || []).find(d => d.kind !== "headset" && splitName(d.name).title === same) || last || null;
  }

  // 애니메이션: 한 시계(rAF 타임스탬프)만 쓰고, 진행도는 [0,1]로 자른다.
  const DURATION = 450;
  const easeOutCubic = t => 1 - Math.pow(1 - clamp01(t), 3);
  const progress = (start, now) => clamp01((now - start) / DURATION);
  const lerp = (a, b, t) => a + (b - a) * t;
  const lerpVec = (a, b, t) => a.map((v, i) => lerp(v, b[i], t));
  // 값 하나(수나 수의 배열)를 옮겨 가는 트윈. 도중에 목표가 바뀌면 지금 보이는 값에서 새로 출발한다.
  function tween(value) { return { from: value, to: value, start: null, value }; }
  function retarget(tw, to) { tw.from = tw.value; tw.to = to; tw.start = null; }
  // 프레임 하나. 아직 움직이는 중이면 참.
  function stepTween(tw, now) {
    if (tw.start === null) tw.start = now;
    const p = progress(tw.start, now), e = easeOutCubic(p);
    tw.value = Array.isArray(tw.to) ? lerpVec(tw.from, tw.to, e) : lerp(tw.from, tw.to, e);
    return p < 1;
  }
  // ── pure:end ───────────────────────────────────────────────

  const k = window.kaleidoscope;
  // build.py가 넣는 다리 {gz: base64(gzip(UTF-8 BOM + bridge.ps1(register.ps1을 넣은 것))), key: 내용에서 지은 프로세스 이름}.
  // 자산 크기 때문에 압축해 싣는다. BOM이 있어야 Windows PowerShell 5.1이 한글을 바로 읽는다.
  const BRIDGE = __BRIDGE__;
  // base64(gzip) → 바이트. DecompressionStream이 gzip의 CRC32·길이도 확인한다.
  async function gunzip(b64) {
    if (typeof DecompressionStream !== "function") throw new Error("이 쉘은 압축 풀기(DecompressionStream)를 지원하지 않아요");
    const gz = Uint8Array.from(atob(b64), c => c.charCodeAt(0));
    return new Uint8Array(await new Response(new Blob([gz]).stream().pipeThrough(new DecompressionStream("gzip"))).arrayBuffer());
  }
  // build.py가 넣는 엔진 자산 정보 {asset: 자산 id, sha256: 날 DLL의 SHA-256}. 엔진 없이 빌드한 판(--no-engine)이면 null이고,
  // 그때는 엔진을 따로 설치해야 한다(설치된 엔진에 설정을 보내는 길은 그대로 돈다).
  const ENGINE = __ENGINE__;
  // build.py가 넣는 헤드폰 서라운드 노출 여부(build.py의 SURROUND). false면 화면에 손잡이가 없으므로 엔진에도 끈 값(0)을
  // 보낸다. 기기에 저장된 값(p.spatial)은 지우지 않아 다시 켜면 그대로 돌아온다.
  const SURROUND = __SURROUND__;
  const WORK = "work"; // 다리를 띄울 작업 폴더 권한(파일은 만들지 않는다).
  // macOS 쉘인지. kaleidoscope의 macOS 쉘 웹뷰는 UA에 "Macintosh; Intel Mac OS X 10_15_7"을 싣는다(Apple Silicon도 같다).
  // macOS에서는 PowerShell 다리 대신 macos/의 keq-mac이 다리와 엔진을 함께 맡는다(CONTRACT.md §5). Windows 길은 그대로다.
  const MAC = /Macintosh|Mac OS X/.test(navigator.userAgent);
  // build.py가 넣는 macOS 엔진 자산 정보 {asset: 자산 id, sha256: 날 keq-mac의 SHA-256}. macos/build/keq-mac 없이
  // 빌드한 판이면 null이고, 그 판은 Mac에서 다리를 띄우지 못한다(설치 화면이 그렇게 알린다).
  const MAC_ENGINE = __MAC__;

  let profiles = {}, mine = [], legacy = null;
  let bridge = { phase: "starting", message: "" };
  let engine = null, device = null, devices = [], lastMusic = null;
  let busy = "", note = "";

  const cat = () => catalog(mine);
  const current = () => {
    const d = musicDevice(device, devices, lastMusic);
    const p = profileOf(profiles, d && d.id, startOf(legacy)), scene = layerOf(cat(), p);
    return { d, p, scene };
  };

  // ── 저장 ────────────────────────────────────────────────────
  // 슬라이더를 끄는 동안 고친 값은 곧바로 화면과 소리에 반영하고, 저장소에는 잠깐 모아 쓴다.
  // 쓰는 중에 저장소에서 돌아오는 예전 값이 지금 값을 덮지 않도록, 쓸 것이 남아 있는 동안은 바깥 변경을 미룬다.
  let saveTimer = null, saving = 0, remoteProfiles = null;
  function updateProfile(change) {
    const { d, p } = current();
    if (!d) return;
    const next = change(p);
    profiles = { ...profiles, [d.id]: { ...next, name: d.name, kind: d.kind } };
    changed();
    clearTimeout(saveTimer);
    saveTimer = setTimeout(saveProfiles, 250);
  }
  async function saveProfiles() {
    saveTimer = null;
    saving++;
    try { await k.setStorage("profiles", profiles); }
    catch (e) { fail(e); }
    finally {
      saving--;
      if (!saving && !saveTimer && remoteProfiles) { profiles = remoteProfiles; remoteProfiles = null; changed(); }
    }
  }
  function onRemoteProfiles(v) {
    const value = v && typeof v === "object" ? v : {};
    if (saving || saveTimer) { remoteProfiles = value; return; }
    profiles = value;
    changed();
  }
  async function saveMine(next) { mine = next; changed(); await k.setStorage("my_presets", next); }

  // ── PowerShell 다리 ─────────────────────────────────────────
  // 다리 스크립트는 이 창의 기기 저장소에 .ps1로 두고, process.file_uri로 받은 경로의 파일을 읽어 실행한다.
  //  - 명령줄(-EncodedCommand)은 32767자 한도를 넘는다.
  //  - 환경 변수로 넘기면 환경 변수가 쉘의 실행 허용 기준에 들어 있어 다리가 바뀔 때마다 허용을 다시 묻는다.
  //  - 압축한 base64를 풀어 iex하는 꼴은 백신(AMSI)이 악성으로 막는다(-Command로 시험해 막힘을 확인했다).
  //  - 기기 저장소 경로는 260자(MAX_PATH)를 넘는다(이 PC에서 268자). Windows PowerShell 5.1의 -File은 긴 경로를
  //    열지 못하고 \\?\ 접두사도 받지 않는다. 그래서 짧은 -Command가 .NET의 긴 경로(\\?\)로 파일을 읽어 실행한다.
  // 파일 경로는 인자라 허용 기준에 들지 않는다. UTF-8 BOM을 붙여야 Windows PowerShell 5.1이 한글을 바로 읽는다.
  // 다리 프로세스 이름은 스크립트 내용으로 짓는다. 다리가 바뀌면 예전 프로세스에 붙지 않고 새로 띄운다.
  // macOS의 다리 이름은 keq-mac의 SHA-256에서 짓는다. 새 실행 파일이면 새 프로세스로 띄우고 창 저장소에도 새로 쓴다.
  const KEY = MAC ? `eq-bridge-mac-${MAC_ENGINE ? MAC_ENGINE.sha256.slice(0, 12) : "none"}` : BRIDGE.key;
  const BLOB = MAC ? `bridge/${KEY}/keq-mac` : `bridge/${KEY}.ps1`;
  let handle = null, after = 0, partial = "", starting = null, seq = 0, writeChain = Promise.resolve();
  const waiting = new Map();

  // macOS: file://의 경로가 곧 POSIX 경로다.
  const macPathOf = uri => decodeURIComponent(new URL(uri).pathname);
  // keq-mac을 창 저장소에 둔다. 자산은 이 key(실행 파일의 해시)가 저장소에 아직 없을 때만 받아 SHA-256을 맞춰 보고 쓴다.
  // 지난 판의 다리 파일은 지운다(이미 뜬 프로세스는 파일이 지워져도 계속 돈다).
  async function macBinary() {
    const have = (await k.blobs.list({ prefix: "bridge/" })) || [];
    if (!have.includes(BLOB)) {
      const raw = await verifiedAsset(MAC_ENGINE, "사운드 엔진 다리 파일 검증에 실패해서 띄우지 않았어요");
      await k.blobs.put({ key: BLOB, bytes: raw.buffer });
    }
    for (const old of have) if (old !== BLOB) await k.blobs.remove({ key: old }).catch(() => {});
  }
  function pathOf(uri) {
    const u = new URL(uri), path = decodeURIComponent(u.pathname);
    const local = /^\/[A-Za-z]:/.test(path) ? path.slice(1) : path;
    return (u.host ? "\\\\" + u.host : "") + local.replace(/\//g, "\\");
  }
  async function scriptPath() {
    if (MAC) { await macBinary(); return macPathOf(await k.process.file_uri(BLOB)); }
    const bytes = await gunzip(BRIDGE.gz);
    await k.blobs.put({ key: BLOB, bytes: bytes.buffer });
    return pathOf(await k.process.file_uri(BLOB));
  }
  // 실행 허용이 아직 없으면 쉘이 창 위에 허용 창을 띄우고 start는 답을 받을 때까지 기다린다. 그동안 화면은
  // 「허용을 기다려요」를 보이고, 허용 창을 놓쳤을 때를 위해 같은 start를 다시 불러 허용 창을 다시 띄울 수 있게 한다.
  const SPEC = file => ({ key: KEY, permission: "bridge", cwd_permission: WORK,
    spec: { name: "EQ 사운드 엔진 다리", command: "powershell",
            args: ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", loader(file)], env: {} } });
  // C:\... → \\?\C:\..., \\server\share\... → \\?\UNC\server\share\... (작은따옴표는 PowerShell 식으로 겹친다)
  // macOS: 창 저장소의 파일에는 실행 권한이 없어 sh가 chmod한 뒤 exec한다. 경로는 인자라 허용 기준에 들지 않고,
  // 쉘은 앱·권한·프로그램(sh)·env마다 한 번만 묻는다. "keq"는 sh -c의 $0 자리다.
  const MAC_SPEC = file => ({ key: KEY, permission: "bridge-mac", cwd_permission: WORK,
    spec: { name: "EQ 사운드 엔진 다리", command: "sh", args: ["-c", 'chmod 700 "$1" && exec "$1" bridge', "keq", file], env: {} } });
  function loader(file) {
    const long = file.startsWith("\\\\") ? "\\\\?\\UNC\\" + file.slice(2) : "\\\\?\\" + file;
    return `& ([scriptblock]::Create([IO.File]::ReadAllText('${long.replace(/'/g, "''")}')))`;
  }
  async function launch() {
    const file = await scriptPath();
    if (!handle) setBridge("permission");
    return k.process.start((MAC ? MAC_SPEC : SPEC)(file));
  }
  // start나 attach의 결과를 다리로 삼는다. 이미 다리가 있으면(다시 띄운 허용 창이 먼저 끝난 경우) 그대로 둔다.
  function adopt(r) {
    if (handle) return true;
    // 이미 끝난 프로세스(attach가 돌려준 지난 다리)에는 붙지 않는다. 같은 key로 start하면 쉘이 새로 띄운다.
    if (!r || r.status === "cancelled" || !r.handle || r.exit) return false;
    handle = r.handle; after = r.after ?? 0; partial = "";
    setBridge("ready");
    drain();
    return true;
  }
  function ensureBridge() {
    if (handle) return Promise.resolve(true);
    if (starting) return starting;
    starting = (async () => {
      try {
        if (MAC && !MAC_ENGINE) return false; // 띄울 다리가 없다(setup이 알린다)
        if (adopt(await k.process.attach({ key: KEY }))) return true;
        if (!k.files.grant(WORK)) { setBridge("need_folder"); return false; }
        if (adopt(await launch())) return true;
        setBridge("cancelled");
        return false;
      } catch (e) {
        setBridge("error", messageOf(e));
        return false;
      } finally { starting = null; }
    })();
    return starting;
  }
  async function askAgain() {
    try { if (adopt(await launch())) await request({ op: "status" }); }
    catch (e) { setBridge("error", messageOf(e)); }
  }
  function dropBridge(message) {
    handle = null;
    for (const resolve of waiting.values()) resolve(null);
    waiting.clear();
    setBridge(message ? "error" : "starting", message || "");
  }
  let draining = false, again = false;
  async function drain() {
    if (draining) { again = true; return; }
    draining = true;
    try { do { again = false; await drainOnce(); } while (again); }
    catch (e) { setBridge("error", messageOf(e)); }
    finally { draining = false; }
  }
  async function drainOnce() {
    if (!handle) return;
    const h = handle;
    for (;;) {
      const r = await k.process.read({ handle: h, after, max_bytes: 65536 });
      const bytes = new Uint8Array(r.bytes || []);
      if (bytes.length) {
        partial += new TextDecoder().decode(bytes, { stream: true });
        const lines = partial.split("\n");
        partial = lines.pop();
        for (const l of lines) if (l.trim()) { try { onLine(JSON.parse(l)); } catch { /* 다리의 진단 줄은 넘긴다 */ } }
        after = r.next;
        await k.process.ack({ handle: h, after, checkpoint: {} });
        continue;
      }
      if (r.exit) {
        // 다리가 끝나면 PowerShell이 남긴 마지막 오류 글을 그대로 보인다(원인을 화면에서 바로 알 수 있게).
        const tail = new TextDecoder().decode(new Uint8Array(r.stderr_tail || [])).trim().split(/\r?\n/).slice(-3).join(" ");
        dropBridge(`사운드 엔진 다리가 멈췄어요${r.exit.code != null ? ` (코드 ${r.exit.code})` : ""}.${tail ? " " + tail.slice(-300) : ""}`);
      }
      return;
    }
  }
  function onLine(msg) {
    // 알림은 장치만 싣는다. 등록이 바뀌었다면 엔진 상태(설치·판)도 바뀌었을 수 있으니 status로 다시 묻는다
    // (엔진을 바깥에서 설치·제거해도 화면이 따라온다).
    if (msg.event === "device") { setDevice(msg.device); changed(); void request({ op: "status" }); return; }
    if (msg.status === "ok") {
      if ("engine" in msg) engine = msg.engine;
      if ("device" in msg) setDevice(msg.device);
      if (Array.isArray(msg.devices)) devices = msg.devices;
      if (bridge.phase === "error") setBridge("ready");
    } else if (msg.status === "error") note = `엔진이 답했어요: ${msg.message || "알 수 없는 오류"}`;
    const resolve = waiting.get(msg.id);
    waiting.delete(msg.id);
    if (resolve) resolve(msg);
    changed();
  }
  function setDevice(d) {
    const before = current().d;
    device = d || null;
    if (device && device.kind !== "headset") lastMusic = device;
    const now = current().d;
    if ((before && before.id) !== (now && now.id)) schedule(); // 장치가 바뀌면 그 장치의 프로필로 곧바로.
  }
  // 요청 한 줄은 DLL을 실으면 1MB를 넘는다. 64KiB씩 나눠 쓰고, 다른 요청과 섞이지 않게 줄 단위로 줄 세운다.
  // 다리가 받는 글은 ASCII만 오도록 \uXXXX로 적는다(콘솔 입력 인코딩에 기대지 않는다).
  const ascii = s => s.replace(/[\u007f-\uffff]/g, c => "\\u" + c.charCodeAt(0).toString(16).padStart(4, "0"));
  async function request(body) {
    if (!(await ensureBridge())) return null;
    const id = ++seq, h = handle;
    const answer = new Promise(resolve => waiting.set(id, resolve));
    const bytes = new TextEncoder().encode(ascii(JSON.stringify({ id, ...body })) + "\n");
    writeChain = writeChain.then(async () => {
      for (let o = 0; o < bytes.length; o += 65536)
        await k.process.write({ handle: h, bytes: bytes.subarray(o, o + 65536), checkpoint: {} });
    }).catch(e => { waiting.delete(id); fail(e); });
    return answer;
  }

  // ── 엔진에 반영 ─────────────────────────────────────────────
  // 다리의 답마다 화면을 다시 내보내고(changed) 그때마다 schedule이 불리므로, 엔진이 이미 가진 설정이면 다시 쓰지 않는다.
  // 그러지 않으면 apply의 답이 또 apply를 불러 .cfg를 끝없이 다시 쓴다. 열쇠에는 보낼 내용과 함께 다리(handle)·엔진·등록
  // 상태를 넣는다. 다리를 다시 띄웠거나 엔진을 다시 설치·연결했으면(.cfg가 지워졌을 수 있다) 같은 설정이라도 다시 보낸다.
  let dirty = false, applying = false, applyTimer = null, applied = "", retries = 0;
  function schedule() {
    dirty = true;
    clearTimeout(applyTimer);
    applyTimer = setTimeout(flush, 40);
  }
  async function flush() {
    if (applying) return;
    applying = true;
    try {
      while (dirty) {
        dirty = false;
        const { d, p, scene } = current();
        if (!handle || !engine || !engine.installed || !d) continue;
        const body = { op: "apply", device_id: d.id, config: renderConfig(SURROUND ? p : { ...p, spatial: 0 }, scene) };
        const key = JSON.stringify([handle, engine, d.registered, body]);
        if (key === applied) continue;
        applied = key;
        const r = await request(body);
        if (r && r.status === "ok") {
          if (retries && note.startsWith("엔진이 답했어요")) note = ""; // 다시 보내서 들어갔으면 오류 안내도 거둔다
          retries = 0;
          continue;
        }
        // 엔진이 아직 이 설정을 갖지 않았다. 같은 것을 곧바로 되풀이하지 않고 0.5, 1, 2초 뒤에 다시 보낸다.
        applied = "";
        if (retries < 3) setTimeout(schedule, 500 * 2 ** retries++);
      }
    } catch (e) { fail(e); }
    finally { applying = false; }
  }

  // ── 화면에 내보낼 값 ─────────────────────────────────────────
  const KIND = { headphones: "헤드폰", headset: "통화용 헤드셋", speakers: "스피커", other: "출력 장치" };
  const messageOf = e => (e && e.message ? e.message : String(e));
  function fail(e) { note = `문제가 생겼어요: ${messageOf(e)}`; changed(); }
  function setBridge(phase, message = "") { bridge = { phase, message }; changed(); }

  // 처음 설정은 지금 해야 할 일 하나만 보인다.
  function setup(d) {
    if (MAC) return setupMac(d);
    if (bridge.phase === "need_folder") return { key: "folder", title: "시작하기",
      text: "EQ가 Windows 소리 설정과 이야기하려면 작업 폴더 하나를 정해야 해요. 아래에서 허용하면 끝이에요. 그 폴더에 파일을 만들지는 않아요." };
    if (bridge.phase === "cancelled") return { key: "allow", title: "실행 허용이 필요해요",
      text: "EQ는 PowerShell로 사운드 엔진과 이야기해요. 「다시 시도」를 누르고 실행을 허용해 주세요.", button: "다시 시도", icon: "refresh-cw" };
    if (bridge.phase === "permission") return { key: "permission", title: "PowerShell 실행 허용을 기다려요",
      text: "이 창에 「이 앱이 이 PC에서 powershell을(를) 실행하게 할까요?」가 떠 있으면 「허용」을 눌러 주세요. 처음 한 번만 물어요. 허용 창이 보이지 않으면 아래 단추로 다시 띄워요.",
      button: "허용 창 다시 띄우기", icon: "refresh-cw" };
    if (bridge.phase === "error") return { key: "allow", title: "사운드 엔진과 연결하지 못했어요",
      text: bridge.message || "알 수 없는 오류", button: "다시 시도", icon: "refresh-cw" };
    if (bridge.phase !== "ready" || !engine) return { key: "loading", title: "사운드 엔진 상태를 읽는 중…",
      text: "처음 한 번은 3~5초쯤 걸려요." };
    if (busy === "install") return { key: "busy", title: "사운드 엔진 설치 중…",
      text: "Windows 확인 창에서 「예」를 눌러 주세요. 소리가 1초쯤 끊겼다가 돌아와요." };
    // 엔진 없이 빌드한 판: 설치·연결은 엔진을 따로 설치해서 한다. 설치된 엔진이 있으면 나머지는 그대로 돈다.
    if (!ENGINE && !engine.installed) return { key: "manual", title: "사운드 엔진이 필요해요",
      text: "이 판에는 사운드 엔진이 들어 있지 않아 따로 설치해야 해요. 설치하면 이 화면이 저절로 알아채요." };
    if (!ENGINE && d && !d.registered) return { key: "manual", title: `「${splitName(d.name).title}」은(는) 아직 연결 전이에요`,
      text: "이 판에는 사운드 엔진이 들어 있지 않아, 이 장치에 연결하려면 엔진을 따로 설치(또는 다시 설치)해야 해요. 연결되면 이 화면이 저절로 알아채요." };
    if (!engine.installed) return { key: "install", title: "사운드 엔진 설치",
      text: "Windows의 모든 소리에 EQ와 공간감을 걸려면 사운드 엔진을 한 번 설치해야 해요. 관리자 확인 창이 한 번 뜨고, 소리가 1초쯤 끊겼다가 돌아와요. 재부팅은 필요 없어요.",
      button: "사운드 엔진 설치", icon: "download" };
    if (d && !d.registered) return { key: "connect", title: `「${splitName(d.name).title}」에 연결`,
      text: "이 장치에는 아직 사운드 엔진이 붙어 있지 않아요. 관리자 확인 한 번이면 연결돼요.", button: "이 장치에 연결", icon: "power" };
    return null;
  }
  // macOS의 처음 설정. 관리자 비밀번호, 드라이버, 재시동이 없고, 엔진이 처음 소리를 받을 때 macOS가
  // 「시스템 오디오 녹음」을 한 번 묻는다(엔진이 바뀌면 다시 묻는다: ad-hoc 서명이라 허용이 실행 파일에 묶인다).
  function setupMac(d) {
    if (!MAC_ENGINE) return { key: "manual", title: "이 판에는 Mac용 사운드 엔진이 없어요",
      text: "이 판은 Mac용 사운드 엔진(keq-mac) 없이 만들어져 Mac에서는 EQ를 걸 수 없어요. Mac용 엔진이 들어 있는 판으로 바꿔 주세요." };
    if (bridge.phase === "need_folder") return { key: "folder", title: "시작하기",
      text: "EQ가 Mac의 소리 설정과 이야기하려면 작업 폴더 하나를 정해야 해요. 아래에서 허용하면 끝이에요. 그 폴더에 파일을 만들지는 않아요." };
    if (bridge.phase === "cancelled") return { key: "allow", title: "실행 허용이 필요해요",
      text: "EQ는 sh로 사운드 엔진 다리(keq-mac)를 띄워 이야기해요. 「다시 시도」를 누르고 실행을 허용해 주세요.", button: "다시 시도", icon: "refresh-cw" };
    if (bridge.phase === "permission") return { key: "permission", title: "sh 실행 허용을 기다려요",
      text: "이 창에 「이 앱이 이 PC에서 sh 프로그램을 실행하게 할까요?」가 떠 있으면 「허용」을 눌러 주세요. 처음 한 번만 물어요. 허용 창이 보이지 않으면 아래 단추로 다시 띄워요.",
      button: "허용 창 다시 띄우기", icon: "refresh-cw" };
    if (bridge.phase === "error") return { key: "allow", title: "사운드 엔진과 연결하지 못했어요",
      text: bridge.message || "알 수 없는 오류", button: "다시 시도", icon: "refresh-cw" };
    if (bridge.phase !== "ready" || !engine) return { key: "loading", title: "사운드 엔진 상태를 읽는 중…", text: "곧 끝나요." };
    if (engine.supported === false) return { key: "manual", title: "이 macOS에서는 쓸 수 없어요",
      text: "Mac의 모든 소리에 EQ를 걸려면 macOS 14.2 이상(Apple Silicon)이 필요해요. macOS를 업데이트한 뒤 이 창을 다시 열어 주세요." };
    if (busy === "install") return { key: "busy", title: "사운드 엔진 설치 중…",
      text: "관리자 비밀번호 없이 내 계정에 설치해요. 곧 macOS가 「시스템 오디오 녹음」을 허용할지 물으면 「허용」을 눌러 주세요." };
    if (!engine.installed) return { key: "install", title: "사운드 엔진 설치",
      text: "Mac의 모든 소리에 EQ와 공간감을 걸려면 사운드 엔진을 한 번 설치해야 해요. 관리자 비밀번호나 드라이버 없이 내 계정에 설치하고, 처음 한 번 macOS가 「시스템 오디오 녹음」 허용을 물어요. 재시동은 필요 없어요.",
      button: "사운드 엔진 설치", icon: "download" };
    if (engine.current === false) return { key: "install", title: "사운드 엔진 업데이트",
      text: "새 사운드 엔진이 있어요. 업데이트하면 macOS가 「시스템 오디오 녹음」 허용을 한 번 더 물어요(엔진 파일이 바뀌면 macOS가 다시 확인해요).",
      button: "사운드 엔진 업데이트", icon: "download" };
    if (engine.permission === "pending") return { key: "mac_permission", title: "시스템 오디오 녹음 허용을 기다려요",
      text: "macOS가 「Kaleidoscope EQ」의 시스템 오디오 녹음을 허용할지 묻고 있으면 「허용」을 눌러 주세요. 물음이 보이지 않으면 시스템 설정 > 개인정보 보호 및 보안 > 시스템 오디오 녹음에서 keq-mac(Kaleidoscope EQ)을 켜 주세요. 받은 소리는 곧바로 스피커나 헤드폰으로 내보내고 저장하지 않아요." };
    if (engine.permission === "denied") return { key: "mac_permission", title: "시스템 오디오 녹음이 꺼져 있어요",
      text: "시스템 설정 > 개인정보 보호 및 보안 > 시스템 오디오 녹음에서 keq-mac(Kaleidoscope EQ)을 켜 주세요. 켜면 몇 초 안에 저절로 적용돼요. 그동안 소리는 원래대로 나와요." };
    if (engine.state === "stopped" || engine.state === "error" || engine.state === "unsupported") return { key: "install", title: "사운드 엔진이 멈춰 있어요",
      text: `${engine.message ? `엔진이 알린 문제: ${engine.message}. ` : ""}다시 설치하면 엔진을 새로 띄워요. 그동안 소리는 원래대로 나와요.`,
      button: "다시 설치", icon: "refresh-cw" };
    return null;
  }
  // 처음 설정의 진행. 단계마다 끝났는지는 이미 가진 상태(폴더 허용, 다리, 엔진, 장치 등록)에서 파생한다.
  function progressOf(d) {
    const steps = MAC ? [
      ["작업 폴더 허용", !!k.files.grant(WORK)],
      ["sh 실행 허용", !!handle],
      ["엔진 상태 읽기", !!engine],
      ["사운드 엔진 설치", !!(engine && engine.installed && engine.current !== false)],
      ["시스템 오디오 녹음 허용", !!(engine && engine.permission === "granted")],
    ] : [
      ["작업 폴더 허용", !!k.files.grant(WORK)],
      ["PowerShell 실행 허용", !!handle],
      ["엔진 상태 읽기", !!engine],
      ["사운드 엔진 설치", !!(engine && engine.installed)],
      ["이 장치에 연결", !!(d && d.registered)],
    ];
    const at = steps.findIndex(([, done]) => !done);
    const n = at < 0 ? steps.length : at;
    return { percent: Math.round((n / steps.length) * 100),
             label: at < 0 ? "" : `${n + 1}/${steps.length}단계: ${steps[at][0]}` };
  }
  function engineState(d) {
    if (MAC) return engineStateMac(d);
    if (bridge.phase !== "ready" || !engine) return "사운드 엔진 확인 중…";
    if (!engine.installed) return "사운드 엔진이 필요해요";
    if (!d) return "출력 장치를 찾는 중…";
    if (!d.registered) return "이 장치에는 아직 연결 전";
    return d.active ? "적용 중" : "소리가 나오면 적용돼요";
  }
  // macOS: 탭 하나가 모든 장치를 덮어 「연결 전」이 없다. 엔진은 소리가 없어도 돌아서 적용 중이 아니면 시작·전환 중이다.
  function engineStateMac(d) {
    if (bridge.phase !== "ready" || !engine) return "사운드 엔진 확인 중…";
    if (!engine.installed) return "사운드 엔진이 필요해요";
    if (!d) return "출력 장치를 찾는 중…";
    if (engine.permission === "pending") return "시스템 오디오 녹음 허용을 기다려요";
    if (engine.permission === "denied") return "시스템 오디오 녹음이 꺼져 있어요";
    if (engine.state === "stopped" || engine.state === "error" || engine.state === "unsupported") return "사운드 엔진이 멈춰 있어요";
    return d.active ? "적용 중" : "사운드 엔진 시작 중…";
  }
  // 축마다 같은 꼴의 화면 값: <field>s(칩), <field>_amount_show/_amount(정도), <field>_desc(설명).
  function axisOutputs(p) {
    const onMine = sceneOf(cat(), p.scene).mine, out = {};
    for (const a of AXES) {
      const x = onMine ? null : pickOf(p, a);
      out[a.field + "s"] = a.list.map(y => ({ key: y.key, name: y.name, on: !!x && y.key === x.key }));
      out[a.field + "_amount_show"] = !!x;
      out[a.field + "_amount"] = x ? Math.round(amountOf(p, x.key) * 100) : 100;
      out[a.field + "_desc"] = x ? `${x.desc}. 다시 누르면 꺼져요` : "공간에 겹쳐 쓸 수 있어요";
    }
    return out;
  }
  function publish() {
    const { d, p, scene } = current();
    const s = setup(d);
    const on = p.enabled !== false;
    const custom = isCustom(p);
    const label = labelOf(p, scene);
    const gains = gainsOf(p, scene);
    const preamp = preampOf(p, scene);
    const callMode = device && device.kind === "headset";
    const name = splitName(d && d.name);
    const others = Object.entries(profiles).filter(([id]) => !d || id !== d.id)
      .map(([id, q]) => ({ value: id, label: `${q.name ? splitName(q.name).title : "다른 기기"}: ${labelOf(profileOf(profiles, id), layerOf(cat(), profileOf(profiles, id)))}` }));
    k.publish({
      device_title: d ? name.title : callMode ? splitName(device.name).title : "출력 장치 없음",
      device_meta: d ? [name.sub || KIND[d.kind] || KIND.other, on ? label : "꺼짐(원래 소리)", engineState(d)].join(", ")
        : callMode ? "통화용 연결(핸즈프리)에서는 EQ가 쉬어요" : engineState(null),
      no_device: !d,
      power: on,
      setup_show: !!s,
      setup_percent: progressOf(d).percent,
      setup_step: progressOf(d).label,
      setup_title: s ? s.title : "",
      setup_text: s ? s.text : "",
      setup_button: s && s.button ? s.button : "",
      setup_icon: s && s.icon ? s.icon : "download",
      setup_has_button: !!(s && s.button),
      need_folder: !!(s && s.key === "folder"),
      note_show: !!note,
      note,
      scenes: SCENES.map(x => ({ key: x.key, name: x.name, on: x.key === p.scene })),
      ...axisOutputs(p),
      space_amount_show: p.scene !== SCENES[0].key && !sceneOf(cat(), p.scene).mine,
      space_amount: Math.round(amountOf(p, p.scene) * 100),
      scene_desc: (s0 => (s0.mine ? "" : s0.desc + (s0.reverb ? `, 잔향 ${s0.reverb.decay.toFixed(1)}초` : "")
        + (s0.key !== SCENES[0].key ? ". 다시 누르면 꺼져요" : "")))(sceneOf(cat(), p.scene)),
      fresh_show: !!d && !profiles[d.id] && others.length > 0,
      fresh_text: startOf(legacy) === FRESH ? "처음 쓰는 기기라 원음으로 시작했어요."
        : `처음 쓰는 기기라 이전 EQ 설정(${layerOf(cat(), startOf(legacy)).name})으로 시작했어요.`,
      copy_options: others,
      copy_value: null, // 고르면 곧바로 가져오고 고르기는 비운다.
      ...(SURROUND ? {
        spatial: SPATIAL.reduce((a, b) => (Math.abs(b.amount - num(p.spatial)) < Math.abs(a.amount - num(p.spatial)) ? b : a)).value,
        spatial_options: SPATIAL.map(({ value, label: l }) => ({ value, label: l })),
        spatial_hint: d && d.kind === "speakers" ? "헤드폰용이에요. 스피커에서는 효과가 거의 없어요."
          : "호를 끌어 소리가 머리를 감싸는 정도를 정해요. 넓힐수록 스피커로 듣는 것처럼 머리 밖으로 퍼져요.",
      } : {}),
      tones: TONES.map(t => ({ key: t.key, label: t.label, hz: t.hz, value: toneOf(p, t) })),
      bands: FREQS.map((f, i) => ({ key: String(i), index: i, label: BAND_NAMES[i],
                                    hz: f >= 1000 ? `${f / 1000} kHz` : `${f} Hz`, gain: round1(gains[i]) })),
      custom,
      not_custom: !custom,
      custom_meta: custom ? "고친 값은 이 기기에 저절로 저장돼요" : "",
      auto_preamp: p.auto_preamp !== false,
      manual_preamp: p.auto_preamp === false,
      preamp: num(p.preamp),
      preamp_text: `지금 ${preamp > 0 ? "+" : ""}${preamp.toFixed(1)} dB${p.auto_preamp === false ? "" : " (자동)"}`,
      mine_show: mine.length > 0,
      mine: mine.map(m => ({ key: m.key, title: m.name, subtitle: m.desc || "" })),
      mine_selected: scene.mine ? scene.key : null,
      engine_show: !!(engine && engine.installed),
      engine_text: engine && engine.installed ? `사운드 엔진 ${engine.version || ""}`.trim() : "",
    });
  }

  // ── 그림 ────────────────────────────────────────────────────
  // EQ 곡선과 서라운드 무대는 격리 프레임(sandbox, graph.js)이 그린다. 쉘의 canvas 블록은 어두운 바탕과 반투명
  // 테두리를 깔아 검은 선이 비치기 때문이다. 그림에 필요한 값은 창 상태 "view"로 넘기고(바뀔 때만),
  // 무대를 끄는 손은 창 상태 "stage_input"으로 돌아온다.
  let lastView = "";
  function retargetAll() {
    const { p, scene } = current();
    const view = { gains: gainsOf(p, scene).map(round1), power: p.enabled !== false ? 1 : 0, spatial: clamp01(num(p.spatial)) };
    const text = JSON.stringify(view);
    if (text === lastView) return;
    lastView = text;
    k.setState("view", view);
  }
  // 그림에서 돌아오는 손: 서라운드 무대(stage_input)와 곡선 위 밴드 끌기(curve_input). seq로 새 것만 받는다.
  let lastStage = 0, lastCurve = 0;
  k.onStateChange(st => {
    const e = st && st.stage_input;
    if (e && typeof e === "object" && e.seq !== lastStage) {
      lastStage = e.seq;
      updateProfile(q => ({ ...q, spatial: clamp01(num(e.v)) }));
    }
    const b = st && st.curve_input;
    if (b && typeof b === "object" && b.seq !== lastCurve && Number.isInteger(b.band) && b.band >= 0 && b.band < FREQS.length) {
      lastCurve = b.seq;
      updateProfile(q => setBand(q, layerOf(cat(), q), b.band, b.gain));
    }
  });

  // ── 동작 ────────────────────────────────────────────────────
  const keyOf = c => (c && c.item && c.item.key !== undefined ? c.item.key : c && c.key);
  const changed = () => { publish(); retargetAll(); schedule(); };
  k.handle("power", c => updateProfile(p => ({ ...p, enabled: !!c.event })));
  // 칩은 토글이다. 켜진 것을 다시 누르면 끈다: 공간은 원음(Original)으로, 장르와 내 프리셋은 없음으로.
  const OFF = SCENES[0].key;
  const toggled = (current, key) => (current === key ? OFF : key);
  k.handle("pick_scene", c => {
    const key = keyOf(c) ?? c.event;
    if (SCENES.some(s => s.key === key)) updateProfile(p => ({ ...p, scene: toggled(p.scene, key) }));
  });
  // 정도 손잡이(0~100%). 지금 고른 공간·장르의 값만 바꾼다.
  const setAmount = (p, key, v) => ({ ...p, amounts: { ...(p.amounts || {}), [key]: clamp01(num(v) / 100) } });
  k.handle("set_space_amount", c => updateProfile(p => (sceneOf(cat(), p.scene).mine ? p : setAmount(p, p.scene, c.event))));
  const noAxes = Object.fromEntries(AXES.map(a => [a.field, null]));
  // 축 고르기는 토글: 같은 것을 다시 누르면 끈다. 내 프리셋을 듣는 중이면 원래 소리(Original) 위에 얹는다.
  function pickAxis(axis, key) {
    if (!axis.list.some(x => x.key === key)) return;
    updateProfile(p => ({ ...p, scene: sceneOf(cat(), p.scene).mine ? SCENES[0].key : p.scene,
                          [axis.field]: p[axis.field] === key ? null : key }));
  }
  function setAxisAmount(axis, v) {
    updateProfile(p => (pickOf(p, axis) && !sceneOf(cat(), p.scene).mine ? setAmount(p, p[axis.field], v) : p));
  }
  for (const axis of AXES) {
    k.handle(`pick_${axis.field}`, c => pickAxis(axis, keyOf(c) ?? c.event));
    k.handle(`set_${axis.field}_amount`, c => setAxisAmount(axis, c.event));
  }
  k.handle("set_band", c => {
    const i = Number(c.item && c.item.index);
    updateProfile(p => setBand(p, layerOf(cat(), p), i, c.event));
  });
  k.handle("set_tone", c => {
    const tone = TONES.find(t => t.key === keyOf(c));
    if (tone) updateProfile(p => setTone(p, tone, c.event));
  });
  if (SURROUND) k.handle("set_spatial", c => {
    const s = SPATIAL.find(x => x.value === String(c.event));
    if (s) updateProfile(p => ({ ...p, spatial: s.amount }));
  });
  k.handle("reset_scene", () => updateProfile(resetScene));
  // 이 기기의 모든 값을 처음으로(FRESH). 내 프리셋과 다른 기기는 건드리지 않는다. 이름·종류는 updateProfile이 다시 붙인다.
  k.handle("reset_all", () => {
    updateProfile(() => ({ ...FRESH }));
  });
  k.handle("set_auto_preamp", c => updateProfile(p => ({ ...p, auto_preamp: !!c.event, preamp: c.event ? p.preamp : preampOf(p, layerOf(cat(), p)) })));
  k.handle("set_preamp", c => updateProfile(p => ({ ...p, preamp: clamp(num(c.event), -24, 6) })));
  k.handle("save_mine", async () => {
    const { d, p, scene } = current();
    if (!d) return;
    const preset = newPreset(mine, p, scene);
    await saveMine(mine.concat([preset]));
    updateProfile(q => ({ ...q, scene: preset.key, ...noAxes }));
  });
  k.handle("pick_mine", c => { const key = c.event; if (mine.some(m => m.key === key)) updateProfile(p => ({ ...p, scene: toggled(p.scene, key), ...noAxes })); });
  // 지운 프리셋을 쓰던 기기는 그 곡선을 그대로 두고 싶지만, 원본이 사라지면 고친 몫의 기준도 사라진다.
  // 그래서 지우기 전에 그런 프로필을 원래 공간(Original) + 같은 곡선의 고친 몫으로 옮긴다.
  k.handle("delete_mine", async c => {
    const key = c.event;
    if (!mine.some(m => m.key === key)) return;
    const moved = Object.fromEntries(Object.entries(profiles).map(([id, q]) => {
      if (q.scene !== key) return [id, q];
      const cur = gainsOf(profileOf(profiles, id), layerOf(cat(), profileOf(profiles, id)));
      const edits = { ...(q.edits || {}) }; delete edits[key];
      return [id, withEdits({ ...q, scene: SCENES[0].key, ...noAxes, edits }, cur.map((v, i) => v - SCENES[0].gains[i]))];
    }));
    profiles = moved;
    await saveMine(mine.filter(m => m.key !== key));
    clearTimeout(saveTimer); await saveProfiles();
  });
  k.handle("copy_from", c => { if (profiles[c.event]) updateProfile(() => profileOf(profiles, c.event)); });
  // 처음 설정 카드의 단추 하나. 지금 단계가 할 일을 정한다.
  k.handle("setup", async () => {
    const { d } = current(), s = setup(d);
    if (!s) return;
    if (s.key === "allow") await restart();
    else if (s.key === "permission") await askAgain();
    else if (s.key === "install") await install();
    else if (s.key === "connect") await install([d.id]);
  });
  k.handle("reinstall", () => install()); // 엔진 없는 판에는 이 단추가 없다(build.py).
  k.handle("uninstall", async () => {
    busy = "uninstall"; note = ""; changed();
    try {
      const r = await request({ op: "uninstall" });
      if (r && r.elevated === "cancelled") note = "제거를 취소했어요.";
      await request({ op: "status" });
    } finally { busy = ""; changed(); }
  });
  k.handle("dismiss_note", () => { note = ""; changed(); });
  k.handle("recheck", () => restart());

  // 자산은 gzip한 DLL의 base64를 담은 JS 문자열 리터럴 한 문장이다(크기 때문, build.py). 프레임에서 풀고(Chromium의
  // DecompressionStream이 gzip의 CRC32·길이도 확인한다), build.py가 적은 날 DLL의 SHA-256과 맞을 때만 넘긴다.
  // 격리 프레임(sandbox="allow-scripts" srcdoc)은 출처가 null이어도 부모(http://tauri.localhost)의 보안 문맥을 이어받아
  // crypto.subtle이 있다(같은 꼴의 프레임을 Edge에서 띄워 isSecureContext·DecompressionStream·digest를 확인했다).
  // 다리에는 계약대로 날 DLL의 base64(dll_b64)를 보낸다.
  // macOS의 eq-mac 자산(keq-mac)도 같은 꼴이다(macBinary).
  async function verifiedAsset(info, failure) {
    const text = new TextDecoder().decode(await k.assets.bytes(info.asset)).trim();
    const raw = await gunzip(JSON.parse(text.replace(/;$/, "")));
    const hash = [...new Uint8Array(await crypto.subtle.digest("SHA-256", raw))].map(b => b.toString(16).padStart(2, "0")).join("");
    if (hash !== info.sha256) throw new Error(failure);
    return raw;
  }
  async function engineDll() {
    if (!ENGINE) throw new Error("이 판에는 사운드 엔진이 들어 있지 않아요");
    const raw = await verifiedAsset(ENGINE, "엔진 파일 검증에 실패해서 설치하지 않았어요");
    let bin = "";
    for (let i = 0; i < raw.length; i += 32768) bin += String.fromCharCode(...raw.subarray(i, i + 32768));
    return btoa(bin);
  }
  async function install(endpoints) {
    busy = "install"; note = ""; changed();
    try {
      // macOS: 다리(keq-mac)가 자기 실행 파일을 설치하므로 실을 것이 없고, 장치마다 연결하는 단계도 없다.
      const r = await request(MAC ? { op: "install" } : { op: "install", dll_b64: await engineDll(), ...(endpoints ? { endpoints } : {}) });
      if (r && r.elevated === "cancelled") note = "설치를 취소했어요. 준비되면 다시 눌러 주세요.";
      await request({ op: "status" });
      schedule();
    } catch (e) { fail(e); }
    finally { busy = ""; changed(); }
  }
  async function restart() {
    if (handle) { const h = handle; dropBridge(); await k.process.stop({ handle: h }).catch(() => {}); }
    setBridge("starting");
    if (await ensureBridge()) await request({ op: "status" });
  }

  // ── 연결 ────────────────────────────────────────────────────
  k.onStorageChange("profiles", onRemoteProfiles);
  k.onStorageChange("my_presets", v => { mine = Array.isArray(v) ? v : []; changed(); });
  k.onStorageChange("eq", v => { legacy = v && typeof v === "object" ? v : null; changed(); });
  k.process.onReady(h => { if (h === handle) drain(); });
  k.files.on_grant(WORK, name => { if (name && !handle) restart(); });
  // 다리는 기본 장치나 그 상태(등록, 적용, 이름)가 바뀔 때만 알린다. 처음 상태는 status로 묻는다(C# 컴파일로 3~5초).
  publish();
  restart();
})();
