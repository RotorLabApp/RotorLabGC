#!/usr/bin/env bash
# RotorLab GC: Linux x86_64 AppImage release build from one tag (G72).
#
# Called by deploy/rotorlab/release.ps1 (-Platform Linux) through WSL, as the
# normal user made by setup-build-host.sh. It can also be run by hand:
#
#   build-appimage.sh --repo <git dir> --tag <tag> --commit <sha> \
#                     --work <new folder> --out <folder> [--dry-run|--test-build] -- <cmake options>
#
# Same recipe as RotorLab's Linux CI job: Qt 6.8.3
# linux_gcc_64, Ninja, Release, "cmake --install" (cmake/CreateAppImage.cmake:
# linuxdeploy + appimagetool) and the "--simple-boot-test" sanity run under
# xvfb. The CMake options are release.ps1's $CommonOptions, passed after "--".
#
# Steps:
#   1. Clones the tag (depth 1) from --repo into <work>/src, checks that it is
#      --commit and (unless --dry-run) an annotated tag, and that
#      "git describe --tags" names the tag. Submodules at their recorded commits.
#   2. Configures FRESH into <work>/build (the folder must not exist).
#   3. Builds; (G108) downloads linuxdeploy, appimagetool and the AppImage
#      runtime from pinned releases, each checked by SHA-256; installs (writes
#      RotorLabGC-x86_64.AppImage with them), then (G108) copies
#      the Debian copyright file of every bundled Ubuntu library into
#      usr/share/doc/<package>/ (fails if one is missing) and packs again.
#   4. Checks: the version string compiled in is the tag (build.ninja and the
#      binary), no debug-only runtime or Qt QML debugger plugin (qmltooling) in
#      the AppDir, the notices and license texts are in it, and
#      deploy/rotorlab/check-notice.py: every bundled library named in NOTICE
#      (Ubuntu ones with their dpkg version) with its license file.
#   5. Boot test: the AppImage starts and exits (--simple-boot-test) under xvfb,
#      with a fresh HOME and XDG folders (a stable build with an empty profile
#      has no license or Device Key, so it contacts no RotorLab service).
#   6. Copies the AppImage to --out as RotorLabGC-<tag>-linux-x86_64.AppImage
#      and writes <out>/BUILD-INFO-linux.txt.
# It signs nothing, uploads nothing, pushes nothing and creates no tags.
set -euo pipefail

QT_DIR=/opt/Qt/6.8.3/gcc_64
TOOLS=/opt/rotorlab-build
APP=RotorLabGC
REPO= TAG= COMMIT= WORK= OUT= DRY=0 TESTBUILD=0
while [ $# -gt 0 ]; do
    case "$1" in
        --repo)    REPO=$2; shift 2 ;;
        --tag)     TAG=$2; shift 2 ;;
        --commit)  COMMIT=$2; shift 2 ;;
        --work)    WORK=$2; shift 2 ;;
        --out)     OUT=$2; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        --test-build) TESTBUILD=1; DRY=1; shift ;;   # a test build (not for release): checks are reported, not enforced
        --)        shift; break ;;
        *) echo "build-appimage.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
CMAKE_OPTS=("$@")

say()  { echo "[linux] $*"; }
fail() { echo "[linux] FAILED: $*" >&2; exit 1; }

[ -n "$REPO" ] && [ -n "$TAG" ] && [ -n "$COMMIT" ] && [ -n "$WORK" ] && [ -n "$OUT" ] || fail "missing --repo, --tag, --commit, --work or --out"
[[ "$TAG" =~ ^[A-Za-z0-9._-]+$ ]] || fail "tag name '$TAG' has characters this script does not accept"
[ "$(id -u)" -ne 0 ] || fail "run as the build user, not root (GC refuses to start as root; the boot test needs it)"
for p in "$QT_DIR/lib/cmake/Qt6/qt.toolchain.cmake" "$TOOLS/venv/bin/cmake" "$TOOLS/venv/bin/ninja"; do
    [ -e "$p" ] || fail "build host not set up: $p is missing. Run once as root: deploy/rotorlab/linux/setup-build-host.sh"
done
command -v xvfb-run >/dev/null || fail "xvfb-run missing; run setup-build-host.sh"
[ ! -e "$WORK" ] || fail "work folder already exists: $WORK (release builds are never incremental)"
mkdir -p "$OUT"
export PATH="$TOOLS/venv/bin:$PATH"

