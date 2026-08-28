#!/usr/bin/env python3
"""
Genera los assets del juego a partir de los sprites de FlappyBird/imgs/.

Salida: FlappyBird/FlappyBird-Linux/assets/ y FlappyBird/FlappyBird-Win/assets/
Los assets originales estan en el historial de git, no se hace copia aparte.

Requiere: pillow, numpy, scipy
"""
import os
import sys

import numpy as np
from PIL import Image, ImageFilter
from scipy import ndimage

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "FlappyBird", "imgs")   # sprites de partida
DESTS = [
    os.path.join(ROOT, "FlappyBird", "FlappyBird-Linux", "assets"),
    os.path.join(ROOT, "FlappyBird", "FlappyBird-Win", "assets"),
]

# ---- Geometria de destino (debe coincidir con las constantes de main.cpp) ----
WINDOW_W, WINDOW_H = 1280, 860
GOAT_W, GOAT_H = 74, 76        # lienzo comun de la cabra
PIPE_W, PIPE_H = 130, 860      # rectangulo de dibujo de la lanza
# Textura del suelo: POTENCIA DE DOS a proposito.
#  - La VideoCore IV (Raspberry Pi 3 / Zero 2) tiene GL_MAX_TEXTURE_SIZE = 2048,
#    asi que los 2628 px que salian del cross-fade harian fallar loadFromFile y
#    el juego saldria con EXIT_FAILURE nada mas arrancar.
#  - Ademas el suelo se dibuja con Texture::setRepeated(true), y SFML advierte de
#    que la repeticion no funciona bien sobre texturas no potencia de dos cuando
#    el hardware no las soporta (es el caso de la VC4).
BASE_W, BASE_H = 2048, 128
BOARD_W, BOARD_H = 392, 206    # marcador

# Recorte de las lanzas (ver build_pipes): un pixel se considera "claramente
# arma" por encima de esta luminancia, y se ignora una fila que no reuna al
# menos PipeMinRun de ellos, para no dejarse enganar por ruido del JPEG.
PipeSolidLum = 60
PipeMinRun = 3


# --------------------------------------------------------------------------
# utilidades
# --------------------------------------------------------------------------
def load_rgb(name):
    return np.array(Image.open(os.path.join(SRC, name)).convert("RGB")).astype(np.int16)


def outer_background(rgb, white=True, thresh=235):
    """Mascara del fondo: solo lo conectado con el borde de la imagen.

    Asi no se agujerea el interior del sujeto (ojos, brillos, contornos negros).
    """
    if white:
        cand = rgb.min(axis=2) >= thresh
    else:
        cand = rgb.max(axis=2) <= thresh
    lbl, _ = ndimage.label(cand)
    corners = {lbl[0, 0], lbl[0, -1], lbl[-1, 0], lbl[-1, -1]}
    # tambien el borde entero, no solo las esquinas
    corners |= set(np.unique(lbl[0, :])) | set(np.unique(lbl[-1, :]))
    corners |= set(np.unique(lbl[:, 0])) | set(np.unique(lbl[:, -1]))
    corners.discard(0)
    return np.isin(lbl, list(corners))


def key_out(rgb, white=True, thresh=235, erode=1):
    """Devuelve RGBA con el fondo exterior a alpha=0.

    `erode` recorta 1px del contorno para matar el halo de compresion JPEG.
    """
    bg = outer_background(rgb, white=white, thresh=thresh)
    subj = ~bg
    if erode:
        subj = ndimage.binary_erosion(subj, iterations=erode)
    alpha = (subj * 255).astype(np.uint8)
    out = np.dstack([rgb.astype(np.uint8), alpha])
    return out


