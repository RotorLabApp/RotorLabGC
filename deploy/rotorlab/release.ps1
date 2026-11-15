<#
.SYNOPSIS
  RotorLab GC release build (Windows installer and Android arm64-v8a APK) from a git tag.

.DESCRIPTION
  release.ps1 builds the RotorLab GC release artifacts from one tag, always from
  scratch:

    1. Checks that -Tag exists, that HEAD is that tag's commit, that the work
       tree is clean (no tracked changes, no untracked files) and that every
       submodule is checked out at its recorded commit. A release tag must be
       an annotated tag named vX.Y.Z: cmake/Git.cmake takes the numeric
       project version from "git describe --abbrev=0", which only sees
       annotated tags, and the version string from "git describe --tags".
       Both must name the tag, or the script stops before configuring.
    2. Configures FRESH into a new build folder per platform (the folder must
       not exist; it is never reused, and ccache is off), so nothing from an
       earlier configure or build reaches the release.
    3. Windows: Release build with Qt 6.8.3 msvc2022_64, MSVC 2022 (vcvars64)
       and Ninja, then "cmake --install", which runs cmake/CreateWinInstaller.cmake
       (NSIS, deploy/windows/nullsoft_installer.nsi) and writes
       RotorLabGC-installer.exe in the build folder.
    4. Android: arm64-v8a Release APK with Qt's android_arm64_v8a kit. Stops
       with a message if the kit is not installed; it never installs anything.
    5. Reads the version off what was built and fails if it does not match the
       tag: the version string compiled into the binary (QGC_APP_VERSION_STR,
       shown in the app) and, for a vX.Y.Z tag, the numeric version (Windows
       file version resource, APK versionName). The Windows exe must carry a
       version resource (ProductVersion = tag, FileVersion = tag numbers .0).
    6. Copies the artifacts to the output folder and PRINTS the signing step.
       It signs nothing, uploads nothing, pushes nothing and creates no tags.

  CMake options are stated below in $CommonOptions; every shipped platform
  uses them.

.PARAMETER Tag
  The release tag, vX.Y.Z (annotated). With -DryRun any existing tag is
  accepted (for example a temporary local tag), and the numeric version check
  is reported but not enforced.

.PARAMETER Platform
  Windows, Android, or both (default).

.PARAMETER WorkDir
  Folder for this run's build trees and output. Default:
  <repo>/build-release/<Tag>-<yyyyMMdd-HHmmss> (ignored by git: build*/).
  Must not exist.

.PARAMETER OutDir
  Artifact folder. Default: <WorkDir>/artifacts. Must not exist or be empty.

.PARAMETER QtRoot
  Qt 6.8.3 install folder that holds msvc2022_64 and android_arm64_v8a.
  Default: C:/Qt/6.8.3.

.PARAMETER VsRoot
  Visual Studio 2022 install folder (vcvars64.bat is found under it).
  Default: the Community edition's standard folder.

.PARAMETER NinjaDir
  Folder that holds ninja.exe. Default: the folder of the first ninja.exe on PATH.

.PARAMETER CpmCache
  CPM.cmake source cache for the third-party dependencies. Default:
  $env:CPM_SOURCE_CACHE, else %LOCALAPPDATA%/RotorLabGC/cpm-cache.

.PARAMETER AndroidSdk
  Android SDK folder. Default: $env:ANDROID_SDK_ROOT, else %LOCALAPPDATA%/Android/Sdk.

.PARAMETER AndroidNdk
  Android NDK folder. Default: $env:ANDROID_NDK_ROOT, else <AndroidSdk>/ndk/26.1.10909125.

.PARAMETER JavaHome
  JDK 17 folder for the Android build. Default: $env:JAVA_HOME.

