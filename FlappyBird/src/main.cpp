#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>
#include <iostream>
#include <deque>
#include <fstream>
#include<sstream>
#include <algorithm>
#include <ctime>
#include <string>   // to_string: en MSVC llega de arrastre, en libstdc++ conviene pedirlo
#include <cctype>
#include "scoredb.h"

#ifdef SFML_SYSTEM_ANDROID
	#include <SFML/System/NativeActivity.hpp>
	#include <android/native_activity.h>
#endif

using namespace sf;
using namespace std;


/*     VELOCIDADES EN UNIDADES POR SEGUNDO     */
// El juego original movia todo "por fotograma" con vsync activo, asi que su
// velocidad dependia de los Hz del monitor (a 144Hz iba 2.4x mas rapido).
// Todo esta ahora en unidades por segundo y se integra con dt.
//
// Las constantes de DESPLAZAMIENTO (scroll, animaciones, botones) son las
// originales convertidas tomando 60 FPS como referencia: el comentario de cada
// linea indica el valor por fotograma del que salen.
// Las de SALTO no: ese control se rediseno a proposito (ver abajo).

// --- Salto ---
// El salto es un IMPULSO por pulsacion: mantener pulsado no sube mas. Con un
// impulso, la altura del arco es v0^2/(2g) y el tiempo en el aire 2*v0/g, o
// sea que AMBOS bajan si sube la gravedad. Para un salto a la vez mas amplio
// y mas lento se usa gravedad asimetrica: floja al subir (ascenso amplio y
// flotado) y fuerte al caer (caida con peso).
//
//   subida:  620/1500 = 0.41 s   hasta 620^2/(2*1500) = 128 px de altura
//   bajada:  esos 128 px en sqrt(2*128/2600) = 0.31 s
//   total en el aire ~0.72 s, con un hueco entre lanzas de 260 px
const float jumpSpeed = 620.f;             // impulso inicial hacia arriba
const float GravityRise = 1500.f;          // mientras sube
const float GravityFall = 2600.f;          // mientras cae
const float MaxFallSpeed = 1150.f;         // velocidad terminal, para que la
                                           // caida siga siendo legible
const float RotationSpeed = 120.f;         // 2 grados/frame
const float ScrollSpeed = 300.f;           // 5 px/frame (suelo y lanzas)
const float PipeSpawnInterval = 85.f / 60.f; // 85 fotogramas entre lanzas
const float WingSlowRate = 9.f;            // 0.15 indice/frame (en el menu)
const float WingFastRate = 21.f;           // 0.35 indice/frame (en partida)
const float UiSpeed = 600.f;               // 10 px/frame (carteles y marcador)
const float ButtonSpeed = 900.f;           // 15 px/frame (botones finales)

// Techo del paso de tiempo. Sin esto, tras un alt-tab o un tiron el juego
// integraria cientos de pixeles de golpe y la cabra atravesaria una lanza sin
// que la colision (AABB por fotograma) llegase a detectarla.
const float MaxFrameTime = 0.05f;

// Red de seguridad para el vsync. En algunas configuraciones (WSL, y segun el
// driver tambien en Raspberry Pi) setVerticalSyncEnabled no esta soportado: sin
// tope el bucle gira sin freno -medido en 969 FPS- y funde un nucleo, que en un
// Pi pasivo significa calor y throttling.
//
// El valor importa. Medido con vsync REAL a 143 Hz:
//     sin tope        -> 144 FPS
//     tope 144        -> 144 FPS   (no interfiere)
//     tope 120        -> 106 FPS   (vsync y tope se pelean)
// Es decir: un tope IGUAL O POR ENCIMA de la tasa de refresco es inocuo, y por
// debajo estropea el ritmo. 144 cubre 60 Hz (Pi) y 144 Hz (escritorio). Si algun
// dia se usa un monitor mas rapido, hay que subirlo.
const int FpsCap = 144;

// LIENZO DE DISENO. Todo el juego (posiciones, tamanos, colisiones, fondos) se
// dibuja SIEMPRE en estas coordenadas, sea cual sea el tamano real de ventana.
// Una sf::View se encarga de escalarlo y de anadir bandas negras para conservar
// la proporcion, asi que cambiar la resolucion no obliga a recolocar nada.
// Los tests de raton usan window.mapPixelToCoords(), que ya aplica esa View.
const unsigned DesignW = 1280, DesignH = 860;

// Zona de mundo realmente visible. Coincide con el lienzo de diseno en una
// pantalla 1280x860, y se ensancha (o se estira a lo alto) en otras
// proporciones. Lo usan el fondo, el suelo, el desove de lanzas y la interfaz
// pegada a los bordes.
float ViewLeft = 0.f, ViewRight = (float)DesignW;
float ViewTop = 0.f, ViewBottom = (float)DesignH;
float viewW() { return ViewRight - ViewLeft; }
float viewH() { return ViewBottom - ViewTop; }


// Geometria de la cabra y de las lanzas, ligada a los sprites generados por
// build_assets.py. Si cambian los sprites hay que revisar estos valores.
const float GoatDrawW = 74.f, GoatDrawH = 76.f;
const float GoatHitScaleX = 0.70f, GoatHitScaleY = 0.66f;
const float PipeDrawW = 130.f;


void get_BirdColor(char& colorChar); //Random Generator (Bird color)
void get_BirdColorNums(char colorChar, int& birdColorIndex, float& flySpeed, int& counter); //Getting the correct numbers for the selected color
int get_topPipeYpos(int topPipe_minYpos, int topPipe_maxYpos); //Getting a random Y-position for the TOP pipe

// Caja de colision de la cabra: getGlobalBounds() devuelve el AABB de la forma
// YA ROTADA, que crece hasta 1.4x al girar 45 grados, y ademas el lienzo del
// sprite lleva margen transparente. Se recorta al cuerpo visible para que la
// colision case con lo que se ve.
FloatRect goatHitbox(const RectangleShape& goat)
{
	FloatRect g = goat.getGlobalBounds();
	float cx = g.left + g.width / 2.f;
	float cy = g.top + g.height / 2.f;
	float w = GoatDrawW * GoatHitScaleX;
	float h = GoatDrawH * GoatHitScaleY;
	return FloatRect(cx - w / 2.f, cy - h / 2.f, w, h);
}

// SILUETA DE LA LANZA, medida sobre el asset (130x860): distancia desde la
// PUNTA, en fraccion de la altura, y semiancho en px a esa altura.
//
// Un unico rectangulo no vale: el arma se afila desde el 88% de su largo hasta
// acabar en pico, y una caja recta cubria ahi hasta 52 px de mas por lado,
// justo en la zona del hueco. Es lo que hacia morir "contra el aire".
struct PipeBand { float desdePunta; float semiancho; };
const PipeBand PipeProfile[] = {
	{ 0.00f,  2.f },   // el pico
	{ 0.02f, 11.f },
	{ 0.05f, 21.f },
	{ 0.08f, 30.f },
	{ 0.10f, 40.f },
	{ 0.12f, 50.f },
	{ 0.15f, 59.f },
	{ 0.20f, 60.f },   // el reborde metalico, la parte mas ancha
	{ 0.25f, 56.f },
	{ 1.00f, 52.f },   // el asta
};
const int PipeBands = (int)(sizeof(PipeProfile) / sizeof(PipeProfile[0]));

// Rectangulo de una banda. Se toma el semiancho MENOR de los dos extremos para
// que la caja nunca sobresalga del dibujo: si hay error, que sea a favor del
// jugador.
FloatRect pipeBandRect(const RectangleShape& pipe, bool puntaAbajo, int i)
{
	FloatRect b = pipe.getGlobalBounds();
	float cx = b.left + b.width / 2.f;

	float f0 = PipeProfile[i].desdePunta;
	float f1 = (i + 1 < PipeBands) ? PipeProfile[i + 1].desdePunta : 1.f;
	float hw = PipeProfile[i].semiancho;
	if (i + 1 < PipeBands)
		hw = min(hw, PipeProfile[i + 1].semiancho);

	float y0, y1;
	if (puntaAbajo)   // lanza de arriba: el pico mira hacia el hueco, abajo
	{
		y0 = b.top + b.height * (1.f - f1);
		y1 = b.top + b.height * (1.f - f0);
	}
	else              // lanza de abajo: el pico esta arriba
	{
		y0 = b.top + b.height * f0;
		y1 = b.top + b.height * f1;
	}
	return FloatRect(cx - hw, y0, 2.f * hw, y1 - y0);
}

bool pipeHits(const FloatRect& caja, const RectangleShape& pipe, bool puntaAbajo)
{
	// Descarte rapido por el rectangulo completo antes de mirar banda a banda.
	if (!caja.intersects(pipe.getGlobalBounds()))
		return false;
	for (int i = 0; i < PipeBands; ++i)
		if (caja.intersects(pipeBandRect(pipe, puntaAbajo, i)))
			return true;
	return false;
}

// Ancla el texto en su centro, para poder situarlo por el punto medio de la
// casilla del tablero en vez de por su esquina superior izquierda.
void centerText(Text& t)
{
	FloatRect r = t.getLocalBounds();
	t.setOrigin(r.left + r.width / 2.f, r.top + r.height / 2.f);
}

/*     MANDO     */
// SFML numera los botones del 0 al 31 SIN darles significado, y el indice de
// cada boton fisico cambia entre modelos de mando y entre plataformas: no hay
// tabla universal (por eso SDL necesita un gamecontrollerdb.txt). En cambio SI
// da significado a los EJES: X/Y son el stick principal y PovX/PovY la cruceta.
//
// De ahi el diseno, que no depende de ningun indice de boton:
//   - navegar   -> ejes (cruceta o stick)
//   - confirmar -> CUALQUIER boton
//   - aletear   -> CUALQUIER boton
//
// El signo de los ejes tampoco esta documentado y se invierte segun el mando,
// asi que la navegacion es CIRCULAR: aunque venga invertida, se alcanza todo
// pulsando repetidamente en la misma direccion.
const float PadDeadZone = 60.f;   // de 100; empujones deliberados solamente

