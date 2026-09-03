#!/usr/bin/env bash
# Compila FlappyBird para Linux (probado apuntando a Raspberry Pi OS / Debian).
#
#   sudo apt install build-essential libsfml-dev
#   cd FlappyBird/FlappyBird-Linux
#   ./build.sh && ./FlappyBird
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
SRC=../src
OUT=.
SQLITE=../../third_party/sqlite

if ! command -v g++ >/dev/null 2>&1; then
    echo "Falta g++.  sudo apt install build-essential" >&2
    exit 1
fi

# Si esta SFML-Pi instalado (el fork que dibuja sin X11), se usa ese. Ver
# install-sfml-pi.sh. Si no, el SFML del sistema.
SFML_PI=/opt/sfml-pi
SFML_INC=""
SFML_LIB=""
SFML_RPATH=""
SFML_EXTRA=""
SFML_DEFS=""
if [ -d "$SFML_PI/include/SFML" ]; then
    SFML_INC="-I $SFML_PI/include"
    SFML_LIB="-L $SFML_PI/lib"
    # rpath para que el binario encuentre estas librerias sin LD_LIBRARY_PATH
    SFML_RPATH="-Wl,-rpath,$SFML_PI/lib"
    # -lgbm lo tiene que poner el ejecutable porque libsfml-window.so de SFML-Pi
    # NO declara libgbm entre sus dependencias, aunque usa sus simbolos:
    #   objdump -p libsfml-window.so | grep NEEDED
    #     -> libdrm, libEGL, libudev, libGL ... pero no libgbm
    # Sin esto el enlazado muere con veinte "referencia a gbm_* sin definir".
    # Es un descuido del empaquetado del fork, no algo que podamos arreglar aqui.
    SFML_EXTRA="-lgbm"
    # Con DRM no existen las ventanas: solo hay MODOS DE VIDEO que el conector
    # anuncia. Pedir un tamano arbitrario -el juego abria a 800x538 en Pi-
    # termina en "Failed to set mode: No space left on device", que es lo que
    # devuelve drmModeSetCrtc cuando el modo no existe. El codigo tiene que
    # saberlo, y quien lo sabe es este script: es el que elige contra que SFML
    # se enlaza.
    SFML_DEFS="-DBAIFOBIRD_DRM"
    echo "usando SFML-Pi de $SFML_PI (sin X11)"
elif ! echo '#include <SFML/Graphics.hpp>' | g++ -E -x c++ - -o /dev/null 2>/dev/null; then
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

# $SFML_INC/$SFML_LIB/$SFML_RPATH van SIN comillas a proposito: cada una son dos
# palabras ("-I" y la ruta) y hay que dejar que el shell las separe. Entre
# comillas llegarian como un unico argumento y g++ no las entenderia; vacias, no
# aportan nada, que es el caso del SFML del sistema.
# shellcheck disable=SC2086
g++ -std=c++17 -O2 -Wall -Wno-unknown-pragmas $SFML_DEFS $SFML_INC -I "$SQLITE" -I "$SRC" \
    "$SRC/main.cpp" "$SRC/scoredb.cpp" "$OUT/sqlite3.o" -o "$OUT/FlappyBird" \
    $SFML_LIB $SFML_RPATH \
    -lsfml-graphics -lsfml-window -lsfml-system -lsfml-audio \
    $SFML_EXTRA \
    -lpthread -ldl -lm

echo "OK -> $(pwd)/FlappyBird"

# Comprobacion, no adorno: durante un tiempo el script anunciaba SFML-Pi y
# enlazaba igualmente contra el del sistema, porque las variables de arriba no
# llegaban al g++. Se verifica sobre el binario ya hecho, que es la unica prueba
# que no se puede falsear.
if [ -n "$SFML_LIB" ]; then
    if ldd "$OUT/FlappyBird" 2>/dev/null | grep -q "$SFML_PI/lib"; then
        echo "verificado: enlazado contra SFML-Pi (sin X11)"
    else
        echo "AVISO: se detecto SFML-Pi pero el binario NO lo esta usando." >&2
        echo "       revisa 'ldd ./FlappyBird'; el kiosco sin X no se activara." >&2
    fi
fi

echo "Ejecutalo desde aqui:  ./FlappyBird"
