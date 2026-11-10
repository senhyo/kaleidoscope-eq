# 엔진 — Kaleidoscope EQ APO

Windows 오디오 엔진(audiodg)이 출력 장치마다 불러 쓰는 Audio Processing Object(APO) DLL과 등록 스크립트.
Equalizer APO 없이 시스템 소리 전체에 EQ·잔향·헤드폰 공간감을 건다. 계약은 `../CONTRACT.md`.

```
src/dsp.{h,cpp}     DSP 핵심(플랫폼 무관): 설정 해석, 계수 설계, 실시간 처리기
src/files.{h,cpp}   .cfg 읽기·감시 스레드, .live 쓰기 (CONTRACT §1, §2)
src/apo.{h,cpp}     COM APO (IAudioProcessingObject/RT/Configuration, IAudioSystemEffects2)
src/dllmain.cpp     클래스 팩토리, DllRegisterServer/DllUnregisterServer
register.ps1        설치·제거 (CONTRACT §3)
build.ps1           x64 Release 빌드 (-Test 면 시험까지, -Demo 면 청취용 A/B 파일까지)
tools/render.cpp    오프라인 WAV 렌더러(keq_render.exe): 같은 dsp.cpp로 파일을 처리한다
test/harness.cpp    오프라인 시험: 같은 dsp.cpp/files.cpp + 빌드된 DLL을 COM으로 직접 구동
test/register.tests.ps1  register.ps1의 붙이기/떼기 논리를 HKCU 임시 키에서 시험
```

## 빌드와 시험

```
powershell -ExecutionPolicy Bypass -File engine\build.ps1 -Test -Demo
```
Visual Studio(C++ x64 도구 집합)와 Windows SDK(`audioenginebaseapo.h`, `audiomediatypecrt.lib`)가 필요하다.
DLL은 OS DLL에만 기대므로 audiodg 쪽에 VC++ 재배포 패키지가 없어도 된다. 산출물: `build\KaleidoscopeEQ.dll`(약 51 KB).

**들어 보기.** `build\keq_render.exe in.wav out.wav "<설정>"`은 APO와 같은 DSP로 WAV를 처리한다(16/24/32비트 PCM·float 입력,
float 출력, 설정 줄은 `;`로 이어 쓴다. 예: `"spatial 1;peak 100 1 3"`). `-Demo`는 15초짜리 스테레오 시험곡
(가운데 킥·베이스·스네어·멜로디, 왼쪽 기타·셰이커, 오른쪽 일렉트릭 피아노·하이햇, 넓은 패드, 약한 잔향)을
`build\demo\source.wav`로 합성하고 `spatial_0.wav`(원음과 비트 단위로 같음), `spatial_50.wav`, `spatial_100.wav`를 만든다.
기호는 `build\KaleidoscopeEQ.pdb`에 따로 있고 DLL에는 경로 없이 파일 이름만 적힌다.

### 크기

DLL은 다리의 `install`이 base64로 실어 나르므로 작아야 한다. 처음 빌드(정적 CRT)는 240 KB였고 그 대부분이 우리 코드가 아니었다
(정적 UCRT 약 135 KB, C++ 예외 런타임 약 15 KB, `std::from_chars` 약 10 KB, `std::wstring`이 끌어온 벡터 알고리즘 약 10 KB).

- **하이브리드 CRT**: `/MT`로 컴파일하되 UCRT만 동적으로 링크한다(`/NODEFAULTLIB:libucrt.lib ucrt.lib`).
  UCRT는 Windows 10부터 OS 구성 요소다(`System32\ucrtbase.dll`, `api-ms-win-crt-*` API 집합, TrustedInstaller 소유,
  Windows Update로 갱신). OS에 없는 `vcruntime140.dll` 쪽(DLL 진입점, `/GS` 쿠키, `/guard:cf` 로드 설정, memcpy)은 계속 정적으로 들어간다.
  가져오는 DLL: kernel32, ole32, advapi32, shell32, `api-ms-win-crt-*`.
- **예외·RTTI 없음**(`/EHs-c- /GR-`): 던지는 코드가 없다. STL 컨테이너 대신 고정 버퍼(`Floats`, `wchar_t[MAX_PATH]`)를 쓴다.
  DLL 안의 `operator new`는 던지지 않고 메모리가 없으면 nullptr를 돌려준다(`dllmain.cpp`). 호출하는 곳은 모두 nullptr를 확인한다.
  CRT의 `operator new`는 `std::bad_alloc` 하나 때문에 예외 런타임 전체를 끌고 들어온다.
