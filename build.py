"""EQ 앱 정의와 업로드 입력(upload.json)을 만든다.

넣는 것:
- bridge.ps1 + engine/register.ps1 → client.js의 __BRIDGE__({gz, key}, gzip).
  register.ps1은 bridge.ps1 머리 주석의 규칙대로 base64(gzip(UTF-8))로 `__REGISTER_PS1__` 자리에 넣는다.
  client는 이 스크립트를 창의 기기 저장소에 .ps1로 두고 `powershell -File <경로>`로 띄운다(까닭은 client.js의
  「PowerShell 다리」 머리 주석). 그래서 스크립트 크기에 명령줄 한도가 없고, 주석을 빼지 않고 그대로 싣는다.
- 엔진 DLL → 자산 eq-engine. 자산 형식에 바이너리가 없어서, base64(gzip -9, mtime 0)를 JS 문자열 리터럴 한 문장
  (`"…";`)으로 담은 javascript 자산으로 올린다(검사는 UTF-8만 본다). 자산은 MCP 도구 호출 한 번에 실려 가므로
  ENGINE_BUDGET(업로드의 base64 글자 수)을 넘으면 빌드가 실패한다. client는 설치할 때만 kaleidoscope.assets.bytes로
  읽어 풀고, 여기서 넣어 둔 날 DLL의 SHA-256과 맞을 때만 다리의 install에 날 DLL의 base64(dll_b64)로 넘긴다.
  창을 열 때마다 받지 않는다.
- `--no-engine`: 엔진 자산을 빼고 빌드한다. client의 ENGINE이 null이 되어 설치 단계는 엔진을 따로 설치하라고 안내하고,
  다시 설치 단추는 정의에서 빠진다. 이미 설치된 엔진에 설정을 보내는 길(status, apply, 장치 바꾸기)은 그대로다.
- 자산 판은 내용의 sha256에서 파생한다. 내용이 같으면 같은 판, 다르면 새 판이라 손으로 올릴 판 번호가 없다.
"""
import base64
import gzip
import hashlib
import json
import sys
from pathlib import Path

NO_ENGINE = "--no-engine" in sys.argv[1:]
if set(sys.argv[1:]) - {"--no-engine"}:
    sys.exit("쓰는 법: python build.py [--no-engine]")

HERE = Path(__file__).parent
BRIDGE_PS1 = HERE / "bridge.ps1"
REGISTER_PS1 = HERE / "engine" / "register.ps1"
ENGINE_DLL = HERE / "engine" / "build" / "KaleidoscopeEQ.dll"  # 엔진 빌드 산출물(engine/build.ps1)
CLIENT_JS = HERE / "client.js"
ICON_SVG = HERE / "icon.svg"
GRAPH_JS = HERE / "graph.js"

ENGINE_ASSET = "eq-engine"
# 엔진 자산이 업로드(upload.json의 data_base64)에서 차지할 수 있는 base64 글자 수. MCP 도구 호출 한 번에 실려 가는 한도다.
ENGINE_BUDGET = 60_000


def need(path, owner):
    if not path.is_file():
        sys.exit(f"build.py: {path.relative_to(HERE)} 이(가) 없습니다 — {owner} 몫입니다. 생기면 다시 빌드하세요.")
    return path


def version_of(data):
    """내용에서 파생한 자산 판(1 이상의 31비트 정수)."""
    return int(hashlib.sha256(data).hexdigest()[:8], 16) & 0x7FFFFFFF or 1


def strip(text, mark):
    """자산 크기를 줄이려고 주석만 있는 줄과 빈 줄을 뺀다."""
    return "\n".join(l for l in text.splitlines() if l.strip() and not l.lstrip().startswith(mark)) + "\n"


def gzip_b64(data):
    # mtime=0으로 두어 같은 입력이면 같은 바이트(같은 자산 판)가 나오게 한다.
    return base64.b64encode(gzip.compress(data, mtime=0)).decode("ascii")


