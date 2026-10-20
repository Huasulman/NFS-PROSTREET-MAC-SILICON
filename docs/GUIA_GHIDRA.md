# Guía de Ghidra para ProStreet Recomp

Cómo abrir el ejecutable del juego en Ghidra y revisar los límites de funciones que
`tools/fix_switch_bounds.py` no pudo resolver solo.

## 1. Crear (o recrear) el proyecto

El proyecto se genera automáticamente con todo lo que ya sabemos del ejecutable:

```sh
tools/ghidra/create_project.sh
```

Tarda varios minutos (el análisis automático de 17 MB de código). Hace esto:

- Carga `analysis/default_image.bin` (el XEX ya descifrado) en `0x82000000` con el lenguaje
  `PowerPC:BE:64:A2ALT-32addr`.
- Divide la memoria en las secciones del XEX (`.text`, `.rdata`, `.data`, ...) con sus permisos.
- Crea las **84.514 funciones** que conoce XenonRecomp, con los mismos nombres (`sub_XXXXXXXX`)
  que en el código C++ generado. Así lo que ves en Ghidra coincide con `ppc/`.
- Pone nombre a los **334 imports** del kernel y de XAM (`__imp__RtlUnwind`, ...), a
  `__setjmp`, `__longjmp`, `xex_entry_point` y a las funciones `__savegprlr_14`, etc.
- Crea **marcadores** (*bookmarks*) en cada caso que hay que revisar.

Para regenerarlo después de cambiar el TOML, vuelve a ejecutar el script: usa `-overwrite`.

## 2. Abrir el proyecto

1. Ejecuta `ghidraRun`.
2. **File → Open Project…** y elige `analysis/ghidra/ProStreet.gpr`.
3. Doble clic en `default_image.bin`. Si pregunta si quieres analizarlo, responde **No**
   (ya está analizado).

Ventanas útiles:

| Ventana | Para qué |
|---|---|
| **Listing** (centro) | El ensamblador |
| **Decompiler** (Window → Decompiler) | El pseudo-C de la función actual |
| **Bookmarks** (Window → Bookmarks) | La lista de casos a revisar |
| **Function Graph** (Window → Function Graph) | El grafo de bloques: muy útil para ver dónde termina una función |
| **Go To** (tecla `G`) | Saltar a una dirección o un nombre, p. ej. `0x82373400` o `sub_823733F0` |

### Limitación: VMX128

Ghidra no conoce las instrucciones vectoriales propias de la Xbox 360 (VMX128: `lvx128`,
`vsel128`, `vupkd3d128`...). Donde aparezcan, el código se verá como `??` o datos sueltos y
Ghidra corta ahí la función. Por ejemplo, `__setjmp` aparece terminando en `0x82A28E63`,
justo antes de su primera `stvlx128`, aunque en realidad sigue hasta `0x82A2907F`.
Para estos casos manda el código generado en `ppc/`, que sí las tiene: cada instrucción
aparece allí como comentario (`// vsel128 v43,v8,v7,v43`).

Por lo mismo, después del análisis automático Ghidra muestra unas **81.000 funciones**, no las
84.514 que le pasamos: une o descarta algunas que quedan cortadas por VMX128. Los límites que
valen para la recompilación son los de XenonRecomp; los de Ghidra son solo una ayuda visual.

## 3. Los casos a revisar

En la ventana **Bookmarks** Ghidra muestra también miles de marcadores propios del análisis.
Ordena por la columna **Category** (o usa el filtro) y quédate con las dos nuestras:

- **Switch con error** (12): XenonRecomp avisa que un destino del switch cae fuera de la
  función. **Estos son los que importan**: en el C++ generado, ese caso del switch hace
  `return` en lugar de saltar, así que el juego se comportaría mal si llega ahí.
- **Revisar límites** (14): lo que el script dejó sin resolver. Casi todos coinciden con los
  anteriores; 3 de ellos (`sub_8233DC58`, `sub_823B50A0`, `sub_82447C30`) ya no dan error y
  solo hay que confirmar que están bien.

