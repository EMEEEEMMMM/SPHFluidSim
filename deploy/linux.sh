#!/usr/bin/env bash
# Build the portable Linux x86_64 package with linuxdeploy + its Qt plugin.
#
# The archive contains an AppDir-style tree:
#   AppRun, SPHFluidSim.desktop, usr/bin/SPHFluidSim,
#   usr/bin/shaders/, usr/lib/ (Qt, assimp, OpenCL, NVRTC), usr/plugins/ (Qt
#   platform/image/QML plugins), README.md.
#
# libcuda.so.1 is deliberately NOT bundled: it belongs to the NVIDIA display
# driver. NVRTC and its builtins are redistributable and included so the CUDA
# backend can JIT its kernels at runtime.
set -euo pipefail

BUILD_DIR="build"
OUTPUT=""
VERSION=""
QT_ROOT=""
CUDA_ROOT="/usr/local/cuda"
LINUXDEPLOY="${LINUXDEPLOY:-linuxdeploy}"
EXE_NAME="SPHFluidSim"

usage() {
  cat <<'EOF'
Usage: deploy/linux.sh [options]
  --build-dir DIR       build tree containing SPHFluidSim (default: build)
  --output FILE         output .tar.gz (default: dist/SPHFluidSim-vMAJOR.MINOR.PATCH-linux-x86_64.tar.gz)
  --version VERSION     version (default: root VERSION)
  --qt-root DIR         Qt installation prefix (default: QT_ROOT_DIR or qmake's prefix)
  --cuda-root DIR       CUDA toolkit root (default: /usr/local/cuda)
  --linuxdeploy PATH    linuxdeploy executable (default: linuxdeploy on PATH)
  --exe-name NAME       executable name (default: SPHFluidSim)
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --output) OUTPUT="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --qt-root) QT_ROOT="$2"; shift 2 ;;
    --cuda-root) CUDA_ROOT="$2"; shift 2 ;;
    --linuxdeploy) LINUXDEPLOY="$2"; shift 2 ;;
    --exe-name) EXE_NAME="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$REPO_ROOT/$BUILD_DIR" ;; esac

if [ -z "$VERSION" ]; then
  VERSION="$(tr -d '\r\n' < "$REPO_ROOT/VERSION")"
fi
if ! printf '%s' "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
  echo "VERSION must be MAJOR.MINOR.PATCH (got '$VERSION')" >&2
  exit 1
fi
if [ -z "$OUTPUT" ]; then
  OUTPUT="$REPO_ROOT/dist/SPHFluidSim-v${VERSION}-linux-x86_64.tar.gz"