def bridge_script():
    """bridge.ps1에 register.ps1을 넣은 다리 스크립트(bridge.ps1 머리 주석의 규칙)."""
    bridge = need(BRIDGE_PS1, "다리 담당").read_text(encoding="utf-8")
    register = need(REGISTER_PS1, "엔진 담당").read_bytes()
    if bridge.count("__REGISTER_PS1__") != 1:
        sys.exit("build.py: bridge.ps1에 __REGISTER_PS1__ 자리표시자가 정확히 한 번 있어야 합니다.")
    return bridge.replace("__REGISTER_PS1__", gzip_b64(register))


def engine_dll():
    dll = need(ENGINE_DLL, "엔진 담당").read_bytes()
    if dll[:2] != b"MZ":
        sys.exit(f"build.py: {ENGINE_DLL.relative_to(HERE)} 이(가) Windows DLL이 아닙니다.")
    return dll


def engine_js(gz_b64):
    """엔진 자산 본문: base64(gzip(DLL))을 담은 JS 문자열 리터럴 한 문장. client의 engineDll()이 거꾸로 푼다."""
    return (json.dumps(gz_b64) + ";\n").encode("ascii")


bridge = bridge_script()
dll = None if NO_ENGINE else engine_dll()
engine_gz_b64 = None if dll is None else gzip_b64(dll)
engine_bytes = None if dll is None else engine_js(engine_gz_b64)
# client가 설치 전에 맞춰 볼 날 DLL의 SHA-256. 엔진 없는 판이면 null.
engine_info = None if dll is None else {"asset": ENGINE_ASSET, "sha256": hashlib.sha256(dll).hexdigest()}
# 다리는 client 안에 gzip해 싣는다(크기). 프로세스 이름은 내용에서 지어, 다리가 바뀌면 예전 프로세스에 붙지 않는다.
bridge_bytes = ("﻿" + bridge).encode("utf-8")
bridge_info = {"gz": gzip_b64(bridge_bytes), "key": "eq-bridge-" + hashlib.sha256(bridge_bytes).hexdigest()[:12]}
client = strip(CLIENT_JS.read_text(encoding="utf-8"), "//")
for mark in ("__BRIDGE__", "__ENGINE__"):
    if client.count(mark) != 1:
        sys.exit(f"build.py: client.js에 {mark} 자리표시자가 정확히 한 번 있어야 합니다.")
client = client.replace("__BRIDGE__", json.dumps(bridge_info)).replace("__ENGINE__", json.dumps(engine_info))
client_bytes = client.encode("utf-8")
icon_bytes = ICON_SVG.read_bytes()
ICON_VERSION = version_of(icon_bytes)
CLIENT_VERSION = version_of(client_bytes)
ENGINE_VERSION = None if engine_bytes is None else version_of(engine_bytes)

# 그림(sandbox) 자산 둘: graph.js 앞에 MODE와 client.js의 pure 구역(필터 응답·트윈 계산)을 붙인 HTML.
# 계산식은 client.js 한 곳에만 두고 여기서 가져온다(복사본이 갈라지지 않게).
_raw_client = CLIENT_JS.read_text(encoding="utf-8")
_a, _z = _raw_client.find("// ── pure:begin"), _raw_client.find("// ── pure:end")
if _a < 0 or _z < 0:
    sys.exit("build.py: client.js에 pure:begin / pure:end 표시가 있어야 합니다.")
PURE = _raw_client[_a:_z]


def graph_html(mode):
    script = f"const MODE = {json.dumps(mode)};\n{PURE}\n{GRAPH_JS.read_text(encoding='utf-8')}"
    return ("<!doctype html><html><head><meta charset=\"utf-8\"><style>"
            "html,body{margin:0;height:100%;background:transparent;overflow:hidden}"
            "canvas{display:block;width:100%;height:100%}</style></head>"
            f"<body><canvas></canvas><script>{script}</script></body></html>").encode("utf-8")


GRAPHS = {mode: graph_html(mode) for mode in ("curve", "stage")}
GRAPH_VERSIONS = {mode: version_of(data) for mode, data in GRAPHS.items()}

# ── 화면 ────────────────────────────────────────────────────
# 값과 동작은 모두 client가 낸다(저장소 profiles·my_presets와 다리의 장치 상태에서 파생). 정의는 배치만 맡는다.
C = lambda name: {"client": name}
act = lambda name: {"client": name}

