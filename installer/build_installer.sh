#!/usr/bin/env bash
#
# Builds the RecLight macOS installer package (.pkg).
#
# Output: installer/dist/RecLight-<version>.pkg
#
# The version comes from RECLIGHT_VERSION, or from the git tag being built, or
# falls back to 0.0.0-dev. It used to be hard-coded as "1.0-Beta" while the
# Windows installer carried "1.0.0" from its own hard-coded copy, so a single
# release shipped two files claiming two different versions.
#
set -euo pipefail

# --- Configuration ----------------------------------------------------------
PRODUCT_NAME="RecLight"
VERSION="${RECLIGHT_VERSION:-$(git -C "$(dirname "${BASH_SOURCE[0]}")/.." describe --tags --exact-match 2>/dev/null || echo "")}"
VERSION="${VERSION#v}"                    # tags are written v1.0.1
VERSION="${VERSION:-0.0.0-dev}"
PKG_VERSION="${VERSION%%-*}"              # pkg metadata wants digits only
IDENTIFIER_BASE="com.steinbach-audio.reclight"

# --- Paths ------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ARTEFACTS="${REPO_DIR}/build/RecLight_artefacts/Release"
RESOURCES="${SCRIPT_DIR}/resources"
BUILD_DIR="${SCRIPT_DIR}/build"
PKG_DIR="${BUILD_DIR}/pkgs"
DIST_DIR="${SCRIPT_DIR}/dist"

VST3_SRC="${ARTEFACTS}/VST3/${PRODUCT_NAME}.vst3"
AU_SRC="${ARTEFACTS}/AU/${PRODUCT_NAME}.component"
APP_SRC="${ARTEFACTS}/Standalone/${PRODUCT_NAME}.app"

# --- Sanity checks ----------------------------------------------------------
for p in "${VST3_SRC}" "${AU_SRC}" "${APP_SRC}"; do
    if [[ ! -e "${p}" ]]; then
        echo "ERROR: missing build artefact: ${p}" >&2
        echo "Build the plugin first:" >&2
        echo "  cmake --build build --target RecLight_VST3 RecLight_AU RecLight_Standalone" >&2
        exit 1
    fi
done

# --- Clean staging ----------------------------------------------------------
rm -rf "${BUILD_DIR}" "${DIST_DIR}"
mkdir -p "${PKG_DIR}" "${DIST_DIR}"

# --- Component packages -----------------------------------------------------
echo "==> Packaging VST3"
pkgbuild \
    --component "${VST3_SRC}" \
    --identifier "${IDENTIFIER_BASE}.vst3" \
    --version "${PKG_VERSION}" \
    --install-location "/Library/Audio/Plug-Ins/VST3" \
    "${PKG_DIR}/RecLight-VST3.pkg"

echo "==> Packaging AU"
pkgbuild \
    --component "${AU_SRC}" \
    --identifier "${IDENTIFIER_BASE}.au" \
    --version "${PKG_VERSION}" \
    --install-location "/Library/Audio/Plug-Ins/Components" \
    "${PKG_DIR}/RecLight-AU.pkg"

echo "==> Packaging Standalone app"
pkgbuild \
    --component "${APP_SRC}" \
    --identifier "${IDENTIFIER_BASE}.app" \
    --version "${PKG_VERSION}" \
    --install-location "/Applications" \
    "${PKG_DIR}/RecLight-App.pkg"

# --- Distribution definition ------------------------------------------------
DIST_XML="${BUILD_DIR}/distribution.xml"
cat > "${DIST_XML}" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>${PRODUCT_NAME} ${VERSION}</title>
    <welcome file="welcome.html" mime-type="text/html"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>

    <choices-outline>
        <line choice="choice_vst3"/>
        <line choice="choice_au"/>
        <line choice="choice_app"/>
    </choices-outline>

    <choice id="choice_vst3" title="VST3 Plug-In" description="Installs the RecLight VST3 plug-in.">
        <pkg-ref id="${IDENTIFIER_BASE}.vst3"/>
    </choice>
    <choice id="choice_au" title="Audio Unit (AU)" description="Installs the RecLight Audio Unit plug-in.">
        <pkg-ref id="${IDENTIFIER_BASE}.au"/>
    </choice>
    <choice id="choice_app" title="Standalone App" description="Installs the RecLight standalone application.">
        <pkg-ref id="${IDENTIFIER_BASE}.app"/>
    </choice>

    <pkg-ref id="${IDENTIFIER_BASE}.vst3" version="${PKG_VERSION}">RecLight-VST3.pkg</pkg-ref>
    <pkg-ref id="${IDENTIFIER_BASE}.au" version="${PKG_VERSION}">RecLight-AU.pkg</pkg-ref>
    <pkg-ref id="${IDENTIFIER_BASE}.app" version="${PKG_VERSION}">RecLight-App.pkg</pkg-ref>
</installer-gui-script>
XML

# --- Final product package --------------------------------------------------
OUTPUT="${DIST_DIR}/${PRODUCT_NAME}-${VERSION}.pkg"
echo "==> Building product installer"
productbuild \
    --distribution "${DIST_XML}" \
    --resources "${RESOURCES}" \
    --package-path "${PKG_DIR}" \
    "${OUTPUT}"

echo ""
echo "Done: ${OUTPUT}"
