#!/usr/bin/env bash
# 编译 Vulkan 桥接验证程序
set -e
export PATH="/usr/bin:/bin:/usr/local/bin:$PATH"

MSVC='C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231'
SDK='C:\Program Files (x86)\Windows Kits\10'
SDKVER='10.0.26100.0'
VK='E:\DLSS for Minecraft\ref\vkheaders\include'
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
  /I"$SDK\\Include\\$SDKVER\\winrt" \
  /I"$VK" \
  /Fo"$OUT\\\\" \
  "$ROOT\\bridge_test_vk.cpp" \
  /link /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:"$OUT\\bridge_test_vk.exe" \
  /LIBPATH:"$MSVC\\lib\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\ucrt\\x64" \
  /LIBPATH:"$SDK\\Lib\\$SDKVER\\um\\x64" \
  d3d11.lib dxgi.lib opengl32.lib user32.lib gdi32.lib

echo "BUILD DONE -> $OUT\\bridge_test_vk.exe"
