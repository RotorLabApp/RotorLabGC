#!/usr/bin/env python3
"""RotorLab GC: NOTICE recipe check (G108).

Fails a release build when a library the package bundles is not named in the
NOTICE the package carries, or when the package lacks that library's license
file. One script for all three release files:

  windows  the install (staging) folder release.ps1 packs into the installer,
           or a listing of the installer (7z l); NOTICE and licenses/ at the root
  android  the APK; NOTICE and licenses under assets/licenses/
  linux    the AppDir or the extracted AppImage (squashfs-root); NOTICE and
           licenses under usr/, each bundled Ubuntu library's Debian copyright
           file under usr/share/doc/<package>/copyright

Inputs (one of):
  --dir DIR           a folder: the staging folder, AppDir or squashfs-root
  --apk FILE          an APK (zip); NOTICE is read from it
  --list FILE         a listing, one path per line (for example unzip -Z1 or
                      unsquashfs -l), with --notice FILE for the NOTICE text
Linux only:
  --debs FILE         lib<TAB>package<TAB>version lines written by
                      build-appimage.sh from dpkg in the build container; each
                      package must match the table below and NOTICE must give
                      "<package> <version>"
Other:
  --self-test         proves the check fails on a missing name, a missing
                      license file, an unknown library and a stale version

Bundled libraries are the shipped files that are code: *.dll, *.so*, *.exe,
*.msi, classes*.dex, and everything under a Qt plugins/ or qml/ folder. Each must match a component in COMPONENTS below (else it is unknown:
add it to NOTICE and to this table). Every component that matches, and every
component marked "always" for the platform (code compiled into GC itself),
must have each of its names in NOTICE (whitespace and line breaks ignored) and
each of its license files in the package.

A release ships no development files (G108): a static library
(*.a, *.lib), a CMake package file (*.cmake), a pkg-config file (*.pc) or a
header under include/ fails the check.

Exit 0: pass. Exit 1: fail. Exit 2: usage error. Standard library only.
"""
import argparse
import os
import re
import sys
import zipfile

# --------------------------------------------------------------------- layout

LAYOUT = {
    # notice: path of NOTICE in the package; lic: prefix of the license texts;
    # top: prefix of LICENSE-GPL, LICENSE-APACHE and COPYING.md.
    'windows': {'notice': 'NOTICE', 'lic': 'licenses/', 'top': ''},
    'android': {'notice': 'assets/licenses/NOTICE', 'lic': 'assets/licenses/', 'top': 'assets/licenses/'},
    'linux':   {'notice': 'usr/NOTICE', 'lic': 'usr/licenses/', 'top': 'usr/'},
}

# Files every package carries, relative to 'top' (T:) or 'lic' (L:).
BASE_FILES = ['T:NOTICE', 'T:LICENSE-GPL', 'T:LICENSE-APACHE', 'T:COPYING.md',
              'L:LGPL-3.0-only.txt', 'L:LGPL-2.1.txt', 'L:MIT.txt', 'L:BSD-3-Clause.txt',
              'L:qt-third-party/QT-THIRD-PARTY.txt']

# GC's own code (not a third-party library).
OWN = [r'(^|/)RotorLabGC(\.exe)?$', r'(^|/)libRotorLabGC(_[A-Za-z0-9-]+)?\.so$']

# ----------------------------------------------------------------- components
#
# key: {platforms, match (regexes on the path inside the package), names (each
# must be in NOTICE), files (T:/L: as above, or D:<package> for a Debian
# copyright file), always (required even when no file matches: compiled into
# GC), exempt (why a component has no license file to ship)}.

ALL = ('windows', 'android', 'linux')

# The AppImage runtime release build-appimage.sh pins (RUNTIME_RELEASE there).
RUNTIME_RELEASE = '20251108'

