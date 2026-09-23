#!/usr/bin/env bash
set -e
export PATH="/usr/bin:/bin:/usr/local/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
export MSYS_NO_PATHCONV=1

MSVC='C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231'
SDK='C:\Program Files (x86)\Windows Kits\10'
SDKVER='10.0.26100.0'
JDK='D:\Java21'
SL_SDK='D:\Backup\Downloads\streamline-sdk-v2.14.1'
VK_SDK='D:\AndroidSdk\ndk\26.1.10909125\sources\third_party\vulkan\src\include\vulkan'
ROOT='E:\DLSS for Minecraft\native'
OUT="$ROOT\build"

export PATH="/c/Program Files/Microsoft Visual Studio/18/Enterprise/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64:$PATH"
mkdir -p '/e/DLSS for Minecraft/native/build'

INCLUDES=(
  "/I$MSVC/include"
  "/I$SDK/Include/$SDKVER/ucrt"
  "/I$SDK/Include/$SDKVER/um"
  "/I$SDK/Include/$SDKVER/shared"
  "/I$SDK/Include/$SDKVER/winrt"
  "/I$JDK/include"
  "/I$JDK/include/win32"
  "/I$SL_SDK/include"
  "/I$VK_SDK"
)

cl.exe /nologo /utf-8 /std:c++17 /O2 /MD /EHsc /DNDEBUG /DUNICODE /D_UNICODE \
  "${INCLUDES[@]}" \
  /Fo"$OUT/dlssmc_native.obj" /c "$ROOT/src/dlssmc_native.cpp"

link.exe /nologo /DLL /MACHINE:X64 /OUT:"$OUT/dlssmc_native.dll" \
  "$OUT/dlssmc_native.obj" \
  /LIBPATH:"$MSVC/lib/x64" \
  /LIBPATH:"$SDK/Lib/$SDKVER/ucrt/x64" \
  "/LIBPATH:$SDK/Lib/$SDKVER/um/x64" \
  "$SL_SDK/lib/x64/sl.interposer.lib" d3d11.lib dxgi.lib

echo "BUILD DONE -> $OUT/dlssmc_native.dll"
