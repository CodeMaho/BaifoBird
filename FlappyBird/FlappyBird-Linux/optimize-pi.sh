#!/usr/bin/env bash
#
# Prepara una Raspberry Pi para que BaifoBird vaya lo mas fluido posible, y
# opcionalmente para que sea LO UNICO que arranque (modo kiosco).
#
#   sudo ./optimize-pi.sh --apply          optimiza el sistema
#   sudo ./optimize-pi.sh --apply --kiosk  ademas, arranca solo el juego
#   sudo ./optimize-pi.sh --apply --kiosk --mode=1024x768   resolucion del kiosco
#   sudo ./optimize-pi.sh --apply --kiosk --rate=60         Hz del kiosco
#   sudo ./optimize-pi.sh --apply --no-bluetooth   apaga tambien el Bluetooth
#                                                  (NO lo uses si tu mando es BT)
#   sudo ./optimize-pi.sh --revert         deshace todo
#   ./optimize-pi.sh --dry-run             enseña que haria, sin tocar nada
#
# QUE NO TOCA, a proposito:
#   - SSH ni la red. Desactivarlos en una Pi headless significa perder el
#     acceso a la maquina, y no compensa por unos FPS.
#   - La memoria de intercambio. Quitarla arriesga que el sistema se quede sin
#     memoria; el juego no la necesita pero apt si.
#   - El overclock. Es cosa de cada placa y de su refrigeracion.
#   - El Bluetooth, salvo que se pida con --no-bluetooth: los mandos
#     inalambricos lo necesitan.
#
# Todo lo que modifica se copia antes a /var/backups/baifobird-<fecha>/.
set -uo pipefail

# La ayuda es la cabecera de este mismo fichero. Antes se sacaba con un rango de
# lineas fijo ('2,25p'), que se desajustaba en cuanto se anadia un parrafo; esto
# imprime los comentarios hasta la primera linea que no lo sea.
ayuda() {
    awk 'NR>1 { if (/^#/) { sub(/^# ?/, ""); print } else { exit } }' "$0"
}

APLICAR=0; REVERTIR=0; KIOSCO=0; SIMULAR=0; SIN_BT=0
# Resolucion de X en modo kiosco (--mode para cambiarla). 800x600 son 480.000
# px, cerca de los 800x538 que el juego usa por defecto en Pi. Antes eran
# 1280x720: 921.600 px, mas del doble, y ademas el monitor de prueba anunciaba
# ese modo a 120 Hz, asi que el vsync pedia 120 fotogramas de 921.600 px. Entre
# las dos cosas, 4,3 veces el relleno que se pretendia.
MODO_KIOSCO=800x600
# Los Hz importan tanto como los pixeles: sin --rate, xrandr coge el primer modo
# de la lista, que puede ser 120 Hz.
HZ_KIOSCO=60
for a in "$@"; do
    case "$a" in
        --apply)   APLICAR=1 ;;
        --revert)  REVERTIR=1 ;;
        --kiosk)   KIOSCO=1 ;;
        --no-bluetooth) SIN_BT=1 ;;
        --mode=*)  MODO_KIOSCO="${a#*=}" ;;
        --rate=*)  HZ_KIOSCO="${a#*=}" ;;
        --dry-run) SIMULAR=1; APLICAR=1 ;;
        -h|--help) ayuda; exit 0 ;;
        *) echo "Opcion desconocida: $a"; exit 1 ;;
    esac
done
if [ $APLICAR -eq 0 ] && [ $REVERTIR -eq 0 ]; then
    ayuda; exit 1
fi

# ---------------------------------------------------------------- utilidades
rojo()  { printf '\033[31m%s\033[0m\n' "$*"; }
verde() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '  %s\n' "$*"; }
paso()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }

MARCA=/var/lib/baifobird-optimizado
RESPALDO=""

ejecutar() {
    if [ $SIMULAR -eq 1 ]; then info "[simulado] $*"; else "$@"; fi
}