COMPONENTS = {
    'qt': {
        'platforms': ALL,
        'match': [r'(^|/)(lib)?Qt6[A-Za-z0-9]+(_[a-z0-9-]+)?\.(dll|so(\.[0-9.]+)?)$',
                  r'^(usr/)?(plugins|qml)/.*\.(dll|so)$',
                  r'^lib/[^/]+/lib(plugins|qml)_[^/]+\.so$'],
        'names': ['Qt 6.8.3'],
        'files': ['T:LICENSE-GPL', 'L:LGPL-3.0-only.txt', 'L:qt-third-party/QT-THIRD-PARTY.txt'],
        'always': True,
    },
    'qt-ffmpeg-stubs': {
        'platforms': ('android', 'linux'),
        'match': [r'(^|/)libQt6FFmpegStub-[^/]+\.so'],
        'names': ["Qt's FFmpeg stub libraries"],
        'files': ['L:LGPL-3.0-only.txt'],
    },
    'ffmpeg': {
        'platforms': ALL,
        'match': [r'(^|/)(lib)?(avcodec|avformat|avutil|swresample|swscale)([-.][0-9.]+)?\.(dll|so(\.[0-9.]+)?)$'],
        'names': ['FFmpeg 7.1'],
        'files': ['L:LGPL-2.1.txt', 'L:components/FFmpeg-7.1-LICENSE.md.txt'],
        'always': True,
    },
    # Compiled into GC (CPM / submodules); their install rules are off
    # (EXCLUDE_FROM_ALL), so no file of theirs ships on its own.
    'geographiclib': {
        'platforms': ALL, 'always': True, 'match': [],
        'names': ['GeographicLib 2.5'], 'files': ['L:components/GeographicLib-2.5-LICENSE.txt'],
    },
    'libevents': {
        'platforms': ALL, 'always': True, 'match': [],
        'names': ['libevents (MAVLink)'], 'files': ['L:components/libevents-LICENSE.txt'],
    },
    'shapelib': {
        'platforms': ALL, 'always': True, 'match': [],
        'names': ['Shapelib 1.6.1'],
        'files': ['L:components/shapelib-1.6.1-LICENSE-MIT.txt', 'L:components/shapelib-1.6.1-LICENSE-LGPL.txt'],
    },
    'mavlink': {'platforms': ALL, 'always': True, 'match': [],
                'names': ['MAVLink C library v2'], 'files': ['L:components/mavlink-c_library_v2-COPYING.txt']},
    'nlohmann-json': {'platforms': ALL, 'always': True, 'match': [],
                      'names': ['nlohmann/json 3.11.3'], 'files': ['L:components/nlohmann-json-3.11.3-LICENSE.txt']},
    'px4-gps': {'platforms': ALL, 'always': True, 'match': [],
                'names': ['PX4 GPS Drivers'], 'files': ['L:components/px4-gpsdrivers-LICENSE.txt']},
    'ulog_cpp': {'platforms': ALL, 'always': True, 'match': [],
                 'names': ['ulog_cpp (PX4)'], 'files': ['L:components/ulog_cpp-LICENSE.txt']},
    'sdl': {'platforms': ALL, 'always': True, 'match': [],
            'names': ['SDL 2.32.4'],
            'files': ['L:components/SDL2-2.32.4-LICENSE.txt', 'L:components/SDL2-hidapi-LICENSE-bsd.txt',
                      'L:components/SDL2-yuv2rgb-LICENSE.txt']},
    'sdl-gcdb': {'platforms': ALL, 'always': True, 'match': [],
                 'names': ['SDL_GameControllerDB'], 'files': ['L:components/SDL_GameControllerDB-LICENSE.txt']},
    'zlib': {'platforms': ALL, 'always': True, 'match': [],
             'names': ['zlib, zlib License'], 'files': ['L:components/zlib-LICENSE.txt']},
    'xz-embedded': {'platforms': ALL, 'always': True, 'match': [],
                    'names': ['XZ Embedded 2024-12-30'], 'files': ['L:components/xz-embedded-COPYING.txt']},
    'monocypher': {'platforms': ALL, 'always': True, 'match': [],
                   'names': ['Monocypher 4.0.3'], 'files': ['L:components/Monocypher-4.0.3-LICENCE.md.txt']},
    'ardupilot-params': {'platforms': ALL, 'always': True, 'match': [],
                         'names': ['ArduPilot parameter metadata'], 'files': ['T:LICENSE-GPL']},

    # ---- Windows
    'msvc-runtime': {
        'platforms': ('windows',),
        'match': [r'(^|/)(vcruntime140(_1)?|msvcp140(_[0-9a-z_]+)?|concrt140)\.dll$', r'(^|/)vc_redist\.x64\.exe$'],
        'names': ['Microsoft Visual C++ runtime 14.44.35211'], 'files': [],
        'exempt': "Microsoft's redistribution terms (Visual Studio 2022 Distributable Code); no license text ships",
    },
    'directx-compilers': {
        'platforms': ('windows',),
        'match': [r'(^|/)(d3dcompiler_47|dxcompiler|dxil)\.dll$'],
        'names': ['d3dcompiler_47.dll', 'dxcompiler.dll', 'dxil.dll'], 'files': [],
        'exempt': "Microsoft's redistribution terms; no license text ships",
    },
    'mesa-opengl32sw': {
        'platforms': ('windows',),
        'match': [r'(^|/)opengl32sw\.dll$'],
        'names': ['Mesa software OpenGL renderer'], 'files': ['L:MIT.txt'],
    },
    'ardupilot-driver': {
        'platforms': ('windows',),
        'match': [r'(^|/)driver\.msi$'],
        'names': ['ArduPilot USB driver package'], 'files': [],
        'exempt': "ArduPilot's driver installer, offered by the GC installer; its terms ship inside driver.msi",
    },
    'nsis': {
        'platforms': ('windows',),
        'match': [r'^\$PLUGINSDIR/'],
        'names': ['NSIS 3'], 'files': ['L:Zlib.txt'],
    },

    # ---- Android
    'openssl': {
        'platforms': ('android',),
        'match': [r'(^|/)lib(ssl|crypto)_3\.so$'],
        'names': ['OpenSSL 3.1.8'],
        'files': ['L:components/OpenSSL-3.1.8-LICENSE.txt', 'L:components/android_openssl-LICENSE.txt'],
    },
    'libcxx': {
        'platforms': ('android',),
        'match': [r'(^|/)libc\+\+_shared\.so$'],
        'names': ['LLVM libc++'], 'files': ['L:Apache-2.0-WITH-LLVM-exception.txt'],
    },
    'android-java': {
        'platforms': ('android',),
        'match': [r'^classes[0-9]*\.dex$'],
        'names': ['usb-serial-for-android 3.8.1', 'AndroidX Core 1.13.1', 'Kotlin standard library',
                  'kotlinx.coroutines 1.6.4'],
        'files': ['L:components/usb-serial-for-android-3.8.1-LICENSE.txt', 'L:components/AndroidX-LICENSE.txt',
                  'L:components/Kotlin-LICENSE.txt', 'L:Apache-2.0.txt'],
    },

    # ---- Linux
    'icu': {
        'platforms': ('linux',),
        'match': [r'(^|/)libicu(uc|i18n|data)\.so(\.[0-9.]+)?$'],
        'names': ['ICU 73'], 'files': ['L:Unicode-3.0.txt'],
    },
    'appimage-runtime': {
        'platforms': ('linux',), 'always': True, 'match': [],
        'names': ['AppImage type2-runtime ' + RUNTIME_RELEASE, 'libfuse', 'squashfuse', 'musl libc'],
        # The runtime's own LICENSE from its pinned release, added by build-appimage.sh.
        'files': ['L:components/AppImage-type2-runtime-%s-LICENSE.txt' % RUNTIME_RELEASE,
                  'L:MIT.txt', 'L:LGPL-2.1.txt', 'L:BSD-2-Clause.txt', 'L:Zlib.txt'],
    },
}