def resize_rgba(arr, size):
    """Reescala premultiplicando alpha para que el fondo no sangre en el borde."""
    im = Image.fromarray(arr, "RGBA")
    pre = np.array(im).astype(np.float32)
    a = pre[:, :, 3:4] / 255.0
    pre[:, :, :3] *= a
    small = Image.fromarray(pre.astype(np.uint8), "RGBA").resize(size, Image.LANCZOS)
    s = np.array(small).astype(np.float32)
    a2 = s[:, :, 3:4] / 255.0
    with np.errstate(divide="ignore", invalid="ignore"):
        s[:, :, :3] = np.where(a2 > 0.004, s[:, :, :3] / np.maximum(a2, 1e-6), 0)
    return Image.fromarray(np.clip(s, 0, 255).astype(np.uint8), "RGBA")


def frame_runs(rgb, thresh=235, min_w=20):
    """Columnas ocupadas -> tramos, para trocear un spritesheet."""
    occ = (rgb.min(axis=2) < thresh).any(axis=0)
    runs, start = [], None
    for i, v in enumerate(occ):
        if v and start is None:
            start = i
        if not v and start is not None:
            runs.append((start, i - 1))
            start = None
    if start is not None:
        runs.append((start, len(occ) - 1))
    return [r for r in runs if r[1] - r[0] + 1 >= min_w]


def body_bbox(alpha):
    """Bbox de la mayor componente conexa opaca (el cuerpo, sin las motas de polvo)."""
    m = alpha > 128
    lbl, n = ndimage.label(m)
    if n == 0:
        return None
    sizes = ndimage.sum(m, lbl, range(1, n + 1))
    big = lbl == (int(np.argmax(sizes)) + 1)
    ys, xs = np.where(big)
    return xs.min(), xs.max(), ys.min(), ys.max()


# --------------------------------------------------------------------------
# cabras
# --------------------------------------------------------------------------
# El sheet coloca los 3 fotogramas sobre un lienzo comun -> conservar la altura
# completa preserva el balanceo vertical tal y como lo dibujo el artista.
# Orden de los tramos dentro del sheet, verificado contra los archivos sueltos:
SHEET_ORDER = ["downflap", "upflap", "midflap"]

GOAT_SHEETS = {
    # sheet             -> prefijo que espera main.cpp
    "goat1Sprites.jpg": "redbird",       # cabra oscura moteada
    "goat2Sprites.jpg": "yellow_bird",   # cabra marron
    "goat3Sprites.jpg": "blue_bird",     # cabra azul
}


def build_goats(report):
    out = {}
    for sheet, prefix in GOAT_SHEETS.items():
        rgb = load_rgb(sheet)
        H = rgb.shape[0]
        runs = frame_runs(rgb)
        if len(runs) != 3:
            raise SystemExit(f"{sheet}: esperaba 3 fotogramas, encontre {len(runs)}")

        # Lienzo comun: ancho del fotograma mas ancho + margen, altura del sheet.
        cell_w = max(r[1] - r[0] + 1 for r in runs) + 10

        for (x0, x1), part in zip(runs, SHEET_ORDER):
            # anclado por el hocico (borde derecho del tramo), que es lo que se
            # mantiene fijo entre fotogramas en esta animacion
            right = x1 + 5
            left = right - cell_w
            canvas = np.full((H, cell_w, 3), 255, dtype=np.int16)
            src_l, src_r = max(0, left), min(rgb.shape[1], right)
            canvas[:, src_l - left:src_r - left] = rgb[:, src_l:src_r]

            rgba = key_out(canvas, white=True, thresh=235, erode=1)
            small = resize_rgba(rgba, (GOAT_W, GOAT_H))
            name = f"{prefix}-{part}.png"
            out[name] = small

            bb = body_bbox(np.array(small)[:, :, 3])
            report.setdefault("goat_bodies", []).append((name, bb))
    return out


