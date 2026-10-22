#!/bin/sh
# Compila el runtime con el código recompilado de ppc/.
# Requiere: XenonRecomp ya ejecutado (ppc/) y la imagen de tools/xex_dump (analysis/default_image.bin).
set -e
cd "$(dirname "$0")/.."
LLVM=/opt/homebrew/opt/llvm

python3 -I tools/gen_imports.py game/default.xex analysis/default_image.bin ppc runtime/generated
cmake -S runtime -B runtime/build -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
    -DCMAKE_CXX_COMPILER=$LLVM/bin/clang++ -DCMAKE_OBJCXX_COMPILER=$LLVM/bin/clang++ -DCMAKE_CXX_COMPILER_LAUNCHER=ccache > /dev/null
cmake --build runtime/build