# Linux AppImage: each bundled Ubuntu 22.04 library, by file name, and its
# Debian binary package (dpkg -S in the Ubuntu 22.04 build image). NOTICE
# must name the package; with --debs, also "<package> <version>"; the AppImage
# must carry usr/share/doc/<package>/copyright.
UBUNTU_LIBS = {
    'libFLAC.so.8': 'libflac8',
    'libXau.so.6': 'libxau6',
    'libXdmcp.so.6': 'libxdmcp6',
    'libapparmor.so.1': 'libapparmor1',
    'libasyncns.so.0': 'libasyncns0',
    'libbrotlicommon.so.1': 'libbrotli1',
    'libbrotlidec.so.1': 'libbrotli1',
    'libbsd.so.0': 'libbsd0',
    'libbz2.so.1': 'libbz2-1.0',
    'libbz2.so.1.0': 'libbz2-1.0',
    'libcap.so.2': 'libcap2',
    'libdbus-1.so.3': 'libdbus-1-3',
    'libffi.so.8': 'libffi8',
    'libgcrypt.so.20': 'libgcrypt20',
    'libgssapi_krb5.so.2': 'libgssapi-krb5-2',
    'libk5crypto.so.3': 'libk5crypto3',
    'libkeyutils.so.1': 'libkeyutils1',
    'libkrb5.so.3': 'libkrb5-3',
    'libkrb5support.so.0': 'libkrb5support0',
    'liblz4.so.1': 'liblz4-1',
    'liblzma.so.5': 'liblzma5',
    'libmd.so.0': 'libmd0',
    'libogg.so.0': 'libogg0',
    'libopus.so.0': 'libopus0',
    'libpcre.so.3': 'libpcre3',
    'libpng16.so.16': 'libpng16-16',
    'libpulse.so.0': 'libpulse0',
    'libpulsecommon-15.99.so': 'libpulse0',
    'libsndfile.so.1': 'libsndfile1',
    'libsystemd.so.0': 'libsystemd0',
    'libvorbis.so.0': 'libvorbis0a',
    'libvorbisenc.so.2': 'libvorbisenc2',
    'libwayland-cursor.so.0': 'libwayland-cursor0',
    'libwayland-egl.so.1': 'libwayland-egl1',
    'libwayland-server.so.0': 'libwayland-server0',
    'libxcb-cursor.so.0': 'libxcb-cursor0',
    'libxcb-icccm.so.4': 'libxcb-icccm4',
    'libxcb-image.so.0': 'libxcb-image0',
    'libxcb-keysyms.so.1': 'libxcb-keysyms1',
    'libxcb-randr.so.0': 'libxcb-randr0',
    'libxcb-render-util.so.0': 'libxcb-render-util0',
    'libxcb-render.so.0': 'libxcb-render0',
    'libxcb-shape.so.0': 'libxcb-shape0',
    'libxcb-shm.so.0': 'libxcb-shm0',
    'libxcb-sync.so.1': 'libxcb-sync1',
    'libxcb-util.so.1': 'libxcb-util1',
    'libxcb-xfixes.so.0': 'libxcb-xfixes0',
    'libxcb-xkb.so.1': 'libxcb-xkb1',
    'libxkbcommon-x11.so.0': 'libxkbcommon-x11-0',
    'libxkbcommon.so.0': 'libxkbcommon0',
    'libzstd.so.1': 'libzstd1',
}