respaldar() {
    [ -f "$1" ] || return 0
    [ $SIMULAR -eq 1 ] && { info "[simulado] copiaria $1"; return 0; }
    mkdir -p "$RESPALDO$(dirname "$1")"
    cp -a "$1" "$RESPALDO$1"
}

# El bloque de autoarranque va entre marcas para poder REEMPLAZARLO.
#
# Antes se anadia con un 'if ! grep -q "BaifoBird kiosco"', que solo evitaba
# duplicados: si ya habia un bloque -el de X- y luego se instalaba SFML-Pi, el
# script daba por hecho que no habia nada que hacer y dejaba el bloque viejo,
# que seguia lanzando startx. Cambiar de kiosco con X a kiosco sin X era
# imposible sin editar .bash_profile a mano.
MARCA_INI="# >>> BaifoBird kiosco >>>"
MARCA_FIN="# <<< BaifoBird kiosco <<<"

quitar_bloque_kiosco() {
    [ -f "$1" ] || return 0
    # Se borran tanto el bloque con marcas como los de versiones anteriores,
    # que empezaban por un comentario "# BaifoBird kiosco" y acababan en 'fi'.
    sed -i -e "/^${MARCA_INI}\$/,/^${MARCA_FIN}\$/d" \
           -e '/^# BaifoBird kiosco/,/^fi$/d' "$1"
}

# ------------------------------------------------------------ comprobaciones
if ! grep -qi "raspberry pi" /proc/device-tree/model 2>/dev/null; then
    rojo "Esto no parece una Raspberry Pi. Abortando por seguridad."
    exit 1
fi
MODELO=$(tr -d '\0' < /proc/device-tree/model)

