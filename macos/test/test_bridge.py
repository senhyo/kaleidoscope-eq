"""keq-mac bridge의 프로토콜 시험(CONTRACT §4, §5). 진짜 ~/Library는 건드리지 않는다.

    python3 macos/test/test_bridge.py [keq-mac 경로]   # 기본 macos/build/keq-mac

KEQ_HOME을 임시 폴더로 두고 다리를 띄워 JSON 줄을 보내고 답을 확인한다. KEQ_HOME이 있으면 install/uninstall은
파일만 만들고 지우며 launchctl은 부르지 않는다(실제 설치는 README의 손 시험).
"""
import json
import os
import plistlib
import select
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
BIN = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "build" / "keq-mac"
KINDS = {"headphones", "speakers", "other"}
failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1


class Bridge:
    def __init__(self, home):
        env = dict(os.environ, KEQ_HOME=str(home))
        self.p = subprocess.Popen([str(BIN), "bridge"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
        self.buf = b""
        self.events = []

    def line(self, timeout=5.0):
        end = time.time() + timeout
        while b"\n" not in self.buf:
            left = end - time.time()
            if left <= 0 or not select.select([self.p.stdout], [], [], left)[0]:
                return None
            chunk = os.read(self.p.stdout.fileno(), 65536)
            if not chunk:
                return None
            self.buf += chunk
        raw, self.buf = self.buf.split(b"\n", 1)
        return json.loads(raw)

    def send_raw(self, text):
        self.p.stdin.write(text.encode() + b"\n")
        self.p.stdin.flush()

    def ask(self, body, timeout=10.0):
        self.send_raw(json.dumps(body))
        while True:
            m = self.line(timeout)
            if m is None or "event" not in m:
                return m
            self.events.append(m)

    def close(self):
        self.p.stdin.close()
        return self.p.wait(5)


def main():
    if not BIN.is_file():
        sys.exit(f"{BIN} 이(가) 없습니다. macos/build.sh로 먼저 빌드하세요.")
    with tempfile.TemporaryDirectory(prefix="keq-test-") as tmp:
        home = Path(tmp)
        support = home / "Library/Application Support/KaleidoscopeEQ"
        plist = home / "Library/LaunchAgents/me.senhyo.kaleidoscope-eq.plist"
        b = Bridge(home)

        # status
        r = b.ask({"id": 1, "op": "status"})
        check(r and r["id"] == 1 and r["status"] == "ok", "status: ok 답")
        devs = r.get("devices") or []
        check(len(devs) > 0, f"status: devices {len(devs)}개")
        check(all(set(d) == {"id", "name", "kind", "registered", "active"} for d in devs), "status: 장치 필드 모양")
        check(all(d["kind"] in KINDS for d in devs), "status: kind " + ", ".join(sorted({d['kind'] for d in devs})))
        dev = r["device"]
        check(dev is not None and dev["id"] in {d["id"] for d in devs}, f"status: 기본 장치 {dev and dev['name']!r}")
        e = r["engine"]
        check(e["installed"] is False and e["version"] is None and e["platform"] == "macos", "status: 설치 전 engine")
        check(dev["registered"] is False and dev["active"] is False, "status: 설치 전 registered/active false")
        for d in devs:
            print(f"       {d['kind']:<10} {d['id']}  {d['name']}")

        # apply: §1 text, same lines bridge.ps1 writes
        uid = "Test:UID/with spaces~1"
        config = {"enabled": True, "preamp": -3.5,
                  "peaks": [[32, 1.41, 3], [1000, 1.41, -2.5], [16000, 1.41, 0.1]],
                  "reverb": {"decay": 2.4, "predelay": 28, "wet": -2}, "spatial": 0.65}
        r = b.ask({"id": 2, "op": "apply", "device_id": uid, "config": config})
        check(r and r["status"] == "ok" and "engine" in r and "device" in r, "apply: ok 답에 engine, device")
        cfg = support / "endpoints" / "Test_UID_with_spaces_1.cfg"
        want = ("# kaleidoscope-eq 1\nenabled 1\npreamp -3.5\npeak 32 1.41 3\npeak 1000 1.41 -2.5\n"
                "peak 16000 1.41 0.1\nreverb 2.4 28 -2\nspatial 0.65\n")
        got = cfg.read_text() if cfg.exists() else None
        check(got == want, "apply: .cfg 본문" + ("" if got == want else f"\n{got!r}\n != {want!r}"))
        check(not [p for p in cfg.parent.iterdir() if p.name.endswith(".tmp")], "apply: 임시 파일이 남지 않음")

        r = b.ask({"id": 3, "op": "apply", "device_id": uid,
                   "config": {"enabled": False, "preamp": 0, "peaks": [[64, 1.41, 2]], "reverb": None, "spatial": 0}})
        got = cfg.read_text()
        check(r["status"] == "ok" and got == "# kaleidoscope-eq 1\nenabled 0\npreamp 0\npeak 64 1.41 2\nspatial 0\n",
              "apply: 꺼짐, 피크 하나, 잔향 없음")

        # errors: the bridge answers and keeps running
        r = b.ask({"id": 4, "op": "apply", "device_id": uid, "config": {"enabled": True, "preamp": "loud", "spatial": 0}})
        check(r and r["id"] == 4 and r["status"] == "error" and "preamp" in r["message"], f"apply: 잘못된 preamp → {r and r.get('message')}")
        check(cfg.read_text() == got, "apply: 실패하면 .cfg를 건드리지 않음")
        r = b.ask({"id": 5, "op": "apply", "device_id": uid, "config": {"preamp": 0, "spatial": 0, "peaks": [[1, 2]]}})
        check(r and r["status"] == "error" and "peaks" in r["message"], f"apply: 잘못된 peaks → {r and r.get('message')}")
        r = b.ask({"id": 6, "op": "apply", "device_id": "", "config": config})
        check(r and r["status"] == "error" and "device_id" in r["message"], f"apply: 빈 device_id → {r and r.get('message')}")
        r = b.ask({"id": 7, "op": "apply", "device_id": uid, "config": {"preamp": True, "spatial": 0}})
        check(r and r["status"] == "error", "apply: 참/거짓은 숫자가 아님")
        b.send_raw("{this is not json")
        r = b.line()
        check(r and r["status"] == "error" and r["id"] is None, f"잘못된 JSON → {r and r.get('message')}")
        r = b.ask({"id": 8, "op": "dance"})
        check(r and r["id"] == 8 and r["status"] == "error" and "dance" in r["message"], f"모르는 op → {r and r.get('message')}")
        r = b.ask({"id": 9, "op": "install", "dll_b64": "AAAA"})  # dll_b64 is ignored on macOS
        check(r and r["status"] == "ok" and r.get("launchd") == "skipped", "install(KEQ_HOME): ok, launchctl 건너뜀")

        # install layout
        inst = support / "keq-mac"
        check(inst.is_file() and os.access(inst, os.X_OK) and inst.read_bytes() == BIN.read_bytes(), "install: 실행 파일 복사(실행 권한)")
        pl = plistlib.loads(plist.read_bytes()) if plist.exists() else {}
        check(pl.get("Label") == "me.senhyo.kaleidoscope-eq" and pl.get("ProgramArguments") == [str(inst), "engine"]
              and pl.get("RunAtLoad") is True and pl.get("KeepAlive") == {"SuccessfulExit": False}
              and pl.get("ProcessType") == "Interactive"
              and pl.get("StandardErrorPath", "").startswith(str(home / "Library/Logs/KaleidoscopeEQ")), "install: LaunchAgent plist")
        check((support / "endpoints").is_dir() and (support / "version").read_text().strip() == "1.0.0", "install: endpoints/, version")
        e, dev = r["engine"], r["device"]
        check(e["installed"] is True and e["version"] == "1.0.0" and e["state"] == "stopped", f"install 뒤 engine {e}")
        check(e.get("current") is True, "install 뒤 current true(설치된 엔진 = 이 실행 파일)")
        check(dev["registered"] is True and dev["active"] is False, "install 뒤 registered true, active false(엔진 없음)")

        # engine state files -> active, permission, and an unsolicited device event.
        # The bridge process itself is a live keq-mac, so its pid stands in for a running engine.
        duid = dev["id"]
        safe = "".join(c if c.isalnum() and c.isascii() or c in "._-" else "_" for c in duid)
        b.events.clear()
        (support / "engine.state").write_text(f"permission_pending {duid}\npid {b.p.pid}\n")
        time.sleep(1.6)
        r = b.ask({"id": 10, "op": "status"})
        check(r["engine"]["state"] == "permission_pending" and r["engine"]["permission"] == "pending", "engine.state permission_pending → permission pending")
        check(any(ev.get("event") == "device" for ev in b.events), "엔진 상태가 바뀌면 device 알림")
        b.events.clear()
        (support / "endpoints" / f"{safe}.live").write_text(f"pid {b.p.pid}\n2026-01-01T00:00:00Z\n")
        (support / "engine.state").write_text(f"running {duid}\npid {b.p.pid}\n")
        ev = b.line(3.0)
        check(ev and ev.get("event") == "device" and ev["device"]["active"] is True, f"running이 되면 알림, active true: {ev}")
        r = b.ask({"id": 11, "op": "status"})
        check(r["engine"]["state"] == "running" and r["engine"]["permission"] == "granted" and r["device"]["active"] is True,
              "status: running, granted, active")
        (support / "engine.state").write_text(f"running {duid}\npid 999999\n")
        r = b.ask({"id": 12, "op": "status"})
        check(r["engine"]["state"] == "stopped" and r["device"]["active"] is False, "죽은 pid → stopped, active false")

        r = b.ask({"id": 13, "op": "uninstall"})
        check(r and r["status"] == "ok" and r["engine"]["installed"] is False, "uninstall: ok, installed false")
        left = [str(p.relative_to(home)) for p in home.rglob("*") if p.is_file()]
        check(not plist.exists() and not support.exists() and not left, f"uninstall: 남은 파일 없음 {left}")
        r = b.ask({"id": 14, "op": "uninstall"})
        check(r and r["status"] == "ok", "uninstall 두 번째도 ok")
        check(b.close() == 0, "입력이 닫히면 다리가 0으로 끝남")

    print(f"\n{'모두 통과' if not failures else f'{failures}개 실패'}")
    sys.exit(1 if failures else 0)


main()