# 머리: 쉘의 도구 막대(toolbar). 높이·여백·아래 구분선을 쉘이 정해 다른 앱과 같은 모양이 된다.
# 한 줄에 기기 이름(큰 글), 상태(작은 글), 그 바로 뒤에 EQ 켜기와 기본값으로 되돌리기.
header = {"type": "toolbar", "start": [
    {"type": "text", "style": "title", "value": C("device_title")},
    {"type": "text", "style": "meta", "value": C("device_meta")},
    {"type": "toggle", "label": "EQ", "value": C("power"), "disabled": C("no_device"), "on_change": act("power")},
    # 이 기기의 모든 값을 처음으로(묻고 나서). EQ 켜기 바로 옆에 둔다.
    {"type": "button", "label": "기본값으로 되돌리기", "icon": "rotate-ccw", "variant": "ghost", "disabled": C("no_device"),
     "on_click": act("reset_all"),
     "confirm": {"title": "이 기기의 설정을 기본값으로 되돌릴까요?",
                 "body": "공간, 장르, 정도, 서라운드, 세부 조정, 프리앰프가 처음 값(원래 소리)으로 돌아갑니다. 내 프리셋과 다른 기기의 설정은 그대로 남아요.",
                 "ok": "되돌리기"}},
]}


# 처음 설정: 지금 할 일 하나만. 작업 폴더 허용은 쉘의 기본 폴더 허용 화면(folder 블록)이 그린다.
setup = {"type": "when", "value": C("setup_show"), "child": {
    "type": "section", "title": C("setup_title"), "child": {
        "type": "stack", "direction": "column", "gap": "small", "children": [
            {"type": "progress", "value": C("setup_percent"), "max": 100, "label": C("setup_step")},
            {"type": "text", "value": C("setup_text")},
            {"type": "when", "value": C("need_folder"), "child": {
                "type": "folder", "root": {"permission": "work"}, "read_only": True, "header": False}},
            {"type": "when", "value": C("setup_has_button"), "child": {
                "type": "stack", "direction": "row", "gap": "small", "children": [
                    {"type": "button", "label": C("setup_button"), "icon": C("setup_icon"), "variant": "primary",
                     "on_click": act("setup")}]}},
        ]}}}

note = {"type": "when", "value": C("note_show"), "child": {
    "type": "notice", "kind": "info", "message": C("note"),
    "actions": [{"label": "닫기", "on_click": act("dismiss_note")}]}}

# 새 기기: 원음(Original)으로 시작했다고 알리고, 다른 기기의 설정을 가져올 수 있게 한다.
fresh = {"type": "when", "value": C("fresh_show"), "child": {
    "type": "stack", "direction": "row", "gap": "small", "align": "center", "fill": 0, "wrap": True, "children": [
        {"type": "text", "style": "meta", "value": C("fresh_text")},
        {"type": "select", "label": "다른 기기 설정 가져오기", "label_position": "hidden",
         "placeholder": "다른 기기 설정 가져오기", "value": C("copy_value"), "options": C("copy_options"), "on_change": act("copy_from")},
    ]}}

# fit cover: 그림이 블록을 빈틈없이 덮는다. contain이면 픽셀 반올림으로 생긴 틈에 쉘의 어두운 캔버스 바탕이 비친다.
# 그림은 sandbox에 그린다(graph.js). 쉘의 canvas 블록은 어두운 바탕과 반투명 테두리로 검은 선이 비친다.
curve = {"type": "sandbox", "asset": "eq-graph-curve", "height": 220, "state_keys": ["view", "curve_input"],
         "title": "EQ 주파수 응답 곡선: 점을 위아래로 끌어 밴드를 고친다"}

scenes = {"type": "section", "title": "공간", "meta": C("scene_desc"), "child": {
    "type": "stack", "direction": "column", "gap": "medium", "children": [
        {"type": "repeat", "key": "key", "layout": "grid", "gap": "small", "items": C("scenes"),
         "grid": {"columns": "auto", "item_width": "small"},
         "template": {"type": "button", "label": {"item": "name"}, "variant": "chip", "pressed": {"item": "on"},
                      "on_click": act("pick_scene")}},
        {"type": "when", "value": C("space_amount_show"), "child": {
            "type": "slider", "label": "공간 정도", "label_position": "hidden", "value": C("space_amount"), "min": 0, "max": 100,
            "step": 5, "show_value": True, "on_change": act("set_space_amount")}},
    ]}}

