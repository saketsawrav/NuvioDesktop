#!/usr/bin/env bash
#
# Build the Nuvio Desktop Linux AppImage (x86_64), including the native libmpv
# player bridge. Run from anywhere inside the repository on an x86_64 Linux host.
#
# Prerequisites:
#   Arch:          sudo pacman -S --needed jdk17-openjdk gcc libx11 mpv fuse2 zsync
#                  appimagetool from AUR (paru -S appimagetool-bin) or download below.
#                  On a Wayland session also: sudo pacman -S --needed xorg-xwayland
#                  Select the JDK: sudo archlinux-java set java-17-openjdk
#   Ubuntu/Debian: sudo apt install -y openjdk-17-jdk build-essential libx11-dev libmpv2 zsync libfuse2
#   Fedora:        sudo dnf install -y java-17-openjdk-devel gcc-c++ libX11-devel mpv-libs zsync fuse
#   appimagetool on PATH (any distro):
#     sudo wget -O /usr/local/bin/appimagetool \
#       https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
#     sudo chmod +x /usr/local/bin/appimagetool
#
# Usage:
#   ./scripts/build-linux-appimage.sh         # build the AppImage
#   ./scripts/build-linux-appimage.sh --run   # skip packaging, run from source (fast playback test)
#
set -euo pipefail

cd "$(dirname "$0")/.."   # repository root

note() { printf '\033[1;34m[build]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

# Distro-aware dependency install hint.
if command -v pacman >/dev/null 2>&1; then
    INSTALL_HINT="sudo pacman -S --needed jdk17-openjdk gcc libx11 mpv fuse2 zsync  (+ appimagetool from AUR; + xorg-xwayland on Wayland)"
elif command -v apt-get >/dev/null 2>&1; then
    INSTALL_HINT="sudo apt install -y openjdk-17-jdk build-essential libx11-dev libmpv2 zsync libfuse2"
elif command -v dnf >/dev/null 2>&1; then
    INSTALL_HINT="sudo dnf install -y java-17-openjdk-devel gcc-c++ libX11-devel mpv-libs zsync fuse"
else
    INSTALL_HINT="install: a full JDK 17+, g++, libX11 headers, libmpv (libmpv.so.2), zsync, fuse2"
fi

# 1. Resolve a FULL JDK (Gradle + jpackage need it; the native bridge needs its JNI headers).
if [ -z "${JAVA_HOME:-}" ] && command -v java >/dev/null 2>&1; then
    JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v java)")")")"
fi
[ -n "${JAVA_HOME:-}" ] || fail "Set JAVA_HOME to a full JDK 17+.  $INSTALL_HINT"
export JAVA_HOME
note "JAVA_HOME=$JAVA_HOME"
[ -f "$JAVA_HOME/include/jni.h" ] || fail "No jni.h under \$JAVA_HOME/include — that is a JRE, not a JDK (the native bridge needs JDK headers).  On Arch: sudo archlinux-java set java-17-openjdk"
[ -x "$JAVA_HOME/bin/jpackage" ] || warn "jpackage missing under \$JAVA_HOME/bin — packaging needs a JDK 17+ with jpackage."

# 2. Check build/runtime tools (warn + aggregate hint; Gradle fails with a precise message if truly missing).
miss=0
command -v g++ >/dev/null 2>&1          || { warn "g++ missing"; miss=1; }
[ -f /usr/include/X11/Xlib.h ]          || { warn "X11 headers (X11/Xlib.h) missing"; miss=1; }
command -v appimagetool >/dev/null 2>&1 || { warn "appimagetool not on PATH"; miss=1; }
ldconfig -p 2>/dev/null | grep -q 'libmpv\.so\.2' \
    || warn "libmpv.so.2 not found (dlopen'd at runtime) — install mpv/libmpv, or set NUVIO_LIBMPV_PATH"
[ "$miss" -eq 0 ] || warn "Install deps:  $INSTALL_HINT"

# 3. Fast path: run from source (verify playback without packaging).
if [ "${1:-}" = "--run" ]; then
    note "Running from source (builds the native bridge, then launches the app)…"
    exec ./gradlew :composeApp:run
fi

# 4. Build the AppImage.
note "Building the Linux AppImage…"
./gradlew :composeApp:packageAppImageWithUpdate

out="composeApp/build/compose/binaries/main-release/appimage"
if ls "$out"/Nuvio-*-x86_64.AppImage >/dev/null 2>&1; then
    note "Done:"
    ls -lh "$out"/Nuvio-*-x86_64.AppImage
    note "Run:  chmod +x $out/Nuvio-*-x86_64.AppImage && $out/Nuvio-*-x86_64.AppImage"
    note "      (no FUSE?  add --appimage-extract-and-run )"
else
    fail "No AppImage produced in $out — check the Gradle output above."
fi