# ------------------------------------------------------------------ 1. source
say "=== Linux x86_64 Release (AppImage) ==="
. /etc/os-release
say "host: $PRETTY_NAME, $(ldd --version | head -1)"
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
SRC="$WORK/src"
say "clone $TAG from $REPO"
# The repository is on the Windows drive (another owner as WSL sees it): trust that one path,
# for this clone only, in the reading side (upload-pack) too.
git -c safe.directory="$REPO" -c advice.detachedHead=false clone --quiet --depth 1 --branch "$TAG" \
    --upload-pack="git -c safe.directory=$REPO upload-pack" "file://$REPO" "$SRC"
head=$(git -C "$SRC" rev-parse HEAD)
[ "$head" = "$COMMIT" ] || fail "the clone of $TAG is $head, not $COMMIT"
ttype=$(git -C "$SRC" cat-file -t "refs/tags/$TAG")
if [ "$DRY" -eq 0 ] && [ "$ttype" != tag ]; then fail "$TAG is a lightweight tag; make it with git tag -a"; fi
desc=$(git -C "$SRC" describe --always --tags)
[ "$desc" = "$TAG" ] || fail "git describe --tags gives '$desc', not '$TAG'"
say "tag $TAG ($ttype) at $head; describe --tags: $desc"
git -C "$SRC" submodule --quiet update --init --recursive
while IFS= read -r line; do
    [ -z "$line" ] && continue
    case "$line" in " "*) ;; *) fail "submodule not at its recorded commit: $line" ;; esac
done < <(git -C "$SRC" submodule status --recursive)
git -C "$SRC" submodule status --recursive | sed 's/^/[linux]   submodule /'

# --------------------------------------------------------------- 2-3. build
BUILD="$WORK/build"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$QT_DIR/lib/cmake/Qt6/qt.toolchain.cmake" \
    -DCMAKE_PREFIX_PATH="$QT_DIR" \
    "${CMAKE_OPTS[@]}"
# A release is a stable build, and a stable
# binary carries no test-key hook (see Test-NoTestKeyHook in release.ps1).
stable=$(sed -n 's/^QGC_STABLE_BUILD:BOOL=//p' "$BUILD/CMakeCache.txt" | head -n1)
case "$stable" in
    ON|on|On|TRUE|true|True|YES|yes|1) say "configured QGC_STABLE_BUILD=$stable" ;;
    *) fail "configured with QGC_STABLE_BUILD='$stable'; a release is a stable build (QGC_STABLE_BUILD=ON)" ;;
esac
cmake --build "$BUILD" --target all --config Release
# Scan the built binary, not the AppImage: its squashfs is compressed, so a scan
# there would pass whatever it holds. Before any packaging.
HOOK_LINE="test-key hook: none (ROTORLAB_LICENSE_PUBKEYS, ROTORLAB_BASE_URL)"
BUILT_BIN="$BUILD/Release/$APP"
[ -f "$BUILT_BIN" ] || fail "built binary not found: $BUILT_BIN"
for name in ROTORLAB_LICENSE_PUBKEYS ROTORLAB_BASE_URL; do
    if grep -a -q -F "$name" "$BUILT_BIN"; then
        fail "TEST-KEY HOOK FOUND in $BUILT_BIN: $name (a Daily build, not a stable one); nothing was packaged"
    fi
done
say "$HOOK_LINE in $BUILT_BIN"

