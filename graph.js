// EQ 곡선과 서라운드 무대를 그리는 격리 프레임(sandbox) 스크립트. build.py가 MODE("curve" | "stage")와
// client.js의 pure 구역(필터 응답·트윈 계산)을 앞에 붙여 HTML 자산 둘로 만든다. 계산식은 client.js 한 곳에만 있다.
//
// 왜 sandbox인가: 쉘의 canvas 블록은 테마와 상관없이 어두운 바탕을 깔고 그 위에 반투명 테두리를 그려
// 가장자리에 검은 선이 비친다. sandbox 블록은 테두리가 없고 바탕이 쉘 패널 색(--ks-color-surface-solid)이라
// 투명한 캔버스를 그 위에 그리면 kaleidoscope와 한 몸처럼 보인다.
//
// 주고받기(창 상태): client가 "view" = {gains, power, spatial}을 쓰면 여기서 읽어 0.45초 감속으로 움직인다.
// 무대를 끄는 손은 "stage_input" = {v, seq}로 돌려준다(client가 그 기기 설정에 저장하고 view로 다시 보낸다).
(() => {
  const k = window.kaleidoscope;
  const canvas = document.querySelector("canvas");
  // kaleidoscope 쉘의 색 토큰(ui/src/styles/foundation.css)에 맞춘다: 선 --ks-color-accent, 글 --ks-color-text,
  // 눈금·둘레 --ks-color-border의 보라빛 회색. 바탕은 칠하지 않는다(쉘 패널 색이 그대로 보인다).
  const PALETTE = {
    light: { head: [255, 255, 255, 1], ring: [73, 57, 89, 0.16], on: [111, 86, 154, 1], off: [150, 140, 160, 0.9], fill: 0.24, grid: [73, 57, 89, 0.09], zero: [73, 57, 89, 0.22], text: [48, 42, 54, 0.66], dot: [255, 255, 255, 1] },
    dark: { head: [62, 52, 70, 1], ring: [230, 214, 240, 0.16], on: [198, 174, 227, 1], off: [160, 150, 172, 0.8], fill: 0.28, grid: [230, 214, 240, 0.08], zero: [230, 214, 240, 0.2], text: [236, 227, 242, 0.62], dot: [36, 27, 44, 1] },
  };
  const rgba = c => `rgba(${Math.round(c[0])},${Math.round(c[1])},${Math.round(c[2])},${c[3].toFixed(3)})`;
  const media = window.matchMedia ? window.matchMedia("(prefers-color-scheme: dark)") : null;
  const dark = () => !!(media && media.matches);
  // 트윈: 밴드 이득, 테마(0 라이트–1 다크), 켜짐(0–1), 공간감. 모두 같은 시계와 같은 곡선으로 움직인다.
  const shown = { gains: tween(zeros()), theme: tween(dark() ? 1 : 0), power: tween(1), spatial: tween(0) };
  let dragging = null, seq = 0, raf = 0;
  const mixed = key => PALETTE.light[key].map((v, i) => lerp(v, PALETTE.dark[key][i], shown.theme.value));

  function apply(view) {
    if (!view || typeof view !== "object") return;
    const g = vec(view.gains), on = view.power ? 1 : 0, sp = clamp01(num(view.spatial));
    if (g.some((v, i) => v !== shown.gains.to[i])) retarget(shown.gains, g);
    if (on !== shown.power.to) retarget(shown.power, on);
    if (sp !== shown.spatial.to && dragging === null) retarget(shown.spatial, sp);
    kick();
  }
  function retheme() { const th = dark() ? 1 : 0; if (th !== shown.theme.to) retarget(shown.theme, th); kick(); }
  function kick() { if (!raf) raf = requestAnimationFrame(frame); }
  function frame(now) {
    raf = 0;
    let moving = false;
    for (const tw of Object.values(shown)) moving = stepTween(tw, now) || moving;
    render();
    if (moving) kick();
  }
  function size() {
    const d = Math.min(2, window.devicePixelRatio || 1);
    const W = Math.max(1, Math.round(canvas.clientWidth * d)), H = Math.max(1, Math.round(canvas.clientHeight * d));
    if (canvas.width !== W) canvas.width = W;
    if (canvas.height !== H) canvas.height = H;
    return { W, H, d };
  }
  const render = () => (MODE === "stage" ? drawStage() : drawCurve());

  // 곡선의 축(d배율 px). 그리기와 끌기가 같은 축을 쓴다.
  const RANGE = 15;
  function axes(W, H, d) {
    const padL = 30 * d, padR = 8 * d, padT = 8 * d, padB = 18 * d;
    const w = W - padL - padR, h = H - padT - padB;
    return { padL, padT, w, h,
             x: f => padL + w * Math.log(f / 20) / Math.log(1000),
             y: db => padT + h * (1 - (clamp(db, -RANGE, RANGE) + RANGE) / (2 * RANGE)),
             db: py => clamp((1 - (py - padT) / h) * 2 * RANGE - RANGE, -RANGE, RANGE) };
  }
  // 끄는 중인 밴드 {i, gain}. 손을 바로 따라가도록 그 밴드만 트윈을 거치지 않고 덮어 그린다.
  let bandDrag = null;
  const shownGains = () => (bandDrag ? shown.gains.value.map((g, i) => (i === bandDrag.i ? bandDrag.gain : g)) : shown.gains.value);
  function drawCurve() {
    const { W, H, d } = size(), g = canvas.getContext("2d");
    const pw = shown.power.value;
    const lit = mixed("on"), line = mixed("off").map((v, i) => lerp(v, lit[i], pw));
    const fillA = lerp(PALETTE.light.fill, PALETTE.dark.fill, shown.theme.value) * lerp(0.35, 1, pw);
    const { padL, padT, w, h, x, y } = axes(W, H, d);
    g.clearRect(0, 0, W, H);
    g.font = `${10 * d}px system-ui, sans-serif`;
    g.lineWidth = d;
    g.fillStyle = rgba(mixed("text"));
    g.textAlign = "right"; g.textBaseline = "middle";
    for (const db of [-12, -6, 0, 6, 12]) {
      g.strokeStyle = rgba(mixed(db === 0 ? "zero" : "grid"));
      g.beginPath(); g.moveTo(padL, y(db)); g.lineTo(padL + w, y(db)); g.stroke();
      g.fillText(`${db > 0 ? "+" : ""}${db}`, padL - 6 * d, y(db));
    }
    g.textAlign = "center"; g.textBaseline = "top";
    g.strokeStyle = rgba(mixed("grid"));
    for (const f of FREQS) {
      g.beginPath(); g.moveTo(x(f), padT); g.lineTo(x(f), padT + h); g.stroke();
      g.fillText(f >= 1000 ? `${f / 1000}k` : `${f}`, x(f), padT + h + 5 * d);
    }
    const gains = shownGains(), fs = filtersOf(gains), pts = curve(gains, 240);
    const path = () => pts.forEach(([f, db], i) => (i ? g.lineTo(x(f), y(db)) : g.moveTo(x(f), y(db))));
    const grad = g.createLinearGradient(0, padT, 0, padT + h), c = line.slice(0, 3);
    grad.addColorStop(0, rgba([...c, fillA]));
    grad.addColorStop(0.5, rgba([...c, fillA * 0.35]));
    grad.addColorStop(1, rgba([...c, fillA]));
    g.beginPath(); path(); g.lineTo(x(20000), y(0)); g.lineTo(x(20), y(0)); g.closePath();
    g.fillStyle = grad; g.fill();
    g.beginPath(); path();
    g.strokeStyle = rgba(line); g.lineWidth = 2.5 * d; g.lineJoin = "round"; g.stroke();
    g.lineWidth = 2 * d;
    for (const f of FREQS) {
      const yy = y(magDb(f, fs));
      g.beginPath(); g.arc(x(f), yy, 3.5 * d, 0, 2 * Math.PI);
      g.fillStyle = rgba(mixed("dot")); g.fill(); g.strokeStyle = rgba(line); g.stroke();
    }
  }

  // 무대의 기하(CSS px). 머리는 가운데, 앞(위)이 0°. 부채꼴은 앞에서 양옆으로 ±(공간감 × 180°)만큼 벌어진다.
  function geometry(W, H) {
    const R = Math.min(W * 0.34, H * 0.4);
    return { cx: W / 2, cy: H * 0.47, R, head: R * 0.36 };
  }
  // 무대 위 한 점 → 공간감. 앞쪽에서 잰 각도의 절댓값을 180°로 나눈다. 앞 가운데 근처는 끔.
  function spatialAt(px, py) {
    const { cx, cy } = geometry(canvas.clientWidth, canvas.clientHeight);
    const a = Math.abs(Math.atan2(px - cx, -(py - cy))) / Math.PI;
    return a < 0.04 ? 0 : Math.round(clamp(a, 0, 1) * 100) / 100;
  }
  function drawStage() {
    const { W, H, d } = size(), g = canvas.getContext("2d");
    const pw = shown.power.value;
    const lit = mixed("on"), line = mixed("off").map((v, i) => lerp(v, lit[i], pw));
    const amount = dragging !== null ? dragging : shown.spatial.value;
    const { cx, cy, R, head } = geometry(W, H);
    g.clearRect(0, 0, W, H);
    g.lineWidth = 1.5 * d;
    g.strokeStyle = rgba(mixed("ring"));
    g.beginPath(); g.arc(cx, cy, R, 0, 2 * Math.PI); g.stroke();
    // 캔버스 각도는 오른쪽이 0이고 시계 방향이라 앞(위)은 -90°다.
    const half = amount * Math.PI, front = -Math.PI / 2;
    if (amount > 0.001) {
      const glow = g.createRadialGradient(cx, cy, head * 0.6, cx, cy, R);
      glow.addColorStop(0, rgba([...line.slice(0, 3), 0.42 * lerp(0.4, 1, pw)]));
      glow.addColorStop(1, rgba([...line.slice(0, 3), 0.06]));
      g.fillStyle = glow;
      g.beginPath(); g.moveTo(cx, cy); g.arc(cx, cy, R, front - half, front + half); g.closePath(); g.fill();
      g.strokeStyle = rgba(line); g.lineWidth = 4 * d; g.lineCap = "round";
      g.beginPath(); g.arc(cx, cy, R, front - half, front + half); g.stroke();
    }
    // 머리(위에서 본 모습): 얼굴은 앞(위)을 본다. 귀 둘, 코 하나.
    g.fillStyle = rgba(mixed("head")); g.strokeStyle = rgba(mixed("ring")); g.lineWidth = 1.5 * d;
    for (const side of [-1, 1]) {
      g.beginPath(); g.ellipse(cx + side * head * 0.98, cy, head * 0.16, head * 0.3, 0, 0, 2 * Math.PI); g.fill(); g.stroke();
    }
    g.beginPath(); g.moveTo(cx - head * 0.18, cy - head * 0.9); g.lineTo(cx, cy - head * 1.18); g.lineTo(cx + head * 0.18, cy - head * 0.9);
    g.closePath(); g.fill(); g.stroke();
    g.beginPath(); g.ellipse(cx, cy, head * 0.9, head, 0, 0, 2 * Math.PI); g.fill(); g.stroke();
    const knob = a => {
      g.beginPath(); g.arc(cx + R * Math.cos(a), cy + R * Math.sin(a), 8 * d, 0, 2 * Math.PI);
      g.fillStyle = rgba(mixed("dot")); g.fill();
      g.strokeStyle = rgba(line); g.lineWidth = 2.5 * d; g.stroke();
    };
    if (amount > 0.001) { knob(front - half); knob(front + half); } else knob(front);
    g.fillStyle = rgba(mixed("text")); g.font = `${11 * d}px system-ui, sans-serif`;
    g.textAlign = "center"; g.textBaseline = "top";
    g.fillText("앞", cx, Math.max(2 * d, cy - R - 16 * d));
    g.textBaseline = "bottom";
    g.font = `600 ${13 * d}px system-ui, sans-serif`;
    g.fillText(amount > 0.001 ? `감싸는 정도 ${Math.round(amount * 100)}%` : "공간감 꺼짐. 호를 옆으로 끌어 보세요", cx, H - 6 * d);
  }

  // 끌기: 누르면 그 자리로, 움직이면 따라가고(트윈 없이 바로), 놓으면 그 값에 머문다. 값은 매번 client로 보낸다.
  function send(v) { k.setState("stage_input", { v, seq: ++seq }); }
  // 곡선 위에서 직접 고치기: 점 가까이를 눌러 위아래로 끌면 그 밴드의 최종 이득이 바뀐다(밴드는 가로 위치로 가장 가까운 것).
  // 값은 "curve_input" = {band, gain, seq}로 client에 보내고, client가 세부 조정 슬라이더와 같은 길로 저장·적용한다.
  if (MODE === "curve") {
    canvas.style.touchAction = "none";
    const local = e => {
      const r = canvas.getBoundingClientRect();
      return { px: e.clientX - r.left, py: e.clientY - r.top, a: axes(r.width, r.height, 1) };
    };
    const nearest = (a, px) => FREQS.reduce((best, f, i) => (Math.abs(a.x(f) - px) < Math.abs(a.x(FREQS[best]) - px) ? i : best), 0);
    const near = e => {
      const { px, py, a } = local(e), i = nearest(a, px);
      const yy = a.y(magDb(FREQS[i], filtersOf(shownGains())));
      return Math.abs(a.x(FREQS[i]) - px) < 22 && Math.abs(yy - py) < 22 ? i : -1;
    };
    const sendBand = () => k.setState("curve_input", { band: bandDrag.i, gain: Math.round(bandDrag.gain * 2) / 2, seq: ++seq });
    canvas.addEventListener("pointerdown", e => {
      const i = near(e);
      if (i < 0) return;
      canvas.setPointerCapture(e.pointerId);
      bandDrag = { i, gain: local(e).a.db(local(e).py) };
      sendBand(); kick();
    });
    canvas.addEventListener("pointermove", e => {
      if (!bandDrag) { canvas.style.cursor = near(e) >= 0 ? "ns-resize" : "default"; return; }
      bandDrag.gain = local(e).a.db(local(e).py);
      sendBand(); kick();
    });
    const endBand = () => {
      if (!bandDrag) return;
      // 놓으면 트윈의 그 밴드도 손이 놓은 자리에 둔다(되돌아가며 튀지 않게). 이후 값은 view로 다시 온다.
      const at = bandDrag;
      shown.gains.value = shown.gains.value.map((g, i) => (i === at.i ? at.gain : g));
      shown.gains.from = shown.gains.value.slice();
      bandDrag = null; kick();
    };
    canvas.addEventListener("pointerup", endBand);
    canvas.addEventListener("pointercancel", endBand);
  }
  if (MODE === "stage") {
    canvas.style.touchAction = "none";
    const at = e => { const r = canvas.getBoundingClientRect(); return spatialAt(e.clientX - r.left, e.clientY - r.top); };
    canvas.addEventListener("pointerdown", e => { canvas.setPointerCapture(e.pointerId); dragging = at(e); send(dragging); kick(); });
    canvas.addEventListener("pointermove", e => { if (dragging === null) return; dragging = at(e); send(dragging); kick(); });
    const end = e => {
      if (dragging === null) return;
      const v = e.type === "pointercancel" ? dragging : at(e);
      dragging = null;
      shown.spatial.from = shown.spatial.to = shown.spatial.value = v;
      send(v); kick();
    };
    canvas.addEventListener("pointerup", end);
    canvas.addEventListener("pointercancel", end);
  }

  if (media) media.addEventListener("change", retheme);
  new ResizeObserver(() => { render(); kick(); }).observe(canvas);
  k.onStateChange(st => apply(st && st.view));
  apply(k.getState("view"));
  render();
})();