fi
case "$OUTPUT" in /*) ;; *) OUTPUT="$REPO_ROOT/$OUTPUT" ;; esac

EXE="$BUILD_DIR/$EXE_NAME"
[ -x "$EXE" ] || { echo "Executable not found: $EXE (build first)" >&2; exit 1; }
command -v tar >/dev/null || { echo "tar is required" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Qt root: explicit flag > QT_ROOT_DIR > qmake -query.
# ---------------------------------------------------------------------------
if [ -z "$QT_ROOT" ] && [ -n "${QT_ROOT_DIR:-}" ]; then QT_ROOT="$QT_ROOT_DIR"; fi
if [ -z "$QT_ROOT" ] && command -v qmake6 >/dev/null; then
  QT_ROOT="$(qmake6 -query QT_INSTALL_PREFIX)"
fi
if [ -z "$QT_ROOT" ] && command -v qmake >/dev/null; then
  QT_ROOT="$(qmake -query QT_INSTALL_PREFIX)"
fi
[ -n "$QT_ROOT" ] && [ -d "$QT_ROOT" ] || {
  echo "Could not resolve the Qt prefix; pass --qt-root DIR" >&2; exit 1
}

# linuxdeploy may be an AppImage. APPIMAGE_EXTRACT_AND_RUN avoids a FUSE
# dependency on CI runners.
if [ ! -x "$LINUXDEPLOY" ]; then
  if command -v "$LINUXDEPLOY" >/dev/null; then
    LINUXDEPLOY="$(command -v "$LINUXDEPLOY")"
  else
    echo "linuxdeploy not found; pass --linuxdeploy PATH" >&2
    exit 1
  fi
fi
command -v linuxdeploy-plugin-qt >/dev/null || {
  echo "linuxdeploy-plugin-qt must be on PATH next to linuxdeploy" >&2
  exit 1
}

# ---------------------------------------------------------------------------
# Stage an AppDir and the desktop metadata linuxdeploy needs.
# ---------------------------------------------------------------------------
DIST_DIR="$REPO_ROOT/dist"
mkdir -p "$DIST_DIR"
APPDIR="$DIST_DIR/SPHFluidSim.AppDir"
case "$APPDIR" in
  "$REPO_ROOT"|"$REPO_ROOT/"|"/"|"") echo "Refusing unsafe AppDir '$APPDIR'" >&2; exit 1 ;;
esac
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/scalable/apps" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
         "$APPDIR/usr/bin/shaders"
touch "$APPDIR/.SPHFluidSimAppDir"

install -m 0755 "$EXE" "$APPDIR/usr/bin/$EXE_NAME"

# Shaders are resolved next to the executable.
for shader in vertex_shader.vert fragment_shader.frag surface_vertex.glsl surface_fragment.glsl; do
  if [ -f "$BUILD_DIR/shaders/$shader" ]; then
    install -m 0644 "$BUILD_DIR/shaders/$shader" "$APPDIR/usr/bin/shaders/$shader"
  elif [ -f "$REPO_ROOT/src/ShaderProgram/$shader" ]; then
    install -m 0644 "$REPO_ROOT/src/ShaderProgram/$shader" "$APPDIR/usr/bin/shaders/$shader"
  else
    echo "Missing shader: $shader" >&2
    exit 1
  fi
done

cat > "$APPDIR/usr/share/applications/SPHFluidSim.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=SPHFluidSim
Comment=Real-time implicit incompressible SPH fluid simulation
Exec=SPHFluidSim
Icon=SPHFluidSim
Terminal=false
Categories=Graphics;Science;Education;
EOF

# Dependency-free SVG application icon.
cat > "$APPDIR/usr/share/icons/hicolor/scalable/apps/SPHFluidSim.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <rect width="256" height="256" rx="48" fill="#1F2638"/>
  <path d="M128 48c30 50 52 75 52 103a52 52 0 1 1-104 0c0-28 22-53 52-103z" fill="#3FA9F5"/>
  <path d="M106 151a25 25 0 0 0 25 25" fill="none" stroke="#BFE7FF" stroke-width="10" stroke-linecap="round"/>
</svg>
EOF
cp "$APPDIR/usr/share/icons/hicolor/scalable/apps/SPHFluidSim.svg" \
   "$APPDIR/usr/share/icons/hicolor/256x256/apps/SPHFluidSim.svg"

export QMAKE="${QMAKE:-$QT_ROOT/bin/qmake}"
export APPIMAGE_EXTRACT_AND_RUN=1
export LD_LIBRARY_PATH="$QT_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

echo "Running linuxdeploy Qt deployment ..."
"$LINUXDEPLOY" --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/$EXE_NAME" \
  --desktop-file "$APPDIR/usr/share/applications/SPHFluidSim.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/SPHFluidSim.svg" \
  --plugin qt

# ---------------------------------------------------------------------------
# Extra runtime libraries that are dlopen/ed by the app or that linuxdeploy may
# classify as system libraries.
# ---------------------------------------------------------------------------
copy_glob() {
  local dest="$1"; shift
  local found=0
  for pattern in "$@"; do
    for f in $pattern; do
      [ -e "$f" ] || continue
      cp -a "$f" "$dest/"
      found=1
    done
  done
  return $found
}
LIBDIR="$APPDIR/usr/lib"
mkdir -p "$LIBDIR"

echo "Collecting OpenCL ICD loader ..."
copy_glob "$LIBDIR" "/usr/lib/x86_64-linux-gnu/libOpenCL.so*" "/usr/lib64/libOpenCL.so*" || \
  echo "warning: OpenCL loader not found (the app runs on CPU without it)"

if [ -d "$CUDA_ROOT" ]; then
  echo "Collecting NVRTC runtime from $CUDA_ROOT ..."
  # A full toolkit installs into lib64/, the redistributable archives use lib/.
  copy_glob "$LIBDIR" \
    "$CUDA_ROOT/lib64/libnvrtc.so*" "$CUDA_ROOT/lib64/libnvrtc-builtins.so*" \
    "$CUDA_ROOT/lib/libnvrtc.so*" "$CUDA_ROOT/lib/libnvrtc-builtins.so*" \
    || { echo "warning: NVRTC libraries not found; CUDA backend will fall back to CPU" >&2; }
fi

# AppRun exported by the Qt plugin already sets the environment. A qt.conf
# makes the tree relocatable even when launched without AppRun.
cat > "$APPDIR/usr/bin/qt.conf" <<'EOF'
[Paths]
Prefix=..
Plugins=plugins
Qml2Imports=qml
EOF

cp "$REPO_ROOT/README.md" "$APPDIR/README.md"

# Strip debug symbols from packaged binaries (best effort).
if command -v strip >/dev/null; then
  find "$APPDIR/usr/bin" -type f -name '*.so*' -o -type f -name "$EXE_NAME" 2>/dev/null | \
    while read -r f; do strip --strip-unneeded "$f" 2>/dev/null || true; done
fi

# ---------------------------------------------------------------------------
# Archive: top-level directory so extraction is tidy.
# ---------------------------------------------------------------------------
PKG_NAME="SPHFluidSim-v${VERSION}-linux-x86_64"
STAGE_PARENT="$DIST_DIR/.stage-linux"
rm -rf "$STAGE_PARENT"
mkdir -p "$STAGE_PARENT/$PKG_NAME"
cp -a "$APPDIR/." "$STAGE_PARENT/$PKG_NAME/"
rm -rf "$STAGE_PARENT/$PKG_NAME/.SPHFluidSimAppDir"

mkdir -p "$(dirname "$OUTPUT")"
rm -f "$OUTPUT"
tar -C "$STAGE_PARENT" -czf "$OUTPUT" "$PKG_NAME"
rm -rf "$STAGE_PARENT"

echo "Linux package: $OUTPUT"
