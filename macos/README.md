# macOS 엔진과 다리 (`keq-mac`)

Mac의 모든 소리에 Windows 판과 같은 EQ, 잔향, 헤드폰 서라운드를 겁니다. DSP는 `engine/src/dsp.cpp`를 고치지 않고 그대로 씁니다. 관리자 비밀번호, 오디오 드라이버, 재시동이 필요 없고 모든 것이 내 계정 안에 설치됩니다. 부품 사이의 약속은 `../CONTRACT.md` §5에 있습니다.

## 구성

| 파일 | 하는 일 |
|---|---|
| `keq_mac.mm` | 실행 파일 하나, 두 가지 모드. `bridge`: 화면과 이야기하는 다리(CONTRACT §4의 JSON 줄). `engine`: 사용자 LaunchAgent로 도는 엔진 |
| `compat.h`, `shim/xmmintrin.h` | `dsp.cpp`를 macOS clang으로 그대로 빌드하기 위한 것. MSVC 로캘 함수 이름, arm64의 FTZ(`_mm_getcsr/_mm_setcsr` → FPCR) |
| `Info.plist` | 실행 파일의 `__TEXT,__info_plist`에 넣는다. 번들 ID `me.senhyo.kaleidoscope-eq`와 시스템 오디오 녹음 안내 문구 |
| `build.sh` | `build/keq-mac`을 만들고 ad-hoc 서명한다 |
| `test/test_bridge.py` | 다리 프로토콜 시험(임시 `KEQ_HOME`, 실제 `~/Library`는 건드리지 않는다) |

## 어떻게 도나

- 엔진은 자기 프로세스를 뺀 전역 스테레오 **프로세스 탭**(macOS 14.2의 Core Audio tap)으로 모든 앱의 소리를 받고, 탭을 읽는 동안 원래 소리는 묵음이 됩니다. 기본 출력 장치를 주 장치로 하는 **비공개 집합 장치**의 IOProc이 탭을 `keq::Processor`에 통과시켜 그 장치로 내보냅니다. 버퍼는 128프레임(48 kHz에서 약 2.7 ms, 더하는 지연은 8 ms쯤)입니다. 64프레임은 시험한 Mac에서 과부하가 났습니다.
- 기본 출력 장치나 그 표본율이 바뀌면 집합 장치를 새로 만듭니다. 설정은 장치마다 `endpoints/<UID>.cfg`(Windows와 같은 본문)이고, 250 ms마다 바뀐 것을 보고 Windows APO와 같은 방법(우편함으로 통째로 바꿔 끼우고 짧게 섞기)으로 반영합니다. 파일이 없거나 `enabled 0`이면 원음 그대로 통과합니다.
- 엔진 프로세스가 끝나면(제거, 오류, 강제 종료) 탭도 함께 사라져 소리는 곧바로 원래대로 돌아옵니다.
- 다리는 화면이 창 저장소에 풀어 둔 실행 파일입니다. 설치(`install`)는 자기 자신을 `~/Library/Application Support/KaleidoscopeEQ/keq-mac`으로 복사하고 LaunchAgent `me.senhyo.kaleidoscope-eq`를 `launchctl bootstrap gui/<uid>`로 올립니다. 다시 설치하면 업데이트입니다.

## 시스템 오디오 녹음 허용

엔진이 처음 소리를 받으려 할 때 macOS가 「Kaleidoscope EQ」의 **시스템 오디오 녹음**을 허용할지 한 번 묻습니다. 허용 창이 떠 있는 동안 Core Audio 호출은 오류 없이 수십 초씩 멈추므로, 엔진은 그 일을 모두 별도 스레드에서 하고 5초 안에 소리가 들어오지 않으면 `permission_pending`을 알립니다. 화면은 이때 시스템 설정 > 개인정보 보호 및 보안 > 시스템 오디오 녹음을 안내합니다. 허용하기 전에 만든 IOProc은 허용한 뒤에도 돌지 않아서, 엔진은 소리가 없으면 20초마다(허용이 확인되면 3초 뒤) 집합 장치를 새로 만듭니다.

엔진은 ad-hoc 서명(고정 identifier `me.senhyo.kaleidoscope-eq`)이라 macOS가 허용을 실행 파일의 CDHash에 묶습니다. **엔진을 새로 빌드하거나 업데이트할 때마다 한 번 다시 묻습니다.** 터미널에서 직접 띄우면 허용이 터미널 앱(부모)의 것으로 계산되므로, 실제 허용 흐름은 LaunchAgent로 설치해서 확인합니다.

## 빌드와 시험

Apple Silicon Mac, Xcode 명령줄 도구가 필요합니다.

```sh
macos/build.sh          # macos/build/keq-mac (gitignore)
macos/build.sh --test   # 빌드 + 다리 프로토콜 시험
python3 build.py        # macos/build/keq-mac이 있으면 앱 정의에 자산 eq-mac과 실행 권한 bridge-mac을 넣는다
```

개발용 명령:

```sh
macos/build/keq-mac --list                                    # 출력 장치(UID, 종류, 파일 이름)
KEQ_HOME=/tmp/k macos/build/keq-mac engine --seconds 15 --log /tmp/k/engine.log   # 1초마다 입력·출력 크기 비율과 과부하를 기록
```

터미널에서 띄운 엔진은 설정을 `$KEQ_HOME/Library/Application Support/KaleidoscopeEQ/endpoints/`에서 읽습니다. `preamp -6`이면 비율이 0.501, `enabled 0`으로 바꾸면 다시 시작하지 않고 1.000이 됩니다.

## 제거

화면의 「엔진 제거」(다리의 `uninstall`)가 LaunchAgent를 내리고 plist, 실행 파일, 설정, 상태, 기록을 모두 지웁니다. 손으로 지울 때:

```sh
launchctl bootout gui/$(id -u)/me.senhyo.kaleidoscope-eq
rm -f ~/Library/LaunchAgents/me.senhyo.kaleidoscope-eq.plist
rm -rf ~/Library/Application\ Support/KaleidoscopeEQ ~/Library/Logs/KaleidoscopeEQ
```

시스템 설정의 시스템 오디오 녹음 목록에 남은 항목은 지워진 실행 파일의 것이라 아무 일도 하지 않습니다.

## 한계

- Apple Silicon(arm64), macOS 14.2 이상. 다리는 macOS 12부터 떠서 「지원하지 않는 판」이라고 답하고, Intel Mac에서는 실행 파일이 뜨지 않습니다(자산 크기 한도 때문에 universal로 빌드하지 않았습니다).
- 엔진을 업데이트할 때마다 시스템 오디오 녹음을 다시 묻습니다(ad-hoc 서명).
- 탭은 스테레오입니다. 채널이 더 많은 출력 장치에는 앞 두 채널에만 내보내고 나머지는 비웁니다.
- 엔진이 도는 동안 집합 장치는 소리가 없어도 돌아서 출력 장치가 쉬지 않습니다.
- 블루투스 출력은 모두 헤드폰으로 봅니다(블루투스 스피커도 헤드폰으로 보입니다).