// Direccion del mando en los dos ejes, cada componente -1, 0 o +1. Se mira
// primero la cruceta y, si no la hay, el stick.
Vector2i padNavVector()
{
	Vector2i d(0, 0);
	for (unsigned j = 0; j < Joystick::Count; ++j)
	{
		if (!Joystick::isConnected(j))
			continue;

		const Joystick::Axis horiz[] = { Joystick::PovX, Joystick::X };
		const Joystick::Axis vert[] = { Joystick::PovY, Joystick::Y };

		for (int i = 0; i < 2 && d.x == 0; ++i)
			if (Joystick::hasAxis(j, horiz[i]))
			{
				float v = Joystick::getAxisPosition(j, horiz[i]);
				if (v > PadDeadZone) d.x = +1;
				else if (v < -PadDeadZone) d.x = -1;
			}

		for (int i = 0; i < 2 && d.y == 0; ++i)
			if (Joystick::hasAxis(j, vert[i]))
			{
				float v = Joystick::getAxisPosition(j, vert[i]);
				if (v > PadDeadZone) d.y = +1;
				else if (v < -PadDeadZone) d.y = -1;
			}

		if (d.x != 0 || d.y != 0)
			break;
	}
	return d;
}

/*     NOMBRE DE JUGADOR     */
// Editor de 8 casillas al estilo recreativa: sirve igual con teclado (se
// escribe directamente) que con mando (arriba/abajo cambia la letra de la
// casilla, izquierda/derecha cambia de casilla, cualquier boton acepta).
const int NameLen = 8;
const string NameAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";

// Siguiente/anterior caracter dentro del alfabeto, circular.
char cycleNameChar(char c, int delta)
{
	size_t i = NameAlphabet.find(c);
	if (i == string::npos)
		i = 0;
	size_t n = NameAlphabet.size();
	return NameAlphabet[(i + n + delta) % n];
}

// Quita los espacios de los extremos; si no queda nada, devuelve un generico.
string trimmedName(const string& s)
{
	size_t a = s.find_first_not_of(' ');
	if (a == string::npos)
		return "JUGADOR";
	size_t b = s.find_last_not_of(' ');
	return s.substr(a, b - a + 1);
}

/*     PUNTERO UNIFICADO     */
// En escritorio es el raton; en Android, el primer dedo. Los cinco tests de
// posicion de los menus siguen escritos igual, y como pasan por
// mapPixelToCoords la View de letterbox ya los coloca en el lienzo de diseno.
bool pointerDown()
{
#ifdef SFML_SYSTEM_ANDROID
	return Touch::isDown(0);
#else
	return Mouse::isButtonPressed(Mouse::Left);
#endif
}

Vector2i pointerPixel(const Window& w)
{
#ifdef SFML_SYSTEM_ANDROID
	return Touch::getPosition(0, w);
#else
	return Mouse::getPosition(w);
#endif
}

// Donde se guarda scores.db. En Android el directorio de trabajo NO es
// escribible: hay que usar el almacenamiento interno de la app, que la
// NativeActivity ya expone.
string scoreDbPath()
{
#ifdef SFML_SYSTEM_ANDROID
	ANativeActivity* act = getNativeActivity();
	if (act && act->internalDataPath)
		return string(act->internalDataPath) + "/scores.db";
	return "/data/local/tmp/scores.db";   // ultimo recurso
#else
	return "scores.db";
#endif
}

// Escala un fondo de 1280x860 para CUBRIR la zona visible y lo centra. Se
// escala por igual en los dos ejes -nada de estirar- asi que en una pantalla
// muy ancha sobra por arriba y por abajo, que es lo que se recorta.
void coverView(Sprite& s)
{
	Vector2u t = s.getTexture()->getSize();
	if (t.x == 0 || t.y == 0)
		return;
	float k = max(viewW() / (float)t.x, viewH() / (float)t.y);
	s.setScale(k, k);
	s.setOrigin((float)t.x / 2.f, (float)t.y / 2.f);
	s.setPosition((ViewLeft + ViewRight) / 2.f, (ViewTop + ViewBottom) / 2.f);
}

// Coloca el recuadro de foco alrededor de un elemento cualquiera (sirve igual
// para un sprite que para un texto, porque se basa en su bounding box).
void placeFocus(RectangleShape& box, const FloatRect& b, float pad = 12.f)
{
	box.setPosition(b.left - pad, b.top - pad);
	box.setSize(Vector2f(b.width + 2.f * pad, b.height + 2.f * pad));
}

// La View OCUPA TODA la ventana, sin bandas negras. En vez de encajar el lienzo
// dentro de la pantalla (letterbox), se mantiene la escala y se muestra MAS
// mundo por el lado que sobra. Asi no hay deformacion -que es lo que pasaria al
// estirar- ni recorte -que en un movil 21:9 se comeria un cuarto de la pantalla
// por arriba y por abajo-.
View gameView(unsigned winW, unsigned winH)
{
	float cx = DesignW / 2.f, cy = DesignH / 2.f;
	float w = (float)DesignW, h = (float)DesignH;

	if (winW > 0 && winH > 0)
	{
		float winRatio = (float)winW / (float)winH;
		float designRatio = (float)DesignW / (float)DesignH;
		if (winRatio > designRatio)
			w = h * winRatio;   // pantalla mas ancha -> se ve mas a los lados
		else
			h = w / winRatio;   // pantalla mas alta  -> se ve mas arriba y abajo
	}

	ViewLeft = cx - w / 2.f;  ViewRight = cx + w / 2.f;
	ViewTop = cy - h / 2.f;   ViewBottom = cy + h / 2.f;

	View v(FloatRect(ViewLeft, ViewTop, w, h));
	v.setViewport(FloatRect(0.f, 0.f, 1.f, 1.f));   // toda la ventana
	return v;
}

// La Raspberry Pi se identifica por el arbol de dispositivos. Su GPU va muy
// justa de relleno, asi que ahi conviene abrir a menos resolucion por defecto;
// en un PC seria un mal valor por defecto, de ahi la deteccion.
bool isRaspberryPi()
{
#ifdef __linux__
	ifstream f("/proc/device-tree/model");
	if (!f)
		return false;
	string modelo;
	getline(f, modelo);
	return modelo.find("Raspberry Pi") != string::npos;
#else
	return false;
#endif
}

struct WindowConfig
{
	unsigned width = DesignW;
	unsigned height = DesignH;
	bool sizeGiven = false;   // el usuario paso un ANCHOxALTO explicito
	bool fullscreen = false;
	bool helpRequested = false;
	bool debug = false;      // FPS + cajas de colision en pantalla
};

void printUsage(const char* exe)
{
	cerr << "Uso: " << exe << " [ANCHOxALTO] [--fullscreen]\n"
	     << "  (sin argumentos)  ventana de " << DesignW << "x" << DesignH << "\n"
	     << "  800x480           ventana de ese tamano\n"
	     << "  --fullscreen, -f  resolucion actual del escritorio\n\n"
	     << "El juego se dibuja siempre en un lienzo de " << DesignW << "x" << DesignH
	     << " y se escala conservando\nla proporcion; el sobrante queda en negro.\n"
	     << "Si el tamano pedido no cabe en la pantalla, se reduce automaticamente.\n";
}

bool parseArgs(int argc, char** argv, WindowConfig& cfg)
{
	for (int i = 1; i < argc; ++i)
	{
		string a = argv[i];
		if (a == "-f" || a == "--fullscreen")
		{
			cfg.fullscreen = true;
			continue;
		}
		if (a == "-d" || a == "--debug")
		{
			cfg.debug = true;
			continue;
		}
		if (a == "-h" || a == "--help")
		{
			cfg.helpRequested = true;
			return false;
		}

		unsigned w = 0, h = 0;
		char sep = 0;
		istringstream is(a);
		if ((is >> w >> sep >> h) && (sep == 'x' || sep == 'X') && w >= 320 && h >= 240)
		{
			cfg.width = w;
			cfg.height = h;
			cfg.sizeGiven = true;
			continue;
		}
		cerr << "Argumento no reconocido: " << a << "\n\n";
		return false;
	}
	return true;
}

