#!/usr/bin/env bash
#
# Compila e instala SFML-Pi, un fork de SFML que dibuja SIN servidor grafico
# usando DRM/KMS. En una Raspberry Pi 3 quitar X11 de la ecuacion es la mayor
# ganancia que queda: se libera la CPU y la memoria del servidor grafico y el
# juego pinta directamente sobre el framebuffer.
#
#   ./install-sfml-pi.sh              compila e instala en /opt/sfml-pi
#   ./install-sfml-pi.sh --uninstall  lo borra y deja el sistema como estaba
#
# DECISIONES QUE CONVIENE CONOCER:
#
#   - Se instala en /opt/sfml-pi, NO en /usr/local. Un 'make install' normal
#     pisaria el SFML del sistema y dejaria dos versiones peleandose. Asi se
#     borra con un rm -rf y no afecta a nada mas.
#
#   - Se usa la variante DRM/KMS, no DISPMANX. DISPMANX solo existe para Pi 0-3
#     pero necesita /opt/vc, que Raspberry Pi OS Bookworm ya no incluye.
#
#   - La entrada (teclado, raton, mando) pasa a leerse por udev desde
#     /dev/input/event*, asi que el usuario tiene que estar en el grupo 'input'.
#     El script lo anade.
#
# AVISO: el propio autor de SFML-Pi lo describe como EXPERIMENTAL y valido solo
# si te basta con una unica ventana a pantalla completa. Es nuestro caso, pero
# si algo va raro, el juego sigue compilando contra el SFML normal: basta con
# desinstalar esto.
set -uo pipefail

PREFIJO=/opt/sfml-pi
FUENTE=/tmp/sfml-pi-src
REPO=https://github.com/mickelson/sfml-pi.git

rojo()  { printf '\033[31m%s\033[0m\n' "$*"; }
verde() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '  %s\n' "$*"; }
paso()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }

if [ "${1:-}" = "--uninstall" ]; then
    paso "Desinstalando SFML-Pi"
    sudo rm -rf "$PREFIJO"
    rm -rf "$FUENTE"
    verde "Borrado. El juego volvera a usar el SFML del sistema en la proxima compilacion."
    info "recompila:  ./build.sh"
    exit 0
fi

if ! grep -qi "raspberry pi" /proc/device-tree/model 2>/dev/null; then
    rojo "Esto solo tiene sentido en una Raspberry Pi. Abortando."
    exit 1
fi
info "placa: $(tr -d '\0' < /proc/device-tree/model)"

paso "1. Dependencias"
# Las mismas que SFML menos las de X11, mas las de DRM/GBM/EGL.
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    git cmake build-essential \
    libflac-dev libogg-dev libvorbis-dev libopenal-dev \
    libjpeg-dev libfreetype6-dev libudev-dev \
    libdrm-dev libgbm-dev libegl1-mesa-dev || {
        rojo "Fallo instalando dependencias"; exit 1; }

paso "2. Descargando SFML-Pi"
rm -rf "$FUENTE"
git clone --depth 1 "$REPO" "$FUENTE" || { rojo "No se pudo clonar"; exit 1; }

paso "3. Compilando (esto tarda bastante en una Pi 3)"
mkdir -p "$FUENTE/build"
cd "$FUENTE/build" || exit 1
# SFML_DRM=1 activa el backend sin X11.
# CMAKE_INSTALL_PREFIX lo aisla en /opt para no tocar el SFML del sistema.
cmake .. \
    -DSFML_DRM=1 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIJO" \
    -DSFML_BUILD_EXAMPLES=FALSE \
    -DSFML_BUILD_DOC=FALSE || { rojo "Fallo el cmake"; exit 1; }

NUCLEOS=$(nproc 2>/dev/null || echo 2)
make -j"$NUCLEOS" || { rojo "Fallo la compilacion"; exit 1; }

paso "4. Instalando en $PREFIJO"
sudo make install || { rojo "Fallo la instalacion"; exit 1; }

paso "5. Permisos de entrada"
# Sin X11, SFML lee los dispositivos directamente. Sin este grupo no responden
# ni el teclado ni el mando.
USUARIO="${SUDO_USER:-$(id -un)}"
if id -nG "$USUARIO" | tr ' ' '\n' | grep -qx input; then
    info "$USUARIO ya esta en el grupo 'input'"
else
    sudo usermod -aG input "$USUARIO"
    info "$USUARIO anadido al grupo 'input'"
    rojo "  hay que cerrar sesion (o reiniciar) para que surta efecto"
fi

paso "Listo"
verde "SFML-Pi instalado en $PREFIJO"
info "recompila el juego, que lo detectara solo:"
info "    ./build.sh"
info ""
info "y ejecutalo DESDE UNA CONSOLA, sin escritorio (Ctrl+Alt+F2):"
info "    ./FlappyBird"
info ""
info "para elegir resolucion ya no vale el argumento ANCHOxALTO: con DRM la fija"
info "el modo de video, asi que se usa una variable de entorno:"
info "    SFML_DRM_MODE=1280x720 ./FlappyBird"
info "    SFML_DRM_DEBUG=1 ./FlappyBird     # imprime el modo elegido"
info ""
info "si algo va mal:  ./install-sfml-pi.sh --uninstall && ./build.sh"
echo
