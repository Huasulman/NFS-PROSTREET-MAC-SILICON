#!/bin/sh
# Crea ProStreet.app en la raíz del proyecto: abre runtime/build/prostreet desde la carpeta del
# proyecto (donde están game/ y data/) y guarda lo que escribe en data/ultima_partida.log.
#
# Uso: tools/make_app.sh [imagen.png]
# La imagen (opcional) se usa como icono, recortada al cuadrado del centro. La app no va al
# repositorio (está en .gitignore): si el icono sale del juego, es material del juego.
set -e
cd "$(dirname "$0")/.."
APP=ProStreet.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

# El ejecutable se lanza por un enlace dentro de la app: así macOS lo trata como la propia app
# (nombre e icono en el Dock) y no como otro programa. Su nombre no puede ser "prostreet": el
# disco no distingue mayúsculas y el script de abajo escribiría a través del enlace.
ln -s ../../../runtime/build/prostreet "$APP/Contents/MacOS/ProStreetGame"
cat > "$APP/Contents/MacOS/Launcher" <<'EOF'
#!/bin/sh
# La app está en la raíz del proyecto: Contents/MacOS -> raíz.
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/../../.." || exit 1
exec "$HERE/ProStreetGame" > data/ultima_partida.log 2>&1
EOF
chmod +x "$APP/Contents/MacOS/Launcher"

ICON_KEY=""
if [ -n "$1" ]; then
    WORK=$(mktemp -d)
    # Recorte cuadrado del centro, y los tamaños de un .icns.
    SIZE=$(sips -g pixelHeight "$1" | awk '/pixelHeight/ {print $2}')
    sips -c "$SIZE" "$SIZE" "$1" --out "$WORK/square.png" > /dev/null
    mkdir "$WORK/ProStreet.iconset"
    for size in 16 32 128 256 512; do
        sips -z $size $size "$WORK/square.png" --out "$WORK/ProStreet.iconset/icon_${size}x${size}.png" > /dev/null
        sips -z $((size * 2)) $((size * 2)) "$WORK/square.png" --out "$WORK/ProStreet.iconset/icon_${size}x${size}@2x.png" > /dev/null
    done
    iconutil -c icns "$WORK/ProStreet.iconset" -o "$APP/Contents/Resources/ProStreet.icns"
    rm -rf "$WORK"
    ICON_KEY="<key>CFBundleIconFile</key><string>ProStreet</string>"
fi

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>ProStreet</string>
    <key>CFBundleDisplayName</key><string>Need for Speed ProStreet</string>
    <key>CFBundleIdentifier</key><string>local.prostreet.recomp</string>
    <key>CFBundleExecutable</key><string>Launcher</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
    $ICON_KEY
</dict>
</plist>
EOF
echo "Creada $APP"
