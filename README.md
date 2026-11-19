# ProStreet Recomp

**Need for Speed ProStreet running natively on Mac with Apple Silicon (ARM64: M1, M2, M3, M4).**

## English

A static recompilation of *Need for Speed ProStreet* (Xbox 360) for macOS on Apple Silicon. The
game's PowerPC code is translated to C++ with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)
and runs natively on ARM64, with no emulator: a custom runtime reimplements the Xbox 360 kernel,
the Xenos GPU (shaders translated to Metal), XMA audio (via FFmpeg), controllers and the macOS window.

- **Playable**: menus, career mode and races, with graphics and sound, at ~60 fps on an M1 Pro.
- **Graphics**: shadows, reflections, character animation, smoke and sparks, **4x MSAA**
  anti-aliasing and **1.5x resolution scaling** (1920×1080), switchable from the *Gráficos* menu.
- **Input**: Xbox / PlayStation controllers supported by macOS, and keyboard.
- **Saves** are stored in `data/content/`.

> **This repository contains no game files.** No ISO, no game data and no C++ generated from it.
> You need your own original copy of the game (Xbox 360, USA). *Need for Speed* and *ProStreet* are
> trademarks of Electronic Arts; this project is not affiliated with EA or Microsoft.

The build steps below (in Spanish) are mostly shell commands: install the Homebrew tools, build
XenonRecomp, extract your ISO, recompile and run `runtime/build.sh`. The code is MIT-licensed.

## Español

Recompilación estática de *Need for Speed ProStreet* (Xbox 360, USA, Title ID `45410822`) para
macOS en Apple Silicon. El código PowerPC del juego se traduce a C++ con
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) y se ejecuta de forma nativa sobre un
runtime propio: kernel de la consola, GPU (traducida a Metal), audio (XMA con FFmpeg), mando y
ventana de macOS.

> **Este repositorio no incluye nada del juego.** Ni la ISO, ni sus archivos, ni el código C++
> generado a partir de ellos. Hace falta tu propia copia original del juego. *Need for Speed* y
> *ProStreet* son marcas de Electronic Arts; este proyecto no está afiliado a EA ni a Microsoft.

## Estado

El juego se puede jugar (menús, modo carrera, carreras) con imagen y sonido:

- **Gráficos**: traductor de shaders de Xenos a MSL y renderer en Metal. Sombras, reflejos (mapas
  cúbicos), animación de personajes, transparencias, mipmaps, texturas 3D, *predicated tiling*,
  sombras de contacto, humo y chispas. **Antialiasing MSAA 4x** y **escala de resolución 1x / 1,5x**
  (1280×720 / 1920×1080), las dos cambiables en caliente desde el menú *Gráficos*.
- **Rendimiento**: ~60 fps en carrera en un M1 Pro. Los shaders se compilan en segundo plano (sin
  tirones) y las texturas solo se reconvierten cuando el juego escribe en su memoria.
- **Audio**: decodificador XMA (FFmpeg) y salida por CoreAudio (la mezcla 5.1 del juego, a estéreo).
- **Entrada**: mandos compatibles con macOS (Xbox, PlayStation...) y teclado.
- **Partidas guardadas** en `data/content/`.

Problemas conocidos:

- Sin juego en línea (los servidores oficiales ya no existen; el juego ve que no hay conexión).
- Al entrar en la carga de una carrera puede oírse un golpe breve en el audio.
- Las texturas de los vídeos se convierten en la CPU (funciona, pero cuesta algo de rendimiento).

## Requisitos

- Mac con Apple Silicon y macOS 13 o posterior.
- Tu copia del juego en ISO (Xbox 360, versión USA).
- Herramientas de Homebrew:

```sh
brew install cmake ninja llvm ccache ffmpeg
brew install ghidra   # opcional: solo para analizar el binario
```

## Compilar

### 1. XenonRecomp

```sh
# XenonRecomp (commit ddd128b) con los parches de este proyecto
git clone --recursive https://github.com/hedge-dev/XenonRecomp.git tools/XenonRecomp
git -C tools/XenonRecomp checkout ddd128b
git -C tools/XenonRecomp apply ../../patches/xenonrecomp-prostreet.patch

# "-include cstdlib": fmt no compila sin él con el SDK actual de macOS
LLVM=/opt/homebrew/opt/llvm
cmake -S tools/XenonRecomp -B tools/XenonRecomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=$LLVM/bin/clang -DCMAKE_CXX_COMPILER=$LLVM/bin/clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  "-DCMAKE_CXX_FLAGS=-include cstdlib"
cmake --build tools/XenonRecomp/build
```

### 2. Extraer y recompilar el juego

```sh
# Extraer el juego de tu ISO (queda en game/)
python3 -I tools/xiso_extract.py extract "<tu ISO>.iso" game

# Volcar la imagen descifrada del XEX (analysis/default_image.bin), con las bibliotecas de XenonRecomp
LLVM=/opt/homebrew/opt/llvm XR=tools/XenonRecomp
$LLVM/bin/clang++ -std=c++20 -O2 -include cstdlib -I$XR/XenonUtils -I$XR/thirdparty/simde -I$XR/thirdparty/disasm \
  tools/xex_dump/xex_dump.cpp $XR/build/XenonUtils/libXenonUtils.a $XR/build/thirdparty/disasm/libdisasm.a \
  -o tools/xex_dump/xex_dump
mkdir -p analysis
tools/xex_dump/xex_dump game/default.xex analysis/default_image.bin

# Recompilar a C++ (salida en ppc/)
python3 -I tools/find_missing_switches.py analysis/default_image.bin config/ProStreet_switch_tables.toml
mkdir -p ppc
tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/ProStreet.toml tools/XenonRecomp/XenonUtils/ppc_context.h
```

