
# 自定位：脚本跟着仓库走，别写死工作区绝对路径
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REFS="$(cd "$SELF_DIR/../.." && pwd)/DLSS-refs"#!/usr/bin/env bash
# Standalone DLL + test cache only; no game DLL or deployment output is touched.
# Optional arguments: absolute production source snapshot, then selected test modes.
set -euo pipefail
export MSYS2_ARG_CONV_EXCL='*' MSYS_NO_PATHCONV=1
MSVC='C:/Program Files/Microsoft Visual Studio/18/Enterprise/VC/Tools/MSVC/14.51.36231'
SDK='C:/Program Files (x86)/Windows Kits/10'
SDKVER='10.0.26100.0'
ROOT="$(cygpath -w "$(cd "$SELF_DIR/.." && pwd)/native")"
OUT="$ROOT/test/build/fsr-frame-check"
SOURCE="${1:-$ROOT/src/dlssmc_dx.cpp}"
export PATH="$MSVC/bin/Hostx64/x64:$PATH"
ls "$ROOT/test/build" >/dev/null
mkdir -p "$OUT"
compile=(/nologo /utf-8 /std:c++17 /O2 /MD /EHsc /DUNICODE /D_UNICODE
  /I"$MSVC/include" /I"$SDK/Include/$SDKVER/ucrt" /I"$SDK/Include/$SDKVER/um"
  /I"$SDK/Include/$SDKVER/shared" /I"$SDK/Include/$SDKVER/winrt"
  /I"$ROOT/vendor/intel/inc/xess_fg" /I"$ROOT/vendor/intel/inc/xess" /I"$ROOT/vendor/intel/inc/xell"
  /I"$ROOT/vendor/amd/include" /I'D:/Java21/include' /I'D:/Java21/include/win32')
link=(/LIBPATH:"$MSVC/lib/x64" /LIBPATH:"$SDK/Lib/$SDKVER/ucrt/x64" /LIBPATH:"$SDK/Lib/$SDKVER/um/x64"
  d3d11.lib d3d12.lib dxgi.lib user32.lib gdi32.lib version.lib)
"$MSVC/bin/Hostx64/x64/cl.exe" "${compile[@]}" /DNDEBUG \
  /Fo"$OUT/dlssmc_dx.obj" "$SOURCE" \
  /link /DLL /MACHINE:X64 /OUT:"$OUT/dlssmc_dx.dll" "${link[@]}"
"$MSVC/bin/Hostx64/x64/cl.exe" "${compile[@]}" \
  "/DDLSSMC_DX_SOURCE=\"$SOURCE\"" /Fo"$OUT/fsr_frame_check.obj" "$ROOT/test/fsr_frame_check.cpp" \
  /link /MACHINE:X64 /OUT:"$OUT/fsr_frame_check.exe" "${link[@]}"
modes=("${@:2}")
if [[ ${#modes[@]} -eq 0 ]]; then
  modes=(order states callback clock shared sr configure-error prepare-error close-error reset-error list-reset-error)
fi
result=0
for mode in "${modes[@]}"; do
  "$OUT/fsr_frame_check.exe" "$mode" || result=1
done
exit "$result"