def axis_section(field, title):
    """겹쳐 쓰는 축 하나의 구역: 칩(여러 줄로 감김) + 고르면 나타나는 정도 손잡이."""
    return {"type": "section", "title": title, "meta": C(field + "_desc"), "child": {
        "type": "stack", "direction": "column", "gap": "medium", "children": [
            {"type": "repeat", "key": "key", "layout": "grid", "gap": "small", "items": C(field + "s"),
             "grid": {"columns": "auto", "item_width": "small"},
             "template": {"type": "button", "label": {"item": "name"}, "variant": "chip", "pressed": {"item": "on"},
                          "on_click": act("pick_" + field)}},
            {"type": "when", "value": C(field + "_amount_show"), "child": {
                "type": "slider", "label": title + " 정도", "label_position": "hidden", "value": C(field + "_amount"),
                "min": 0, "max": 100, "step": 5, "show_value": True, "on_change": act("set_" + field + "_amount")}},
        ]}}


AXES = [("genre", "장르"), ("color", "음색")]
axis_sections = [axis_section(f, t) for f, t in AXES]

mine = {"type": "when", "value": C("mine_show"), "child": {
    "type": "section", "title": "내 프리셋", "child": {
        "type": "list", "items": C("mine"), "selected": C("mine_selected"),
        "on_select": act("pick_mine"), "on_remove": act("delete_mine")}}}

spatial = {"type": "section", "title": "서라운드", "meta": "헤드폰", "child": {
    "type": "stack", "direction": "column", "gap": "small", "children": [
        {"type": "sandbox", "asset": "eq-graph-stage", "height": 300, "state_keys": ["view", "stage_input"],
         "title": "서라운드: 위에서 본 머리 둘레의 호를 끌어 감싸는 정도를 정한다"},
        {"type": "select", "style": "segmented", "label": "공간감", "label_position": "hidden",
         "value": C("spatial"), "options": C("spatial_options"), "disabled": C("no_device"),
         "on_change": act("set_spatial")},
        {"type": "text", "style": "meta", "value": C("spatial_hint")},
    ]}}


def labelled_slider(value, lo, hi, action):
    """쉬운 이름은 본문 글자로, 주파수는 작게, 그 아래 슬라이더."""
    return {"type": "stack", "direction": "column", "gap": "none", "children": [
        {"type": "stack", "direction": "row", "gap": "small", "align": "baseline", "fill": 1, "children": [
            {"type": "text", "value": {"item": "label"}},
            {"type": "text", "style": "meta", "value": {"item": "hz"}}]},
        {"type": "slider", "label": {"item": "label"}, "label_position": "hidden", "value": {"item": value},
         "min": lo, "max": hi, "step": 0.5, "show_value": True, "on_change": act(action)}]}


details = {"type": "section", "title": "세부 조정", "meta": C("custom_meta"), "collapsible": True, "collapsed": True,
           "child": {"type": "stack", "direction": "column", "gap": "medium", "children": [
               {"type": "repeat", "key": "key", "layout": "grid", "gap": "small",
                "grid": {"columns": 3}, "items": C("tones"),
                "template": labelled_slider("value", -12, 12, "set_tone")},
               {"type": "text", "style": "meta", "value": "밴드별"},
               {"type": "repeat", "key": "key", "layout": "grid", "gap": "small",
                "grid": {"columns": "auto", "item_width": "small"}, "items": C("bands"),
                "template": labelled_slider("gain", -15, 15, "set_band")},
               {"type": "stack", "direction": "row", "gap": "small", "wrap": True, "children": [
                   {"type": "button", "label": "내 프리셋으로 저장", "icon": "bookmark", "on_click": act("save_mine"),
                    "disabled": C("no_device")},
                   {"type": "button", "label": "고친 값 지우기", "icon": "x", "variant": "ghost",
                    "on_click": act("reset_scene"), "disabled": C("not_custom")},
               ]},
               {"type": "toggle", "label": "클리핑 방지 (곡선 꼭대기에 맞춰 자동으로 줄임)",
                "value": C("auto_preamp"), "on_change": act("set_auto_preamp")},
               {"type": "when", "value": C("manual_preamp"), "child": {
                   "type": "slider", "label": "프리앰프", "value": C("preamp"), "min": -24, "max": 6, "step": 0.5,
                   "show_value": True, "on_change": act("set_preamp")}},
               {"type": "text", "style": "meta", "value": C("preamp_text")},
           ]}}

