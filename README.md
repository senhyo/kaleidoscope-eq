# EQ for kaleidoscope

EQ를 몰라도 공간을 고르면 Windows의 모든 소리가 바뀌는 [kaleidoscope](https://github.com/eiaserinnys/kaleidoscope) 앱과, 그 앱이 쓰는 Windows 오디오 엔진(APO)의 소스입니다. 앱스토어에 올린 `eq` 앱의 `eq-engine` 자산(KaleidoscopeEQ.dll)은 이 저장소의 `engine/`을 빌드한 것입니다.

## 구성

| 부분 | 파일 | 하는 일 |
|---|---|---|
| 화면 | `client.js`, `graph.js`, `build.py` | 공간, 장르, 음색 고르기, 기기별 저장, EQ 곡선 그림. `build.py`가 앱 정의와 자산을 만든다. 헤드폰 서라운드 화면은 `build.py`의 `SURROUND`로 숨겨 두었다(엔진에는 0을 보낸다) |
| 다리 | `bridge.ps1` | 화면과 엔진 사이를 잇는 PowerShell. 기기 목록, 기기 바뀜 알림, 설정 파일 쓰기, 설치(UAC) |
| 엔진 | `engine/` | C++ APO. 장치의 EFX 목록 맨 앞에 붙어 모든 소리에 EQ, 잔향, 헤드폰 서라운드를 건다. 설치·제거 스크립트 `register.ps1` |
| macOS 엔진·다리 | `macos/` | 실행 파일 하나(`keq-mac`). Core Audio 프로세스 탭으로 Mac의 모든 소리를 받아 `engine/`과 같은 DSP를 건다. 다리 모드는 화면과 이야기하고 사용자 LaunchAgent로 엔진을 설치한다 |

부품 사이의 약속은 `CONTRACT.md`, 엔진의 설계와 크기·테스트는 `engine/README.md`에 있습니다.

## 엔진 빌드와 검증

Visual Studio 2022 이상(C++ 데스크톱 워크로드)과 Windows SDK가 필요합니다.

```powershell
engine\build.ps1 -Test   # DLL, 오프라인 테스트(keq_test), 렌더 도구(keq_render)를 만들고 테스트를 돈다
engine\build.ps1 -Demo   # 서라운드 비교용 WAV(demo\spatial_0/50/100.wav)를 만든다
```

앱스토어 `eq` 앱에 실린 엔진은 이 저장소의 첫 커밋을 빌드한 것입니다. MSVC 링커가 DLL에 빌드 시각과 PDB 식별자를 넣기 때문에 다시 빌드한 DLL의 SHA-256은 실린 것과 다를 수 있습니다. 실린 DLL의 SHA-256은 앱의 client가 설치 전에 확인하는 값과 같습니다.

## macOS

Apple Silicon, macOS 14.2 이상에서 돕니다. 관리자 비밀번호, 드라이버, 재시동 없이 내 계정에 설치됩니다.

```sh
macos/build.sh --test   # macos/build/keq-mac을 빌드하고 다리 프로토콜 시험을 돈다
```

- **어떻게 도나**: 엔진(`keq-mac engine`, LaunchAgent `me.senhyo.kaleidoscope-eq`)이 자기 프로세스를 뺀 전역 프로세스 탭으로 모든 앱의 소리를 받아(원래 소리는 묵음), 기본 출력 장치를 주 장치로 하는 비공개 집합 장치에서 `engine/src/dsp.cpp`로 EQ를 걸어 내보냅니다. 기본 출력 장치가 바뀌면 따라갑니다. 엔진이 끝나면 소리는 곧바로 원래대로 돌아옵니다.
- **허용**: 화면은 `sh`로 다리를 띄우므로 kaleidoscope가 처음 한 번 `sh` 실행을 묻고, 엔진이 처음 소리를 받을 때 macOS가 **시스템 오디오 녹음**(시스템 설정 > 개인정보 보호 및 보안)을 한 번 묻습니다.
- **제거**: 화면의 「엔진 제거」가 LaunchAgent, 실행 파일, 설정, 기록을 모두 지웁니다.
- **한계**: 엔진은 ad-hoc 서명이라 macOS의 허용이 실행 파일에 묶여, 엔진을 업데이트할 때마다 시스템 오디오 녹음을 다시 묻습니다. Intel Mac과 macOS 14.2 미만은 지원하지 않습니다.

자세한 설계와 손 시험은 `macos/README.md`, 파일과 프로토콜은 `CONTRACT.md` §5에 있습니다.

## 앱 빌드

```powershell
python build.py   # app.json, upload.json(앱 정의와 자산) 생성
```

`macos/build/keq-mac`이 있으면 macOS 엔진 자산(`eq-mac`)과 실행 권한(`bridge-mac`, `sh`)이 함께 들어갑니다. 없으면 정의는 Windows 판 그대로입니다.

## 라이선스

MIT
