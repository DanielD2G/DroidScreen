#!/bin/bash
# ─────────────────────────────────────────────────────────────
# DroidScreen macOS DMG Builder
#
# Crea un .dmg que contiene DroidScreen.app + symlink a /Applications.
# El usuario abre el .dmg y arrastra DroidScreen.app a Applications.
#
# Uso:
#   ./installer/macos/build_dmg.sh <version> <app_bundle_path> <output_dir>
#
# Requiere: hdiutil (incluido en macOS)
# ─────────────────────────────────────────────────────────────
set -e

VERSION="${1:?Uso: $0 <version> <app_bundle> <output_dir>}"
APP_BUNDLE="${2:?}"
OUTPUT_DIR="${3:?}"

if [ ! -d "$APP_BUNDLE" ]; then
    echo "ERROR: App bundle no encontrado: $APP_BUNDLE"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

DMG_NAME="DroidScreen-macOS-arm64-v${VERSION}"
DMG_OUT="$OUTPUT_DIR/${DMG_NAME}.dmg"
STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT

echo "=== DroidScreen macOS DMG Build ==="
echo "Version:    $VERSION"
echo "App bundle: $APP_BUNDLE"
echo "Output:     $DMG_OUT"
echo ""

# Copiar el bundle con nombre correcto
ditto "$APP_BUNDLE" "$STAGING/DroidScreen.app"

# Symlink a /Applications para el drag-install
ln -s /Applications "$STAGING/Applications"

# Crear DMG comprimido
hdiutil create \
    -volname "DroidScreen $VERSION" \
    -srcfolder "$STAGING" \
    -ov \
    -format UDZO \
    "$DMG_OUT"

echo ""
echo "=== DMG creado ==="
echo "  $DMG_OUT"
echo ""
echo "Para instalar: abrir el .dmg y arrastrar DroidScreen.app a Applications"
