# BaifoBird

Juego tipo Flappy Bird escrito en C++ con SFML, que corre en **Windows, Linux
(incluida Raspberry Pi) y Android** desde una única base de código.

Una cabra con sombrero de paja esquiva lanzas sobre un paisaje volcánico. Las
puntuaciones se guardan en SQLite con el nombre del jugador, y se juega con
teclado, ratón, mando o pantalla táctil.

<img src="FlappyBird/imgs/gameplay.png" width="900" />

---

## Cómo se juega

La cabra cae sola y sube cada vez que pulsas. Hay que pasar entre las lanzas;
tocar una lanza, el suelo o salir por arriba termina la partida. Cada par
superado suma un punto, y **se gana al pasar 100**.

Antes de cada partida se pide un **nombre de jugador**, y al terminar la partida
se guarda con ese nombre.

### Controles

| Acción | Teclado | Ratón | Mando | Táctil |
|---|---|---|---|---|
| Aletear / confirmar | Espacio, Enter | clic | **cualquier botón** | toque |
| Mover el foco del menú | flechas | — | cruceta o stick | — |
| Pulsar un elemento | — | clic encima | confirmar | toque encima |
| Escribir el nombre | teclear, Retroceso | — | ↑↓ letra, ←→ casilla | — |
| Pausar | ESC | — | cruceta/stick abajo | — |
| Reanudar | P | — | cualquier botón | — |
| Volver (créditos, récords) | ESC | — | cualquier botón | toque |

Los medallas dependen de la puntuación: bronce a partir de 10, plata 20, oro 30
y platino 40.

---

## Ejecutar en Windows

