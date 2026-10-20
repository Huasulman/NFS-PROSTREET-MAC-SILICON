#!/bin/sh
# Genera, compila y ejecuta las pruebas de las instrucciones VMX añadidas a XenonRecomp.
# Requiere haber ejecutado antes XenonRecomp (carpeta ppc/).
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/prostreet_vmx_test"
python3 -I tests/gen_vmx_tests.py ppc "$OUT.cpp"
/opt/homebrew/opt/llvm/bin/clang++ -std=c++20 -O1 -w -Ippc -Itools/XenonRecomp/thirdparty/simde "$OUT.cpp" -o "$OUT"
"$OUT"
