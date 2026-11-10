# EQ 앱 — 부품 사이 계약

Equalizer APO(서드파티)를 걷어내고 우리가 만든 APO로 Windows 시스템 소리 전체에 EQ·공간감을 건다.
설치 때 관리자 승인(UAC) 한 번, 오디오 서비스 재시작(재부팅 없음). 이후 값 변경은 승인 없이 즉시 반영.

macOS는 같은 화면과 같은 DSP(`engine/src/dsp.cpp`)를 쓰고, 다리와 엔진을 실행 파일 하나(`keq-mac`)가 맡는다(§5).

부품과 소유 파일 (서로 남의 파일을 고치지 않는다):

| 부품 | 소유 파일 |
|---|---|
| 엔진 (C++ APO + 등록 스크립트) | `eq-app/engine/**` |
| 다리 (PowerShell, 앱 ↔ 엔진) | `eq-app/bridge.ps1` (기존 `apo.ps1`을 대체·삭제) |
| macOS 엔진·다리 (Objective-C++, 실행 파일 하나) | `eq-app/macos/**` (`engine/src/dsp.cpp`는 고치지 않고 그대로 넣는다) |
| 화면 (kaleidoscope 앱) | `eq-app/client.js`, `eq-app/build.py`, `eq-app/icon.svg`, 생성물(`app.json`, `client.built.js`, `upload.json`, `def.min.json`) |

## 1. 엔진 설정 파일 (다리가 쓰고, APO가 읽는다)

경로: `%ProgramData%\KaleidoscopeEQ\endpoints\<GUID>.cfg`
`<GUID>`는 출력 장치 ID(`{0.0.0.00000000}.{xxxxxxxx-...}`)의 마지막 `{...}` 부분(중괄호 포함, 소문자).
설치 스크립트가 `%ProgramData%\KaleidoscopeEQ\endpoints`를 만들고 Users에게 수정 권한, audiodg(LocalService)에게 쓰기 권한을 준다.

UTF-8 텍스트, 한 줄에 한 지시. 모르는 줄은 무시한다(앞으로 확장).
```
# kaleidoscope-eq 1
enabled 1
preamp -3.5
peak 32 1.41 3
peak 64 1.41 2.5
...                      (peak <중심Hz> <Q> <이득dB>, 개수 자유, 0~20개)
reverb 2.4 28 -2         (decay 초, predelay ms, wet dB) — 줄이 없으면 잔향 끔
spatial 0.6              (0~1, 헤드폰 공간감: 크로스피드 + 가상 스피커화 강도) — 없으면 0
```
- 파일이 없거나 `enabled 0`이면 원음 그대로 통과(bit-exact).
- 다리는 임시 파일에 쓴 뒤 원자적으로 교체(rename)한다. APO는 실시간 스레드 밖에서 변경을 감지해 매개변수를 통째로 바꿔 끼우고, 바뀔 때 클릭 잡음이 없도록 짧게(약 20~50ms) 섞는다.

## 2. 엔진 상태 (APO가 쓰고, 다리가 읽는다)

- 레지스트리 `HKLM\SOFTWARE\KaleidoscopeEQ`: `InstallPath`(DLL 폴더), `Version`(문자열).
  하위 키 `Endpoints\<GUID>`가 있으면 그 장치에 엔진이 붙어 있다(등록됨).
- APO 인스턴스가 실제로 소리를 처리하기 시작하면(LockForProcess) `%ProgramData%\KaleidoscopeEQ\endpoints\<GUID>.live`에
  `pid <audiodg pid>` 와 UTC 시각 한 줄을 쓴다. 다리는 이 파일이 지금 audiodg 프로세스의 pid와 맞으면 "적용 중(active)"으로 본다.

## 3. 등록 스크립트 `engine/register.ps1` (관리자 권한으로 실행됨)