Requisitos: **Visual Studio** (bastan las Build Tools) con las herramientas de
C++ x64, y **SFML 2.6.2** descomprimido en `SFML-2.6.2/` en la raíz del
proyecto ([descarga](https://www.sfml-dev.org/download/sfml/2.6.2/), versión
para Visual C++ de 64 bits).

```bat
cd FlappyBird\FlappyBird-Win
build.bat
FlappyBird.exe
```

El script deja el ejecutable en esa misma carpeta, junto a `assets\`, `audios\`
y `fonts\`, y **hay que lanzarlo desde ahi**: esas rutas son relativas al
directorio de trabajo, no al del binario.

Opciones:

```bat
FlappyBird.exe                 :: ventana de 1280x860
FlappyBird.exe 800x480         :: cualquier tamaño
FlappyBird.exe --fullscreen    :: pantalla completa (ESC en el menú sale)
FlappyBird.exe --help
FlappyBird.exe --debug         :: FPS, dt y cajas de colision en pantalla
```

## Ejecutar en Linux y Raspberry Pi

```bash
sudo apt install build-essential libsfml-dev
cd FlappyBird/FlappyBird-Linux
./build.sh
./FlappyBird
```

Acepta las mismas opciones que en Windows. La primera compilación tarda porque
compila SQLite; luego se reutiliza el `.o`.

**En Raspberry Pi hay que compilar en el propio Pi**: un binario x86_64 no se
ejecuta en ARM. Copia el proyecto y lanza `./build.sh` allí, desde `FlappyBird/FlappyBird-Linux/`. Para arranque en
modo kiosco, `--fullscreen`.

### Rendimiento en Raspberry Pi

La Pi 3 va justa a 1280x860: son 1.1 Mpx dibujados varias veces por fotograma y
la VideoCore IV se queda sin relleno. **Baja la resolución**, que es lo que más
se nota:

```bash
./FlappyBird 800x480     # 3 veces menos pixeles que por defecto
./FlappyBird 640x480
```

Para medir en vez de suponer, `--debug` muestra FPS reales, el `dt` máximo y las
cajas de colisión:

```bash
./FlappyBird 800x480 --debug
```

Si **`dt max` se queda pegado a 50 ms**, la máquina no llega a 20 FPS: el paso de
tiempo toca su tope y el juego pasa a cámara lenta, con saltos grandes entre
fotogramas que hacen que las colisiones *parezcan* injustas aunque sean
correctas. Es la señal de que hay que bajar resolución.

Si quieres jugar con mando, hace falta el módulo `joydev` y estar en el grupo
`input` (ambos vienen de serie en Raspberry Pi OS). Comprobación rápida:
`ls /dev/input/js*` debe listar algo con el mando encendido.

## Ejecutar en Android

Hay un APK de depuración listo: **`FlappyBird/BaifoBird-debug.apk`**
(arm64-v8a, Android 5.0 o superior).

```bash
adb install -r FlappyBird/BaifoBird-debug.apk
```

Para recompilarlo hacen falta SDK 34, NDK 25.2.9519653, CMake 3.31, Gradle 8.7
y **JDK 17** (Gradle 8.7 y AGP 8.1 no admiten JDK más nuevos), además de SFML
compilado para Android e instalado en el NDK:

```bash
cd SFML-2.6.2 && mkdir bld-arm64 && cd bld-arm64
cmake -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
      -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release ..
cmake --build . && cmake --install .

cd FlappyBird/FlappyBird-Android && gradle assembleDebug
```

---

## Cómo funciona

### Una sola fuente para las tres plataformas

`FlappyBird/src/main.cpp` es la **única** fuente del juego. La compilan
`build.bat`, `build.sh` y el `Android.mk` de Gradle. No hay copias por
plataforma, así que un cambio llega a las tres a la vez. Lo específico de
Android son unas pocas líneas bajo `#ifdef SFML_SYSTEM_ANDROID`.

### Movimiento por tiempo, no por fotograma

El juego original movía todo "por fotograma" con vsync, así que su velocidad
dependía de los Hz del monitor: a 144 Hz iba **2.4 veces más rápido** que a 60.
Ahora todo está en unidades por segundo e integrado con `dt`. Comprobado a
30, 60 y 144 FPS: misma trayectoria y mismo punto de aterrizaje.

El paso de tiempo está acotado (`MaxFrameTime`); sin ese tope, tras un alt-tab
el juego integraría cientos de píxeles de golpe y la cabra atravesaría una lanza
sin que la colisión llegara a detectarla.

### El salto

Es un **impulso por pulsación**: mantener pulsado no sube más. Usa gravedad
asimétrica —floja al subir, fuerte al caer— porque con un impulso la altura del
arco es `v₀²/2g` y el tiempo en el aire `2v₀/g`, o sea que **ambos bajan si sube
la gravedad**. Así se consigue un salto amplio y lento con una caída que pesa:
122 px de arco y 0.72 s en el aire.

### Un lienzo fijo escalado a cualquier pantalla

Todo se dibuja en coordenadas de **1280×860** y una `sf::View` lo lleva a la
pantalla real. En proporciones distintas no se deforma ni se recorta: se ve
**más mundo a los lados**. Por eso el mismo código sirve para una ventana de
escritorio, una pantalla pequeña de Pi y un móvil 21:9.

Los tests de posición usan `mapPixelToCoords()`, que aplica esa misma View, así
que ratón y dedo aciertan a cualquier escala sin código aparte.

### Puntuaciones en SQLite

En `scores.db`, junto al ejecutable (en Android, en el almacenamiento interno de
la app). Se usa la amalgamación oficial incrustada en `third_party/sqlite`, así
que **no hay que instalar nada** en ninguna plataforma.

```sql
CREATE TABLE scores (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  name      TEXT    NOT NULL,
  score     INTEGER NOT NULL,
  played_at TEXT    NOT NULL DEFAULT (datetime('now'))
);
```

El nombre se inserta con sentencia preparada, nunca concatenado en el SQL. La
pantalla de récords muestra las 10 mejores y permite borrarlas todas, con un
paso de confirmación que dice cuántas se van a borrar.

### El mando funciona con cualquier modelo

SFML numera los botones del 0 al 31 **sin darles significado**, y el índice de
cada botón físico cambia entre modelos y entre plataformas: no existe una tabla
universal. En cambio sí da significado a los **ejes** (`X`/`Y` es el stick,
`PovX`/`PovY` la cruceta).

Por eso el diseño no usa ningún índice de botón: se **navega por ejes** y
**cualquier botón confirma**. Como el signo de los ejes tampoco está
documentado, la navegación es **circular**: aunque venga invertido, se alcanza
todo pulsando en la misma dirección.

### Los assets se generan con un script

`build_assets.py` construye los 23 assets del juego a partir de los sprites
originales de `FlappyBird/imgs/`: recorta fondos (blanco en las cabras, negro en las
lanzas), normaliza los fotogramas de animación a un lienzo común, hace el suelo
repetible y compone las medallas.

```bash
pip install pillow numpy scipy
python build_assets.py
```

Dos detalles que conviene no romper:

- La textura del suelo es de **2048×128, potencia de dos**. La GPU de la
  Raspberry Pi 3 tiene `GL_MAX_TEXTURE_SIZE = 2048`, y el suelo se dibuja con
  `setRepeated(true)`, que SFML no garantiza sobre texturas que no lo sean.
- Las lanzas se recortan rellenando el hueco de cada fila, no por umbral de
  color: el collar bajo la punta es casi negro como el fondo, y un umbral lo
  volvía transparente.

---

## Estructura

```
build_assets.py           genera los assets desde los sprites originales
third_party/sqlite/       amalgamación de SQLite

FlappyBird/
  src/                    FUENTE ÚNICA de las tres plataformas
  imgs/                   sprites originales (entrada de build_assets.py)
                          y capturas del juego
  FlappyBird-Win/         build.bat + assets + DLL; el .exe queda aquí
  FlappyBird-Linux/       build.sh  + assets;      el binario queda aquí
  FlappyBird-Android/     proyecto Gradle; usa ../src vía Android.mk
  BaifoBird-debug.apk     APK listo para instalar
```

Cada script de compilación vive en la carpeta de su plataforma y deja ahí el
binario, junto a los datos que necesita. `build_assets.py` se queda en la raíz
porque no es de ninguna plataforma: alimenta a las tres.

`SFML-2.6.2/` no está en el repositorio: descárgalo aparte (ver arriba).

---

## Capturas

Menú principal
:-------------------------:
<img src="FlappyBird/imgs/mainMenu.png" width="900" />

Nombre del jugador — se pide antes de cada partida
:-------------------------:
<img src="FlappyBird/imgs/nameEntry.png" width="900" />

En partida
:-------------------------:
<img src="FlappyBird/imgs/gameplay.png" width="900" />

Pausa
:-------------------------:
<img src="FlappyBird/imgs/pause.png" width="900" />

Game Over
:-------------------------:
<img src="FlappyBird/imgs/gameOver.png" width="900" />

Récords — con el recuadro de foco y la opción de borrar
:-------------------------:
<img src="FlappyBird/imgs/highScore.png" width="900" />

Créditos
:-------------------------:
<img src="FlappyBird/imgs/credits.png" width="900" />

Victoria — al pasar las 100 lanzas
:-------------------------:
<img src="FlappyBird/imgs/winner.png" width="900" />

---

## Autor

- Doramas

Basado en un clon de Flappy Bird en SFML; el arte, la física, el sistema de
puntuaciones, el soporte de mando y la versión de Android son propios.
