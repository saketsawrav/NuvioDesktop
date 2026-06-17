#!/usr/bin/env bash
#
# Build the Nuvio Desktop Linux AppImage (x86_64), including the native libmpv
# player bridge. Run from anywhere inside the repository on an x86_64 Linux host.
#
# Prerequisites (Ubuntu 24.04+ / Debian 13+ — these ship libmpv.so.2):
#   sudo apt update
#   sudo apt install -y openjdk-17-jdk build-essential libx11-dev libmpv2 zsync libfuse2
#   # appimagetool on PATH:
#   sudo wget -O /usr/local/bin/appimagetool \
#     https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
#   sudo chmod +x /usr/local/bin/appimagetool
#
# Usage:
#   ./scripts/build-linux-appimage.sh         # build the AppImage
#   ./scripts/build-linux-appimage.sh --run   # skip packaging, just run from source (fast playback test)
#
set -euo pipefail

cd "$(dirname "$0")/.."   # repository root

note() { printf '\033[1;34m[build]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

# 1. Resolve a FULL JDK (Gradle + jpackage need it, and the native bridge needs its JNI headers).
if [ -z "${JAVA_HOME:-}" ] && command -v java >/dev/null 2>&1; then
    JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v java)")")")"
fi
[ -n "${JAVA_HOME:-}" ] || fail "Set JAVA_HOME to a full JDK 17+ (e.g. sudo apt install openjdk-17-jdk)."
export JAVA_HOME
note "JAVA_HOME=$JAVA_HOME"
[ -f "$JAVA_HOME/include/jni.h" ] || fail "No jni.h under \$JAVA_HOME/include — that is a JRE, not a JDK. The native bridge needs JDK headers."
[ -x "$JAVA_HOME/bin/jpackage" ] || warn "jpackage missing under \$JAVA_HOME/bin — packaging needs a JDK 17+ with jpackage."

# 2. Check the build/runtime tools (warn, then let Gradle fail with a precise message if truly missing).
command -v g++ >/dev/null 2>&1            || warn "g++ missing — sudo apt install build-essential"
[ -f /usr/include/X11/Xlib.h ]            || warn "X11 headers missing — sudo apt install libx11-dev"
command -v appimagetool >/dev/null 2>&1   || warn "appimagetool not on PATH — see the header of this script"
ldconfig -p 2>/dev/null | grep -q 'libmpv\.so\.2' \
    || warn "libmpv.so.2 not found (dlopen'd at runtime) — sudo apt install libmpv2  (or set NUVIO_LIBMPV_PATH)"

# 3. Fast path: run from source to verify playback without packaging.
if [ "${1:-}" = "--run" ]; then
    note "Running from source (builds the native bridge, then launches the app)…"
    exec ./gradlew :composeApp:run
fi

# 4. Build the AppImage (compiles the native bridge, jpackages the app image, runs appimagetool).
note "Building the Linux AppImage…"
./gradlew :composeApp:packageAppImageWithUpdate

out="composeApp/build/compose/binaries/main-release/appimage"
note "Done."
if ls "$out"/Nuvio-*-x86_64.AppImage >/dev/null 2>&1; then
    ls -lh "$out"/Nuvio-*-x86_64.AppImage
    note "Run it with:"
    note "  chmod +x $out/Nuvio-*-x86_64.AppImage && $out/Nuvio-*-x86_64.AppImage"
    note "  (if FUSE is unavailable:  $out/Nuvio-*-x86_64.AppImage --appimage-extract-and-run )"
else
    fail "No AppImage produced in $out — check the Gradle output above."
fi