# --------------------------------------------------------------------------
# lanzas (pipes)
# --------------------------------------------------------------------------
def build_pipes(report):
    out = {}
    for src, targets in (
        ("topPipe.jpg", ["gTopPipe.png", "rTopPipe.png"]),
        ("bottomPipe.jpg", ["gBottomPipe.png", "rBottomPipe.png"]),
    ):
        rgb = load_rgb(src)

        # NO se puede usar un umbral de negro con relleno desde el borde. El
        # collar bajo la punta tiene los flancos muy sombreados (por debajo del
        # umbral) y tocan el fondo negro por el canto del asta, asi que el
        # relleno se los llevaba y quedaban dos franjas transparentes por las
        # que se veia el escenario.
        #
        # El arma es CONVEXA en horizontal: en cada fila ocupa un solo tramo.
        # Asi que basta con marcar los pixeles claramente del arma y rellenar
        # el vano entre el primero y el ultimo de cada fila.
        lum = rgb.max(axis=2)
        strong = lum > PipeSolidLum
        alpha = np.zeros(lum.shape, dtype=np.uint8)
        for y in range(lum.shape[0]):
            xs = np.where(strong[y])[0]
            if xs.size < PipeMinRun:
                continue          # fila vacia o ruido suelto del JPEG
            alpha[y, xs.min():xs.max() + 1] = 255

        rgba = np.dstack([rgb.astype(np.uint8), alpha])
        small = resize_rgba(rgba, (PIPE_W, PIPE_H))

        arr = np.array(small)[:, :, 3]
        cols = np.where((arr > 128).any(axis=0))[0]
        report.setdefault("pipe_bodies", []).append(
            (src, int(cols.min()), int(cols.max()))
        )
        for t in targets:
            out[t] = small
    return out


# --------------------------------------------------------------------------
# suelo
# --------------------------------------------------------------------------
def build_base(report):
    rgb = load_rgb("base.jpg")
    rgb = rgb[5:, :, :]                       # recorta la franja magenta superior
    H, W, _ = rgb.shape
    N = 300                                    # ancho del cross-fade de la costura
    res = rgb[:, : W - N].astype(np.float32).copy()
    for i in range(N):
        t = i / N
        res[:, i] = rgb[:, i] * t + rgb[:, W - N + i] * (1.0 - t)
    res = np.clip(res, 0, 255).astype(np.uint8)

    seam = float(np.abs(res[:, 0].astype(int) - res[:, -1].astype(int)).mean())
    report["base_seam"] = seam

    # A potencia de dos (ver BASE_W/BASE_H). El reescalado es uniforme, asi que
    # la costura sigue siendo continua.
    im = Image.fromarray(res, "RGB").resize((BASE_W, BASE_H), Image.LANCZOS)
    report["base_size"] = im.size
    report["base_seam_final"] = float(
        np.abs(np.array(im)[:, 0].astype(int) - np.array(im)[:, -1].astype(int)).mean()
    )
    return {"base.png": im.convert("RGBA")}


# --------------------------------------------------------------------------
# fondos
# --------------------------------------------------------------------------
def build_backgrounds(report):
    out = {}
    day = Image.open(os.path.join(SRC, "background-day.jpg")).convert("RGB")
    night = Image.open(os.path.join(SRC, "background-night.jpg")).convert("RGB")
    credit = Image.open(os.path.join(SRC, "Credit Back ground.jpg")).convert("RGB")

    day = day.resize((WINDOW_W, WINDOW_H), Image.LANCZOS)
    night = night.resize((WINDOW_W, WINDOW_H), Image.LANCZOS)
    credit = credit.resize((WINDOW_W, WINDOW_H), Image.LANCZOS)

    # El desenfoque del menu se deriva del fondo NUEVO: usar el original dejaria
    # el cielo del Flappy Bird clasico detras del menu y el volcan en la partida.
    blur = day.filter(ImageFilter.GaussianBlur(14))

    out["background-day.png"] = day.convert("RGBA")
    out["background-night.png"] = night.convert("RGBA")
    out["background-day-blur.png"] = blur.convert("RGBA")
    out["Credit Back ground.jpg"] = credit
    report["bg"] = "1280x860"
    return out


# --------------------------------------------------------------------------
# marcadores / medallas
# --------------------------------------------------------------------------
# El margen de las medallas no es blanco puro: el borde derecho es un gris ~234,
# asi que un umbral alto (250) deja el fondo sin detectar y el recorte sale entero.
BOARD_THRESH = 225