.PARAMETER VerifyExe
  Only run the version check of an already built Windows exe (or Android .so)
  against -Tag and exit. No git checks, no build. Used to test the check.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File deploy/rotorlab/release.ps1 -Tag v1.0.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Tag,
    [ValidateSet('Windows', 'Android')] [string[]] $Platform = @('Windows', 'Android'),
    [string] $SourceDir,
    [string] $WorkDir,
    [string] $OutDir,
    [string] $QtRoot        = 'C:/Qt/6.8.3',
    [string] $VsRoot        = 'C:/Program Files/Microsoft Visual Studio/2022/Community',
    [string] $NinjaDir,
    [string] $CpmCache,
    [string] $AndroidSdk,
    [string] $AndroidNdk,
    [string] $JavaHome      = $env:JAVA_HOME,
    [switch] $DryRun,
    [string] $VerifyExe
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

# PowerShell 5.1 does not set $PSScriptRoot while binding parameter defaults.
if (-not $SourceDir) { $SourceDir = Split-Path (Split-Path (Split-Path -Parent $MyInvocation.MyCommand.Path)) }

# Machine-specific locations. Pass them as parameters, or let these defaults
# find them from the environment and PATH.
if (-not $NinjaDir) {
    $ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($ninja) { $NinjaDir = Split-Path -Parent $ninja.Source }
}
if (-not $CpmCache) {
    $CpmCache = if ($env:CPM_SOURCE_CACHE) { $env:CPM_SOURCE_CACHE } else { Join-Path $env:LOCALAPPDATA 'RotorLabGC/cpm-cache' }
}
if (-not $AndroidSdk) {
    $AndroidSdk = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
}
if (-not $AndroidNdk) {
    $AndroidNdk = if ($env:ANDROID_NDK_ROOT) { $env:ANDROID_NDK_ROOT } else { Join-Path $AndroidSdk 'ndk/26.1.10909125' }
}

$AppName      = 'RotorLabGC'
$QtVersion    = '6.8.3'
$AndroidAbi   = 'arm64-v8a'
$QtDesktopKit = Join-Path $QtRoot 'msvc2022_64'
$QtAndroidKit = Join-Path $QtRoot 'android_arm64_v8a'

# The release CMake options. Every shipped platform builds with exactly these.
$CommonOptions = @(
    '-DCMAKE_BUILD_TYPE=Release',
    '-DQGC_STABLE_BUILD=ON',                 # stable build: compiles out the dev-only server address overrides (QGC_DAILY_BUILD)
    '-DQGC_ENABLE_GST_VIDEOSTREAMING=OFF',
    '-DQGC_ENABLE_QT_VIDEOSTREAMING=ON',
    '-DQGC_ENABLE_HERELINK=OFF',
    '-DQGC_AIRLINK_DISABLED=ON',
    '-DQGC_ZEROCONF_ENABLED=OFF',
    '-DQGC_UTM_ADAPTER=OFF',
    '-DQGC_BUILD_TESTING=OFF',
    '-DQGC_USE_CACHE=OFF',                   # no ccache: a release is never built from cached objects
    "-DQGC_CPM_SOURCE_CACHE=$CpmCache",
    "-DCPM_SOURCE_CACHE=$CpmCache"
)

function Say([string] $t) { Write-Host "[release] $t" }
function Fail([string] $t) { throw "release.ps1: $t" }

function Invoke-Native {
    param([string] $Exe, [string[]] $Arguments, [string] $WorkingDir)
    Say ("> " + $Exe + ' ' + ($Arguments -join ' '))
    if ($WorkingDir) { Push-Location $WorkingDir }
    try {
        & $Exe @Arguments
        $code = $LASTEXITCODE
    } finally {
        if ($WorkingDir) { Pop-Location }
    }
    if ($code -ne 0) { Fail "$Exe exited with code $code" }
}

function Invoke-Git([string[]] $Arguments) {
    $out = & git.exe -C $SourceDir @Arguments
    if ($LASTEXITCODE -ne 0) { Fail ("git " + ($Arguments -join ' ') + " failed ($LASTEXITCODE)") }
    return (($out | Out-String).Trim())
}

# ---------------------------------------------------------------- version check

# Returns $true when $Bytes holds $Text as a whole NUL-terminated C string
# (not a prefix of a longer string such as "v1.2.3-4-gabcdef0").
function Test-EmbeddedString([string] $Path, [string] $Text) {
    $latin1 = [System.Text.Encoding]::GetEncoding(28591)
    $data   = $latin1.GetString([System.IO.File]::ReadAllBytes($Path))
    $rx     = '(?<![\x20-\x7E])' + [regex]::Escape($Text) + '\x00'
    return [regex]::IsMatch($data, $rx)
}