### 3. Runtime

```sh
runtime/build.sh    # genera los imports y compila (unos minutos la primera vez)
```

## Jugar

```sh
runtime/build/prostreet   # desde la raíz del proyecto
```

O crea una app para abrirlo desde Finder o el Dock (debe quedarse en la carpeta del proyecto;
la imagen del icono es opcional):

```sh
tools/make_app.sh [icono.png]   # crea ProStreet.app; el registro queda en data/ultima_partida.log
```

### Controles

Con mando, los de la consola. Con teclado:

| Tecla | Botón | Tecla | Botón |
|---|---|---|---|
| W / S | Acelerar (RT) / frenar (LT) | A / D | Volante |
| Flechas | Cruceta | Intro / Espacio | A |
| Esc / Borrar | B | X / Y | X / Y |
| Q / E | LB / RB | P | START |
| Tab | BACK | I J K L | Cámara (stick derecho) |

### Menú Gráficos

| Opción | Atajo |
|---|---|
| Resolución 1x (1280×720) | ⌘1 |
| Resolución 1,5x (1920×1080) | ⌘2 |
| Antialiasing (MSAA 4x) | ⌥⌘A |
| Pantalla completa | ⌃⌘F |

Las opciones se recuerdan entre partidas.

## Variables de entorno

Para jugar no hace falta ninguna. Útiles para pruebas y depuración:

| Variable | Efecto |
|---|---|
| `PROSTREET_SCALE=1.5` | Escala de resolución al empezar (manda sobre el menú) |
| `PROSTREET_NO_MSAA=1` | Sin antialiasing |
| `PROSTREET_LANGUAGE=n` | Idioma de la consola (1 inglés, 3 alemán, 4 francés, 5 español —por defecto—, 6 italiano...) |
| `PROSTREET_NO_WINDOW=1` | Sin ventana (pruebas automáticas) |
| `PROSTREET_NO_AUDIO=1` | Sin salida de sonido |
| `PROSTREET_PRESS="A@30,RT@40-60"` | Pulsa botones en esos segundos (`RT@40-60`: mantener) |
| `PROSTREET_DUMP=carpeta` | Guarda un PNG cada 60 fotogramas (`PROSTREET_DUMP_EVERY=n`) |
| `PROSTREET_AUDIO_DUMP=mezcla.wav` | Graba el sonido en un WAV |
| `PROSTREET_SHADERS=carpeta` | Guarda cada shader distinto (`vs_*.bin` / `ps_*.bin`) |
| `PROSTREET_SYNC_SHADERS=1` | Compila los shaders en el acto (más tirones, pero determinista) |
| `PROSTREET_TRACE=1` | Registro detallado (kernel, texturas, pipelines) |
| `PROSTREET_GPU_TRACE=1` | Cada paquete PM4 que procesa la GPU |

## Cómo está hecho

| Carpeta | Contenido |
|---|---|
| `config/` | Configuración de XenonRecomp: funciones, tablas de saltos y límites corregidos |
| `patches/` | Instrucciones añadidas a XenonRecomp (VMX128, empaquetado D3D...) |
| `runtime/kernel/` | `xboxkrnl.exe` y `xam.xex`: memoria, hilos, sincronización, archivos, XMA, guardado |
| `runtime/gpu/` | Procesador de comandos PM4, traductor de shaders y renderer en Metal |
| `runtime/platform/` | Ventana, mando y teclado, salida de sonido |
| `tools/` | Extracción de la ISO, análisis del XEX, desensambladores de PowerPC y de shaders, Ghidra |
| `tests/` | Pruebas de las instrucciones VMX añadidas y del traductor de shaders |

| Herramienta | Uso |
|---|---|
| `tools/xiso_extract.py` | Lista o extrae el sistema de archivos XDVDFS de la ISO |
| `tools/xex_info.py` | Muestra la cabecera del XEX (cifrado, XDK, imports) |
| `tools/xex_dump/` | Vuelca la imagen descifrada y busca las funciones save/rest de registros |
| `tools/find_missing_switches.py` | Añade al TOML las tablas de saltos que XenonAnalyse no detecta |
| `tools/fix_switch_bounds.py` | Corrige los límites de funciones con tablas de saltos |
| `tools/check_overlaps.py` | Lista las funciones generadas que se solapan con la siguiente |
| `tools/gen_imports.py` | Genera los stubs de los imports y su tabla (`runtime/generated/`) |
| `tools/xenos_disasm.py` | Desensambla los shaders capturados con `PROSTREET_SHADERS` |
| `tools/disasm/` | Desensambla un rango de PowerPC con contexto |
| `tools/ghidra/` | Crea el proyecto de Ghidra (ver `docs/GUIA_GHIDRA.md`) |
| `tools/make_app.sh` | Crea `ProStreet.app` |

## Créditos

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev): el recompilador estático.
- [Xenia](https://github.com/xenia-project/xenia): la documentación de su código (formatos de la
  GPU Xenos, mosaico de texturas, contextos XMA, comportamiento del kernel) ha sido la referencia
  para buena parte del runtime. Licencia BSD de 3 cláusulas.
- [FFmpeg](https://ffmpeg.org): decodificación de XMA (enlazado dinámicamente desde Homebrew, LGPL).

## Licencia

El código de este repositorio se publica bajo la [licencia MIT](LICENSE). No cubre el juego ni
nada derivado de él, que siguen siendo de sus propietarios.