```
register.ps1 -Dll <임시 경로의 DLL> [-Endpoints <GUID,...>]   # 생략하면 모든 활성 출력 장치
register.ps1 -Uninstall
```
DLL을 `%ProgramFiles%\KaleidoscopeEQ\`로 복사·COM 등록, 장치에 붙이고(기존 제조사 효과는 보존), ProgramData 폴더와 권한을 만들고,
레지스트리 상태를 쓰고, `audiosrv`를 재시작한다. 끝나면 종료 코드 0, 실패는 0이 아닌 코드와 stderr 메시지.
이미 설치돼 있으면 덮어쓰기(업데이트)로 동작한다. 제거는 모든 흔적을 원래대로 되돌린다.

## 4. 다리 프로토콜 (화면 ↔ bridge.ps1, 표준 입출력 JSON 한 줄씩)

요청 `{"id":n,"op":...}` → 답 `{"id":n,"status":"ok"|"error",...}`. 모든 ok 답에 `engine`과 `device`를 싣는다.
```
engine: {"installed":bool,"version":"x"|null}
device: {"id":"{guid}","name":"AirPods Pro","kind":"headphones|headset|speakers|other","registered":bool,"active":bool} | null
```
| op | 인자 | 하는 일 |
|---|---|---|
| `status` | — | 상태만 답한다. 답에 `devices`(모든 활성 출력 장치 목록, 같은 모양)도 싣는다. |
| `apply` | `device_id`, `config` | 그 장치의 .cfg를 쓴다. `config = {enabled, preamp, peaks:[[f,q,g],...], reverb:{decay,predelay,wet}|null, spatial}` |
| `install` | `dll_b64`, 선택 `endpoints` | DLL을 임시 파일로 풀고 register.ps1을 관리자 권한(UAC)으로 실행. 거절 시 `"elevated":"cancelled"`. |
| `uninstall` | — | register.ps1 -Uninstall을 관리자 권한으로. |

요청 없이 다리가 먼저 보내는 알림: 기본 출력 장치가 바뀌면 `{"event":"device","device":{...}}` (1초 간격 감시).
register.ps1 본문은 빌드 때 `build.py`가 bridge.ps1 안의 자리표시자 `__REGISTER_PS1__`에 넣는다(다리는 이것을 임시 .ps1로 써서 실행).
블루투스 기기의 통화용(Hands-Free) 끝점은 `kind:"headset"`으로 구분만 하고, 화면은 음악용 끝점을 기준으로 설정을 저장한다.

## 5. macOS (`macos/`, 실행 파일 `keq-mac`)

관리자 권한, 드라이버, 재시동이 없다. 모든 것이 사용자 계정 안에 있다. Apple Silicon, macOS 14.2 이상.

| 경로 (`~` = 사용자 홈) | 쓰는 쪽 | 내용 |
|---|---|---|
| `~/Library/Application Support/KaleidoscopeEQ/keq-mac` | 다리(install) | 설치된 엔진. 다리가 자기 실행 파일을 원자적으로 복사한다 |
| `…/KaleidoscopeEQ/version` | 다리(install) | 설치한 판(`1.0.0`) |
| `…/KaleidoscopeEQ/endpoints/<safe(UID)>.cfg` | 다리(apply) | §1과 같은 본문. 임시 파일에 쓴 뒤 rename |
| `…/KaleidoscopeEQ/endpoints/<safe(UID)>.live` | 엔진 | IOProc이 실제로 불리기 시작하면 `pid <pid>`와 UTC 시각 |
| `…/KaleidoscopeEQ/engine.state` | 엔진 | 첫 줄 상태, 둘째 줄 `pid <pid>` (아래) |
| `~/Library/LaunchAgents/me.senhyo.kaleidoscope-eq.plist` | 다리(install) | `[keq-mac, "engine"]`, RunAtLoad, KeepAlive `{SuccessfulExit: false}`, ProcessType Interactive |
| `~/Library/Logs/KaleidoscopeEQ/engine.log` | 엔진 | 생명 주기 기록(시작, 장치 전환, 오류, 상태 변화) |

- 장치 id는 Core Audio의 장치 UID다. `safe(UID)`는 `[A-Za-z0-9._-]` 밖의 바이트를 `_`로 바꾼 것이다.
- 엔진(`keq-mac engine`)은 이 프로세스를 뺀 전역 스테레오 프로세스 탭(원래 소리는 탭을 읽는 동안 묵음) + 기본 출력 장치를
  주 장치로 하는 비공개 집합 장치를 만들어, IOProc에서 탭을 `keq::Processor`에 통과시켜 출력에 쓴다(버퍼 128프레임).
  기본 출력 장치와 그 표본율을 따라가고(바뀌면 다시 만든다), 지금 장치의 .cfg를 250ms마다 보고(mtime·크기·inode) 바뀌면
  Windows APO와 같이 `ParseConfig` → `Design` → `Inbox().Publish()`로 넘긴다. .cfg가 없거나 `enabled 0`이면 원음 그대로.
  프로세스가 끝나면 탭이 사라져 소리는 원래대로 돌아온다.
- `engine.state` 첫 줄: `starting`, `running <UID>`, `permission_pending <UID>`(시작 뒤 5초 동안 IOProc이 불리지 않음:
  시스템 오디오 녹음 허용을 기다림), `permission_denied`, `unsupported`(macOS 14.2 미만), `error <글>`, `stopped`.
- TCC(시스템 오디오 녹음)는 실행 파일에 넣은 Info.plist(번들 ID `me.senhyo.kaleidoscope-eq`)로 묻고, ad-hoc 서명이라
  허용이 실행 파일의 CDHash에 묶인다. 엔진이 바뀌면(업데이트) 한 번 다시 묻는다.

다리(`keq-mac bridge`)는 §4와 같은 줄 프로토콜·op·필드를 쓰고 다음만 다르다.
- `install`: `dll_b64`·`endpoints`가 없다(있어도 무시). 실행 파일 복사, `endpoints/` 생성, LaunchAgent plist를 쓰고
  `launchctl bootout`(없으면 무시) 뒤 `launchctl bootstrap gui/<uid>`. 다시 부르면 업데이트. 답에 `"launchd":"loaded"`.
- `uninstall`: bootout, plist·실행 파일·상태·설정·기록을 모두 지운다(남기는 것 없음).
- `device.kind`: 블루투스 → `headphones`, 내장 장치는 데이터 소스 `hdpn` → `headphones`, `ispk` → `speakers`, 그 밖은 `other`.
  `headset`은 없다. `registered` = 엔진 설치됨(탭 하나가 모든 장치를 덮는다), `active` = 엔진이 돌고 있고 지금 그 장치를
  처리 중(`engine.state`가 `running <그 UID>`이고 .live의 pid가 같은 살아 있는 keq-mac).
- `engine`에 더하는 필드: `platform:"macos"`, `supported`(macOS 14.2 이상), `state`(위 상태, 프로세스가 없으면 `stopped`,
  설치 전 null), `permission`(`granted`|`pending`|`denied`|null), `current`(설치된 엔진이 이 다리의 실행 파일과 같은가;
  false면 화면이 업데이트를 권한다), `message`(state가 error일 때).
- 알림 `{"event":"device","device":…}`은 기본 장치나 그 상태, 또는 엔진 상태가 마지막으로 알린 것과 달라지면(1초 간격).
- 화면은 이 실행 파일을 자산 `eq-mac`(gzip+base64, `build.py`)에서 창 저장소 `bridge/eq-bridge-mac-<sha256 12자>/keq-mac`으로
  풀고 `sh -c 'chmod 700 "$1" && exec "$1" bridge' keq <경로>`로 띄운다(process 권한 `bridge-mac`, command `sh`).
- 시험용 환경 변수 `KEQ_HOME`: `~` 대신 쓰고, install/uninstall이 launchctl을 부르지 않는다.