int main(int argc, char** argv)
{
	srand((unsigned)time(nullptr)); // sin esto el color de cabra, el dia/noche y
	                                // las alturas salian identicos en cada partida

	WindowConfig cfg;
	if (!parseArgs(argc, argv, cfg))
	{
		printUsage(argv[0]);
		// pedir ayuda no es un error; un argumento invalido si
		return cfg.helpRequested ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	// En una Pi, y solo si no se pidio un tamano concreto, se arranca a menos
	// resolucion: el coste dominante es el relleno, o sea los pixeles de la
	// ventana, no el tamano de las texturas. 800x538 son el 39% de los pixeles
	// de 1280x860, conservando la proporcion del lienzo.
	if (!cfg.sizeGiven && !cfg.fullscreen && isRaspberryPi())
	{
		cfg.width = 800;
		cfg.height = (unsigned)(800.0 * DesignH / DesignW + 0.5);
		cerr << "Raspberry Pi detectada: abriendo a " << cfg.width << "x" << cfg.height
		     << " por rendimiento (pasa un tamano para forzar otro)" << endl;
	}

	VideoMode desktop = VideoMode::getDesktopMode();
	VideoMode mode(cfg.width, cfg.height);
	Uint32 style = Style::Titlebar | Style::Close;

#ifdef SFML_SYSTEM_ANDROID
	// En Android no hay linea de ordenes ni ventanas: siempre a pantalla
	// completa con la resolucion del dispositivo. La View de letterbox se
	// encarga de encajar el lienzo de diseno en cualquier proporcion.
	cfg.fullscreen = true;
	mode = desktop;
	style = Style::Fullscreen;
#endif

	if (cfg.fullscreen && desktop.isValid())
	{
		mode = desktop;
		style = Style::Fullscreen;
	}
	else
	{
		cfg.fullscreen = false;
		// Si lo pedido no cabe en la pantalla -se da con el tamano por defecto
		// en las pantallas pequenas de Raspberry Pi- se reduce conservando la
		// proporcion, en vez de abrir una ventana que se sale.
		if (desktop.width > 0 && desktop.height > 0
			&& (mode.width > desktop.width || mode.height > desktop.height))
		{
			float s = min((float)desktop.width / mode.width,
			              (float)desktop.height / mode.height);
			mode.width = (unsigned)(mode.width * s);
			mode.height = (unsigned)(mode.height * s);
			cerr << "La ventana no cabia en la pantalla; ajustada a "
			     << mode.width << "x" << mode.height << endl;
		}
	}

	RenderWindow window(mode, "BaifoBird", style);
	// Todo el dibujado ocurre en coordenadas de diseno; la View lo escala.
	window.setView(gameView(window.getSize().x, window.getSize().y));
	window.setVerticalSyncEnabled(true);
	// Sin esto, mantener espacio pulsado hace que el sistema emita KeyPressed
	// repetidos (~30/s tras medio segundo) y el salto volveria a ser mantenible.
	window.setKeyRepeatEnabled(false);
	window.setFramerateLimit(FpsCap);
	Clock frameClock;

	/*     TEXTURES & SPRITES     */


#pragma region Font

	Font gameFont;
	if (!gameFont.loadFromFile("fonts/Flappy_Bird.ttf"))
		return EXIT_FAILURE;

#pragma endregion   

	/*........................................... ....................*/
#pragma region File

	// Las puntuaciones viven en SQLite (scores.db, junto al ejecutable). Si la
	// base no se pudiera abrir, el juego sigue siendo jugable: simplemente no
	// guarda nada.
	ScoreDb scores;
	if (!scores.open(scoreDbPath()))
		cerr << "aviso: no se pudo abrir scores.db (" << scores.lastError()
		     << "); las partidas no se guardaran" << endl;

	int bestScore = scores.bestScore();

#pragma endregion


#pragma region Sound


	struct Allsounds
	{
		SoundBuffer pointBuffer, flyBuffer, hitBuffer, swooshBuffer, dieBuffer;
	} Sounds;

	if (!Sounds.pointBuffer.loadFromFile("audios/point.wav")
		|| !Sounds.flyBuffer.loadFromFile("audios/wing.wav")
		|| !Sounds.hitBuffer.loadFromFile("audios/hit.wav")
		|| !Sounds.swooshBuffer.loadFromFile("audios/swooshing.wav")
		|| !Sounds.dieBuffer.loadFromFile("audios/die.wav")
		)
		return EXIT_FAILURE;

	Sound point_sound, fly_sound, hit_sound, swoosh_sound, die_sound;
	point_sound.setBuffer(Sounds.pointBuffer);
	fly_sound.setBuffer(Sounds.flyBuffer);
	hit_sound.setBuffer(Sounds.hitBuffer);
	swoosh_sound.setBuffer(Sounds.swooshBuffer);
	die_sound.setBuffer(Sounds.dieBuffer);


#pragma endregion		 



#pragma region Main Menu

	// Play Button
	Texture play_button;
	if (!play_button.loadFromFile("assets/PlayButton.png"))
		return EXIT_FAILURE;

	RectangleShape playButton(Vector2f(251, 135));
	playButton.setTexture(&play_button);
	playButton.setOrigin(251 / 2, 135 / 2);
	playButton.setPosition(DesignW / 2, 450);
	bool isPlayButton_Pressed = false;

	// FlappBird Word
	Text flappyBird_word("BaifoBird", gameFont, 150);
	flappyBird_word.setFillColor(Color::White);
	flappyBird_word.setOutlineThickness(10);
	flappyBird_word.setOutlineColor(Color(47, 79, 79));
	// Centrado sobre la zona visible: el ancho del texto cambia con el
	// titulo y la vista se ensancha en pantallas panoramicas.
	flappyBird_word.setPosition(
		(ViewLeft + ViewRight) / 2.f - flappyBird_word.getGlobalBounds().width / 2.f, 65);

	// Credits Word
	Text credits_word("Credits", gameFont, 60);
	credits_word.setPosition(ViewLeft + 60, DesignH - 120);
	credits_word.setFillColor(Color(255, 245, 0));
	credits_word.setOutlineThickness(3.0);
	credits_word.setOutlineColor(Color::Black);
	bool isCredits_Pressed = false;

	// HighScore Word
	Text highScore_word("HighScore", gameFont, 60);
	highScore_word.setPosition(ViewRight - 336, DesignH - 120);
	highScore_word.setFillColor(Color(255, 245, 0));
	highScore_word.setOutlineThickness(2.0);
	highScore_word.setOutlineColor(Color::Black);
	bool isHighScore_Pressed = false;

#pragma endregion

#pragma region Creidts

	Text credit_names("Doramas", gameFont, 110);
	Text madyBy("A Game By", gameFont, 70);
	Text return_to_mainMenu("Press \"ESC\" to return to Main Menu", gameFont, 25);

	Texture credit_names_BG;
	if (!credit_names_BG.loadFromFile("assets/Credit Back ground.jpg"))  // with the bird 
		return EXIT_FAILURE;
	Sprite credit_names_BGSprite(credit_names_BG);

	// Names
	// La maquetacion anterior era una rejilla de tabuladores para seis nombres;
	// con uno solo se centra en la pantalla.
	credit_names.setFillColor(Color(255, 245, 0));
	credit_names.setOutlineThickness(5.0);
	credit_names.setOutlineColor(Color::Black);
	credit_names.setPosition(DesignW / 2.f - credit_names.getGlobalBounds().width / 2.f, 330);

	// Made By
	madyBy.setFillColor(Color::White);
	madyBy.setOutlineThickness(3.0);
	madyBy.setOutlineColor(Color(47, 79, 79));
	madyBy.setPosition(DesignW / 2.f - madyBy.getGlobalBounds().width / 2.f, 210);

	// Return to Main Menu
	return_to_mainMenu.setFillColor(Color(50, 168, 82));
	return_to_mainMenu.setOutlineThickness(2.5);
	return_to_mainMenu.setOutlineColor(Color::White);
	return_to_mainMenu.setPosition(ViewLeft + 50, DesignH - 240);

#pragma endregion

#pragma region Get Ready

	Texture get_Ready;
	if (!get_Ready.loadFromFile("assets/GetReady.png"))
		return EXIT_FAILURE;

	RectangleShape getReady(Vector2f(400, 400));
	getReady.setTexture(&get_Ready);
	getReady.setOrigin(getReady.getSize().x / 2, getReady.getSize().y / 2);
	getReady.setPosition(DesignW / 2, DesignH / 2);
	bool isGetReady_Pressed = false;

#pragma endregion


#pragma region Background 



	struct BG_Imag
	{
		Texture bkImage[3];

	} BGImages;

	if (!BGImages.bkImage[0].loadFromFile("assets/background-day.png")
		|| !BGImages.bkImage[1].loadFromFile("assets/background-night.png")
		|| !BGImages.bkImage[2].loadFromFile("assets/background-day-blur.png")
		)
		return EXIT_FAILURE;

	Sprite bgi[3];
	bgi[0].setTexture(BGImages.bkImage[0]);
	bgi[1].setTexture(BGImages.bkImage[1]);
	bgi[2].setTexture(BGImages.bkImage[2]);

	//Random Generator (day - night)
	bool day;
	day = rand() % 2;

#pragma endregion



#pragma region Bird & Gravity

	struct birdimgs
	{
		Texture Bird[9];

	}BirdImages;

	if (
		//Red Bird
		!BirdImages.Bird[0].loadFromFile("assets/redbird-upflap.png")
		|| !BirdImages.Bird[1].loadFromFile("assets/redbird-midflap.png")
		|| !BirdImages.Bird[2].loadFromFile("assets/redbird-downflap.png")
		//Blue Bird
		|| !BirdImages.Bird[3].loadFromFile("assets/blue_bird-upflap.png")
		|| !BirdImages.Bird[4].loadFromFile("assets/blue_bird-midflap.png")
		|| !BirdImages.Bird[5].loadFromFile("assets/blue_bird-downflap.png")
		//Yellow Bird
		|| !BirdImages.Bird[6].loadFromFile("assets/yellow_bird-upflap.png")
		|| !BirdImages.Bird[7].loadFromFile("assets/yellow_bird-midflap.png")
		|| !BirdImages.Bird[8].loadFromFile("assets/yellow_bird-downflap.png")
		)
		return EXIT_FAILURE;

	// Lienzo comun de los 9 fotogramas de cabra. Los tres fotogramas de cada
	// color se normalizaron al mismo tamano en build_assets.py; si tuvieran
	// tamanos distintos (como venian recortados) la cabra se estiraria y
	// encogeria en cada ciclo de la animacion.
	RectangleShape bird(Vector2f(GoatDrawW, GoatDrawH));
	bird.setOrigin(bird.getSize().x / 2, bird.getSize().y / 2);
	bird.setPosition(DesignW / 2, DesignH / 2);
	char colorChar;

	//Getting bird color
	get_BirdColor(colorChar);

	//Fly Speed 
	int birdColorIndex;
	float flySpeed;
	int counter;

	//Getting bird color's Numbers
	get_BirdColorNums(colorChar, birdColorIndex, flySpeed, counter);

	//---Gravity---  (las constantes viven ahora arriba, en px/s y px/s^2)
	float rotationAngle = 0;
	Vector2f jumpVelocity(0, 0);

#pragma endregion



#pragma region Base


	Texture Base;
	if (!Base.loadFromFile("assets/base.png"))
		return EXIT_FAILURE;

	// El suelo original se desplazaba moviendo un rectangulo de 1900px y
	// reposicionandolo al llegar a x=-625, una constante calculada a mano para
	// el patron de base.png. La textura nueva no es periodica, asi que ese
	// truco dejaria un corte visible. Aqui el scroll es infinito de verdad:
	// textura repetida + desplazamiento del textureRect en modulo.
	Base.setRepeated(true);

	RectangleShape base(Vector2f(viewW(), 112));
	base.setTexture(&Base);

	float groundHeight = DesignH - base.getSize().y;
	base.setPosition(ViewLeft, groundHeight);

	const int baseTexH = (int)Base.getSize().y;
	// pixeles de textura que caben en el ancho de pantalla conservando el aspecto
	const int baseTexW = (int)(base.getSize().x * baseTexH / base.getSize().y);
	// el textureRect avanza mas rapido que la pantalla en esa misma proporcion
	const float baseTexRate = (float)baseTexW / base.getSize().x;
	float baseScroll = 0.f;
	base.setTextureRect(IntRect(0, 0, baseTexW, baseTexH));

	Vector2f baseVelocity(-ScrollSpeed, 0); // usado tambien por las lanzas

#pragma endregion

#pragma region Pipes

	Texture gPipe[2], rPipe[2];
	if (!gPipe[0].loadFromFile("assets/gTopPipe.png")
		|| !gPipe[1].loadFromFile("assets/gBottomPipe.png"))
		return EXIT_FAILURE;

	if (!rPipe[0].loadFromFile("assets/rTopPipe.png")
		|| !rPipe[1].loadFromFile("assets/rBottomPipe.png"))
		return EXIT_FAILURE;

	RectangleShape top_pipe(Vector2f(PipeDrawW, DesignH));
	RectangleShape bottom_pipe(Vector2f(PipeDrawW, DesignH));
	top_pipe.setOrigin(0, top_pipe.getSize().y);

	deque <RectangleShape> topPipe, bottomPipe;
	int pipeIndex = 0;
	int sizeOf_PipesDeque = 0;
	// Primera lanza que todavia importa. Las anteriores ya salieron por la
	// izquierda y no hace falta ni moverlas ni dibujarlas. Sin esto se
	// recorrian las 100 en cada fotograma: 200 draw calls, casi todos fuera
	// de pantalla, que es lo que ahogaba a la Raspberry Pi.
	int firstActivePipe = 0;

	const int topPipe_minYpos = 120, topPipe_maxYpos = 380; // Top Pipe minimum & bestScoremum positions on Y-axis 
	float topPipeYpos, bottomPipeYpos; // To save top & bottom pipe's positions 

	const int numOfPipes = 100;
	int numOfPipesCounter = 0;
	float pipeSpawnTimer = 0.f; // segundos acumulados desde la ultima lanza
	bool isCollided = false;
	bool isScored = false;

	const float distanceY_betweenPipes = 260; // Distance between Pipes on Y-axis

#pragma endregion


	/*........................................... ....................*/
#pragma region Show score during the game

	int scoreCounter = 0;
	ostringstream ssScore;
	ssScore << scoreCounter;
	Text labelScore;
	labelScore.setCharacterSize(60);
	labelScore.setFont(gameFont);
	labelScore.setOutlineThickness(2.5);
	labelScore.setOutlineColor(Color::Black);
	labelScore.setString(ssScore.str());
	labelScore.setPosition(DesignW / 2, 60);


#pragma endregion

#pragma region GamePause

	bool isPaused = false;
	Text Pause("Game Paused", gameFont, 120);
	Text resume("Press \"P\" or any pad button to resume", gameFont, 40);

	//Pause word
	Pause.setPosition(280, 200);
	Pause.setOutlineThickness(4.0);
	Pause.setOutlineColor(Color::Black);

	//Resume word 
	resume.setPosition(430, 390);
	resume.setOutlineThickness(3.5);
	resume.setOutlineColor(Color::Black);

#pragma endregion 

#pragma region Game Over

	Texture game_over;
	if (!game_over.loadFromFile("assets/gameover.png"))
		return EXIT_FAILURE;

	RectangleShape gameOver(Vector2f(500, 115));
	gameOver.setTexture(&game_over);
	gameOver.setOrigin(gameOver.getSize().x / 2, gameOver.getSize().y / 2);
	gameOver.setPosition(DesignW / 2, -200);
	bool isGameOver = false;

#pragma endregion



	/*........................................... ....................*/
#pragma region Score Board 

	Texture score_board[5];
	if (!score_board[0].loadFromFile("assets/emptyboard.png")
		|| !score_board[1].loadFromFile("assets/bronzemedal.png")
		|| !score_board[2].loadFromFile("assets/silvermedal.png")
		|| !score_board[3].loadFromFile("assets/goldmedal.png")
		|| !score_board[4].loadFromFile("assets/platinummedal.png")
		)
		return EXIT_FAILURE;

	RectangleShape scoreBoard(Vector2f(400, 220));
	scoreBoard.setOrigin(scoreBoard.getSize().x / 2, scoreBoard.getSize().y / 2);
	scoreBoard.setPosition(DesignW / 2, DesignH + 200);

#pragma endregion



	/*........................................... ....................*/
#pragma region New & High Score (Numbers)

	// Los numeros se centran bajo las etiquetas SCORE y BEST del tablero nuevo,
	// medidas sobre el asset final: ambas caen en x=0.825 del ancho del tablero,
	// y las cifras van justo debajo -> (770, 343) y (770, 420) en pantalla.
	const float currentScoreTargetY = 343.f;
	const float highScoreTargetY = 420.f;
	const float scoreBoardTargetY = 380.f;

	// Current Score (Number)
	Text currentScoreNum;
	currentScoreNum.setFont(gameFont);
	currentScoreNum.setCharacterSize(25);
	currentScoreNum.setPosition(770, scoreBoard.getPosition().y - 37);

	// High Score (Number)
	Text highScoreNum;
	highScoreNum.setFont(gameFont);
	highScoreNum.setPosition(770, scoreBoard.getPosition().y + 40);

#pragma endregion

#pragma region Replay & Main Menu Buttons

	// Replay Button
	Texture replay_button;
	if (!replay_button.loadFromFile("assets/PlayButton.png"))
		return EXIT_FAILURE;

	RectangleShape replayButton(Vector2f(150, 90));
	replayButton.setTexture(&replay_button);
	replayButton.setPosition(ViewLeft - 200, 510);

	// Main Menu Button
	Text mainMenuButton("Main\nMenu", gameFont, 47);
	mainMenuButton.setFillColor(Color::White);
	mainMenuButton.setOutlineThickness(2.5);
	mainMenuButton.setOutlineColor(Color::Black);
	mainMenuButton.setPosition(ViewRight + 10, 510);

#pragma endregion

#pragma region Congratulations

	Text congrats("Congratulations", gameFont, 90);
	congrats.setFillColor(sf::Color::White);
	congrats.setOutlineColor(sf::Color(60, 179, 113));
	congrats.setOutlineThickness(30);
	congrats.setPosition((DesignW) - 975, -200);
	bool isWon = false;

#pragma endregion

#pragma region Foco de menu (mando / teclado)

	// Elementos navegables: menu principal [Play, HighScore, Credits] y
	// pantalla final [Replay, Main Menu].
	int menuFocus = 0;
	int overFocus = 0;
	Vector2i padDirPrev(0, 0);  // para detectar flancos en los ejes
	bool padActive = false;  // el resaltado solo se ve si se navega sin raton
	bool pointerPrev = false;   // para detectar el flanco de pulsacion

	// --- diagnostico (--debug) ---
	Text dbgText("", gameFont, 26);
	dbgText.setFillColor(Color(0, 255, 120));
	dbgText.setOutlineThickness(2.f);
	dbgText.setOutlineColor(Color::Black);
	RectangleShape dbgBox;
	dbgBox.setFillColor(Color::Transparent);
	dbgBox.setOutlineThickness(2.f);
	float fpsAcc = 0.f; int fpsFrames = 0; float fpsShown = 0.f; float dtWorst = 0.f;

	// --- Pantalla de nombre (entre "Play" y "Get Ready") ---
	bool isNameEntry = false;
	string playerName(NameLen, ' ');
	playerName.replace(0, 7, "JUGADOR");
	int nameCursor = 0;
	bool nameFresh = true;     // la primera tecla sustituye el nombre anterior
	bool scoreSaved = false;   // para guardar la partida una sola vez

	Text nameTitle("Nombre del jugador", gameFont, 70);
	nameTitle.setFillColor(Color::White);
	nameTitle.setOutlineThickness(4.f);
	nameTitle.setOutlineColor(Color(47, 79, 79));

	Text nameSlots("", gameFont, 90);
	nameSlots.setFillColor(Color(255, 245, 0));
	nameSlots.setOutlineThickness(4.f);
	nameSlots.setOutlineColor(Color::Black);

	Text nameHelp("Escribe, o mueve con la cruceta.  Cualquier boton / Enter para empezar",
		gameFont, 26);
	nameHelp.setFillColor(Color::White);
	nameHelp.setOutlineThickness(2.f);
	nameHelp.setOutlineColor(Color::Black);

	// --- Pantalla de records ---
	int hsFocus = 0;            // 0 Volver, 1 Borrar todos
	bool confirmClear = false;  // paso de confirmacion del borrado

	Text hsTitle("Records", gameFont, 80);
	hsTitle.setFillColor(Color(255, 245, 0));
	hsTitle.setOutlineThickness(4.f);
	hsTitle.setOutlineColor(Color::Black);

	Text hsList("", gameFont, 34);
	hsList.setFillColor(Color::White);
	hsList.setOutlineThickness(2.f);
	hsList.setOutlineColor(Color::Black);

	Text hsBack("Volver", gameFont, 44);
	Text hsClear("Borrar todos", gameFont, 44);
	// Se colocan ya aqui (y no solo al dibujar) para que el test de posicion
	// funcione desde el primer fotograma.
	hsBack.setPosition(240, DesignH - 150.f);
	hsClear.setPosition(760, DesignH - 150.f);
	for (Text* t : { &hsBack, &hsClear })
	{
		t->setFillColor(Color::White);
		t->setOutlineThickness(3.f);
		t->setOutlineColor(Color::Black);
	}

	// La lista se relee de la base solo cuando hace falta, no cada fotograma.
	vector<ScoreEntry> hsRows;
	bool hsDirty = true;

	RectangleShape focusBox;
	focusBox.setFillColor(Color::Transparent);
	// Naranja fuerte: contrasta con el amarillo de "Credits"/"HighScore", con el
	// boton de play (casi blanco) y con el fondo azul-verdoso.
	focusBox.setOutlineColor(Color(255, 90, 0));
	focusBox.setOutlineThickness(5.f);

#pragma endregion

	/*     END-TEXTURE & SPRITES     */

	// El reloj se pone a cero AQUI, no al declararlo: si no, el primer dt
	// incluiria todo el tiempo de carga de texturas y sonidos (segundos).
	frameClock.restart();

	while (window.isOpen())
	{
		// Paso de tiempo real de este fotograma, acotado (ver MaxFrameTime).
		float dt = frameClock.restart().asSeconds();
		if (dt > MaxFrameTime)
			dt = MaxFrameTime;

		if (cfg.debug)
		{
			// dt ya viene acotado, asi que "dt max" tocando MaxFrameTime (50 ms)
			// significa que la maquina no llega a 20 FPS y el juego va a camara
			// lenta: es la senal de que hay que bajar la resolucion.
			fpsAcc += dt;
			++fpsFrames;
			if (dt > dtWorst)
				dtWorst = dt;
			if (fpsAcc >= 0.5f)
			{
				fpsShown = fpsFrames / fpsAcc;
				fpsAcc = 0.f;
				fpsFrames = 0;
			}
		}

		// Un unico impulso por pulsacion. Se detecta por EVENTO, nunca con
		// isKeyPressed: consultar el estado daria un impulso en cada fotograma
		// mientras la tecla siguiera abajo (subida continua, y ademas tanto
		// mas fuerte cuantos mas FPS diera el equipo).
		bool flapPressed = false;

		// Intenciones de menu de este fotograma, todas por flanco.
		bool confirmPressed = false;
		bool navPrev = false, navNext = false;
		// El toque no confirma el elemento con foco (usa posicion), pero en las
		// pantallas que no tienen destinos posicionales si tiene que valer.
		bool touchedThisFrame = false;

		Event event;
		while (window.pollEvent(event))
		{
			if (event.type == Event::Closed)
				window.close();

			// Si alguna vez se hace redimensionable, la View se recalcula sola.
			if (event.type == Event::Resized)
				window.setView(gameView(event.size.width, event.size.height));

			// En pantalla completa no hay boton de cerrar: ESC en el menu
			// principal (sin creditos ni records abiertos) sale del juego.
			if (cfg.fullscreen && event.type == Event::KeyPressed
				&& event.key.code == Keyboard::Escape
				&& !isPlayButton_Pressed && !isCredits_Pressed && !isHighScore_Pressed)
				window.close();

			// Un toque en pantalla equivale a un clic: aletea y confirma. El
			// test de posicion de los menus lo resuelve pointerDown() mas
			// abajo, igual que con el raton.
			bool touched = (event.type == Event::TouchBegan);
			if (touched)
				touchedThisFrame = true;

			// CUALQUIER boton del mando vale para aletear y para confirmar: en
			// un juego de un solo boton no hay ambiguedad, y asi no dependemos
			// de una numeracion que cambia con cada modelo de mando.
			bool padButton = (event.type == Event::JoystickButtonPressed);
			if (padButton)
				padActive = true;

			if (event.type == Event::MouseMoved)
				padActive = false;   // vuelve a mandar el raton

			if (event.type == Event::KeyPressed)
			{
				switch (event.key.code)
				{
				case Keyboard::Up:
				case Keyboard::Left:
					navPrev = true; padActive = true; break;
				case Keyboard::Down:
				case Keyboard::Right:
					navNext = true; padActive = true; break;
				case Keyboard::Enter:
					confirmPressed = true; padActive = true; break;
				default: break;
				}
			}

			if ((event.type == Event::KeyPressed && event.key.code == Keyboard::Space)
				|| (event.type == Event::MouseButtonPressed && event.mouseButton.button == Mouse::Left)
				|| padButton || touched)
			{
				flapPressed = true;
				// Ni el raton ni el dedo confirman el elemento con foco: los dos
				// tienen sus propios tests de posicion mas abajo.
				if (event.type != Event::MouseButtonPressed && !touched)
					confirmPressed = true;
				// La misma pulsacion que arranca la partida da el primer aleteo.
				// Se resuelve aqui, antes del update, porque el bloque de la
				// cabra se ejecuta antes que el del boton "Get Ready".
				// Mientras se escribe el nombre la partida no puede arrancar.
				if (isPlayButton_Pressed && !isNameEntry)
					isGetReady_Pressed = true;
			}

			// Escritura del nombre con teclado.
			if (isNameEntry)
			{
				if (event.type == Event::TextEntered)
				{
					Uint32 u = event.text.unicode;
					if (u >= 32 && u < 127)
					{
						// Al abrir la pantalla el campo trae el nombre anterior.
						// La primera tecla lo sustituye entero, en vez de ir
						// sobrescribiendo letra a letra y dejar restos del viejo.
						if (nameFresh)
						{
							playerName.assign(NameLen, ' ');
							nameCursor = 0;
							nameFresh = false;
						}
						if (nameCursor < NameLen)
						{
							playerName[nameCursor] = (char)toupper((int)u);
							if (nameCursor < NameLen - 1)
								++nameCursor;
						}
					}
				}
				if (event.type == Event::KeyPressed
					&& event.key.code == Keyboard::Backspace)
				{
					nameFresh = false;
					// Borra la casilla ANTERIOR al cursor, como cualquier campo
					// de texto; si ya esta al principio, limpia la primera.
					if (nameCursor > 0)
						--nameCursor;
					playerName[nameCursor] = ' ';
				}
			}
		}

		// Navegacion por ejes: un solo paso por empujon (deteccion de flanco).
		bool axUp = false, axDown = false, axLeft = false, axRight = false;
		{
			Vector2i dir = padNavVector();
			if (dir.y != padDirPrev.y)
			{
				if (dir.y > 0) axDown = true;
				else if (dir.y < 0) axUp = true;
				padDirPrev.y = dir.y;
			}
			if (dir.x != padDirPrev.x)
			{
				if (dir.x > 0) axRight = true;
				else if (dir.x < 0) axLeft = true;
				padDirPrev.x = dir.x;
			}
			if (axUp || axDown || axLeft || axRight)
				padActive = true;
			// Para los menus, cualquiera de los cuatro mueve el foco.
			if (axDown || axRight) navNext = true;
			if (axUp || axLeft) navPrev = true;
		}

		// Flanco del puntero (raton o dedo). Hace falta donde una accion no debe
		// repetirse mientras se mantiene pulsado: sin esto, un toque largo sobre
		// "Borrar todos" pedia confirmacion y la aceptaba al fotograma siguiente,
		// borrando las puntuaciones sin confirmacion real.
		bool pointerNow = pointerDown();
		bool pointerEdge = pointerNow && !pointerPrev;
		pointerPrev = pointerNow;

		// Edicion del nombre con el mando: vertical cambia la letra,
		// horizontal cambia de casilla.
		if (isNameEntry)
		{
			if (axUp || axDown || axLeft || axRight)
				nameFresh = false;   // se esta editando el nombre existente
			if (axUp)    playerName[nameCursor] = cycleNameChar(playerName[nameCursor], +1);
			if (axDown)  playerName[nameCursor] = cycleNameChar(playerName[nameCursor], -1);
			if (axRight) nameCursor = (nameCursor + 1) % NameLen;
			if (axLeft)  nameCursor = (nameCursor + NameLen - 1) % NameLen;

			// Las flechas del teclado mueven el cursor, no cambian la letra:
			// ahi ya se escribe directamente.
			if (navNext && !axDown && !axRight) nameCursor = (nameCursor + 1) % NameLen;
			if (navPrev && !axUp && !axLeft) nameCursor = (nameCursor + NameLen - 1) % NameLen;

			// Ninguna de estas intenciones debe llegar a los menus.
			navNext = navPrev = false;
		}

		/*     UPDATE     */

#pragma region Update

		if (!isPaused && !isGameOver)
		{

#pragma region Bird

			if (isPlayButton_Pressed)
			{
				// Wings Motion
				if (flySpeed < birdColorIndex && !isCollided)
				{
					counter = flySpeed;
					bird.setTexture(&BirdImages.Bird[counter]);
					flySpeed += (isGetReady_Pressed ? WingFastRate : WingSlowRate) * dt;
				}
				else
					get_BirdColorNums(colorChar, birdColorIndex, flySpeed, counter); //To reset the correct numbers for the selected color

				// Jumping, rotationAngle & Gravity
				if (isGetReady_Pressed && !isWon)
				{
					if (!isCollided)
					{
						//---Jumping & rotationAngle---
						// Un solo impulso por pulsacion: mantener no sube mas.
						if (flapPressed)
						{
							fly_sound.play();

							// Jumping
							jumpVelocity.y = -jumpSpeed;

							// Rotation
							rotationAngle = -35;
						}
					}
					//---Gravity---
					if (bird.getPosition().y + bird.getSize().y / 2 <= groundHeight) //   /2 NEW
					{
						// Gravedad asimetrica: sube flotando, cae con peso.
						jumpVelocity.y += (jumpVelocity.y < 0.f ? GravityRise : GravityFall) * dt;
						if (jumpVelocity.y > MaxFallSpeed)
							jumpVelocity.y = MaxFallSpeed;

						rotationAngle += RotationSpeed * dt;
						if (rotationAngle > 90)
						{
							rotationAngle = 90;
						}
					}
					else //---Game Over---
					{
						bird.setPosition(bird.getPosition().x, groundHeight - bird.getSize().y / 2); //    /2 NEW
						die_sound.play();
						isGameOver = true;
						jumpVelocity.y = 0;
					}

					// Motion
					bird.move(jumpVelocity * dt);
					bird.setRotation(rotationAngle);


				}
			}
#pragma endregion

#pragma region Base

			//Base
			if (isPlayButton_Pressed && !isCollided)
			{
				// Scroll infinito por textureRect: el rectangulo no se mueve,
				// se desplaza la ventana de muestreo sobre la textura repetida.
				baseScroll += ScrollSpeed * baseTexRate * dt;
				if (baseScroll >= (float)Base.getSize().x)
					baseScroll -= (float)Base.getSize().x;
				base.setTextureRect(IntRect((int)baseScroll, 0, baseTexW, baseTexH));
			}

#pragma endregion

#pragma region Pipes & Collision

			if (isGetReady_Pressed && !isCollided)
			{
				// Pushing Pipes
				topPipeYpos = get_topPipeYpos(topPipe_minYpos, topPipe_maxYpos); // Setting TOP pipe's position
				bottomPipeYpos = topPipeYpos + distanceY_betweenPipes; // Setting BOTTOM pipe's position

				if (numOfPipesCounter < numOfPipes)
				{
					// El intervalo era de 85 FOTOGRAMAS; ahora son segundos.
					pipeSpawnTimer += dt;

					if (pipeSpawnTimer >= PipeSpawnInterval)
					{
						top_pipe.setPosition(ViewRight + 30, topPipeYpos); // Sets the Top Pipe position
						bottom_pipe.setPosition(ViewRight + 30, bottomPipeYpos); // Sets the Bottom Pipe position

						// La textura se asigna aqui y no al dibujar: 'day' solo
						// cambia al reiniciar, asi que repetirlo cada fotograma
						// para cada lanza era trabajo tirado.
						top_pipe.setTexture(day ? &gPipe[0] : &rPipe[0]);
						bottom_pipe.setTexture(day ? &gPipe[1] : &rPipe[1]);

						topPipe.push_back(RectangleShape(top_pipe)); // Add a pipe to the top_deque
						bottomPipe.push_back(RectangleShape(bottom_pipe)); // Add a pipe to the bottom_deque
						sizeOf_PipesDeque++;
						numOfPipesCounter++;
						pipeSpawnTimer = 0.f;
					}
				}
				// Se descartan las que ya salieron por la izquierda.
				while (firstActivePipe < sizeOf_PipesDeque
					&& topPipe[firstActivePipe].getPosition().x + PipeDrawW < ViewLeft)
					++firstActivePipe;

				// Moving Pipes (solo las que siguen en juego)
				for (int i = firstActivePipe; i < sizeOf_PipesDeque; i++)
				{
					topPipe[i].move(baseVelocity * dt); // Moves the TOP pipe to the left
					bottomPipe[i].move(baseVelocity * dt);	// Moves the BOTTOM pipe to the left
				}


				/* .....................................................*/
#pragma region Collisions & Scoring

				if (sizeOf_PipesDeque > 0 && scoreCounter != numOfPipes)
				{
					//---TOP Pipe Collision---
					if (pipeHits(goatHitbox(bird), topPipe[pipeIndex], true))
					{
						hit_sound.play();
						isCollided = true;
					}
					//---BOTTOM Pipe Collision---
					else if (pipeHits(goatHitbox(bird), bottomPipe[pipeIndex], false))
					{
						hit_sound.play();
						isCollided = true;
					}

					//---Scoring---
					if (bird.getPosition().x >= topPipe[pipeIndex].getPosition().x)
					{
						if (bird.getPosition().x - bird.getSize().x / 2 <= topPipe[pipeIndex].getPosition().x + topPipe[pipeIndex].getSize().x)
						{
							if (!isScored)
							{
								point_sound.play();
								isScored = true;
								scoreCounter++;
								// La partida se guarda una sola vez al acabar,
								// con el nombre del jugador; ya no se escribe
								// una linea por cada punto.
								if (scoreCounter > bestScore)
									bestScore = scoreCounter;
							}
						}
					}
					if (bird.getPosition().x - bird.getSize().x / 2 >= topPipe[pipeIndex].getPosition().x + topPipe[pipeIndex].getSize().x)
					{
						pipeIndex++;
						isScored = false;
					}
				}

				//---Ground Collision---
				if (goatHitbox(bird).intersects(base.getGlobalBounds()))
				{
					hit_sound.play();
					isCollided = true;
				}

				//---Sky Collision---
				if (bird.getPosition().y <= -90)
				{
					hit_sound.play();
					isCollided = true;
				}
				// End Collision

#pragma endregion

			} // end if !isCollided

#pragma endregion

		}

#pragma region Game Pause & Return to Main Menu

		//Return to main menu before starting
		if (isPlayButton_Pressed && !isGetReady_Pressed)
		{
			if (Keyboard::isKeyPressed(Keyboard::Escape))
				isPlayButton_Pressed = false;
		}


		//Game Pause
		if (isGetReady_Pressed && !isCollided /*NEW*/ && !isWon)
		{
			// En partida cualquier boton aletea, asi que no queda ninguno libre
			// para pausar sin romper la regla de "cualquier boton". Se usa un
			// EJE (cruceta o stick), que ahi no hace nada: tambien es generico.
			if (Keyboard::isKeyPressed(Keyboard::Escape) || (!isPaused && navNext))
				isPaused = true;

			// Reanudar: P, o cualquier boton del mando estando ya en pausa.
			if (Keyboard::isKeyPressed(Keyboard::P) || (isPaused && confirmPressed))
				isPaused = false;
		}

#pragma endregion

#pragma region Menu principal (Play / HighScore / Credits)

		// Los tres elementos comparten un modelo de foco para poder recorrerlos
		// con mando o teclado. El raton sigue funcionando igual que antes, por
		// posicion, sin pasar por el foco.
		if (!isPlayButton_Pressed && !isCredits_Pressed && !isHighScore_Pressed)
		{
			const int menuItems = 3;   // 0 Play, 1 HighScore, 2 Credits
			if (navNext)
				menuFocus = (menuFocus + 1) % menuItems;
			if (navPrev)
				menuFocus = (menuFocus + menuItems - 1) % menuItems;

			bool hitPlay = false, hitHigh = false, hitCredits = false;
			if (pointerDown())
			{
				Vector2f mouse = window.mapPixelToCoords(pointerPixel(window));
				hitPlay = playButton.getGlobalBounds().contains(mouse);
				hitHigh = highScore_word.getGlobalBounds().contains(mouse);
				hitCredits = credits_word.getGlobalBounds().contains(mouse);
			}

			if (hitPlay || (confirmPressed && menuFocus == 0))
			{
				isPlayButton_Pressed = true;
				isNameEntry = true;      // primero el nombre, luego "Get Ready"
				nameCursor = 0;
				nameFresh = true;
				swoosh_sound.play();
				confirmPressed = false;
			}
			else if (hitHigh || (confirmPressed && menuFocus == 1))
			{
				isHighScore_Pressed = true;
				hsFocus = 0;
				confirmClear = false;
				hsDirty = true;          // releer la lista de la base
				swoosh_sound.play();
				confirmPressed = false;
			}
			else if (hitCredits || (confirmPressed && menuFocus == 2))
			{
				isCredits_Pressed = true;
				swoosh_sound.play();
				confirmPressed = false;
			}
			// La pulsacion se CONSUME aqui. Si no, los bloques de "volver"
			// que vienen despues veran el mismo confirmPressed en este mismo
			// fotograma y cerraran la pantalla nada mas abrirla.
		}

#pragma endregion

		// El boton "Get Ready" se resuelve ahora en el bucle de eventos, junto al
		// aleteo, para que la misma pulsacion arranque la partida y de el primer
		// impulso.

#pragma region Aceptar el nombre

		if (isNameEntry && (confirmPressed || touchedThisFrame))
		{
			playerName = trimmedName(playerName);
			playerName.resize(NameLen, ' ');   // se mantiene el ancho del editor
			isNameEntry = false;
			swoosh_sound.play();
			confirmPressed = false;   // que no lo reaproveche nadie mas
		}

#pragma endregion

#pragma region Pantalla de records (lista + borrar todos)

		if (isHighScore_Pressed)
		{
			const int hsItems = 2;   // 0 Volver / Cancelar,  1 Borrar / Confirmar
			if (navNext) hsFocus = (hsFocus + 1) % hsItems;
			if (navPrev) hsFocus = (hsFocus + hsItems - 1) % hsItems;

			// Test de posicion para raton y dedo. Faltaba: esta pantalla solo
			// respondia al foco (mando/teclado) y sus dos botones estaban
			// muertos al pulsarlos.
			bool hitBack = false, hitClear = false;
			if (pointerEdge)
			{
				Vector2f p = window.mapPixelToCoords(pointerPixel(window));
				hitBack = hsBack.getGlobalBounds().contains(p);
				hitClear = hsClear.getGlobalBounds().contains(p);
				if (hitBack) hsFocus = 0;
				if (hitClear) hsFocus = 1;
			}

			if (Keyboard::isKeyPressed(Keyboard::Escape))
			{
				swoosh_sound.play();
				isHighScore_Pressed = false;
				confirmClear = false;
				hsFocus = 0;
			}
			else if (confirmPressed || hitBack || hitClear)
			{
				if (!confirmClear)
				{
					if (hsFocus == 0)          // Volver
					{
						isHighScore_Pressed = false;
						hsFocus = 0;
					}
					else                        // Borrar todos -> pedir confirmacion
					{
						confirmClear = true;
						hsFocus = 0;            // por defecto, "Cancelar"
					}
				}
				else
				{
					if (hsFocus == 1)           // confirmado: se borra
					{
						scores.clearAll();
						bestScore = 0;
						hsDirty = true;
					}
					confirmClear = false;
					hsFocus = 0;
				}
				swoosh_sound.play();
				confirmPressed = false;
			}
		}

#pragma endregion

#pragma region Volver desde Credits

		if (isCredits_Pressed && (confirmPressed || touchedThisFrame
			|| Keyboard::isKeyPressed(Keyboard::Escape)))
		{
			swoosh_sound.play();
			isCredits_Pressed = false;
		}

#pragma endregion

#pragma region Game Over

		// Motion
		// Con dt variable un "if (pos <= objetivo) move(...)" se pasa de largo y
		// se queda vibrando, asi que las animaciones se acotan al objetivo.
		if (isGameOver)
		{
			if (gameOver.getPosition().y < 150)
				gameOver.setPosition(gameOver.getPosition().x,
					min(150.f, gameOver.getPosition().y + UiSpeed * dt));
		}

#pragma endregion

		// La partida se guarda UNA sola vez, al terminar, con el nombre elegido.
		if ((isGameOver || isWon) && !scoreSaved)
		{
			scoreSaved = true;
			string who = trimmedName(playerName);
			if (!scores.addScore(who, scoreCounter))
				cerr << "aviso: no se pudo guardar la puntuacion ("
				     << scores.lastError() << ")" << endl;
			if (scoreCounter > bestScore)
				bestScore = scoreCounter;
			hsDirty = true;
		}


#pragma region Score Board

		if (isGameOver || isWon)
		{
			// Moves the Score Board
			if (scoreBoard.getPosition().y > scoreBoardTargetY)
				scoreBoard.setPosition(scoreBoard.getPosition().x,
					max(scoreBoardTargetY, scoreBoard.getPosition().y - UiSpeed * dt));

			// Moves the Current Score
			if (currentScoreNum.getPosition().y > currentScoreTargetY)
				currentScoreNum.setPosition(currentScoreNum.getPosition().x,
					max(currentScoreTargetY, currentScoreNum.getPosition().y - UiSpeed * dt));

			// Moves the High Score
			if (highScoreNum.getPosition().y > highScoreTargetY)
				highScoreNum.setPosition(highScoreNum.getPosition().x,
					max(highScoreTargetY, highScoreNum.getPosition().y - UiSpeed * dt));
		}
#pragma endregion

#pragma region Replay & Main Menu Buttons

		if (isGameOver || isWon)
		{

#pragma region Motion

			// Motion of both buttons when (isGameOver)
			if (scoreBoard.getPosition().y <= 700)
			{
				const float replayTargetX = DesignW / 2.f - 150.f;
				const float menuTargetX = DesignW / 2.f + 50.f;

				if (replayButton.getPosition().x < replayTargetX)
					replayButton.setPosition(
						min(replayTargetX, replayButton.getPosition().x + ButtonSpeed * dt),
						replayButton.getPosition().y);

				if (mainMenuButton.getPosition().x > menuTargetX)
					mainMenuButton.setPosition(
						max(menuTargetX, mainMenuButton.getPosition().x - ButtonSpeed * dt),
						mainMenuButton.getPosition().y);
			}

#pragma endregion

			// Foco entre los dos botones finales (0 Replay, 1 Main Menu).
			if (navNext || navPrev)
				overFocus = 1 - overFocus;

#pragma region Replay Button

			{
				bool hitReplay = false;
				if (pointerDown())
				{
					Vector2f mouse = window.mapPixelToCoords(pointerPixel(window));
					hitReplay = replayButton.getGlobalBounds().contains(mouse);
				}

				// El boton solo responde una vez ha terminado de entrar.
				if ((hitReplay || (confirmPressed && overFocus == 0))
					&& replayButton.getPosition().x >= DesignW / 2 - 150)
				{
					swoosh_sound.play();
					// Reset Day
					day = rand() % 2;

					// Reset Pipes
					sizeOf_PipesDeque = 0;
					pipeIndex = 0;
					firstActivePipe = 0;
					pipeSpawnTimer = 0;
					numOfPipesCounter = 0;
					topPipe.clear();
					bottomPipe.clear();

					// Reset Bird
					bird.setPosition(DesignW / 2, DesignH / 2);
					bird.setRotation(0);

					get_BirdColor(colorChar);
					get_BirdColorNums(colorChar, birdColorIndex, flySpeed, counter);

					// Reset Game Over & Score Board
					gameOver.setPosition(DesignW / 2, -200);
					scoreBoard.setPosition(DesignW / 2, DesignH + 200);

					// Reset Buttons except "Play Button"
					replayButton.setPosition(ViewLeft - 200, 510);
					mainMenuButton.setPosition(ViewRight + 10, 510);
					isGetReady_Pressed = false;

					// Reset Collision & Won booleans
					isGameOver = false;
					isWon = false;
					isCollided = false;

					// Reset Score
					scoreCounter = 0;
					isScored = false;

					// El foco de la pantalla final vuelve a "Replay"
					overFocus = 0;
					scoreSaved = false;   // la siguiente partida se guardara

					// Reset High & Current Scores' Positions
					highScoreNum.setPosition(770, scoreBoard.getPosition().y + 40);
					currentScoreNum.setPosition(770, scoreBoard.getPosition().y - 37);

					// Reset Congrats Pos
					congrats.setPosition((DesignW) - 975, -200);
				}
			}

#pragma endregion

#pragma region Main Menu Button


			// Main Menu Button
			{
				bool hitMenu = false;
				if (pointerDown())
				{
					Vector2f mouse = window.mapPixelToCoords(pointerPixel(window));
					hitMenu = mainMenuButton.getGlobalBounds().contains(mouse);
				}

				if ((hitMenu || (confirmPressed && overFocus == 1))
					&& mainMenuButton.getPosition().x <= DesignW / 2 + 50)
				{
					swoosh_sound.play();

					// Reset Day
					day = rand() % 2;

					// Reset Pipes
					sizeOf_PipesDeque = 0;
					pipeIndex = 0;
					firstActivePipe = 0;
					pipeSpawnTimer = 0;
					numOfPipesCounter = 0;
					topPipe.clear();
					bottomPipe.clear();

					// Reset Bird
					bird.setPosition(DesignW / 2, DesignH / 2);
					bird.setRotation(0);

					get_BirdColor(colorChar);
					get_BirdColorNums(colorChar, birdColorIndex, flySpeed, counter);

					// Reset Game Over & Score Board
					gameOver.setPosition(DesignW / 2, -200);
					scoreBoard.setPosition(DesignW / 2, DesignH + 200);

					// Reset Buttons
					isPlayButton_Pressed = false;
					isGetReady_Pressed = false;
					replayButton.setPosition(ViewLeft - 200, 510);
					mainMenuButton.setPosition(ViewRight + 10, 510);

					// Reset Collision & Won booleans
					isGameOver = false;
					isWon = false;
					isCollided = false;

					// Reset Score
					scoreCounter = 0;
					isScored = false;

					// El foco de la pantalla final vuelve a "Replay"
					overFocus = 0;
					scoreSaved = false;   // la siguiente partida se guardara

					// Reset HighScore Pos & CurrentScore Pos
					highScoreNum.setPosition(770, scoreBoard.getPosition().y + 40);
					currentScoreNum.setPosition(770, scoreBoard.getPosition().y - 37);

					// Reset Congrats Pos
					congrats.setPosition((DesignW) - 975, -200);
				}
			}
#pragma endregion

		}

#pragma endregion

#pragma region Win

		if (scoreCounter == numOfPipes)
		{
			if (topPipe.back().getPosition().x < -top_pipe.getSize().x)
			{
				isWon = true;
				if (congrats.getPosition().y < 100)
					congrats.setPosition(congrats.getPosition().x,
						min(100.f, congrats.getPosition().y + UiSpeed * dt));
				bird.setPosition(DesignW / 2, DesignH / 2);
				bird.setRotation(0);
			}
		}

#pragma endregion


#pragma endregion

		/*     END-UPDATE     */

		window.clear();
		/*     DRAWING     */

		// Los fondos cubren toda la zona visible (ver coverView). Se recalcula
		// cada fotograma porque es barato y asi sobrevive a un cambio de vista.
		for (int i = 0; i < 3; ++i)
			coverView(bgi[i]);
		coverView(credit_names_BGSprite);

#pragma region Draiwing

		//Background Image
		if (day)
			window.draw(bgi[0]);
		else
			window.draw(bgi[1]);

		//Pipes
		for (int i = firstActivePipe; i < sizeOf_PipesDeque; i++)
		{
			// Las de la derecha aun no han entrado: en cuanto se llega a una
			// fuera de pantalla, las siguientes tambien lo estan.
			if (topPipe[i].getPosition().x > ViewRight)
				break;
			window.draw(topPipe[i]);
			window.draw(bottomPipe[i]);
		}

		//Base
		window.draw(base);

		//Bird
		window.draw(bird);

		//Game Paused
		if (isPaused && isGetReady_Pressed && !isGameOver)
		{
			window.draw(Pause);
			window.draw(resume);
		}


		/*.................................................................................................................*/

		//Current score
		if (isGetReady_Pressed && !isGameOver && !isWon)
		{
			ssScore.str(""); // Updates the Score Value
			ssScore << scoreCounter; // Sets the new Value 
			labelScore.setString(ssScore.str()); // Sets the value to a string 
			window.draw(labelScore);
		}


		/*.................................................................................................................*/

		//Game Over || Won
		if (isGameOver || isWon)
		{
			//Game Over
			window.draw(gameOver);

			//Score Board
			if (scoreCounter < 10) // NO Medal
				scoreBoard.setTexture(&score_board[0]);

			else if (scoreCounter >= 10 && scoreCounter < 20) // Bronze Medal
				scoreBoard.setTexture(&score_board[1]);

			else if (scoreCounter >= 20 && scoreCounter < 30) // Silver Medal
				scoreBoard.setTexture(&score_board[2]);

			else if (scoreCounter >= 30 && scoreCounter < 40) // Gold Medal
				scoreBoard.setTexture(&score_board[3]);

			else // Platinum Medal
				scoreBoard.setTexture(&score_board[4]);

			window.draw(scoreBoard);

			//Replay & Main Menu Buttons
			window.draw(replayButton);
			window.draw(mainMenuButton);

			// Foco, una vez los botones han terminado de entrar.
			if (padActive && replayButton.getPosition().x >= DesignW / 2 - 150)
			{
				placeFocus(focusBox, overFocus == 0 ? replayButton.getGlobalBounds()
				                                    : mainMenuButton.getGlobalBounds());
				window.draw(focusBox);
			}

			//High Score number
			highScoreNum.setCharacterSize(25);
			highScoreNum.setString(to_string(bestScore));
			highScoreNum.setOutlineThickness(2.0);
			highScoreNum.setOutlineColor(Color::Black);
			centerText(highScoreNum);

			currentScoreNum.setString(to_string(scoreCounter));
			currentScoreNum.setOutlineThickness(2.0);
			currentScoreNum.setOutlineColor(Color::Black);
			centerText(currentScoreNum);
			window.draw(highScoreNum);
			window.draw(currentScoreNum);
		}

		//Blur Background Image
		if (!isPlayButton_Pressed)
			window.draw(bgi[2]);

		//Main Menu Texts
		if (!isPlayButton_Pressed)
		{
			window.draw(credits_word);
			window.draw(highScore_word);
			window.draw(flappyBird_word);
		}

		//Play Button
		if (!isPlayButton_Pressed)
		{
			window.draw(playButton);
		}

		// Resaltado del elemento con el foco. Solo se dibuja si se esta
		// navegando con mando o teclado; con el raton estorbaria.
		if (padActive && !isPlayButton_Pressed && !isCredits_Pressed && !isHighScore_Pressed)
		{
			if (menuFocus == 0)      placeFocus(focusBox, playButton.getGlobalBounds());
			else if (menuFocus == 1) placeFocus(focusBox, highScore_word.getGlobalBounds());
			else                     placeFocus(focusBox, credits_word.getGlobalBounds());
			window.draw(focusBox);
		}

		//Credits Button
		if (isCredits_Pressed)
		{
			window.draw(credit_names_BGSprite);
			window.draw(credit_names);
			window.draw(madyBy);
			window.draw(return_to_mainMenu);
		}


		/*.......................................................................................................................*/
		//HighScore Button -> tabla de records
		if (isHighScore_Pressed)
		{
			window.draw(credit_names_BGSprite);

			if (hsDirty)
			{
				hsRows = scores.topScores(10);
				hsDirty = false;
			}

			hsTitle.setPosition(DesignW / 2.f - hsTitle.getGlobalBounds().width / 2.f, 60);
			window.draw(hsTitle);

			const float rowsX = DesignW / 2.f - 230.f;
			const float rowsY = 190.f;
			const float scoreRightX = DesignW / 2.f + 230.f;
			const float lineH = gameFont.getLineSpacing(hsList.getCharacterSize());

			if (hsRows.empty())
			{
				hsList.setString("Todavia no hay ninguna partida guardada.");
				hsList.setPosition(DesignW / 2.f - hsList.getGlobalBounds().width / 2.f, rowsY);
				window.draw(hsList);
			}
			else
			{
				// Puesto y nombre en una columna a la izquierda...
				ostringstream names;
				for (size_t i = 0; i < hsRows.size(); ++i)
					names << (i + 1 < 10 ? " " : "") << (i + 1) << ".  "
					      << hsRows[i].name << "\n";
				hsList.setString(names.str());
				hsList.setPosition(rowsX, rowsY);
				window.draw(hsList);

				// ...y la puntuacion linea a linea, alineada a la DERECHA. En
				// esta fuente el espacio no mide igual que un digito, asi que
				// rellenar con espacios no alinearia.
				Text num("", gameFont, hsList.getCharacterSize());
				num.setFillColor(Color::White);
				num.setOutlineThickness(2.f);
				num.setOutlineColor(Color::Black);
				for (size_t i = 0; i < hsRows.size(); ++i)
				{
					num.setString(to_string(hsRows[i].score));
					num.setPosition(scoreRightX - num.getGlobalBounds().width,
						rowsY + lineH * (float)i);
					window.draw(num);
				}
			}

			// Dos opciones abajo; en el paso de confirmacion cambian de texto.
			hsBack.setString(confirmClear ? "Cancelar" : "Volver");
			hsClear.setString(confirmClear ? "Si, borrar todo" : "Borrar todos");
			hsBack.setPosition(240, DesignH - 150.f);
			hsClear.setPosition(760, DesignH - 150.f);
			hsClear.setFillColor(confirmClear ? Color(255, 90, 90) : Color::White);
			window.draw(hsBack);
			window.draw(hsClear);

			if (confirmClear)
			{
				Text warn("Se borraran las " + to_string(scores.scoreCount())
					+ " puntuaciones. No se puede deshacer.", gameFont, 30);
				warn.setFillColor(Color(255, 160, 160));
				warn.setOutlineThickness(2.f);
				warn.setOutlineColor(Color::Black);
				warn.setPosition(DesignW / 2.f - warn.getGlobalBounds().width / 2.f,
					DesignH - 215.f);
				window.draw(warn);
			}

			placeFocus(focusBox, hsFocus == 0 ? hsBack.getGlobalBounds()
			                                  : hsClear.getGlobalBounds());
			window.draw(focusBox);
		}

		//Nombre del jugador
		if (isNameEntry)
		{
			window.draw(credit_names_BGSprite);

			nameTitle.setPosition(DesignW / 2.f - nameTitle.getGlobalBounds().width / 2.f, 180);
			window.draw(nameTitle);

			// Las casillas se separan para que se vea cual se esta editando.
			string shown;
			for (int i = 0; i < NameLen; ++i)
			{
				shown += playerName[i];
				if (i + 1 < NameLen)
					shown += ' ';
			}
			nameSlots.setString(shown);
			float slotsX = DesignW / 2.f - nameSlots.getGlobalBounds().width / 2.f;
			nameSlots.setPosition(slotsX, 360);
			window.draw(nameSlots);

			// Subrayado de la casilla activa, medido sobre el propio texto para
			// que cuadre sea cual sea la anchura de cada caracter.
			float x0 = nameSlots.findCharacterPos(nameCursor * 2).x;
			float x1 = nameSlots.findCharacterPos(nameCursor * 2 + 1).x;
			RectangleShape caret(Vector2f(max(18.f, x1 - x0 - 6.f), 6.f));
			caret.setPosition(x0, 360 + 105.f);
			caret.setFillColor(Color(255, 90, 0));
			window.draw(caret);

			nameHelp.setPosition(DesignW / 2.f - nameHelp.getGlobalBounds().width / 2.f, 560);
			window.draw(nameHelp);
		}

		//Get Ready
		if (isPlayButton_Pressed && !isGetReady_Pressed && !isNameEntry)
		{
			window.draw(getReady);
		}

		// Congrats 
		if (isWon)
			window.draw(congrats);

#pragma endregion

		/*     END-DRAWING     */

		if (cfg.debug)
		{
			// Cajas de colision REALES, las mismas que usa el juego.
			auto marco = [&](const FloatRect& r, Color c) {
				dbgBox.setOutlineColor(c);
				dbgBox.setPosition(r.left, r.top);
				dbgBox.setSize(Vector2f(r.width, r.height));
				window.draw(dbgBox);
			};
			if (isGetReady_Pressed)
			{
				marco(goatHitbox(bird), Color(0, 255, 120));
				for (int i = firstActivePipe; i < sizeOf_PipesDeque; i++)
				{
					if (topPipe[i].getPosition().x > ViewRight) break;
					Color c = (i == pipeIndex) ? Color(255, 60, 60) : Color(255, 200, 0);
					for (int b = 0; b < PipeBands; ++b)
					{
						marco(pipeBandRect(topPipe[i], true, b), c);
						marco(pipeBandRect(bottomPipe[i], false, b), c);
					}
				}
			}

			ostringstream d;
			d << "FPS " << (int)(fpsShown + 0.5f)
			  << "   dt max " << (int)(dtWorst * 1000.f) << " ms"
			  << "   lanzas dibujadas " << (sizeOf_PipesDeque - firstActivePipe)
			  << " de " << sizeOf_PipesDeque
			  << "\n"
			  << "ventana " << window.getSize().x << "x" << window.getSize().y
			  << "   vista " << (int)viewW() << "x" << (int)viewH();
			dbgText.setString(d.str());
			dbgText.setPosition(ViewLeft + 12.f, 12.f);
			window.draw(dbgText);
		}

		window.display();

	}

	return 0; // End of application
}

void get_BirdColor(char& colorChar)
{
	int random = rand() % 3; //DevSkim: ignore DS148264
	if (random == 0)
		colorChar = 'R';
	if (random == 1)
		colorChar = 'B';
	if (random == 2)
		colorChar = 'Y';
}
void get_BirdColorNums(char colorChar, int& birdColorIndex, float& flySpeed, int& counter)
{
	if (colorChar == 'R')
	{
		flySpeed = 0;
		counter = 0;
		birdColorIndex = 3;
	}
	else if (colorChar == 'B')
	{
		flySpeed = 3;
		counter = 3;
		birdColorIndex = 6;
	}
	else if (colorChar == 'Y')
	{
		flySpeed = 6;
		counter = 6;
		birdColorIndex = 9;
	}
}
int get_topPipeYpos(int topPipe_minYpos, int topPipe_maxYpos)
{
	// Antes esto era un bucle de (max-min) vueltas que descartaba todos los
	// valores menos el ultimo, y devolvia una variable SIN INICIALIZAR si el
	// bucle no llegaba a ejecutarse (comportamiento indefinido). Una sola
	// extraccion es equivalente y segura.
	if (topPipe_maxYpos <= topPipe_minYpos)
		return topPipe_minYpos;
	return (rand() % (topPipe_maxYpos - topPipe_minYpos)) + topPipe_minYpos;
}

