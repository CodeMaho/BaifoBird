#!/usr/bin/env bash
# Compila FlappyBird para Linux (probado apuntando a Raspberry Pi OS / Debian).
#
#   sudo apt install build-essential libsfml-dev
#   ./build.sh
#   cd FlappyBird/FlappyBird-Linux && ./FlappyBird
#
# La fuente esta en src/ y la comparten Windows, Linux y Android. El binario se
# deja en FlappyBird-Linux/ y DEBE ejecutarse desde ahi: las rutas de assets/,
# audios/ y fonts/ son relativas al directorio de trabajo, no al del binario.
#
#   ./FlappyBird              # ventana de 1280x860
#   ./FlappyBird 800x480      # otro tamano
#   ./FlappyBird --fullscreen # pantalla completa (ESC en el menu sale)
set -euo pipefail

cd "$(dirname "$0")"
SRC=FlappyBird/src
OUT=FlappyBird/FlappyBird-Linux
SQLITE=third_party/sqlite

if ! command -v g++ >/dev/null 2>&1; then
    echo "Falta g++.  sudo apt install build-essential" >&2
    exit 1
fi

if ! echo '#include <SFML/Graphics.hpp>' | g++ -E -x c++ - -o /dev/null 2>/dev/null; then
    echo "Faltan las cabeceras de SFML.  sudo apt install libsfml-dev" >&2
    exit 1
fi

if [ ! -f "$SQLITE/sqlite3.c" ]; then
    echo "Falta $SQLITE/sqlite3.c (amalgamacion de SQLite)." >&2
    exit 1
fi

# sqlite3.c se compila aparte y se cachea: es grande y no cambia nunca.
if [ ! -f "$OUT/sqlite3.o" ] || [ "$SQLITE/sqlite3.c" -nt "$OUT/sqlite3.o" ]; then
    echo "compilando SQLite (solo la primera vez, tarda)..."
    gcc -O2 -c "$SQLITE/sqlite3.c" -o "$OUT/sqlite3.o" \
        -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0
fi

g++ -std=c++17 -O2 -Wall -Wno-unknown-pragmas -I "$SQLITE" -I "$SRC" \
    "$SRC/main.cpp" "$SRC/scoredb.cpp" "$OUT/sqlite3.o" -o "$OUT/FlappyBird" \
    -lsfml-graphics -lsfml-window -lsfml-system -lsfml-audio \
    -lpthread -ldl -lm

echo "OK -> $(pwd)/$OUT/FlappyBird"
echo "Ejecutalo asi:  cd $OUT && ./FlappyBird"