- **숫자 해석**: `.cfg`의 숫자는 `std::from_chars`와 같은 문법(`[-]숫자[.숫자][e[+-]숫자]`, inf/nan 거부, 0으로 언더플로하면 거부)을
  직접 검사한 뒤 "C" 로캘의 `_strtod_l`로 바꾼다. 그래서 audiodg 안의 다른 구성 요소가 로캘을 바꿔도 값은 같다.
  `from_chars`와 400만 개 토큰을 비교해 받아들이는 토큰과 값(비트)이 모두 같았다. 다른 점은 64자 이상인 토큰을 거부한다는 것 하나다.
- **`/O1 /GL` + `/LTCG /OPT:REF,ICF`**: DSP는 double 산술이 병목이라 `/O2`도 더 빠르지 않았다(둘 다 약 0.4%, 측정 잡음 안).
- `/GS`, `/guard:cf`, `/CETCOMPAT`는 유지한다. `/guard:cf`가 512 B, `/GS`는 0에 가깝다.

## 어디에 붙나: EFX(끝점 효과), 합성 목록의 맨 앞

APO가 들어갈 수 있는 칸은 셋이다. SFX(스트림마다, 섞기 전), MFX(처리 모드마다), EFX(끝점마다, 모든 소리가 섞인 뒤).
**EFX를 고른 이유**

- **한 장치에 인스턴스 하나.** 잔향·공간감은 앱마다 따로 걸리면 안 된다. SFX는 스트림마다 인스턴스가 생겨
  잔향이 여러 번 겹치고 CPU도 스트림 수만큼 든다. EFX는 섞인 결과에 한 번만 건다.
- **모든 처리 모드에 걸린다.** RAW 모드 스트림(일부 게임·DAW·통화 앱)은 SFX/MFX를 건너뛰지만 EFX는 거친다.
- **장치 형식 하나로 받는다.** 엔진이 정한 끝점 혼합 형식(float32, 장치 샘플레이트·채널) 하나만 처리하면 된다.

**제조사 효과를 지키는 방법.** Windows 10 1803부터 각 칸은 CLSID *목록*(`PKEY_CompositeFX_*`, MULTI_SZ)을 받는다.
이 PC의 Realtek 장치도 이미 이 형식(`{d04e05a6-…},13/14/15`)을 쓴다. register.ps1은
`PKEY_CompositeFX_EndpointEffectClsid`(`…,15`) 목록 **맨 앞에** 우리 CLSID를 넣을 뿐이다.

- 목록이 이미 있으면: `[우리, 제조사 EFX…]`
- 예전 한 칸짜리 `PKEY_FX_EndpointEffectClsid`(`…,7`)만 있으면: 그 값을 이어 `[우리, 그 값]`
- 아무것도 없으면: `[우리]` (FxProperties 키가 없으면 복원 권한으로 만든다)

SFX/MFX 칸과 다른 값은 건드리지 않는다. 맨 앞인 까닭은 제조사의 끝점 처리(스피커 보호·리미터·보정)가
사용자 EQ의 부스트 *뒤에* 와야 하기 때문이다. Equalizer APO는 제조사 APO 자리를 차지하고 안에서 원래 APO를
자식으로 불러 모든 인터페이스를 대신 전달하는 방식인데, 합성 목록은 그런 중계 없이 엔진이 직접 순서대로 부른다.
(Equalizer APO 코드는 쓰지 않았다. APO 골격은 Microsoft 문서와 SDK 헤더를 따랐다.)

처음 붙일 때 원래 값(또는 "없었음")을 `HKLM\SOFTWARE\KaleidoscopeEQ\Endpoints\<GUID>`에 보관하고,
제거할 때 우리 CLSID만 빼서 되돌린다. 원래 없던 값은 지우고, 우리가 만든 FxProperties 키는 비면 지운다.
설치 뒤 다른 프로그램이 목록에 무언가를 더했다면 그것은 남긴다.

## DSP

처리 순서(스테레오 쌍 = 채널 0, 1):  프리앰프 → 피킹 바이쿼드 N개 → 잔향 → 공간감.

