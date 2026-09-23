#!/usr/bin/env bash
# 构建 DLSS 帧生成原生层 dlssmc_fg.dll
set -e
export PATH="/usr/bin:/bin:/usr/local/bin:$PATH"
# Git Bash 会把 /nologo、/link 这类参数当成 POSIX 路径转换掉（cl 收到 D:\Git\nologo），必须豁免
export MSYS2_ARG_CONV_EXCL='*'
export MSYS_NO_PATHCONV=1

MSVC='C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231'
SDK='C:\Program Files (x86)\Windows Kits\10'
SDKVER='10.0.26100.0'
VK='E:\DLSS for Minecraft\ref\vkheaders\include'
SL='D:\Backup\Downloads\streamline-sdk-v2.14.1'
JDK='D:\Java21'
ROOT='E:\DLSS for Minecraft\native'
OUT="$ROOT\\build"

CL="$MSVC\\bin\\Hostx64\\x64\\cl.exe"
export PATH="/c/Program Files/Microsoft Visual Studio/18/Enterprise/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64:$PATH"

mkdir -p '/e/DLSS for Minecraft/native/build'

SOURCE="$ROOT\\src\\dlssmc_fg.cpp"
TARGET="$OUT\\dlssmc_fg.dll"
COMPILE_FLAGS=(/DNDEBUG)
LINK_FLAGS=(/DLL)
if [[ "${1:-}" == "--check-reflex" ]]; then
    SOURCE="$ROOT\\test\\reflex_options_check.cpp"
    TARGET="$OUT\\reflex_options_check.exe"
    COMPILE_FLAGS=()
    LINK_FLAGS=(/SUBSYSTEM:CONSOLE)
fi

"$CL" /nologo /utf-8 /std:c++17 /O2 /MD /EHsc "${COMPILE_FLAGS[@]}" /DUNICODE /D_UNICODE \
  /I"$MSVC\\include" \
  /I"$SDK\\Include\\$SDKVER\\ucrt" \
  /I"$SDK\\Include\\$SDKVER\\um" \
  /I"$SDK\\Include\\$SDKVER\\shared" \
  /I"$SDK\\Include\\$SDKVER\\winrt" \
  /I"$VK" \
  /I"$SL\\include" \
  /I"$JDK\\include" \
  /I"$JDK\\include\\win32" \
  /Fo"$OUT\\\\" \
  "$SOURCE" \
  /link "${LINK_FLAGS[@]}" /MACHINE:X64 /OUT:"$TARGET" \
  /LIBPATH:"$MSVC\\lib\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\ucrt\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\um\\x64" \
  "$SL\\lib\\x64\\sl.interposer.lib" d3d11.lib dxgi.lib user32.lib gdi32.lib version.lib

echo "BUILD DONE -> $TARGET"
if [[ "${1:-}" == "--check-reflex" ]]; then
    PATH="$(cygpath -u "$SL")/bin/x64:$PATH" "$TARGET"
fi
