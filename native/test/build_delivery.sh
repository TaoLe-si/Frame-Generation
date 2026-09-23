#!/usr/bin/env bash
# 编译 GL->D3D12 交付路径探针（cl.exe 直编，本机无 cmake / cmd.exe 不可用）
set -e
# 自定位：脚本跟着仓库走，别写死工作区绝对路径
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REFS="$(cd "$SELF_DIR/../.." && pwd)/DLSS-refs"
export PATH="/usr/bin:/bin:/usr/local/bin:$PATH"

MSVC='C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231'
SDK='C:\Program Files (x86)\Windows Kits\10'
SDKVER='10.0.26100.0'
ROOT="$(cygpath -w "$SELF_DIR")"
OUT="$ROOT\\build"

export PATH="/c/Program Files/Microsoft Visual Studio/18/Enterprise/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64:$PATH"
mkdir -p "$SELF_DIR/build"

cl.exe /nologo /utf-8 /std:c++17 /O2 /MD /EHsc /DUNICODE /D_UNICODE \
  /I"$MSVC\\include" \
  /I"$SDK\\Include\\$SDKVER\\ucrt" \
  /I"$SDK\\Include\\$SDKVER\\um" \
  /I"$SDK\\Include\\$SDKVER\\shared" \
  /I"$SDK\\Include\\$SDKVER\\winrt" \
  /Fo"$OUT\\\\" \
  "$ROOT\\delivery_probe.cpp" \
  /link /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:"$OUT\\delivery_probe.exe" \
  /LIBPATH:"$MSVC\\lib\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\ucrt\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\um\\x64" \
  d3d11.lib d3d12.lib dxgi.lib opengl32.lib user32.lib gdi32.lib

echo "BUILD DONE -> $OUT\\delivery_probe.exe"
