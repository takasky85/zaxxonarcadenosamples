#!/bin/sh
# Builds zaxxon (needs a C++17 compiler and SDL3).
#
# on Windows: MSYS2 UCRT64, one time:
#   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-pkgconf
# Linux:  install your distro's SDL3 development package.
set -e
cd "$(dirname "$0")"

if pkg-config --exists sdl3 2>/dev/null; then
	SDL="$(pkg-config --cflags --libs sdl3)"
else
	SDL="-lSDL3"
fi

EXTRA=""
case "$(uname -s)" in
	MINGW*|MSYS*) EXTRA="-static-libgcc -static-libstdc++" ;;
esac

g++ -O3 -std=gnu++17 zaxxon.cpp -o zaxxon $SDL $EXTRA

# On Windows, put SDL3.dll next to the program so it also starts from Explorer.
if [ -n "$MINGW_PREFIX" ] && [ -f "$MINGW_PREFIX/bin/SDL3.dll" ]; then
	cp "$MINGW_PREFIX/bin/SDL3.dll" .
fi
echo "built: $(pwd)/zaxxon"