- **EQ**: RBJ cookbook peaking. `client.js`의 `peaking()`과 같은 식이며 계수와 상태는 double, TDF-II.
  시험에서 측정한 응답이 client.js가 그리는 곡선과 0.0000 dB 차이로 같다. 이득 0인 줄은 건너뛴다.
- **잔향**: 8줄 FDN(29.7~61.9 ms, 직교 Hadamard 혼합, 줄마다 7 kHz 감쇠 필터로 꼬리가 점점 어두워진다),
  앞단 Schroeder allpass 2개, predelay. `decay`는 RT60(줄마다 피드백 `10^(-3·len/(fs·decay))`).
  `wet` dB는 client.js의 임펄스 응답과 같은 뜻(잔향 에너지 / 직접음 에너지)이다. 감시 스레드가 같은 잔향을 임펄스로 한 번 돌려 보정한다.
- **공간감(0~1)**: 아래 「헤드폰 가상 서라운드」.
- **채널**: 1채널이면 EQ와 잔향(모노). 3채널 이상이면 모든 채널(16채널까지)에 프리앰프와 EQ를 걸고,
  잔향·공간감은 앞쪽 L/R에만 건다. 샘플레이트는 엔진이 정한 대로 받는다(44.1~192 kHz에서 시험).
- **바이패스**: 파일이 없거나 `enabled 0`이거나 설정이 모두 중립이면 버퍼를 한 비트도 건드리지 않는다.

### 헤드폰 가상 서라운드 (`spatial`)

스테레오를 가상 스피커 다섯 개(가운데, 앞 ±30°, 옆뒤 ±110°)와 작은 방으로 펼쳐 두 귀로 옮긴다(Volvo Individual Stage,
Apple 「스테레오 공간화」와 같은 갈래). 측정 HRIR 데이터(라이선스)는 쓰지 않고 머리 모형을 식으로 계산한다.

- **머리 모형 (`SpeakerPair`)**: Brown & Duda(1998)의 구조 모형. 귀는 ±100°.
  - 두 귀 시간차: Woodworth 구형 머리 `a/c·(θ+sinθ)`(a = 8.75 cm, 옆 각도 기준), 선형 보간 분수 지연.
  - 머리 그림자: 귀마다 1극 1영점 필터 `(1+α·sτ)/(1+sτ)`, `α(입사각)`는 B&D 식(정면 2 → 뒤쪽 150°에서 0.1).
    쌍마다 가까운 귀의 고음과 두 귀의 저음을 기준으로 정규화한다. 고음에서는 머리가 두 귀를 갈라 놓아
    헤드폰과 스피커가 같고, 저음에서는 머리가 투명해 두 귀가 각 스피커를 같은 크기로 듣는다.
    필터의 위상이 저음 ITD를 실제 머리처럼 키운다: 30°에서 0.38 ms, 110°에서 0.78 ms(Kuhn의 저음 ITD `3a/c·sin θ`로는 0.38 / 0.72 ms).
  - 귓바퀴: B&D 첫 귓바퀴 반사(지연 `cos(θ/2)+2` 샘플 @44.1 kHz, 방위에 따라 노치가 7.4→8.6 kHz로 움직임)와
    뒤에서 오는 소리의 귓바퀴 그늘(3 kHz 위 하이 셸프 최대 −8 dB). 둘 다 `(1−cos θ)/2`에 비례해 정면에서 0이 된다.
    그래서 앞 스피커는 빗살(comb)이 거의 없고 뒤 스피커는 고음이 분명히 어둡다(가까운 귀, 500 Hz 기준
    5/8/12 kHz: 앞 +4.3/+4.5/+4.8 dB, 뒤 −0.3/−2.7/−1.3 dB). 이것이 앞뒤 단서다.
- **앞 무대**: 가운데 M = (L+R)/2는 실제 가운데 스피커로 보내고(정면이라 원음 그대로), 옆 성분 S = (L−R)/2만
  ±30° 쌍으로 보낸다. 스피커 삼각형에서 허상 가운데(phantom centre)의 빗살을 평탄하게 고친 것과 같다.
  두 스피커로 모노를 재생하면 귀마다 0.26 ms 늦은 사본이 겹쳐 2 kHz 근처에 약 9 dB의 골이 생기는데(목소리가 거칠어진다),
  이 구조에서는 가운데 소리가 필터를 거치지 않는다. 별도의 상관 추정·추출기를 두지 않아도 되므로 펌핑도 없다.
  왼쪽 끝 음원은 저음에서 두 귀가 거의 같은 크기로 듣고(스피커처럼), 고음은 오른쪽 귀에서 14 dB 어둡다.
