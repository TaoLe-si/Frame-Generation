#!/usr/bin/env bash
# 编译 XeSS-FG / XeLL 能力探针（cl.exe 直编；不链接厂商 import lib，运行时按路径 LoadLibrary）
set -e
# 自定位：脚本跟着仓库走，别写死工作区绝对路径
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REFS="$(cd "$SELF_DIR/../.." && pwd)/DLSS-refs"
export PATH="/usr/bin:/bin:/usr/local/bin:$PATH"

MSVC='C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231'
SDK='C:\Program Files (x86)\Windows Kits\10'
SDKVER='10.0.26100.0'
ROOT="$(cygpath -w "$(cd "$SELF_DIR/.." && pwd)/native")"
OUT="$ROOT\\test\\build"

export PATH="/c/Program Files/Microsoft Visual Studio/18/Enterprise/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64:$PATH"
mkdir -p "$SELF_DIR/build"

cl.exe /nologo /utf-8 /std:c++17 /O2 /MD /EHsc /DUNICODE /D_UNICODE \
  /I"$MSVC\\include" \
  /I"$SDK\\Include\\$SDKVER\\ucrt" \
  /I"$SDK\\Include\\$SDKVER\\um" \
  /I"$SDK\\Include\\$SDKVER\\shared" \
  /I"$SDK\\Include\\$SDKVER\\winrt" \
  /I"$ROOT\\vendor\\intel" \
  /Fo"$OUT\\\\" \
  "$ROOT\\test\\xess_probe.cpp" \
  /link /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:"$OUT\\xess_probe.exe" \
  /LIBPATH:"$MSVC\\lib\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\ucrt\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\um\\x64" \
  d3d12.lib dxgi.lib user32.lib gdi32.lib

echo "BUILD DONE -> $OUT\\xess_probe.exe"