| Switch (bctr) | Función actual | Diagnóstico del script |
|---|---|---|
| `0x82373414` | `sub_823733F0` | El switch está en la continuación de la función anterior |
| `0x823C3814` | `sub_823C37D8` | Límite calculado por el script, pero aún queda un destino fuera |
| `0x8244190C` | `sub_824418F8` | Un destino (`0x824418EC`) está antes del inicio |
| `0x824BBE28` | `sub_824BBCB8` | Extenderla absorbería la función real `sub_824BC4F8` |
| `0x824BC058` | `sub_824BC024` | Continuación de la función anterior |
| `0x824BC23C` | `sub_824BC214` | Continuación de la función anterior |
| `0x8257E5C0` | `sub_8257E590` | Continuación de la función anterior |
| `0x825ED1E0` | `sub_825ED1C0` | Continuación de la función anterior |
| `0x8262245C` | `sub_82622440` | Continuación de la función anterior |
| `0x82622D94` | `sub_82622D80` | Continuación de la función anterior |
| `0x82723C90` | `sub_82723C60` | Continuación de la función anterior |
| `0x827277F4` | `sub_827277D8` | Continuación de la función anterior |

## 4. Cómo revisar un caso

Para cada fila:

1. **Ve al switch** (`G` → dirección). Verás algo como:
   ```
   lis   r12, ...      ; dirección de la tabla
   addi  r12, r12, ...
   rlwinm r0, r11, 2, 0, 29
   lwzx  r0, r12, r0
   mtctr r0
   bctr                ; <- el salto del switch
   ```
2. **Mira los destinos.** En `config/ProStreet_switch_tables.toml` busca `base = 0x...` con la
   dirección del `lis` (el marcador "Revisar límites" está ahí) y anota los `labels`.
3. **Encuentra el inicio real de la función.** Sube desde el switch hasta encontrar:
   - un prólogo (`mflr r12` / `bl __savegprlr_14` / `stwu r1, -X(r1)`), o
   - el final de la función anterior (`blr` o un `b` a otra función) **sin** saltos que
     vengan de antes hacia aquí.

   Pista: en "continuación de la función anterior", lo normal es que el inicio real sea el de
   **la función anterior**: el analizador la cortó en un `blr` intermedio (un retorno
   temprano) o en otro `bctr`.
4. **Encuentra el final real.** Baja hasta el último bloque alcanzable desde el inicio
   (incluidos todos los destinos del switch). El **Function Graph** ayuda: si un bloque no
   tiene flechas que entren desde la función, ya es otra cosa.
5. **Comprueba que no te comes otra función.** Si en medio hay una función a la que se llama
   con `bl` desde otros sitios (Ghidra lo muestra en *References*, tecla `Ctrl+Shift+F`), el
   límite no es ese.
6. **Anota el resultado** en `config/manual_functions.tsv`:
   ```
   0x823733A8  0x120  # sub_823733F0 era continuación de sub_823733A8 (retorno temprano)
   ```
   El tamaño es `fin - inicio` (en bytes, múltiplo de 4).

## 5. Aplicar los cambios

```sh
python3 -I tools/fix_switch_bounds.py analysis/default_image.bin ppc/ppc_func_mapping.cpp \
    config/ProStreet_switch_tables.toml config/ProStreet.toml
rm -rf ppc && mkdir ppc
tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/ProStreet.toml tools/XenonRecomp/XenonUtils/ppc_context.h \
    > analysis/recomp_log.txt 2>&1
grep -c "ERROR: Switch" analysis/recomp_log.txt
```

El script mete las entradas de `manual_functions.tsv` en el TOML (marcadas con `# manual`) y
quita las funciones automáticas que se solapen con ellas. El objetivo es que el último comando
devuelva **0**. Luego recrea el proyecto de Ghidra para que refleje los nuevos límites.

## 6. Ya resuelto con ayuda de este análisis

- `setjmp` = `0x82A28DB0`: guarda f14–f31, r13–r31, v64–v127, CR (+304) y LR (+308) y devuelve 0.
- `longjmp` = `0x82A26C50`: la única función que carga desde ese mismo formato de búfer; llama a
  `RtlUnwind` cuando el búfer lo pide.

Ambos están en `config/ProStreet.toml`, y XenonRecomp ya redirige sus 7 y 10 llamadas a las
versiones nativas.