# 엔진 정보와 다시 설치·제거는 맨 아래에 조용히.
footer = {"type": "when", "value": C("engine_show"), "child": {
    "type": "section", "title": "사운드 엔진", "meta": C("engine_text"), "collapsible": True, "collapsed": True, "child": {
    "type": "stack", "direction": "row", "gap": "small", "align": "center", "wrap": True, "children": [
        {"type": "text", "style": "meta", "value": "엔진을 새 판으로 바꾸거나 이 PC에서 지울 수 있어요."},
        *([] if NO_ENGINE else [{"type": "button", "label": "다시 설치", "variant": "ghost", "on_click": act("reinstall")}]),
        {"type": "button", "label": "엔진 제거", "variant": "ghost", "on_click": act("uninstall"),
         "confirm": {"title": "사운드 엔진을 제거할까요?",
                     "body": "모든 장치에서 EQ가 꺼지고 소리가 1초쯤 끊깁니다. 설정은 남아 있어 다시 설치하면 그대로 돌아옵니다.",
                     "ok": "제거"}},
    ]}}}

# 넓은 창: 왼쪽은 고르는 것(공간·장르·내 프리셋·세부 조정), 오른쪽은 보는 것(EQ 곡선·서라운드 무대).
# 왼쪽만 스크롤되어 손을 대는 동안 오른쪽 그림이 늘 보인다. 좁은 창(600px 미만)에서는 한 줄로 쌓이고
# 쉘이 그림 칸을 위로 올린다.
WIDE = 640  # 이 폭(CSS px) 이상이면 두 칸


def scroll(*children):
    """넘치면 이 칸 안에서 스크롤하는 세로 칸. stack의 채우는 자식(fill)은 남은 높이를 갖고 넘치면 그 안에서 스크롤한다."""
    return {"type": "stack", "direction": "column", "gap": "none", "fill": 0, "children": [
        {"type": "stack", "direction": "column", "gap": "large", "children": list(children)}]}


settings = [scenes, *axis_sections, mine, details]
# 그림 둘은 같은 꼴의 구역에 담아 폭이 같게 한다(구역 안쪽 여백까지 같다).
curve_section = {"type": "section", "title": "EQ", "child": curve}
visuals = [curve_section, spatial]
# 넓은 창: 왼쪽은 고르는 것, 오른쪽은 보는 것. 두 칸이 반씩 나눠 갖고 각자 스크롤한다(설정을 내려도 그림은 그대로).
wide = {"type": "when", "min_width": WIDE, "child": {
    "type": "stack", "direction": "row", "gap": "large", "fill": [0, 1],
    "children": [scroll(*settings), scroll(*visuals)]}}
# 좁은 창: 한 칸에 그림 먼저, 그 아래 설정. 칸 전체가 스크롤한다.
narrow = {"type": "when", "max_width": WIDE - 1, "child": scroll(*visuals, *settings)}
# 머리·안내는 위에, 엔진 구역은 아래에 붙고, 가운데 몸통(wide/narrow, 이웃한 폭 조건 when끼리 같은 채움 자리)이
# 남은 높이를 가진다. 간격은 모두 large(구역 사이) / medium(구역 안).
ui = {"type": "stack", "direction": "column", "gap": "large", "fill": 4, "children": [
    header, setup, note, fresh, wide, narrow, footer,
]}

PROFILE_SCHEMA = {"type": "object", "properties": {
    "name": {"type": "string"}, "kind": {"type": "string"}, "scene": {"type": "string"},
    "genre": {"type": ["string", "null"]}, "color": {"type": ["string", "null"]},
    "amounts": {"type": "object", "additionalProperties": {"type": "number"}},
    "edits": {"type": "object", "additionalProperties": {"type": "array", "items": {"type": "number"}}},
    "spatial": {"type": "number"}, "enabled": {"type": "boolean"},
    "auto_preamp": {"type": "boolean"}, "preamp": {"type": "number"}}}