def norm_ws(text):
    return re.sub(r'\s+', ' ', text)


DEV_RX = [(r'\.(a|lib)$', 'static library'), (r'\.cmake$', 'CMake package file'),
          (r'\.pc$', 'pkg-config file'), (r'^(usr/)?include/', 'header')]


def dev_kind(path):
    for rx, why in DEV_RX:
        if re.search(rx, path, re.IGNORECASE):
            return why
    return None


def is_code(path):
    base = path.rsplit('/', 1)[-1]
    if re.match(r'^classes[0-9]*\.dex$', base):
        return True
    if path.startswith('$PLUGINSDIR/') and base.lower().endswith('.dll'):
        return True
    return bool(re.search(r'\.(dll|exe|msi)$', base, re.IGNORECASE) or re.search(r'\.so(\.[0-9][0-9.]*)?$', base))


def resolve(spec, lay):
    kind, rest = spec.split(':', 1)
    if kind == 'T':
        return lay['top'] + rest
    if kind == 'L':
        return lay['lic'] + rest
    if kind == 'D':
        return 'usr/share/doc/%s/copyright' % rest
    raise ValueError(spec)


def check(platform, paths, notice_text, debs=None, out=print):
    """Returns the list of failures (empty: pass). paths: package-relative, '/'-separated."""
    lay = LAYOUT[platform]
    paths = sorted({p.replace('\\', '/').lstrip('/') for p in paths if p and not p.endswith('/')})
    present = set(paths)
    notice = norm_ws(notice_text or '')
    fails = []

    def need_name(name, why):
        if norm_ws(name) not in notice:
            fails.append('NOT IN NOTICE: "%s" (%s)' % (name, why))

    def need_file(rel, why):
        if rel not in present:
            fails.append('NO LICENSE FILE: %s (%s)' % (rel, why))

    if not notice.strip():
        fails.append('NO NOTICE TEXT: %s' % lay['notice'])
    for spec in BASE_FILES:
        need_file(resolve(spec, lay), 'every release file carries it')

    comps = {k: c for k, c in COMPONENTS.items() if platform in c['platforms']}
    used = {}
    unknown = []
    debs_seen = {}
    dev = [(p, dev_kind(p)) for p in paths if dev_kind(p)]
    for p, why in dev:
        fails.append('DEV FILE SHIPPED: %s (%s; a release ships no development files)' % (p, why))
    libs = [p for p in paths if is_code(p) and not any(re.search(rx, p) for rx in OWN)]
    for p in libs:
        base = p.rsplit('/', 1)[-1]
        hit = None
        if platform == 'linux' and p.startswith('usr/lib/') and p.count('/') == 2 and base in UBUNTU_LIBS:
            hit = 'deb:' + UBUNTU_LIBS[base]
        if hit is None:
            for k, c in comps.items():
                if any(re.search(rx, p) for rx in c['match']):
                    hit = k
                    break
        if hit is None:
            unknown.append(p)
            continue
        used.setdefault(hit, []).append(p)

    for p in unknown:
        fails.append('UNKNOWN LIBRARY: %s (not in NOTICE or check-notice.py: name it, with version and '
                     'license, in NOTICE and add it to the table)' % p)

    keys = [k for k, c in comps.items() if c.get('always')] + [k for k in used if not k.startswith('deb:')]
    for k in sorted(set(keys)):
        c = comps[k]
        why = k + (' (%d files)' % len(used[k]) if k in used else ' (compiled in)')
        for n in c['names']:
            need_name(n, why)
        for f in c['files']:
            need_file(resolve(f, lay), why)
        if not c['files'] and not c.get('exempt'):
            fails.append('NO LICENSE FILE: component %s lists none and no exemption' % k)

    deb_pkgs = sorted({k[4:] for k in used if k.startswith('deb:')})
    if debs is not None:
        for lib, (pkg, ver) in debs.items():
            want = UBUNTU_LIBS.get(lib)
            if want is None:
                fails.append('UNKNOWN LIBRARY: usr/lib/%s is Debian package %s %s, not in check-notice.py' % (lib, pkg, ver))
            elif want != pkg:
                fails.append('TABLE OUT OF DATE: %s is in %s (dpkg), the table says %s' % (lib, pkg, want))
            debs_seen[pkg] = ver
    for pkg in deb_pkgs:
        why = 'Ubuntu 22.04 library: ' + ', '.join(x.rsplit('/', 1)[-1] for x in used['deb:' + pkg])
        need_name(pkg, why)
        if pkg in debs_seen:
            need_name('%s %s' % (pkg, debs_seen[pkg]), why + ', version from dpkg')
        need_file(resolve('D:' + pkg, lay), why)

    out('notice check (%s): %d files, %d bundled libraries, %d components, %d Ubuntu packages, %d development files'
        % (platform, len(paths), len(libs), len(set(keys)), len(deb_pkgs), len(dev)))
    for k in sorted(set(keys)):
        c = comps[k]
        tag = ('%d files' % len(used[k])) if k in used else 'compiled in'
        extra = ('; exempt: ' + c['exempt']) if c.get('exempt') else ''
        out('  %-18s %-12s %s%s' % (k, tag, ' | '.join(c['names']), extra))
    for pkg in deb_pkgs:
        out('  %-18s %-12s %s' % ('ubuntu', pkg, debs_seen.get(pkg, '(version not checked: no --debs)')))
    for f in fails:
        out('  FAIL: ' + f)
    out('NOTICE CHECK PASS' if not fails else 'NOTICE CHECK FAIL (%d)' % len(fails))
    return fails