- **서라운드 (±110°)**: `±S + 0.6·M`을 velvet-noise 비상관기(2.25 ms 칸마다 무작위 위치·부호의 탭 하나, 7~43 ms,
  25 ms로 감쇠)로 흩뿌려 옆뒤 스피커로 보낸다. 좌우의 탭 열이 달라 모노 음악도 머리를 감싼다.
  되먹임이 없어 예전의 allpass처럼 금속성 울림이 생기지 않는다.
- **방**: 귀마다 다른 초기 반사 8개(3.1~28.6 ms, 같은 쪽·반대쪽 스피커에서 번갈아, 공통 간격이 없는 지연이라
  빗살 대신 촘촘한 물결), 6 kHz 저역통과. 머리 밖으로 소리를 꺼내는(externalisation) 가장 큰 단서다.
- **세기**: S는 원음 → 스피커 쌍으로 선형으로 바뀌고 방·서라운드는 세기에 비례한다. 방·서라운드 입력은
  120 Hz 고역통과라 저음은 마르고 단단하게 남는다. 가운데 음원이 얻는 에너지 E = 0.5² + (0.7·0.6)²만큼
  원음 경로를 `(1+s²E)^(−1/4)`로 줄인다(보탠 에너지의 절반만 갚는다: 중음 +0.8 dB, 저음 −0.8 dB 이내).
- **잰 값**(`build.ps1 -Test`, 48 kHz): 가운데 핑크 잡음 레벨 세기 0/0.5/1에서 +0.00/+0.10/+0.30 dB(150 Hz 아래 +0.18/+0.12),
  상관 0.6 핑크 잡음의 원음 대비 차이 −24.6(0.1) → −16.6 → −10.6 → −7.1 → −4.6 dB(1), 레벨 변화 −0.02 dB,
  두 귀 상호상관 IACC(500 Hz~2 kHz, ±1 ms) 0.61 → 0.39. 전체 대역 상관은 0.60 → 0.65로 오히려 오른다.
  500 Hz 아래에서는 실제 스피커처럼 두 귀가 같은 소리를 듣기 때문이다. 예전 시험이 쓴 「전체 상관이 떨어지면 들린다」는
  틀린 잣대였고, 예전 설계는 그 수치는 바꿨지만 머리 단서는 틀렸다(왼쪽 끝 음원의 ITD 0.6 ms, 저음 ILD 7.7 dB로
  헤드폰 크로스피드에 그쳤고, 가운데 저음이 +3 dB 불었다).
- CPU: 10밴드 + 잔향 + 공간감, 48 kHz 스테레오에서 한 코어의 약 1.1~1.4%(공간감 몫 약 0.7%). 할당과 잠금 없이
  모든 버퍼를 Init에서 잡는다(192 kHz에서 기록 버퍼 약 256 KB).

### 실시간 규칙

- `APOProcess`: 잠금·할당·입출력이 없다. 메모리는 `LockForProcess`에서 미리 잡는다.
- 감시 스레드(낮은 우선순위)가 50 ms마다 `.cfg`의 시각·크기를 보고, 바뀌면 읽고 설계하고 보정해서
  wait-free 3중 버퍼(`Mailbox`)에 넣는다. 실시간 스레드는 블록 시작에 최신 값을 집어 간다.
- 바뀔 때마다 30 ms 동안 옛 매개변수와 새 매개변수를 함께 돌려 smoothstep으로 섞는다(EQ는 필터 뱅크 두 벌,
  나머지는 이득 보간, predelay는 두 탭을 섞는다). 비어 있는 지연선에서 다시 시작하는 단계(잔향, 공간감)는 입력도
  같은 시간 동안 서서히 넣는다. 그러지 않으면 첫 소리가 지연 탭에 닿는 순간 계단이 생긴다.
- 반복 상태의 비정규수(denormal)는 처리하는 동안 FTZ/DAZ로 막는다. 출력에 NaN/Inf가 생기면 그 블록을 무음으로 하고 상태를 비운다.
- 엔진이 "무음"으로 표시한 버퍼: 잔향 꼬리가 남아 있으면 0으로 채워 꼬리를 낸다. −120 dBFS 아래로 내려가면 다시 손대지 않는다.

