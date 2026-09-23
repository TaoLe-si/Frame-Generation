#!/usr/bin/env bash
# 构建无窗口 A/B 探针，运行时加载固定版本的 Streamline。
set -e
export PATH="/usr/bin:/bin:/usr/local/bin:$PATH"

MSVC='C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231'
SDK='C:\Program Files (x86)\Windows Kits\10'
SDKVER='10.0.26100.0'
VK='E:\DLSS for Minecraft\ref\vkheaders\include'
SL='D:\Backup\Downloads\streamline-sdk-v2.14.1'
ROOT='E:\DLSS for Minecraft\native\test'
OUT="$ROOT\\build"

CL="$MSVC\\bin\\Hostx64\\x64\\cl.exe"
export PATH="/c/Program Files/Microsoft Visual Studio/18/Enterprise/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64:$PATH"

mkdir -p '/e/DLSS for Minecraft/native/test/build'

"$CL" /nologo /std:c++17 /O2 /MD /EHsc /DUNICODE /D_UNICODE /DVK_USE_PLATFORM_WIN32_KHR \
  /I"$MSVC\\include" \
  /I"$SDK\\Include\\$SDKVER\\ucrt" \
  /I"$SDK\\Include\\$SDKVER\\um" \
  /I"$SDK\\Include\\$SDKVER\\shared" \
  /I"$VK" \
  /I"$SL\\include" \
  /Fo"$OUT\\\\" \
  "$ROOT\\fg_probe.cpp" \
  /link /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:"$OUT\\fg_probe.exe" \
  /LIBPATH:"$MSVC\\lib\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\ucrt\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\um\\x64" \
  user32.lib

echo "BUILD DONE -> $OUT\\fg_probe.exe"