if [ $SIMULAR -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    rojo "Hace falta root:  sudo $0 $*"
    exit 1
fi

# El config.txt cambio de sitio en Bookworm
CONFIG=/boot/firmware/config.txt
[ -f "$CONFIG" ] || CONFIG=/boot/config.txt
[ -f "$CONFIG" ] || { rojo "No encuentro config.txt"; exit 1; }

# Usuario real (el que hizo sudo), para el autoarranque
USUARIO="${SUDO_USER:-$(id -un)}"
CASA=$(getent passwd "$USUARIO" | cut -d: -f6)

# Directorio del juego: este script vive junto al binario
JUEGO_DIR=$(cd "$(dirname "$0")" && pwd)

echo
verde "BaifoBird - preparacion de Raspberry Pi"
info "placa:    $MODELO"
info "config:   $CONFIG"
info "usuario:  $USUARIO"
info "juego:    $JUEGO_DIR"
[ $SIMULAR -eq 1 ] && rojo "MODO SIMULACION: no se modifica nada"

# =========================================================== REVERTIR
if [ $REVERTIR -eq 1 ]; then
    paso "Deshaciendo"
    if [ ! -f "$MARCA" ]; then
        rojo "No hay constancia de que se aplicara nada. Nada que deshacer."
        exit 1
    fi
    ORIGEN=$(cat "$MARCA")
    if [ -d "$ORIGEN" ]; then
        info "restaurando ficheros desde $ORIGEN"
        (cd "$ORIGEN" && find . -type f -print0 | while IFS= read -r -d '' f; do
            cp -a "$f" "/${f#./}"
            echo "    /${f#./}"
        done)
    fi
    for s in bluetooth hciuart avahi-daemon triggerhappy ModemManager cups cups-browsed; do
        systemctl unmask "$s" 2>/dev/null
        systemctl enable "$s" 2>/dev/null
    done
    systemctl disable baifobird-kiosk.service 2>/dev/null
    rm -f /etc/systemd/system/baifobird-kiosk.service
    # El autoarranque se quita del .bash_profile explicitamente. Restaurar la
    # copia de seguridad no basta: si el fichero no existia antes, 'respaldar'
    # no guardo nada y el bloque se quedaria ahi para siempre.
    quitar_bloque_kiosco "$CASA/.bash_profile"
    info "autoarranque retirado de $CASA/.bash_profile"
    # El gobernador vuelve a manos del sistema (ondemand en Raspberry Pi OS).
    systemctl disable baifobird-governor.service 2>/dev/null
    rm -f /etc/systemd/system/baifobird-governor.service
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [ -w "$g" ] && echo ondemand > "$g" 2>/dev/null
    done
    rm -f /etc/systemd/system/getty@tty1.service.d/baifobird-autologin.conf
    rmdir /etc/systemd/system/getty@tty1.service.d 2>/dev/null
    systemctl set-default graphical.target 2>/dev/null
    systemctl daemon-reload
    rm -f "$MARCA"
    verde "Deshecho. Reinicia para volver al estado anterior:  sudo reboot"
    exit 0
fi

# =========================================================== APLICAR
RESPALDO="/var/backups/baifobird-$(date +%Y%m%d-%H%M%S)"
if [ $SIMULAR -eq 0 ]; then
    mkdir -p "$RESPALDO"
    echo "$RESPALDO" > "$MARCA"
fi
info "respaldo: $RESPALDO"

# ---------------------------------------------------------------- 1. GPU
paso "1. Memoria para la GPU"
# La VideoCore IV reparte la RAM con la CPU. Por defecto le tocan 76 MB, justos
# para las texturas del juego (fondos de 1280x860 y el suelo de 2048x128).
if grep -q '^gpu_mem=' "$CONFIG"; then
    ACTUAL=$(grep '^gpu_mem=' "$CONFIG" | tail -1 | cut -d= -f2)
    info "ya definido: gpu_mem=$ACTUAL"
    if [ "$ACTUAL" -lt 128 ] 2>/dev/null; then
        respaldar "$CONFIG"
        ejecutar sed -i 's/^gpu_mem=.*/gpu_mem=128/' "$CONFIG"
        info "subido a 128"
    fi
else
    respaldar "$CONFIG"
    if [ $SIMULAR -eq 0 ]; then
        printf '\n# BaifoBird: mas memoria para texturas\ngpu_mem=128\n' >> "$CONFIG"
    fi
    info "anadido gpu_mem=128"
fi

# ------------------------------------------------------- 2. gobernador de CPU
paso "2. Gobernador de CPU en 'performance'"
# Por defecto es 'ondemand': la CPU baja a 600 MHz y sube tarde, lo que provoca
# tirones al empezar a moverse. 'performance' la deja fija a su maximo.
#
# Antes esto escribia GOVERNOR="performance" en /etc/default/cpufrequtils. Ese
# fichero SOLO lo lee el paquete cpufrequtils, que en Raspberry Pi OS no viene
# instalado: en la Pi de pruebas el ajuste llevaba semanas sin aplicarse y el
# gobernador seguia en 'ondemand' tras cada reinicio. La escritura directa al
# sysfs si funcionaba, pero se perdia al apagar.
#
# Una unidad de systemd no depende de ningun paquete y se ejecuta en cada
# arranque, que es justo lo que hacia falta.
UNIDAD_GOB=/etc/systemd/system/baifobird-governor.service
if [ $SIMULAR -eq 0 ]; then
    cat > "$UNIDAD_GOB" <<'GEOF'
[Unit]
Description=BaifoBird: gobernador de CPU en performance
After=multi-user.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do [ -w "$g" ] && echo performance > "$g"; done; exit 0'

[Install]
WantedBy=multi-user.target
GEOF
    systemctl daemon-reload
    systemctl enable baifobird-governor.service >/dev/null 2>&1
    systemctl start baifobird-governor.service >/dev/null 2>&1
else
    info "[simulado] crearia $UNIDAD_GOB"
fi
ACTUAL_GOB=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "?")
info "gobernador ahora: $ACTUAL_GOB  (unidad systemd, se reaplica en cada arranque)"

