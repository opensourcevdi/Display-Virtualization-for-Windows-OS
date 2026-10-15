#!/usr/bin/env bash
# Builds and test-signs the IntelVirtDisplay driver package on Linux, using
# MSBuild under Wine from the image in ci/Dockerfile.
#
# Usage: ci/build.sh [Release|Debug]
#
# Output, laid out like Intel's ZCBuild_<build>_Installer.zip releases:
#   ZCBuild_<build>_Installer/
#     IntelVirtDisplay/            driver packages (.sys/.dll, .inf, .cat)
#     IntelVirtDisplayEnabler.dll
#     IntelVirtDisplayInstaller.ps1
#     IntelVirtDisplay.cer, IntelVirtDisplayKMD.cer   test certificates (not
#                                  in Intel's production-signed releases)
#     LICENSE, Readme.txt
#     ZC_Installer/IntelVirtDisplayInstallerPackage.exe
#   ZCBuild_<build>_Symbols/       .pdb files
#
# Optional environment:
#   BUILD_NUMBER         Build number: names the output and is appended to the
#                        file versions by generate_version_header.bat
#   TEST_CERT, TEST_KEY  PEM files of a code signing certificate and key to sign
#                        with. Without them a fresh self-signed certificate is
#                        created per build, like the makecert post-build step.
#   TIMESTAMP_URL        RFC 3161 timestamp server, e.g. http://timestamp.digicert.com

set -euo pipefail

CONFIG=${1:-Release}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT/x64/$CONFIG"
OUT="$ROOT/ZCBuild_${BUILD_NUMBER:-local}_Installer"
SYMBOLS="$ROOT/ZCBuild_${BUILD_NUMBER:-local}_Symbols"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cd "$ROOT"

# Signing, inf2cat and the makecert post-build steps don't work under Wine;
# they are done with osslsigncode and ci/makecat.py below.
# No /m: MSBuild worker nodes crash under Wine (missing VS telemetry assembly).
# msvc-wine's msbuild wrapper turns Spectre mitigation off; the image has the
# Spectre libraries, so restore the WDK default.
build() { # solution [msbuild args...]
    msbuild "$@" /nologo /t:Rebuild \
        /p:Configuration="$CONFIG" /p:Platform=x64 \
        /p:Driver_SpectreMitigation=Spectre \
        /p:SignMode=Off /p:Inf2CatNoCatalog=true /p:ApiValidator_Enable=False \
        /p:PostBuildEventUseInBuild=false \
        /p:ForceImportBeforeCppTargets="$(winepath -w "$ROOT/ci/wine-overrides.props")"
}

build IntelVirtDisplayDriver.sln
# The installer project targets SDK 10.0.22621; the image has 10.0.28000 only.
build IntelVirtDisplayInstallerPackage/IntelVirtDisplayInstaller.sln \
    /p:WindowsTargetPlatformVersion=10.0.28000.0

if [[ -z "${TEST_CERT:-}" || -z "${TEST_KEY:-}" ]]; then
    TEST_KEY="$WORK/test.key"
    TEST_CERT="$WORK/test.crt"
    GENERATE_CERT=1
fi

sign() { # file...
    for f in "$@"; do
        osslsigncode sign -h sha256 -certs "$TEST_CERT" -key "$TEST_KEY" \
            ${TIMESTAMP_URL:+-ts "$TIMESTAMP_URL"} \
            -in "$f" -out "$WORK/signed"
        mv "$WORK/signed" "$f"
    done
}

package() { # name image catalog
    local name=$1 image=$2 catalog=$3
    local dir="$BIN/$name"
    if [[ -n "${GENERATE_CERT:-}" ]]; then
        openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
            -subj "/CN=$name" -addext extendedKeyUsage=codeSigning \
            -keyout "$TEST_KEY" -out "$TEST_CERT" 2>/dev/null
    fi
    # Sign the image before cataloging it: signing pads the file, and the
    # catalog hash must cover the final bytes.
    sign "$dir/$image"
    python3 "$ROOT/ci/makecat.py" --inf "$dir/$name.inf" \
        --out "$dir/$catalog" "$dir/$image" "$dir/$name.inf"
    sign "$dir/$catalog"
    openssl x509 -in "$TEST_CERT" -outform DER -out "$BIN/$name.cer"
    cp "$dir/$image" "$dir/$name.inf" "$dir/$catalog" "$OUT/IntelVirtDisplay/"
}

rm -rf "$OUT" "$SYMBOLS"
mkdir -p "$OUT/IntelVirtDisplay" "$OUT/ZC_Installer" "$SYMBOLS"

# Catalog names as in Intel's releases and IntelVirtDisplayInstaller.iss
package IntelVirtDisplayKMD IntelVirtDisplayKMD.sys IntelVirtDisplaykmd.cat
package IntelVirtDisplay IntelVirtDisplay.dll IntelVirtDisplay.cat

cp "$BIN/IntelVirtDisplayEnabler.dll" "$BIN/IntelVirtDisplay.cer" "$BIN/IntelVirtDisplayKMD.cer" \
   IntelVirtDisplayInstaller.ps1 LICENSE Readme.txt "$OUT/"

# Setup package, built like Intel's: AppVersion is the installer version with
# the build number appended, e.g. 1.0.2.2537.
installer_bin="IntelVirtDisplayInstallerPackage/x64/$CONFIG"
version=$(sed -n 's/^.*_Version *= *\([0-9.]*\).*$/\1/p' \
    IntelVirtDisplayInstallerPackage/IntelVirtDisplayInstaller/IntelVirtDisplayInstaller_Version.txt)
version="$version${BUILD_NUMBER:+.$BUILD_NUMBER}"
wine 'C:\InnoSetup\ISCC.exe' /Q \
    /O"$(winepath -w "$OUT/ZC_Installer")" \
    /DMyAppVersion="$version" /DMyVersionInfoVersion="$version" \
    /DIntelVirtDisplay="$(winepath -w "$OUT")" \
    /DIntelVirtDisplayUMD="$(winepath -w "$OUT/IntelVirtDisplay")" \
    /DIntelVirtDisplayKMD="$(winepath -w "$OUT/IntelVirtDisplay")" \
    /DIntelVirtDisplayInstaller="$(winepath -w "$installer_bin")" \
    "$(winepath -w IntelVirtDisplayInstallerPackage/IntelVirtDisplayInstaller.iss)"

cp "$BIN/IntelVirtDisplay.pdb" "$BIN/IntelVirtDisplayKMD.pdb" \
   "$BIN/IntelVirtDisplayEnabler.pdb" "$installer_bin/IntelVirtDisplayInstaller.pdb" "$SYMBOLS/"

find "$OUT" "$SYMBOLS" -type f | sort