REVERB_SCHEMA = {"type": "object", "required": ["decay", "predelay", "wet"], "properties": {
    "decay": {"type": "number"}, "predelay": {"type": "number"}, "wet": {"type": "number"}}}
# 예전 판의 user-presets와 같은 키·모양이라 저장해 둔 내 프리셋이 그대로 이어진다.
PRESETS_SCHEMA = {"type": "array", "items": {"type": "object", "required": ["key", "name", "gains"], "properties": {
    "key": {"type": "string"}, "name": {"type": "string"}, "desc": {"type": "string"},
    "gains": {"type": "array", "items": {"type": "number"}}, "reverb": {"type": ["object", "null"]}}}}

OUTPUTS = {
    "device_title": "EQ", "device_meta": "사운드 엔진 확인 중…", "no_device": True, "power": True,
    "setup_show": False, "setup_percent": 0, "setup_step": "", "setup_title": "", "setup_text": "", "setup_button": "", "setup_icon": "download",
    "setup_has_button": False, "need_folder": False, "note_show": False, "note": "",
    "scenes": [], "space_amount_show": False, "space_amount": 100,
    **{k: v for f, _ in AXES for k, v in ((f + "s", []), (f + "_amount_show", False), (f + "_amount", 100), (f + "_desc", ""))}, "scene_desc": "", "fresh_show": False, "fresh_text": "", "copy_options": [], "copy_value": None,
    "spatial": "0", "spatial_options": [], "spatial_hint": "",
    "tones": [], "bands": [], "custom": False, "not_custom": True, "custom_meta": "",
    "auto_preamp": True, "manual_preamp": False, "preamp": 0, "preamp_text": "",
    "mine_show": False, "mine": [], "mine_selected": None, "engine_show": False, "engine_text": "",
}
ACTIONS = ["power", "setup", "pick_scene", "pick_mine", "delete_mine", "set_spatial", "set_tone", "set_band",
           "reset_scene", "save_mine", "set_auto_preamp", "set_preamp", "copy_from", "reinstall", "uninstall",
           "dismiss_note", "recheck", "set_space_amount", "reset_all"] + [a for f, _ in AXES for a in ("pick_" + f, "set_" + f + "_amount")]

definition = {
    "id": "eq",
    "name": "EQ",
    "description": "Windows의 모든 소리에 공간과 EQ를 겁니다. 출력 장치마다 따로 기억합니다.",
    "icon": {"asset": "eq-icon", "version": ICON_VERSION},
    "assets": [{"id": "eq-icon", "version": ICON_VERSION}, {"id": "eq-client", "version": CLIENT_VERSION}]
              + [{"id": f"eq-graph-{m}", "version": v} for m, v in GRAPH_VERSIONS.items()]
              + ([] if NO_ENGINE else [{"id": ENGINE_ASSET, "version": ENGINE_VERSION}]),
    # 47: 기본 폴더(default_folder), 54: single_window.
    "requires": {"blocks": 54, "modules": {"client": 4, "process": 2, "files": 4, "storage": 1, "assets": 1}},
    "single_window": True,
    "window_size": {"width": 1040, "height": 760},
    "permissions": [
        {"module": "process", "id": "bridge", "label": "사운드 엔진과 이야기하기 (PowerShell)", "command": "powershell"},
        # process.start는 작업 폴더 권한이 꼭 있어야 한다. 다리는 그 폴더에 아무것도 쓰지 않는다.
        {"module": "files", "id": "work", "access": "read", "label": "EQ 작업 폴더", "default_folder": "music"},
    ],
    "storage": {"entries": [
        {"id": "profiles", "key": "device-profiles", "kind": "json", "label": "기기별 설정", "presentation": "hidden",
         "schema": {"type": "object", "additionalProperties": PROFILE_SCHEMA}, "default": {}},
        {"id": "my_presets", "key": "user-presets", "kind": "json", "label": "내 프리셋", "presentation": "hidden",
         "schema": PRESETS_SCHEMA, "default": []},
        # 예전 EQ 앱(Equalizer APO 판)의 설정. 값이 남아 있어 선언을 지울 수 없고(저장값 보호), 처음 기기의 시작점을
        # 여기서 파생한다(client.js startOf). 새 판은 이 항목에 쓰지 않는다.
        {"id": "eq", "key": "eq", "kind": "json", "label": "이전 EQ 설정", "presentation": "hidden",
         "schema": {"type": "object", "required": ["enabled", "gains", "auto_preamp", "preamp"], "properties": {
             "enabled": {"type": "boolean"}, "presets": {"type": "array", "items": {"type": "string"}},
             "gains": {"type": "array", "items": {"type": "number"}}, "auto_preamp": {"type": "boolean"},
             "preamp": {"type": "number"}}},
         "default": {"enabled": True, "presets": [], "gains": [0] * 10, "auto_preamp": True, "preamp": 0}},
    ]},
    "client": {
        "runtime": "javascript",
        "entry": {"id": "eq-client", "version": CLIENT_VERSION},
        "storage_entries": ["profiles", "my_presets", "eq"],
        "state_keys": ["view", "stage_input", "curve_input"],
        "process_permissions": ["bridge"],
        # 다리 스크립트 파일. process.file_uri는 창 범위 저장소만 가리킨다. 창을 지우면 쉘이 함께 지운다.
        "device_storage": {"prefixes": ["bridge/"], "scope": "window"},
        "actions": ACTIONS,
        "outputs": {k: {"default": v} for k, v in OUTPUTS.items()},
    },
    "window_title": {"title": "EQ"},
    # client ↔ 그림(sandbox): view = 그릴 값, stage_input = 무대를 끈 값, curve_input = 곡선에서 끈 밴드.
    "state": {"view": None, "stage_input": None, "curve_input": None},
    "ui": ui,
}