# ------------------------------------------------------------ 3. servicios
paso "3. Servicios que no hacen falta para jugar"
# NO se tocan ssh, networking ni wpa_supplicant: perder el acceso a la Pi no
# compensa. El Bluetooth tampoco, porque los mandos inalambricos lo necesitan;
# solo se apaga si se pide con --no-bluetooth.
SERVICIOS="avahi-daemon triggerhappy ModemManager cups cups-browsed"
if [ $SIN_BT -eq 1 ]; then
    info "se desactiva tambien Bluetooth (--no-bluetooth)"
    SERVICIOS="$SERVICIOS bluetooth hciuart"
else
    info "Bluetooth se deja ACTIVO: hace falta para los mandos inalambricos."
    info "  si juegas solo con teclado, apagalo con --no-bluetooth"
fi
for s in $SERVICIOS; do
    if systemctl list-unit-files 2>/dev/null | grep -q "^$s"; then
        ejecutar systemctl disable "$s" >/dev/null 2>&1
        ejecutar systemctl stop "$s" >/dev/null 2>&1
        info "desactivado: $s"
    fi
done

# --------------------------------------------------------- 4. ahorro pantalla
paso "4. Apagado de pantalla y salvapantallas"
mkdir -p "$CASA/.config" 2>/dev/null
if [ $SIMULAR -eq 0 ]; then
    cat > "$CASA/.xinitrc.baifobird-common" <<'XEOF'
# sin salvapantallas ni apagado de pantalla mientras se juega
xset s off
xset s noblank
xset -dpms
XEOF
    chown "$USUARIO":"$USUARIO" "$CASA/.xinitrc.baifobird-common" 2>/dev/null
fi
info "xset s off / -dpms al arrancar la sesion grafica"

# ------------------------------------------------------------- 5. kiosco
if [ $KIOSCO -eq 1 ]; then
    paso "5. Modo kiosco: solo arranca el juego"
    # SFML necesita X11, asi que no se puede prescindir del servidor grafico.
    # Lo que si se evita es el ESCRITORIO entero (compositor, panel, gestor de
    # archivos...), arrancando X con el juego como unico cliente. En una Pi 3
    # eso libera bastante CPU y memoria.

    # Con SFML-Pi no hace falta servidor grafico: el juego pinta directo por
    # DRM/KMS desde la consola. Es el mejor caso posible en una Pi 3.
    SIN_X=0
    # Se mira el RUNPATH de la cabecera ELF, no 'ldd': ldd resuelve ejecutando el
    # binario, y aqui un falso negativo no da un error visible, monta el kiosco
    # CON servidor grafico y te deja creyendo que va sin el.
    if [ -d /opt/sfml-pi/include/SFML ] \
       && readelf -d "$JUEGO_DIR/FlappyBird" 2>/dev/null | grep -qE "R(UN)?PATH.*/opt/sfml-pi/lib"; then
        SIN_X=1
        info "SFML-Pi detectado: kiosco SIN servidor grafico"
    elif ! command -v xinit >/dev/null 2>&1; then
        rojo "Falta xinit:  sudo apt install --no-install-recommends xserver-xorg xinit"
        info "o instala SFML-Pi para prescindir de X:  ./install-sfml-pi.sh"
        exit 1
    fi

    if [ ! -x "$JUEGO_DIR/FlappyBird" ]; then
        rojo "No encuentro el binario en $JUEGO_DIR/FlappyBird"
        info "compilalo antes:  cd $JUEGO_DIR && ./build.sh"
        exit 1
    fi

    # arrancar en consola, sin escritorio
    ejecutar systemctl set-default multi-user.target

    # autologin en tty1
    if [ $SIMULAR -eq 0 ]; then
        mkdir -p /etc/systemd/system/getty@tty1.service.d
        cat > /etc/systemd/system/getty@tty1.service.d/baifobird-autologin.conf <<AEOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin $USUARIO --noclear %I \$TERM
AEOF
    fi
    info "autologin de $USUARIO en tty1"

    # --- kiosco sin X (SFML-Pi): el juego se lanza directo en la consola ---
    if [ $SIN_X -eq 1 ] && [ $SIMULAR -eq 0 ]; then
        PERFIL="$CASA/.bash_profile"
        respaldar "$PERFIL"
        touch "$PERFIL"
        quitar_bloque_kiosco "$PERFIL"
        cat >> "$PERFIL" <<PEOF