# Finds the describe-style version strings (tag, or tag-N-gHASH) for display.
function Get-EmbeddedVersionStrings([string] $Path, [string] $Text) {
    $latin1 = [System.Text.Encoding]::GetEncoding(28591)
    $data   = $latin1.GetString([System.IO.File]::ReadAllBytes($Path))
    $rx     = '(?<![\x20-\x7E])(' + [regex]::Escape($Text) + '[\x21-\x7E]*)\x00'
    return @([regex]::Matches($data, $rx) | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
}

function Get-TagNumeric([string] $T) {
    if ($T -match '^v(\d+)\.(\d+)\.(\d+)$') { return @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3]) }
    return $null
}

# The leading vX.Y.Z of a tag, also of a describe-style one (v5.0.9-107-gabcdef0 gives 5,0,9).
function Get-TagNumericPrefix([string] $T) {
    if ($T -match '^v(\d+)\.(\d+)\.(\d+)(?:$|[^0-9.])') { return @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3]) }
    return $null
}

# Windows: checks the version string compiled into the exe and its VERSIONINFO resource
# (deploy/windows/QGroundControl.rc.in, A177). The resource is REQUIRED: ProductVersion must
# equal the tag and FileVersion must start with the tag's numbers (5.1.0 -> 5.1.0.0). Only a
# dry run with a tag that is not vX.Y.Z reports the resource without failing on it.
function Test-WindowsExeVersion([string] $Exe, [string] $T, [bool] $Enforce) {
    if (-not (Test-Path $Exe)) { Fail "exe not found: $Exe" }
    $ok = $true
    $strOk = Test-EmbeddedString $Exe $T
    $found = Get-EmbeddedVersionStrings $Exe $T
    $vi = (Get-Item $Exe).VersionInfo
    $fileVer = '{0}.{1}.{2}.{3}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart
    $prodVerStr = if ($vi.ProductVersion) { $vi.ProductVersion } else { '' }
    $fileVerStr = if ($vi.FileVersion) { $vi.FileVersion } else { '' }
    $resEnforce = $Enforce -or [bool](Get-TagNumeric $T)
    Say "version check: $Exe"
    Say ("  version string in exe : " + $(if ($strOk) { "'$T' (matches tag)" } else { "no exact '$T' (found: " + ($(if ($found) { $found -join ', ' } else { 'none' })) + ')' }))
    if (-not $strOk) { $ok = $false }
    $resOk = $true
    $hasResource = (($vi.FileMajorPart + $vi.FileMinorPart + $vi.FileBuildPart + $vi.FilePrivatePart) -gt 0) -or $prodVerStr -or $fileVerStr
    if (-not $hasResource) {
        Say "  version resource      : NONE (the exe has no VERSIONINFO; deploy/windows/QGroundControl.rc.in is not in the build)"
        $resOk = $false
    } else {
        Say "  version resource      : FileVersion $fileVer ('$fileVerStr'), ProductVersion '$prodVerStr'"
        if ($prodVerStr -ceq $T) {
            Say "  ProductVersion matches tag ('$T')"
        } else {
            Say "  ProductVersion MISMATCH: expected '$T', got '$prodVerStr'"
            $resOk = $false
        }
        $n = Get-TagNumericPrefix $T
        if ($n) {
            $want = '{0}.{1}.{2}' -f $n[0], $n[1], $n[2]
            $partsOk = (('{0}.{1}.{2}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart) -eq $want)
            $strVerOk = ($fileVerStr -eq $want) -or $fileVerStr.StartsWith("$want.")
            if ($partsOk -and $strVerOk) {
                Say "  FileVersion starts with tag numbers ($want)"
            } else {
                Say "  FileVersion MISMATCH: expected $want.x, got $fileVer ('$fileVerStr')"
                $resOk = $false
            }
        } else {
            Say "  FileVersion not compared: '$T' has no vX.Y.Z numbers"
            $resOk = $false
        }
    }
    if (-not $resOk) {
        if ($resEnforce) { $ok = $false } else { Say "  version resource not enforced: '$T' is not a vX.Y.Z tag (dry run)" }
    }
    if ($ok) { Say "VERSION CHECK PASS: $T" } else { Say "VERSION CHECK FAIL: built exe does not match $T" }
    return $ok
}

# The QGC_APP_VERSION_STR the compiler was given (build.ninja of the fresh tree).
function Test-ConfiguredVersion([string] $BuildDir, [string] $T) {
    $hit = Select-String -Path (Join-Path $BuildDir 'build.ninja') -Pattern 'QGC_APP_VERSION_STR=\\?"([^"\\]*)\\?"' -List
    $v = if ($hit) { $hit.Matches[0].Groups[1].Value } else { '' }
    Say "  configured version str: '$v' (build.ninja)"
    return ($v -eq $T)
}

if ($VerifyExe) {
    if (Test-WindowsExeVersion $VerifyExe $Tag (-not $DryRun)) { exit 0 } else { exit 1 }
}

# ---------------------------------------------------------------- preconditions

$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$WinOk = $true

try {
    if ($DryRun) {
        if ($Tag -notmatch '^[A-Za-z0-9._-]+$') { Fail "tag name '$Tag' has characters this script does not accept" }
        Say "DRY RUN: artifacts are for testing only; do not sign or publish them."
    } else {
        if (-not (Get-TagNumeric $Tag)) { Fail "-Tag must be vX.Y.Z (got '$Tag'); use -DryRun for a test tag" }
        $n = Get-TagNumeric $Tag
        if ($n[0] -gt 9 -or $n[1] -gt 9 -or $n[2] -gt 99) { Fail "$Tag is outside what the Android version code allows (major and minor up to 9, patch up to 99)" }
    }

    if (-not (Test-Path (Join-Path $SourceDir 'CMakeLists.txt'))) { Fail "no CMakeLists.txt in $SourceDir" }
    $SourceDir = (Resolve-Path $SourceDir).Path

    & git.exe -C $SourceDir rev-parse -q --verify "refs/tags/$Tag" | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "tag '$Tag' does not exist in $SourceDir" }
    $tagCommit  = Invoke-Git @('rev-parse', "refs/tags/$Tag^{commit}")
    $tagType    = Invoke-Git @('cat-file', '-t', "refs/tags/$Tag")
    $headCommit = Invoke-Git @('rev-parse', 'HEAD')
    if ($headCommit -ne $tagCommit) { Fail "HEAD is $headCommit, not $Tag ($tagCommit). Check out the tag first: git checkout $Tag" }
    if (-not $DryRun -and $tagType -ne 'tag') { Fail "$Tag is a lightweight tag. cmake/Git.cmake reads the numeric version from annotated tags only; make it with git tag -a" }

    $dirty = Invoke-Git @('status', '--porcelain', '--untracked-files=all', '--ignore-submodules=none')
    if ($dirty) { Fail "work tree is not clean at ${Tag}:`n$dirty" }
    # Not through Invoke-Git: its Trim() would drop the status column (' ', '+', '-', 'U').
    $sub = @(& git.exe -C $SourceDir submodule status --recursive)
    if ($LASTEXITCODE -ne 0) { Fail "git submodule status failed ($LASTEXITCODE)" }
    foreach ($line in $sub) {
        if ($line -and $line[0] -ne ' ') { Fail "submodule not checked out at its recorded commit: $line (run git submodule update --init --recursive)" }
    }

    # What cmake/Git.cmake will compute at configure time.
    $descTags = Invoke-Git @('describe', '--always', '--tags')
    $descAnn  = Invoke-Git @('describe', '--always', '--abbrev=0')
    Say "tag $Tag ($tagType) at $tagCommit; describe --tags: $descTags; describe --abbrev=0: $descAnn"
    if ($descTags -ne $Tag) { Fail "git describe --tags gives '$descTags', not '$Tag' (another tag on the same commit?). The app's version string would not be the tag." }
    if (-not $DryRun -and $descAnn -ne $Tag) { Fail "git describe --abbrev=0 gives '$descAnn', not '$Tag'. The numeric project version would not be the tag." }

    if (-not $WorkDir) { $WorkDir = Join-Path $SourceDir "build-release/$Tag-$Stamp" }
    if (Test-Path $WorkDir) { Fail "work folder already exists: $WorkDir (release builds are never incremental)" }
    if (-not $OutDir) { $OutDir = Join-Path $WorkDir 'artifacts' }
    if ((Test-Path $OutDir) -and (Get-ChildItem $OutDir -Force | Select-Object -First 1)) { Fail "output folder is not empty: $OutDir" }
    New-Item -ItemType Directory -Force -Path $WorkDir, $OutDir | Out-Null
    $WorkDir = (Resolve-Path $WorkDir).Path
    $OutDir  = (Resolve-Path $OutDir).Path
    if (-not (Test-Path $CpmCache)) { New-Item -ItemType Directory -Force -Path $CpmCache | Out-Null }

    $info = New-Object System.Collections.Generic.List[string]
    $info.Add("RotorLab GC release build")
    $info.Add("tag:        $Tag ($tagType)$(if ($DryRun) { '  DRY RUN, not for release' })")
    $info.Add("commit:     $tagCommit")
    $info.Add("built:      $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')")
    $info.Add("qt:         $QtVersion")
    $info.Add("options:    " + ($CommonOptions -join ' '))

    $artifacts = New-Object System.Collections.Generic.List[string]

    # ------------------------------------------------------------ Windows
    if ($Platform -contains 'Windows') {
        Say "=== Windows x64 Release ==="
        $vcvars = Join-Path $VsRoot 'VC/Auxiliary/Build/vcvars64.bat'
        foreach ($p in @($vcvars, (Join-Path $QtDesktopKit 'bin/qt-cmake.bat'), (Join-Path $NinjaDir 'ninja.exe'))) {
            if (-not (Test-Path $p)) { Fail "missing: $p" }
        }

        $savedEnv = @{}
        Get-ChildItem env: | ForEach-Object { $savedEnv[$_.Name] = $_.Value }
        try {
            Say "loading MSVC environment: $vcvars"
            $envDump = & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
            if ($LASTEXITCODE -ne 0) { Fail "vcvars64.bat failed ($LASTEXITCODE)" }
            foreach ($l in $envDump) {
                if ($l -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
            }
            $env:PATH = "$NinjaDir;$env:PATH"

            $bw = Join-Path $WorkDir 'windows'
            $staging = Join-Path $bw 'staging'
            $winArgs = @('-S', $SourceDir, '-B', $bw, '-G', 'Ninja',
                         "-DQT_HOST_PATH=$QtDesktopKit",
                         "-DCMAKE_INSTALL_PREFIX=$staging") + $CommonOptions
            Invoke-Native (Join-Path $QtDesktopKit 'bin/qt-cmake.bat') $winArgs
            Invoke-Native 'cmake' @('--build', $bw, '--target', 'all', '--config', 'Release')
            Invoke-Native 'cmake' @('--install', $bw, '--config', 'Release')
        } finally {
            Get-ChildItem env: | ForEach-Object { if (-not $savedEnv.ContainsKey($_.Name)) { Remove-Item "env:$($_.Name)" } }
            foreach ($k in $savedEnv.Keys) { Set-Item -Path "env:$k" -Value $savedEnv[$k] }
        }

        $installer = Join-Path $bw "$AppName-installer.exe"
        if (-not (Test-Path $installer)) { Fail "installer was not created: $installer (makensis output above; CreateWinInstaller.cmake does not stop on a makensis error)" }

        $exeBuilt   = Join-Path $bw "Release/$AppName.exe"
        $exeStaging = Join-Path $staging "bin/$AppName.exe"
        $ok1 = Test-WindowsExeVersion $exeBuilt $Tag (-not $DryRun)
        $ok2 = Test-WindowsExeVersion $exeStaging $Tag (-not $DryRun)
        $ok3 = Test-ConfiguredVersion $bw $Tag
        if (-not ($ok1 -and $ok2 -and $ok3)) { $WinOk = $false; Fail "Windows version check failed; artifacts were not copied" }

        $winOut = Join-Path $OutDir "$AppName-$Tag-windows-x64-installer.exe"
        Copy-Item $installer $winOut
        $artifacts.Add($winOut)
        $vi = (Get-Item $exeStaging).VersionInfo
        $info.Add("windows:    msvc2022_64, $(Split-Path $winOut -Leaf), exe file version $($vi.FileMajorPart).$($vi.FileMinorPart).$($vi.FileBuildPart).$($vi.FilePrivatePart), product version '$($vi.ProductVersion)', version string '$Tag'")
    }

    # ------------------------------------------------------------ Android
    if ($Platform -contains 'Android') {
        Say "=== Android $AndroidAbi Release ==="
        $kitCmake = Join-Path $QtAndroidKit 'bin/qt-cmake.bat'
        $kitToolchain = Join-Path $QtAndroidKit 'lib/cmake/Qt6/qt.toolchain.cmake'
        if (-not ((Test-Path $kitCmake) -and (Test-Path $kitToolchain))) {
            Say "Android Qt kit NOT installed: expected $QtAndroidKit (bin/qt-cmake.bat and lib/cmake/Qt6/qt.toolchain.cmake)."
            Say "Install Qt $QtVersion > Android ARM64-v8a with the Qt Maintenance Tool, then run again. Nothing was installed."
            exit 3
        }
        Say "Android Qt kit found: $QtAndroidKit"
        foreach ($p in @($AndroidSdk, $AndroidNdk, (Join-Path $QtDesktopKit 'bin'), (Join-Path $NinjaDir 'ninja.exe'))) {
            if (-not (Test-Path $p)) { Fail "missing: $p" }
        }
        if (-not $JavaHome -or -not (Test-Path (Join-Path $JavaHome 'bin/java.exe'))) { Fail "JDK 17 not found; pass -JavaHome" }

        $savedPath = $env:PATH; $savedJava = $env:JAVA_HOME
        try {
            $env:JAVA_HOME = $JavaHome
            $env:PATH = "$NinjaDir;$JavaHome\bin;$env:PATH"
            $ba = Join-Path $WorkDir 'android'
            $andArgs = @('-S', $SourceDir, '-B', $ba, '-G', 'Ninja',
                         "-DQT_HOST_PATH=$QtDesktopKit",
                         "-DANDROID_SDK_ROOT=$AndroidSdk",
                         "-DANDROID_NDK_ROOT=$AndroidNdk",
                         "-DQT_ANDROID_ABIS=$AndroidAbi",
                         '-DQT_ANDROID_BUILD_ALL_ABIS=OFF',
                         '-DQT_ANDROID_SIGN_APK=OFF',        # signed later with RotorLab's Android key (signing step)
                         '-DCMAKE_CXX_SCAN_FOR_MODULES=OFF',
                         '-DCMAKE_WARN_DEPRECATED=FALSE',
                         "-DCMAKE_INSTALL_PREFIX=$(Join-Path $ba 'staging')") + $CommonOptions
            Invoke-Native $kitCmake $andArgs
            Invoke-Native 'cmake' @('--build', $ba, '--target', 'all', '--config', 'Release')
            Invoke-Native 'cmake' @('--build', $ba, '--target', 'apk', '--config', 'Release')
        } finally {
            $env:PATH = $savedPath; $env:JAVA_HOME = $savedJava
        }

        $apk = Get-ChildItem (Join-Path $ba 'android-build') -Recurse -Filter '*.apk' |
            Where-Object { $_.FullName -match '[\\/]outputs[\\/]apk[\\/]release[\\/]' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $apk) { Fail "no release APK under $ba/android-build/build/outputs/apk/release" }
        $so = Get-ChildItem (Join-Path $ba 'android-build/libs') -Recurse -Filter "lib$AppName*.so" | Select-Object -First 1
        if (-not $so) { Fail "lib$AppName*.so not found under $ba/android-build/libs" }

        Say "version check: $($so.FullName)"
        $soOk  = Test-EmbeddedString $so.FullName $Tag
        Say ("  version string in .so : " + $(if ($soOk) { "'$Tag' (matches tag)" } else { 'MISMATCH (found: ' + ((Get-EmbeddedVersionStrings $so.FullName $Tag) -join ', ') + ')' }))
        $andOk = (Test-ConfiguredVersion $ba $Tag) -and $soOk
        $aapt = Get-ChildItem (Join-Path $AndroidSdk 'build-tools') -Recurse -Filter 'aapt.exe' -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
        $apkVer = ''
        if ($aapt) {
            $badging = & $aapt.FullName dump badging $apk.FullName | Select-Object -First 1
            if ($badging -match "versionName='([^']*)'") { $apkVer = $Matches[1] }
            Say "  APK versionName       : $apkVer"
            $n = Get-TagNumeric $Tag
            if ($n) {
                $want = '{0}.{1}.{2}' -f $n[0], $n[1], $n[2]
                if ($apkVer -ne $want) { Say "  versionName MISMATCH: expected $want"; if (-not $DryRun) { $andOk = $false } }
            } else { Say "  versionName not compared: '$Tag' is not a vX.Y.Z tag (dry run)" }
        } else { Say "  aapt.exe not found under $AndroidSdk/build-tools; APK versionName not read" }
        if (-not $andOk) { Fail "Android version check failed; APK was not copied" }
        Say "VERSION CHECK PASS (Android): $Tag"

        # Unsigned on purpose; the signing step signs it (zipalign, apksigner).
        $apkOut = Join-Path $OutDir "$AppName-$Tag-android-$AndroidAbi.apk"
        Copy-Item $apk.FullName $apkOut
        $artifacts.Add($apkOut)
        $info.Add("android:    $AndroidAbi, $(Split-Path $apkOut -Leaf) (unsigned until the signing step), versionName $apkVer, version string '$Tag'")
    }

    # The source must still be the tag, unchanged, after the build.
    if ((Invoke-Git @('rev-parse', 'HEAD')) -ne $tagCommit) { Fail "HEAD moved during the build; discard these artifacts" }
    if (Invoke-Git @('status', '--porcelain', '--untracked-files=all')) { Fail "the work tree changed during the build; discard these artifacts" }

    $info.Add('')
    $info.Add('artifacts:')
    foreach ($a in $artifacts) { $info.Add(('  {0}  {1:N0} bytes' -f (Split-Path $a -Leaf), (Get-Item $a).Length)) }
    Set-Content -Encoding ascii -Path (Join-Path $OutDir 'BUILD-INFO.txt') -Value $info

    Say "=== Done: artifacts in $OutDir ==="
    foreach ($a in $artifacts) { Say ('  {0}  {1:N0} bytes' -f (Split-Path $a -Leaf), (Get-Item $a).Length) }
    Say ''
    if ($DryRun) {
        Say 'DRY RUN: these artifacts are NOT for release. Do not sign them.'
    } else {
        # The signing step. It needs RotorLab's private keys, which never live in
        # this repository, so this script only describes it. RotorLab signs:
        #   1. the Windows installer with an Authenticode certificate;
        #   2. the APK with RotorLab's Android signing key (zipalign, apksigner);
        #   3. SHA256SUMS.txt, the SHA-256 of the signed installer and APK;
        #   4. a detached signature (.sig) for each of those three files, made with
        #      the RotorLab release key: ssh-keygen -Y sign -n rotorlab-release.
        # Anyone can check a release with the public key line in README.md, with no
        # secret: ssh-keygen -Y verify -f allowed_signers -I release@rotorlab.app
        #   -n rotorlab-release -s <file>.sig < <file>
        Say 'Next, the signing step (RotorLab signs releases with keys kept outside this repository):'
        Say '  Authenticode for the installer, apksigner for the APK, then SHA256SUMS.txt and'
        Say '  a detached ssh-keygen signature (.sig, namespace rotorlab-release) for each file.'
        Say '  README.md, "Verify a release", shows how anyone checks them.'
    }
    exit 0
} catch {
    Write-Host "[release] FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
