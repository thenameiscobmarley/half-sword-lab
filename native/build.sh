#!/usr/bin/env bash
# Builds dist/version.dll (the Half Sword Lab DLL) with the MinGW-w64 cross compiler.
#   Arch/CachyOS: sudo pacman -S mingw-w64-gcc      Debian/Mint: sudo apt install g++-mingw-w64-x86-64
# Everything (ImGui, MinHook, libstdc++) is linked statically, so the DLL has no runtime dependencies
# beyond Windows/Wine system DLLs.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

CXX=${CXX:-x86_64-w64-mingw32-g++}
CC=${CC:-x86_64-w64-mingw32-gcc}
command -v "$CXX" >/dev/null || { echo "missing $CXX (install mingw-w64)"; exit 1; }

OUT=build
mkdir -p "$OUT" dist
IMGUI=third_party/imgui
MH=third_party/minhook

CXXFLAGS="-O2 -std=c++17 -DNDEBUG -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -Isrc -I$IMGUI -I$MH/include -fno-strict-aliasing"
objs=()

compile() {  # src obj compiler flags...
    local src=$1 obj=$2; shift 2
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "${FORCE:-0}" = 1 ]; then
        echo "  cc $src"
        "$@" -c "$src" -o "$obj"
    fi
    objs+=("$obj")
}

for f in imgui imgui_draw imgui_tables imgui_widgets; do
    compile "$IMGUI/$f.cpp" "$OUT/$f.o" "$CXX" $CXXFLAGS
done
for f in win32 dx11 dx12; do
    compile "$IMGUI/backends/imgui_impl_$f.cpp" "$OUT/imgui_impl_$f.o" "$CXX" $CXXFLAGS
done
for f in buffer hook trampoline hde/hde64; do
    compile "$MH/src/$f.c" "$OUT/mh_$(basename $f).o" "$CC" -O2 -I$MH/include
done
# Our own sources always rebuild when headers change.
for f in src/*.cpp; do
    FORCE=1 compile "$f" "$OUT/$(basename "${f%.cpp}").o" "$CXX" $CXXFLAGS -Wall -Wno-unused-function
done

"$CXX" -shared -o dist/version.dll "${objs[@]}" src/version.def \
    -static -static-libgcc -static-libstdc++ -s \
    -ld3d11 -ld3d12 -ldxgi -ld3dcompiler -ldxguid -luuid -lgdi32 -ldwmapi -limm32
echo "built dist/version.dll ($(du -h dist/version.dll | cut -f1))"
