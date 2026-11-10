# EQ for kaleidoscope

EQ를 몰라도 공간을 고르면 Windows의 모든 소리가 바뀌는 [kaleidoscope](https://github.com/eiaserinnys/kaleidoscope) 앱과, 그 앱이 쓰는 Windows 오디오 엔진(APO)의 소스입니다. 앱스토어에 올린 `eq` 앱의 `eq-engine` 자산(KaleidoscopeEQ.dll)은 이 저장소의 `engine/`을 빌드한 것입니다.

## 구성

| 부분 | 파일 | 하는 일 |
|---|---|---|
| 화면 | `client.js`, `graph.js`, `build.py` | 공간, 장르, 음색 고르기, 기기별 저장, EQ 곡선 그림. `build.py`가 앱 정의와 자산을 만든다. 헤드폰 서라운드 화면은 `build.py`의 `SURROUND`로 숨겨 두었다(엔진에는 0을 보낸다) |
| 다리 | `bridge.ps1` | 화면과 엔진 사이를 잇는 PowerShell. 기기 목록, 기기 바뀜 알림, 설정 파일 쓰기, 설치(UAC) |
| 엔진 | `engine/` | C++ APO. 장치의 EFX 목록 맨 앞에 붙어 모든 소리에 EQ, 잔향, 헤드폰 서라운드를 건다. 설치·제거 스크립트 `register.ps1` |

부품 사이의 약속은 `CONTRACT.md`, 엔진의 설계와 크기·테스트는 `engine/README.md`에 있습니다.

## 엔진 빌드와 검증

Visual Studio 2022 이상(C++ 데스크톱 워크로드)과 Windows SDK가 필요합니다.

```powershell
engine\build.ps1 -Test   # DLL, 오프라인 테스트(keq_test), 렌더 도구(keq_render)를 만들고 테스트를 돈다
engine\build.ps1 -Demo   # 서라운드 비교용 WAV(demo\spatial_0/50/100.wav)를 만든다
```

앱스토어 `eq` 앱에 실린 엔진은 이 저장소의 첫 커밋을 빌드한 것입니다. MSVC 링커가 DLL에 빌드 시각과 PDB 식별자를 넣기 때문에 다시 빌드한 DLL의 SHA-256은 실린 것과 다를 수 있습니다. 실린 DLL의 SHA-256은 앱의 client가 설치 전에 확인하는 값과 같습니다.

## 앱 빌드

```powershell
python build.py   # app.json, upload.json(앱 정의와 자산) 생성
```

## 라이선스

MIT
