#!/bin/bash
cd "$(dirname "$0")"

mkdir -p build/mac build/win

echo "Compilazione versione macOS..."
clang++ -std=c++17 -O3 src/main.cpp -o build/mac/NumSound \
  -Ilibs libs/libraylib_mac.a \
  -framework CoreAudio -framework AudioToolbox -framework CoreMIDI -framework CoreFoundation \
  -framework IOKit -framework Cocoa -framework OpenGL

echo "Compilazione versione Windows..."
x86_64-w64-mingw32-g++ -std=c++17 -O3 -mwindows -static -Wno-stringop-overflow \
  src/main.cpp -o build/win/NumSound.exe \
  -Ilibs libs/libraylib_win.a -lopengl32 -lgdi32 -lwinmm

cp libs/raylib.dll build/win/

echo "Fatto! Trovi le versioni pronte in build/mac/ e build/win/"