# ------------------------------------------- 3a. pinned packaging tools (G108)
# Never "continuous". linuxdeploy, appimagetool and the AppImage
# runtime (the start of the shipped file) come from fixed releases, each
# checked by SHA-256 (GitHub's release asset digests). cmake/CreateAppImage.cmake
# downloads its tools only when they are missing, so they are put where it
# looks: linuxdeploy as is, appimagetool as a wrapper that always passes the
# pinned runtime (appimagetool would otherwise download the continuous one).
# The runtime's LICENSE comes from the release's commit and is checked by its
# git blob id; it ships in usr/licenses/components/.
LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
LINUXDEPLOY_SHA256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
RUNTIME_RELEASE=20251108
RUNTIME_COMMIT=dd6cebedcbddde9c82f89b011e8e1d40b6e43868
RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_RELEASE/runtime-x86_64
RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
RUNTIME_LICENSE_URL=https://raw.githubusercontent.com/AppImage/type2-runtime/$RUNTIME_COMMIT/LICENSE
RUNTIME_LICENSE_BLOB=a9fd00b042f392357a84fd81b7789976a53f5b4e
PIN="$WORK/pinned"
mkdir -p "$PIN"
fetch() {   # fetch <url> <out> sha256 <hex> | blob <git blob id>
    python3 - "$@" <<'PY'
import hashlib, sys, urllib.request
url, out, kind, want = sys.argv[1:5]
req = urllib.request.Request(url, headers={'User-Agent': 'build-appimage'})
data = urllib.request.urlopen(req, timeout=120).read()
sha = hashlib.sha256(data).hexdigest()
got = sha if kind == 'sha256' else hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()
if got != want:
    sys.exit('[linux] FAILED: %s: %s %s, expected %s' % (url, kind, got, want))
open(out, 'wb').write(data)
print('[linux]   %s: %d bytes, sha256 %s%s' % (url, len(data), sha, '' if kind == 'sha256' else ', git blob ' + got))
PY
}
say "pinned packaging tools"
fetch "$LINUXDEPLOY_URL" "$BUILD/linuxdeploy-x86_64.AppImage" sha256 "$LINUXDEPLOY_SHA256"
fetch "$APPIMAGETOOL_URL" "$PIN/appimagetool-x86_64.AppImage" sha256 "$APPIMAGETOOL_SHA256"
fetch "$RUNTIME_URL" "$PIN/runtime-x86_64" sha256 "$RUNTIME_SHA256"
fetch "$RUNTIME_LICENSE_URL" "$PIN/type2-runtime-LICENSE" blob "$RUNTIME_LICENSE_BLOB"
chmod 755 "$BUILD/linuxdeploy-x86_64.AppImage" "$PIN/appimagetool-x86_64.AppImage"
# The parts the runtime says it contains must be the ones NOTICE names.
python3 - "$PIN/runtime-x86_64" <<'PY'
import re, sys
data = open(sys.argv[1], 'rb').read()
names = sorted(m.decode() for m in re.findall(rb'\* ([A-Za-z0-9 ._-]+), licensed under', data))
want = sorted(['runtime', 'musl libc', 'libfuse', 'squashfuse', 'libzstd', 'zlib'])
print('[linux]   runtime contains: ' + ', '.join(names))
if names != want:
    sys.exit('[linux] FAILED: the pinned runtime lists %s, NOTICE names %s' % (names, want))
PY
APPIMAGETOOL="$PIN/appimagetool-x86_64.AppImage"
RUNTIME="$PIN/runtime-x86_64"
cat >"$BUILD/appimagetool-x86_64.AppImage" <<EOF
#!/bin/sh
# build-appimage.sh (G108): the pinned appimagetool with the pinned runtime.
exec "$APPIMAGETOOL" --runtime-file "$RUNTIME" "\$@"
EOF
chmod 755 "$BUILD/appimagetool-x86_64.AppImage"

# From inside the build folder, as CI does: cmake/CreateAppImage.cmake puts the AppDir and its
# tools in CMAKE_BINARY_DIR, which an install script takes from the working directory.
(cd "$BUILD" && cmake --install . --config Release)
APPIMAGE="$BUILD/$APP-x86_64.AppImage"
[ -f "$APPIMAGE" ] || fail "AppImage was not created: $APPIMAGE (linuxdeploy/appimagetool output above)"
APPDIR="$BUILD/AppDir"

# ------------------------------------------------- 3b. Debian copyright files
# G108: every Ubuntu library linuxdeploy bundled ships with its Debian package's
# copyright file at usr/share/doc/<package>/copyright. linuxdeploy copies most
# of them but misses the ones dpkg lists under /lib (usrmerge: bzip2, libcap2,
# dbus, keyutils, xz-utils, pcre3 in 5.3.0), so this step copies them all from
# this (Ubuntu 22.04) system and fails if one is missing. Qt's own libraries,
# its ICU 73 and FFmpeg are not Ubuntu packages; NOTICE covers them. Writes
# lib<TAB>package<TAB>version for check-notice.py, then packs the AppImage again
# from the AppDir with the pinned appimagetool and runtime.
DEBS="$WORK/bundled-debs.txt"
: >"$DEBS"
say "Debian copyright files: $APPDIR/usr/lib"
missing=0
while IFS= read -r -d '' lib; do
    n=${lib##*/}
    case "$n" in
        libQt6*|libicuuc.so.73*|libicui18n.so.73*|libicudata.so.73*) continue ;;
        libavcodec.so*|libavformat.so*|libavutil.so*|libswresample.so*|libswscale.so*) continue ;;
    esac
    # Most are in x86_64-linux-gnu/, some one folder down (pulseaudio/libpulsecommon).
    owner=$(dpkg -S "*/x86_64-linux-gnu/$n" 2>/dev/null | grep -v '^diversion' | head -n1 | cut -d: -f1 || true)
    [ -n "$owner" ] || owner=$(dpkg -S "*/x86_64-linux-gnu/*/$n" 2>/dev/null | grep -v '^diversion' | head -n1 | cut -d: -f1 || true)
    if [ -z "$owner" ]; then
        say "  NO DEBIAN PACKAGE: usr/lib/$n (name it in NOTICE and deploy/rotorlab/check-notice.py, or keep it out)"
        missing=$((missing + 1)); continue
    fi
    ver=$(dpkg-query -W -f='${Version}' "$owner")
    src="/usr/share/doc/$owner/copyright"
    if [ ! -f "$src" ]; then
        say "  NO COPYRIGHT FILE: $src (usr/lib/$n, $owner $ver)"
        missing=$((missing + 1)); continue
    fi
    mkdir -p "$APPDIR/usr/share/doc/$owner"
    cp -L "$src" "$APPDIR/usr/share/doc/$owner/copyright"
    printf '%s\t%s\t%s\n' "$n" "$owner" "$ver" >>"$DEBS"
