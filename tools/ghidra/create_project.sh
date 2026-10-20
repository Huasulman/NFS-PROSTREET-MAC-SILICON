#!/bin/sh
# Crea el proyecto de Ghidra analysis/ghidra/ProStreet con la imagen descifrada del XEX.
# Requiere: tools/xex_dump (imagen + imports), XenonRecomp ya ejecutado (ppc/) y sus logs.
set -e
cd "$(dirname "$0")/../.."
GHIDRA="$(brew --prefix ghidra)/libexec"

tools/xex_dump/xex_dump game/default.xex analysis/default_image.bin analysis/imports.tsv > analysis/xex_dump.txt
python3 -I tools/ghidra/gen_labels.py > analysis/ghidra_labels.tsv

mkdir -p analysis/ghidra
# A2ALT-32addr: PowerPC de 64 bits con AltiVec y direcciones de 32 bits (como el Xenon).
# Ghidra no decodifica las instrucciones VMX128; aparecerán como datos o instrucciones inválidas.
"$GHIDRA/support/analyzeHeadless" analysis/ghidra ProStreet \
    -import analysis/default_image.bin \
    -loader BinaryLoader -loader-baseAddr 0x82000000 \
    -processor "PowerPC:BE:64:A2ALT-32addr" \
    -scriptPath tools/ghidra \
    -preScript ProStreetSetup.java analysis/ghidra_labels.tsv \
    -overwrite \
    -max-cpu 8 \
    -log analysis/ghidra_log.txt
