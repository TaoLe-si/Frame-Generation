#!/usr/bin/env bash
# 构建 D3D12 帧生成后端 dlssmc_dx.dll
set -e
export PATH="/usr/bin:/bin:/usr/local/bin:$PATH"
# Git Bash 会把 /nologo、/link 这类参数当成 POSIX 路径转换掉（cl 收到 D:\Git\nologo），必须豁免
export MSYS2_ARG_CONV_EXCL='*'
export MSYS_NO_PATHCONV=1
# 自定位：脚本跟着仓库走，别写死工作区绝对路径
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REFS="$(cd "$SELF_DIR/../.." && pwd)/DLSS-refs"

MSVC='C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231'
SDK='C:\Program Files (x86)\Windows Kits\10'
SDKVER='10.0.26100.0'
VK="$(cygpath -w "$REFS/vkheaders/include")"
JDK='D:\Java21'
ROOT="$(cygpath -w "$(cd "$SELF_DIR/.." && pwd)/native")"
VENDOR="$ROOT\\vendor\\intel"
OUT="$ROOT\\build"

CL="$MSVC\\bin\\Hostx64\\x64\\cl.exe"
export PATH="/c/Program Files/Microsoft Visual Studio/18/Enterprise/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64:$PATH"

mkdir -p "$SELF_DIR/../build"

"$CL" /nologo /utf-8 /std:c++17 /O2 /MD /EHsc /DNDEBUG /DUNICODE /D_UNICODE \
  /I"$MSVC\\include" \
  /I"$SDK\\Include\\$SDKVER\\ucrt" \
  /I"$SDK\\Include\\$SDKVER\\um" \
  /I"$SDK\\Include\\$SDKVER\\shared" \
  /I"$SDK\\Include\\$SDKVER\\winrt" \
  /I"$VENDOR\\inc\\xess_fg" \
  /I"$VENDOR\\inc\\xess" \
  /I"$VENDOR\\inc\\xell" \
  /I"$ROOT\\vendor\\amd\\include" \
  /I"$JDK\\include" \
  /I"$JDK\\include\\win32" \
  /Fo"$OUT\\\\" \
  "$ROOT\\src\\dlssmc_dx.cpp" \
  /link /DLL /MACHINE:X64 /OUT:"$OUT\\dlssmc_dx.dll" \
  /LIBPATH:"$MSVC\\lib\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\ucrt\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\um\\x64" \
  d3d11.lib d3d12.lib dxgi.lib user32.lib gdi32.lib version.lib

echo "BUILD DONE -> $OUT\\dlssmc_dx.dll"