def asset(id_, version, fmt, data, source):
    return {"id": id_, "version": version, "format": fmt, "sha256": hashlib.sha256(data).hexdigest(),
            "source": source, "license": "MIT", "data_base64": base64.b64encode(data).decode()}


uploads = [
    asset("eq-icon", ICON_VERSION, "svg", icon_bytes, "eq-app/icon.svg"),
    asset("eq-client", CLIENT_VERSION, "javascript", client_bytes, "eq-app/client.js"),
] + [asset(f"eq-graph-{m}", GRAPH_VERSIONS[m], "html", GRAPHS[m], "eq-app/graph.js") for m in GRAPHS] + ([] if NO_ENGINE else [
    asset(ENGINE_ASSET, ENGINE_VERSION, "javascript", engine_bytes, "eq-app/engine (KaleidoscopeEQ.dll, gzip+base64)"),
])
upload = json.dumps({"definition": definition, "asset_uploads": uploads}, ensure_ascii=False)

(HERE / "client.built.js").write_text(client, encoding="utf-8")
(HERE / "app.json").write_text(json.dumps(definition, ensure_ascii=False, indent=2), encoding="utf-8")
(HERE / "def.min.json").write_text(json.dumps(definition, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
(HERE / "upload.json").write_text(upload, encoding="utf-8")

print(f"다리 {len(bridge):,}자, 판 icon {ICON_VERSION} client {CLIENT_VERSION} engine {ENGINE_VERSION or '없음(--no-engine)'}")
for u in uploads:
    print(f"  자산 {u['id']:<9} base64 {len(u['data_base64']):>9,}자")
if dll is not None:
    gz_len = len(base64.b64decode(engine_gz_b64))
    print(f"  엔진 DLL {len(dll):,}B → gzip {gz_len:,}B, sha256 {engine_info['sha256']}")
print(f"  upload.json {len(upload.encode('utf-8')):,}B")
if dll is not None:
    used = len(uploads[-1]["data_base64"])
    if used > ENGINE_BUDGET:
        sys.exit(f"build.py: 엔진 자산이 base64 {used:,}자로 한도 ENGINE_BUDGET {ENGINE_BUDGET:,}자를 넘습니다. "
                 "MCP 도구 호출 한 번에 실을 수 없으니 엔진 DLL을 줄여야 합니다(한도를 올리지 말 것). "
                 "생성물은 써 두었지만 올리지 마세요.")
