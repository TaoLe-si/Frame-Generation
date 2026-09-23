#!/usr/bin/env bash
# 编译 DLSS-G 可用性探针（链接 sl.interposer.lib，vk* 由 interposer 代理）
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
  "$ROOT\\fg_loop.cpp" \
  /link /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:"$OUT\\fg_loop.exe" \
  /LIBPATH:"$MSVC\\lib\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\ucrt\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\um\\x64" \
  "$SL\\lib\\x64\\sl.interposer.lib" user32.lib advapi32.lib opengl32.lib gdi32.lib

echo "BUILD DONE -> $OUT\\fg_loop.exe"