def matte_white(rgb, loose=200, solid=170):
    """Recorta un fondo blanco con alpha progresivo (matting).

    Un umbral binario deja una banda gris de 2-3px alrededor del tablero: es el
    degradado JPEG entre el borde oscuro y el margen blanco, cuyos valores
    intermedios quedan por debajo del umbral y sobreviven. Aqui se calcula un
    alpha proporcional dentro de la region de fondo y se descontamina el color
    del blanco que lleva mezclado.
    """
    region = outer_background(rgb, white=True, thresh=loose)
    lum = rgb.min(axis=2).astype(np.float32)
    a = np.clip((255.0 - lum) / float(255 - solid), 0.0, 1.0)
    alpha = np.where(region, a, 1.0)

    out = rgb.astype(np.float32)
    safe = np.maximum(alpha, 1e-3)[:, :, None]
    # observado = color*alpha + blanco*(1-alpha)  ->  despejar color
    out = (out - 255.0 * (1.0 - alpha)[:, :, None]) / safe
    out = np.clip(out, 0, 255)
    return np.dstack([out.astype(np.uint8), (alpha * 255).astype(np.uint8)])


def trim_white_border(rgba, near=242, frac=0.5):
    """Quita filas/columnas del borde que siguen siendo fondo casi blanco opaco.

    El flood-fill se queda una fila corto por el degradado JPEG del margen y deja
    un fleco blanco de 1px en el canto inferior del tablero.
    """
    while rgba.shape[0] > 2 and rgba.shape[1] > 2:
        cut = False
        for axis, idx in ((0, 0), (0, -1), (1, 0), (1, -1)):
            line = rgba[idx, :] if axis == 0 else rgba[:, idx]
            op = line[:, 3] > 128
            if op.sum() == 0:
                continue
            if (line[:, :3][op].min(axis=1) >= near).mean() >= frac:
                if axis == 0:
                    rgba = rgba[1:] if idx == 0 else rgba[:-1]
                else:
                    rgba = rgba[:, 1:] if idx == 0 else rgba[:, :-1]
                cut = True
                break
        if not cut:
            break
    return rgba


def crop_board(name):
    rgb = load_rgb(name)
    bg = outer_background(rgb, white=True, thresh=BOARD_THRESH)
    ys, xs = np.where(~bg)
    sub = rgb[ys.min(): ys.max() + 1, xs.min(): xs.max() + 1]
    return trim_white_border(matte_white(sub))