# ----------------------------------------------------------------- inputs

def from_dir(d):
    root = os.path.abspath(d)
    paths = []
    for dp, dns, fns in os.walk(root):
        for n in fns + [x for x in dns if os.path.islink(os.path.join(dp, x))]:
            paths.append(os.path.relpath(os.path.join(dp, n), root).replace(os.sep, '/'))
    return paths


def read_debs(path):
    debs = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            parts = line.rstrip('\n').split('\t')
            if len(parts) == 3 and parts[0]:
                debs[parts[0]] = (parts[1], parts[2])
    return debs


def self_test():
    """Builds a passing package per platform, then breaks it four ways; each must fail."""
    ok = True
    sink = []

    def quiet(s):
        sink.append(s)

    def full_notice(platform):
        names = []
        for c in COMPONENTS.values():
            if platform in c['platforms']:
                names += c['names']
        if platform == 'linux':
            names += ['%s 1.0-1' % p for p in sorted(set(UBUNTU_LIBS.values()))]
        return '\n'.join(names)

    def package(platform):
        lay = LAYOUT[platform]
        files = [resolve(s, lay) for s in BASE_FILES]
        for c in COMPONENTS.values():
            if platform in c['platforms']:
                files += [resolve(f, lay) for f in c['files']]
        if platform == 'windows':
            files += ['bin/RotorLabGC.exe', 'bin/Qt6Core.dll', 'bin/avcodec-61.dll', 'bin/vcruntime140.dll',
                      'bin/opengl32sw.dll', 'bin/dxil.dll', 'plugins/platforms/qwindows.dll', 'driver.msi',
                      '$PLUGINSDIR/System.dll']
        elif platform == 'android':
            files += ['lib/arm64-v8a/libRotorLabGC_arm64-v8a.so', 'lib/arm64-v8a/libQt6Core_arm64-v8a.so',
                      'lib/arm64-v8a/libc++_shared.so', 'lib/arm64-v8a/libssl_3.so', 'lib/arm64-v8a/libavutil.so',
                      'lib/arm64-v8a/libQt6FFmpegStub-ssl_arm64-v8a.so',
                      'lib/arm64-v8a/libplugins_platforms_qtforandroid_arm64-v8a.so', 'classes.dex']
        else:
            files += ['usr/bin/RotorLabGC', 'usr/lib/libQt6Core.so.6', 'usr/lib/libicuuc.so.73',
                      'usr/lib/libavutil.so.59', 'usr/lib/libQt6FFmpegStub-va.so.2', 'usr/plugins/platforms/libqxcb.so',
                      'usr/lib/libQt6Gui.so.6.8.3']
            for lib, pkg in UBUNTU_LIBS.items():
                files += ['usr/lib/' + lib, 'usr/share/doc/%s/copyright' % pkg]
        return files

    for platform in ALL:
        files, notice = package(platform), full_notice(platform)
        debs = {lib: (pkg, '1.0-1') for lib, pkg in UBUNTU_LIBS.items()} if platform == 'linux' else None
        base = check(platform, files, notice, debs, quiet)
        cases = [('passes when complete', not base)]
        # 1. a library's name missing from NOTICE
        victim = {'windows': 'FFmpeg 7.1', 'android': 'OpenSSL 3.1.8', 'linux': 'libpcre3'}[platform]
        broken = notice.replace(victim, 'removed')
        if platform == 'linux':
            broken = re.sub(r'(?m)^libpcre3 .*$', 'removed', notice)
        cases.append(('fails on a missing name (%s)' % victim,
                      any(victim in f and 'NOT IN NOTICE' in f for f in check(platform, files, broken, debs, quiet))))
        # 2. a license file missing
        lay = LAYOUT[platform]
        gone = {'windows': lay['lic'] + 'components/FFmpeg-7.1-LICENSE.md.txt',
                'android': lay['lic'] + 'components/OpenSSL-3.1.8-LICENSE.txt',
                'linux': 'usr/share/doc/libdbus-1-3/copyright'}[platform]
        cases.append(('fails on a missing license file (%s)' % gone,
                      any(gone in f for f in check(platform, [f for f in files if f != gone], notice, debs, quiet))))
        # 3. a library nobody listed
        extra = {'windows': 'bin/libfoo.dll', 'android': 'lib/arm64-v8a/libfoo.so', 'linux': 'usr/lib/libfoo.so.1'}[platform]
        cases.append(('fails on an unknown library (%s)' % extra,
                      any('UNKNOWN LIBRARY' in f and 'libfoo' in f for f in check(platform, files + [extra], notice, debs, quiet))))
        # 4. development files: a static library and a CMake package file
        for devfile in {'windows': ['lib/shp.lib', 'share/shapelib/shapelib-config.cmake'],
                        'android': ['lib/arm64-v8a/libshp.a', 'cmake/libeventsConfig.cmake'],
                        'linux': ['usr/lib/libGeographicLib.a', 'usr/lib/cmake/libevents/libeventsConfig.cmake']}[platform]:
            cases.append(('fails on a development file (%s)' % devfile,
                          any('DEV FILE SHIPPED' in f and devfile in f
                              for f in check(platform, files + [devfile], notice, debs, quiet))))
        # 5. Linux: NOTICE gives another version than dpkg
        if platform == 'linux':
            newer = dict(debs)
            newer['libsystemd.so.0'] = ('libsystemd0', '1.0-2')
            cases.append(('fails on a stale version (libsystemd0 1.0-2)',
                          any('libsystemd0 1.0-2' in f for f in check(platform, files, notice, newer, quiet))))
        for what, good in cases:
            print('self-test %-8s %-55s %s' % (platform, what, 'ok' if good else 'WRONG'))
            ok = ok and good
    print('SELF-TEST PASS' if ok else 'SELF-TEST FAIL')
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--platform', choices=ALL)
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--dir')
    g.add_argument('--apk')
    g.add_argument('--list')
    ap.add_argument('--notice', help='NOTICE text file (default: the one in the package)')
    ap.add_argument('--debs')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()

    if a.self_test:
        return 0 if self_test() else 1
    if not a.platform or not (a.dir or a.apk or a.list):
        ap.error('--platform and one of --dir, --apk or --list are required (or --self-test)')
    lay = LAYOUT[a.platform]
    notice = None
    if a.dir:
        paths = from_dir(a.dir)
        src = os.path.join(a.dir, *lay['notice'].split('/'))
        if not a.notice and os.path.isfile(src):
            with open(src, encoding='utf-8', errors='replace') as f:
                notice = f.read()
    elif a.apk:
        with zipfile.ZipFile(a.apk) as z:
            paths = z.namelist()
            if not a.notice and lay['notice'] in paths:
                notice = z.read(lay['notice']).decode('utf-8', 'replace')
    else:
        with open(a.list, encoding='utf-8', errors='replace') as f:
            paths = [line.strip() for line in f]
    if a.notice:
        with open(a.notice, encoding='utf-8', errors='replace') as f:
            notice = f.read()
    debs = read_debs(a.debs) if a.debs else None
    print('input: %s; NOTICE: %s' % (a.dir or a.apk or a.list, a.notice or lay['notice']))
    return 1 if check(a.platform, paths, notice, debs) else 0


if __name__ == '__main__':
    sys.exit(main())
