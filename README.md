# ProStreet Recomp

Recompilación estática de *Need for Speed ProStreet* (Xbox 360, USA, Title ID `45410822`)
para macOS en Apple Silicon, con [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

Este repositorio contiene solo herramientas y configuración. **No incluye ni debe incluir**
archivos del juego ni el código C++ generado a partir de él: cada persona usa su propia copia.

## Preparar el entorno

```sh
brew install cmake ninja llvm ccache ghidra

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

## Recompilar

```sh
# 1. Extraer el juego de tu ISO
python3 -I tools/xiso_extract.py extract "<tu ISO>.iso" game

# 2. Recompilar a C++ (salida en ppc/)
mkdir -p ppc
tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/ProStreet.toml tools/XenonRecomp/XenonUtils/ppc_context.h

# 3. Probar las instrucciones VMX añadidas
tests/run_vmx_tests.sh

# 4. Comprobar que los solapamientos entre funciones son solo los esperados
python3 -I tools/check_overlaps.py
```

## Ejecutar (runtime)

```sh
runtime/build.sh                       # genera los imports y compila (≈1,5 min la primera vez)
runtime/build/prostreet                # desde la raíz del proyecto
PROSTREET_TRACE=1 runtime/build/prostreet   # muestra también las llamadas implementadas
```

El runtime (`runtime/`) reserva los 4 GB del invitado, carga `analysis/default_image.bin`,
rellena la tabla de funciones y los imports, y llama al punto de entrada. Cada import que aún no
está implementado aparece como `[falta] Nombre r3=... lr=...` y devuelve 0.

## Herramientas

| Archivo | Uso |
|---|---|
| `tools/xiso_extract.py` | Lista o extrae el sistema de archivos XDVDFS de la ISO |
| `tools/xex_info.py` | Muestra la cabecera del XEX (cifrado, XDK, imports) |
| `tools/xex_dump/` | Vuelca la imagen descifrada y busca las funciones save/rest de registros |
| `tools/fix_switch_bounds.py` | Corrige los límites de funciones con tablas de saltos o saltos de cola a la siguiente (+ `config/manual_functions.tsv`) |
| `tools/check_overlaps.py` | Lista las funciones generadas que se solapan con la siguiente y las clasifica |
| `tools/ghidra/` | Crea el proyecto de Ghidra con funciones, imports y marcadores |
| `tools/disasm/` | Desensambla un rango con contexto (inicios de función, `.pdata`, saltos, switch) |
| `tests/gen_vmx_tests.py` | Prueba fragmentos reales del código generado contra una referencia |
| `patches/xenonrecomp-prostreet.patch` | Instrucciones añadidas a XenonRecomp |
| `tools/gen_imports.py` | Genera los stubs de los 334 imports y la tabla de slots (`runtime/generated/`) |

## Estado

- [x] Juego extraído y XEX analizado (XDK 2.0.6132, sin RTTI)
- [x] Funciones save/rest de registros localizadas
- [x] Límites de funciones con switch: 156 → 0 con errores
- [x] Todas las instrucciones reconocidas (12 tipos añadidos, probados: 580.000 casos)
- [x] Sin `__builtin_debugtrap`: `vcmpbfp(128)` implementada; `vupkd3d128` tipos 3 y 5 y `vpkd3d128` tipo 3 (half floats)
- [x] `setjmp` (`0x82A28DB0`) y `longjmp` (`0x82A26C50`) localizados
- [x] Proyecto de Ghidra automático (`tools/ghidra/create_project.sh`, ver `docs/GUIA_GHIDRA.md`)
- [x] Funciones generadas que se solapan con la siguiente: 102 → 34, todas explicadas (`tools/check_overlaps.py`)
  - 58 eran el handler (`__C_specific_handler`, `0x82ADBA14`) y la tabla de scopes que hay delante
    de cada función con `__try/__except`: se saltan con `invalid_instructions`
  - 2 seguían un salto de cola condicional a la función siguiente: recortados por `fix_switch_bounds.py`
  - 33 restantes son manejadores `__finally` con dos entradas (la de `.pdata`, usada al desenrollar,
    salta al cuerpo de la otra, que la función padre llama con `bl`) y 1 es `neg r12,r3` que cae en
    `__chkstk`: el código compartido se duplica en las dos funciones, lo cual es correcto
- [x] Runtime mínimo: arranca la CRT, reserva memoria (montón y 334 MB físicos), TLS dinámico
- [ ] Hilos (`ExCreateThread`), objetos de sincronización y esperas: el juego se queda esperando aquí
- [ ] Resto del kernel (`xboxkrnl.exe`) y `xam.xex`
- [ ] Gráficos, audio, input
