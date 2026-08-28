@echo off
REM Compila FlappyBird (release) con MSVC + SFML 2.6.2.
REM El ejecutable y sus DLL van a FlappyBird\FlappyBird-Win\, que es donde
REM estan assets\, audios\ y fonts\.

setlocal enabledelayedexpansion
set "ROOT=%~dp0"
set "SFML=%ROOT%SFML-2.6.2"
set "OUT=%ROOT%FlappyBird\FlappyBird-Win"
set "SRCDIR=%ROOT%FlappyBird\src"
set "SRC=%SRCDIR%\main.cpp"

REM Localiza vcvars64.bat. Se prueban las rutas de instalacion habituales; el
REM parentesis de "Program Files (x86)" rompe el parseo de un for /f con
REM vswhere, asi que se busca directamente.
set "VCVARS="
for %%e in (Enterprise Professional Community BuildTools) do (
    for %%v in (2022 2019) do (
        for %%p in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
            if not defined VCVARS if exist "%%~p\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" (
                set "VCVARS=%%~p\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat"
            )
        )
    )
)
if not defined VCVARS (
    echo No se encontro Visual Studio con herramientas de C++ x64.
    exit /b 1
)
call "!VCVARS!" >nul
if errorlevel 1 exit /b 1

if not exist "%OUT%\build" mkdir "%OUT%\build"

set "SQLITE=%ROOT%third_party\sqlite"
if not exist "%SQLITE%\sqlite3.c" (
    echo Falta %SQLITE%\sqlite3.c ^(amalgamacion de SQLite^).
    exit /b 1
)

pushd "%OUT%"

REM sqlite3.c se compila aparte y se cachea: es grande y no cambia nunca.
if not exist "build\sqlite3.obj" (
    echo compilando SQLite ^(solo la primera vez, tarda^)...
    cl /nologo /c /O2 /MD /DSQLITE_OMIT_LOAD_EXTENSION /DSQLITE_THREADSAFE=0 ^
       "%SQLITE%\sqlite3.c" /Fo:"build\sqlite3.obj"
    if errorlevel 1 ( popd & exit /b 1 )
)

cl /nologo /EHsc /O2 /std:c++17 /MD ^
   /I "%SFML%\include" /I "%SQLITE%" /I "%SRCDIR%" ^
   "%SRC%" "%SRCDIR%\scoredb.cpp" "build\sqlite3.obj" ^
   /Fe:"FlappyBird.exe" /Fo:"build\\" ^
   /link /LIBPATH:"%SFML%\lib" ^
   sfml-graphics.lib sfml-window.lib sfml-system.lib sfml-audio.lib
set ERR=%ERRORLEVEL%
popd
if not "%ERR%"=="0" (
    echo.
    echo FALLO LA COMPILACION
    exit /b %ERR%
)

REM DLL de release que necesita el exe en tiempo de ejecucion
for %%d in (graphics window system audio) do copy /y "%SFML%\bin\sfml-%%d-2.dll" "%OUT%\" >nul
copy /y "%SFML%\bin\openal32.dll" "%OUT%\" >nul

echo.
echo OK -^> %OUT%\FlappyBird.exe
endlocal
