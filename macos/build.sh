#!/bin/sh
# macOS 엔진·다리(keq-mac)를 빌드한다. Apple Silicon, Xcode 명령줄 도구(clang++, codesign)가 필요하다.
#   macos/build.sh          → macos/build/keq-mac
#   macos/build.sh --test   → 빌드한 뒤 다리 프로토콜 시험(macos/test/test_bridge.py)을 돈다
# engine/src/dsp.cpp는 고치지 않고 그대로 넣는다(-include compat.h: MSVC 로캘 함수, -Ishim: arm64용 _mm_getcsr/_mm_setcsr).
# 최소 판은 12.0으로 둔다. 탭 API(macOS 14.2)는 약하게 연결되고 실행할 때 확인하므로, 14.2보다 낮은 macOS에서도
# 다리는 떠서 「지원하지 않는 판」이라고 답할 수 있다(엔진은 14.2 미만이면 unsupported를 쓰고 끝난다).
# Info.plist는 실행 파일의 __TEXT,__info_plist에 넣는다. TCC(시스템 오디오 녹음)는 여기의 번들 ID와
# NSAudioCaptureUsageDescription을 보고 묻는다. 서명은 ad-hoc(-s -)에 고정 identifier: 허용은 CDHash에 묶여
# 엔진을 새로 빌드할 때마다 한 번 다시 묻는다.
set -eu
cd "$(dirname "$0")"
ENG=../engine/src
OUT=build
ID=me.senhyo.kaleidoscope-eq
MIN=12.0
mkdir -p "$OUT"
CXX="clang++ -std=c++17 -fno-exceptions -arch arm64 -mmacosx-version-min=$MIN -Wall -Wextra -Wno-unused-parameter"
$CXX -Os -include compat.h -Ishim -I"$ENG" -c "$ENG/dsp.cpp" -o "$OUT/dsp.o"
$CXX -Oz -fobjc-arc -fno-objc-exceptions -I"$ENG" -Werror=unguarded-availability-new -c keq_mac.mm -o "$OUT/keq_mac.o"
clang++ -arch arm64 -mmacosx-version-min=$MIN "$OUT/keq_mac.o" "$OUT/dsp.o" -o "$OUT/keq-mac" \
  -framework CoreAudio -framework Foundation -Wl,-dead_strip -Wl,-S -Wl,-x \
  -Wl,-sectcreate,__TEXT,__info_plist,Info.plist
codesign -s - -f --identifier "$ID" "$OUT/keq-mac"
rm -f "$OUT/dsp.o" "$OUT/keq_mac.o"
SIZE=$(wc -c < "$OUT/keq-mac" | tr -d ' ')
GZ=$(gzip -9n < "$OUT/keq-mac" | wc -c | tr -d ' ')
echo "keq-mac: ${SIZE} B, gzip ${GZ} B ($OUT/keq-mac)"
if [ "${1:-}" = "--test" ]; then python3 test/test_bridge.py; fi
