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
```

## Herramientas

| Archivo | Uso |
|---|---|
| `tools/xiso_extract.py` | Lista o extrae el sistema de archivos XDVDFS de la ISO |
| `tools/xex_info.py` | Muestra la cabecera del XEX (cifrado, XDK, imports) |
| `tools/xex_dump/` | Vuelca la imagen descifrada y busca las funciones save/rest de registros |
| `tools/fix_switch_bounds.py` | Corrige los límites de funciones que contienen tablas de saltos (+ `config/manual_functions.tsv`) |
| `tools/ghidra/` | Crea el proyecto de Ghidra con funciones, imports y marcadores |
| `tests/gen_vmx_tests.py` | Prueba fragmentos reales del código generado contra una referencia |
| `patches/xenonrecomp-prostreet.patch` | Instrucciones añadidas a XenonRecomp |

## Estado

- [x] Juego extraído y XEX analizado (XDK 2.0.6132, sin RTTI)
- [x] Funciones save/rest de registros localizadas
- [x] Límites de funciones con switch: 156 → 12 con errores
- [x] Todas las instrucciones reconocidas (12 tipos añadidos, probados: 580.000 casos)
- [x] Sin `__builtin_debugtrap`: `vcmpbfp(128)` implementada; `vupkd3d128` tipos 3 y 5 y `vpkd3d128` tipo 3 (half floats)
- [x] `setjmp` (`0x82A28DB0`) y `longjmp` (`0x82A26C50`) localizados
- [x] Proyecto de Ghidra automático (`tools/ghidra/create_project.sh`, ver `docs/GUIA_GHIDRA.md`)
- [ ] 12 switch con destinos fuera de su función (revisión manual en Ghidra)
- [ ] Runtime: memoria, kernel (`xboxkrnl.exe`), `xam.xex`
- [ ] Gráficos, audio, input