done < <(find "$APPDIR/usr/lib" -maxdepth 1 \( -type f -o -type l \) -name 'lib*.so*' -print0 | sort -z)
pkgs=$(cut -f2 "$DEBS" | sort -u | wc -l)
say "  $(wc -l <"$DEBS") Ubuntu libraries from $pkgs packages; copyright files in usr/share/doc/<package>/"
[ "$missing" -eq 0 ] || fail "$missing bundled libraries without a Debian package or copyright file; nothing was packaged"
cp "$DEBS" "$OUT/BUNDLED-DEBS-linux.txt"
# The runtime's own license text, from its pinned release.
mkdir -p "$APPDIR/usr/licenses/components"
cp "$PIN/type2-runtime-LICENSE" "$APPDIR/usr/licenses/components/AppImage-type2-runtime-$RUNTIME_RELEASE-LICENSE.txt"
# Pack again: the AppDir now holds the copyright files. The pinned appimagetool
# with the pinned runtime, as the first pack (the wrapper in 3a).
rm -f "$APPIMAGE"
(cd "$BUILD" && ARCH=x86_64 "$APPIMAGETOOL" --runtime-file "$RUNTIME" "$APPDIR")
[ -f "$APPIMAGE" ] || fail "AppImage was not created again: $APPIMAGE"

# --------------------------------------------------------------- 4. checks
OK=1
BIN="$APPDIR/usr/bin/$APP"
cfg=$(grep -o -m1 'QGC_APP_VERSION_STR=\\\?"[^"\\]*' "$BUILD/build.ninja" | sed 's/.*"//')
say "version check: $BIN"
say "  configured version str: '$cfg' (build.ninja)"
[ "$cfg" = "$TAG" ] || { say "  MISMATCH: expected '$TAG'"; OK=0; }
if python3 - "$BIN" "$TAG" <<'PY'
import re, sys
data = open(sys.argv[1], 'rb').read()
tag = sys.argv[2].encode()
ok = re.search(rb'(?<![\x20-\x7e])' + re.escape(tag) + rb'\x00', data) is not None
found = sorted(set(m.group(1).decode() for m in re.finditer(rb'(?<![\x20-\x7e])(' + re.escape(tag) + rb'[\x21-\x7e]*)\x00', data)))
print("[linux]   version string in binary: " + (f"'{tag.decode()}' (matches tag)" if ok else f"no exact '{tag.decode()}' (found: {', '.join(found) or 'none'})"))
sys.exit(0 if ok else 1)
PY
then :; else OK=0; fi

# Shipped files: same rules as release.ps1 (G21): no debug-only runtime, no qmltooling,
# notices and license texts present.
mapfile -t FILES < <(cd "$APPDIR" && find . -type f -o -type l | sed 's|^\./||' | sort)
say "shipped-files check: $APPDIR (${#FILES[@]} files)"
hits=0
for f in "${FILES[@]}"; do
    n=${f##*/}
    case "$f" in *qmltooling*) say "  QML DEBUGGER PLUGIN: $f"; hits=$((hits + 1)); continue ;; esac
    # GLib must come from the system: a bundled copy breaks the system libsecret on newer
    # Ubuntu (the Device Key and TAK password keyring), see cmake/CreateAppImage.cmake.
    if [[ "$n" =~ ^lib(glib|gobject|gio|gmodule|gthread)-2\.0\.so ]]; then say "  SYSTEM LIBRARY BUNDLED: $f"; hits=$((hits + 1)); continue; fi
    if [[ "$n" =~ ^libclang_rt\..*\.so ]] || [[ "$n" =~ ^(gdbserver|lldb-server)$ ]] || [[ "$n" =~ ^libQt6.*_debug\.so ]]; then
        say "  DEBUG RUNTIME: $f"; hits=$((hits + 1))
    fi