def find_coin(rgba):
    """Localiza la moneda: mayor blob que no es ni el cream del tablero ni texto."""
    rgb = rgba[:, :, :3].astype(int)
    h, w, _ = rgb.shape
    # color dominante del tablero
    inner = rgb[h // 4: 3 * h // 4, w // 4: 3 * w // 4].reshape(-1, 3)
    cream = np.median(inner, axis=0)
    dist = np.abs(rgb - cream).sum(axis=2)
    blob = dist > 90
    blob[:, w // 2:] = False              # la moneda esta en la mitad izquierda
    blob = ndimage.binary_closing(blob, np.ones((9, 9)))
    lbl, n = ndimage.label(blob)
    sizes = ndimage.sum(blob, lbl, range(1, n + 1))
    big = lbl == (int(np.argmax(sizes)) + 1)
    ys, xs = np.where(big)
    return (xs.min(), xs.max(), ys.min(), ys.max()), cream


def build_boards(report):
    out = {}
    boards = {}
    for src, dst in (
        ("bronzemedal.jpg", "bronzemedal.png"),
        ("silvermedal.jpg", "silvermedal.png"),
        ("goldmedal.jpg", "goldmedal.png"),
    ):
        rgba = crop_board(src)
        boards[dst] = rgba
        out[dst] = resize_rgba(rgba, (BOARD_W, BOARD_H))

    # --- tablero vacio: se borra la moneda del tablero de oro ---
    base = boards["goldmedal.png"].copy()
    (cx0, cx1, cy0, cy1), cream = find_coin(base)
    pad = 6
    base[max(0, cy0 - pad): cy1 + pad, max(0, cx0 - pad): cx1 + pad, :3] = cream.astype(np.uint8)
    out["emptyboard.png"] = resize_rgba(base, (BOARD_W, BOARD_H))
    report["coin_box"] = (int(cx0), int(cx1), int(cy0), int(cy1))
    report["board_size"] = (base.shape[1], base.shape[0])

    # color medio de la moneda de cada tablero, para detectar un posible cruce
    # entre bronce y oro
    for dst, rgba in boards.items():
        crop = rgba[cy0:cy1 + 1, cx0:cx1 + 1]
        m = crop[:, :, 3] > 128
        px = crop[:, :, :3][m].astype(float)
        report.setdefault("coin_colors", []).append(
            (dst, px.mean(axis=0).round(0).astype(int).tolist())
        )

    # --- platino: tablero vacio + moneda de plata aclarada y con tinte frio ---
    coin = np.array(Image.open(os.path.join(SRC, "silvercoin.png")).convert("RGBA"))
    cw, ch = cx1 - cx0 + 1, cy1 - cy0 + 1
    coin_im = resize_rgba(coin, (cw, ch))
    c = np.array(coin_im).astype(np.float32)
    c[:, :, 0] *= 1.02
    c[:, :, 1] *= 1.08
    c[:, :, 2] *= 1.20
    c[:, :, :3] = np.clip(c[:, :, :3] + 26, 0, 255)
    coin_im = Image.fromarray(np.clip(c, 0, 255).astype(np.uint8), "RGBA")

    plat = Image.fromarray(base, "RGBA")
    plat.alpha_composite(coin_im, (int(cx0), int(cy0)))
    out["platinummedal.png"] = resize_rgba(np.array(plat), (BOARD_W, BOARD_H))
    return out


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main():
    report = {}
    assets = {}
    assets.update(build_goats(report))
    assets.update(build_pipes(report))
    assets.update(build_base(report))
    assets.update(build_backgrounds(report))
    assets.update(build_boards(report))

    for dest in DESTS:
        if not os.path.isdir(dest):
            print(f"  !! no existe {dest}, lo salto")
            continue
        # Antes se hacia una copia en assets_original/. Se quito: los assets
        # estan versionados, asi que el respaldo real es el historial de git
        # (`git checkout -- FlappyBird-*/assets`). Mantener la copia era ademas
        # peligroso, porque al reejecutarse sin ella habria guardado los assets
        # NUEVOS haciendolos pasar por los originales.
        for name, im in assets.items():
            path = os.path.join(dest, name)
            if name.lower().endswith(".jpg"):
                im.convert("RGB").save(path, quality=94)
            else:
                im.save(path)
        print(f"  {len(assets)} archivos escritos en {dest}")

    print()
    print("--- informe ---")
    for name, bb in report.get("goat_bodies", []):
        x0, x1, y0, y1 = bb
        print(f"  {name:28s} lienzo {GOAT_W}x{GOAT_H} cuerpo x[{x0}..{x1}] y[{y0}..{y1}]")
    for src, c0, c1 in report.get("pipe_bodies", []):
        print(f"  {src:16s} -> arma ocupa x[{c0}..{c1}] de {PIPE_W}")
    print(f"  suelo: {report['base_size']} costura tras el cross-fade = {report['base_seam']:.1f} / tras reescalar = {report['base_seam_final']:.1f} (original 29.1)")
    bw, bh = report["board_size"]
    print(f"  tablero: {bw}x{bh} ratio={bw / bh:.3f} (original 1.903)  moneda en {report['coin_box']}")
    for dst, col in report.get("coin_colors", []):
        print(f"    {dst:20s} color medio de la moneda RGB={col}")


if __name__ == "__main__":
    main()