$MARCA_INI
# SFML-Pi: sin servidor grafico, se pinta por DRM/KMS.
if [ "\$XDG_VTNR" = 1 ]; then
    cd "$JUEGO_DIR" || exit 1
    SFML_DRM_MODE=$MODO_KIOSCO exec ./FlappyBird
fi
$MARCA_FIN
PEOF
        chown "$USUARIO":"$USUARIO" "$PERFIL"
        info "el juego arranca directo en tty1, sin X, a $MODO_KIOSCO"
    fi

    # sesion X con el juego como unico cliente
    if [ $SIN_X -eq 0 ] && [ $SIMULAR -eq 0 ]; then
        cat > "$CASA/.xinitrc" <<XEOF
#!/bin/sh
[ -f "\$HOME/.xinitrc.baifobird-common" ] && . "\$HOME/.xinitrc.baifobird-common"

# A pantalla completa se usa la resolucion del ESCRITORIO. En un monitor 1080p
# eso son 2 Mpx y la Pi 3 se arrastra, asi que primero se baja el modo de X.
# Si xrandr no puede, se sigue igual: el juego se adapta a lo que haya.
#
# Los Hz cuentan tanto como los pixeles. Sin --rate, xrandr coge el PRIMER modo
# de la lista con ese tamano, y en el monitor de pruebas 1280x720 aparecia como
# 120 Hz: el vsync pedia 120 fotogramas por segundo a una Pi 3. De ahi que se
# intente primero con la tasa pedida y solo se caiga a "sin --rate" si falla.
if command -v xrandr >/dev/null 2>&1; then
    SALIDA=\$(xrandr | awk '/ connected/{print \$1; exit}')
    for MODO in $MODO_KIOSCO 800x600 1024x768 1280x720; do
        xrandr --output "\$SALIDA" --mode "\$MODO" --rate $HZ_KIOSCO 2>/dev/null && break
        xrandr --output "\$SALIDA" --mode "\$MODO" 2>/dev/null && break
    done
fi

# sin gestor de ventanas: el juego es el unico cliente de X
cd "$JUEGO_DIR" || exit 1
exec ./FlappyBird --fullscreen
XEOF
        chmod +x "$CASA/.xinitrc"
        chown "$USUARIO":"$USUARIO" "$CASA/.xinitrc"

        # lanzar X al entrar en tty1, y solo en tty1
        PERFIL="$CASA/.bash_profile"
        respaldar "$PERFIL"
        touch "$PERFIL"
        quitar_bloque_kiosco "$PERFIL"
        cat >> "$PERFIL" <<PEOF
$MARCA_INI
# Arranca X con el juego como unico cliente, al entrar en la primera consola.
if [ -z "\$DISPLAY" ] && [ "\$XDG_VTNR" = 1 ]; then
    exec startx
fi
$MARCA_FIN
PEOF
        chown "$USUARIO":"$USUARIO" "$PERFIL"
    fi
    [ $SIN_X -eq 0 ] && info "sesion X a $MODO_KIOSCO @ ${HZ_KIOSCO}Hz con solo ./FlappyBird --fullscreen"
    # El juego, a partir de esta version, tambien elige por su cuenta el modo de
    # pantalla completa mas pequeno que le sirva, y se topa a 60 fps en Pi. Lo
    # de aqui es la primera linea de defensa; el codigo es la segunda.
    info "para salir del juego: ESC en el menu principal"
    info "para recuperar una consola: Ctrl+Alt+F2"
fi

paso "Listo"
if [ $SIMULAR -eq 1 ]; then
    verde "Simulacion terminada. Nada se ha modificado."
else
    verde "Aplicado. Reinicia para que surta efecto:  sudo reboot"
    info "para deshacerlo todo:  sudo $0 --revert"
fi
echo