done
for n in NOTICE COPYING.md LICENSE-GPL LICENSE-APACHE licenses/LGPL-3.0-only.txt licenses/LGPL-2.1.txt licenses/MIT.txt licenses/BSD-3-Clause.txt licenses/qt-third-party/QT-THIRD-PARTY.txt; do
    [ -e "$APPDIR/usr/$n" ] || { say "  MISSING: usr/$n"; hits=$((hits + 1)); }
done
if [ "$hits" -eq 0 ]; then say "SHIPPED FILES CHECK PASS: notices and license texts present, no debug runtime, no qmltooling, GLib from the system"; else say "SHIPPED FILES CHECK FAIL ($hits)"; OK=0; fi
# G108: every bundled library named in NOTICE (with the dpkg version for the
# Ubuntu ones) and its license file in the AppDir. The script of this tag.
if python3 "$SRC/deploy/rotorlab/check-notice.py" --platform linux --dir "$APPDIR" --debs "$DEBS" | sed 's/^/[linux]   /'; then :; else OK=0; fi
# The AppImage must carry what the AppDir holds: list it and compare the count.
inner=$(cd "$WORK" && "$APPIMAGE" --appimage-extract >/dev/null 2>&1 && find squashfs-root -type f -o -type l | wc -l; rm -rf "$WORK/squashfs-root")
say "  AppImage holds $inner files (AppDir: ${#FILES[@]})"

if [ "$OK" -ne 1 ]; then
    if [ "$TESTBUILD" -eq 1 ]; then say "TEST BUILD: checks failed (reported only; this build is never released)"
    else fail "Linux checks failed; the AppImage was not copied"; fi
fi

# --------------------------------------------------------------- 5. boot test
BOOT=$(mktemp -d)
mkdir -p "$BOOT/home" "$BOOT/config" "$BOOT/data" "$BOOT/cache" "$BOOT/runtime"
chmod 700 "$BOOT/runtime"
say "boot test: --simple-boot-test under xvfb, fresh HOME and XDG folders"
set +e
env -i PATH=/usr/bin:/bin HOME="$BOOT/home" XDG_CONFIG_HOME="$BOOT/config" \
    XDG_DATA_HOME="$BOOT/data" XDG_CACHE_HOME="$BOOT/cache" XDG_RUNTIME_DIR="$BOOT/runtime" \
    APPIMAGE_EXTRACT_AND_RUN=1 LANG=C.UTF-8 QT_QPA_PLATFORM=xcb \
    xvfb-run -a timeout 300 "$APPIMAGE" --simple-boot-test >"$WORK/boot-test.log" 2>&1
rc=$?
set -e
tail -5 "$WORK/boot-test.log" | sed 's/^/[linux]   /'
rm -rf "$BOOT"
[ "$rc" -eq 0 ] || fail "boot test exited with $rc (log: $WORK/boot-test.log)"
say "BOOT TEST PASS"

# --------------------------------------------------------------- 6. output
NAME="$APP-$TAG-linux-x86_64.AppImage"
cp "$APPIMAGE" "$OUT/$NAME"
chmod 755 "$OUT/$NAME"
size=$(stat -c %s "$OUT/$NAME")
{
    echo "RotorLab GC Linux release build"
    echo "tag:        $TAG ($ttype)$([ "$DRY" -eq 1 ] && echo '  DRY RUN, not for release')"
    echo "commit:     $head"
    echo "built:      $(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "build host (setup-build-host.sh):"
    sed 's/^/  /' "$TOOLS/versions.txt"
    echo "options:    ${CMAKE_OPTS[*]}"
    echo "linux:      $NAME, $size bytes, version string '$TAG'"
    echo "  linux $HOOK_LINE"
    echo "packaging:  linuxdeploy 1-alpha-20251107-1 (sha256 $LINUXDEPLOY_SHA256), appimagetool 1.9.1 (sha256 $APPIMAGETOOL_SHA256)"
    echo "runtime:    AppImage type2-runtime $RUNTIME_RELEASE ($RUNTIME_COMMIT, runtime-x86_64 sha256 $RUNTIME_SHA256)"
    echo "notices:    $(wc -l <"$DEBS") Ubuntu libraries from $pkgs packages with their copyright files (BUNDLED-DEBS-linux.txt)"
} >"$OUT/BUILD-INFO-linux.txt"
say "=== Done: $OUT/$NAME ($size bytes) ==="