### `.live` (CONTRACT §2)

`LockForProcess` 때 `%ProgramData%\KaleidoscopeEQ\endpoints\<GUID>.live`를 임시 파일 + 교체로 쓴다.
```
pid 12345
2026-11-10T03:06:40Z
```
첫 줄은 `^\s*pid\s+(\d+)`에 맞는다. 끝점 GUID는 `APOInitSystemEffects2.pDeviceCollection`의 마지막 장치 ID에서 얻고,
없으면 `PKEY_AudioEndpoint_GUID`에서 얻는다.

## register.ps1

관리자 PowerShell(5.1/7)에서 `-File`로 실행한다. `-Endpoints`는 쉼표로 이은 문자열 하나이고, 중괄호와 대소문자는 정규화한다.
설치 순서: audiosrv 정지(audiodg 종료 대기) → DLL을 `%ProgramFiles%\KaleidoscopeEQ\`에 복사 → `regsvr32` →
`%ProgramData%\KaleidoscopeEQ\endpoints` 만들고 Users·LOCAL SERVICE에 수정 권한(SID로 지정) → `HKLM\SOFTWARE\KaleidoscopeEQ`
(`InstallPath`, `Version`은 DLL 버전 리소스에서 읽음) → 장치마다 붙이기 → audiosrv와 멈췄던 종속 서비스 시작(실패해도 `finally`에서).
다시 실행하면 업데이트로 동작한다. 원래 백업은 그대로 두고, 이미 붙어 있던 장치는 계속 붙여 둔다.
`-Uninstall`은 모든 장치를 원래대로 되돌리고 COM 등록, 프로그램 폴더, ProgramData 폴더, HKLM 상태를 지운다.
스크립트는 ASCII만 쓴다. Windows PowerShell 5.1은 BOM 없는 스크립트를 ANSI(CP949)로 읽어 한글 주석이 다음 줄을 삼킨 일이 있었다.

## 알려진 위험

- **"오디오 개선" 끔**: 장치 속성에서 개선(시스템 효과)을 끄면(`PKEY_AudioEndpoint_Disable_SysFx`) Windows가 모든 APO를 건너뛴다.
  등록은 되지만 `.live`가 생기지 않는다. 사용자의 선택이므로 스크립트는 바꾸지 않는다.
- **배타 모드·오프로드**: WASAPI 배타 모드 스트림과 하드웨어 오프로드 스트림은 APO를 거치지 않는다.
- **합성 목록과 단일 키의 우선순위 (실기기에서 확인 필요)**: 칸마다 합성 목록이 있으면 엔진이 그것을 쓴다고 가정했다.
  이 가정대로라면 이 PC의 Realtek 끝점처럼 단일 키 `…,7`(지금은 Equalizer APO가 쓴 값)과 목록 `…,15`가 함께 있을 때 목록이 이긴다.
  어떤 장치에서 붙었는데 `.live`가 안 생기면 가장 먼저 의심할 곳이다.
- **APO를 막는 드라이버**: 일부 드라이버(특히 자체 DSP를 쓰는 노트북·USB DAC·Dolby/DTS 묶음)는 끝점 효과를 쓰지 않거나
  드라이버 업데이트 때 FxProperties를 다시 써서 우리 항목을 지운다. 다시 설치하면 다시 붙는다.
- **서명**: DLL은 서명되어 있지 않다. 지금의 Windows는 시스템 효과 APO에 서명을 요구하지 않는다(Equalizer APO도 같은 조건으로 돈다).
  `HKLM\…\Audio\DisableProtectedAudioDG`(이 PC에는 Equalizer APO가 1로 둠)는 Vista/7 시절 보호 audiodg용 스위치라 건드리지 않는다.
  보안 정책(WDAC, 스마트 앱 컨트롤)이 켜진 PC에서는 서명되지 않은 DLL을 audiodg가 불러오지 못할 수 있다.
- **Equalizer APO와 함께 있을 때**: Equalizer APO가 붙인 단일 키는 그대로 두므로 두 엔진이 함께 돌 수 있다(합성 목록이 이기는 칸은 제외).
  Equalizer APO를 먼저 제거하길 권한다.
