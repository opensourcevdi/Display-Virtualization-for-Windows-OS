# Linux build image for the IntelVirtDisplay drivers: MSVC, Windows SDK, WDK
# and MSBuild running under Wine (https://github.com/mstorsjo/msvc-wine).
#
# The Microsoft toolchain inside this image is not redistributable. Push it
# only to a private registry. Building it means accepting the Visual Studio
# Build Tools license: https://go.microsoft.com/fwlink/?LinkId=2327714

FROM ubuntu:24.04

# Wine 9.0 from Ubuntu 24.04 pairs with wine-mono 8.1.0, see
# https://gitlab.winehq.org/wine/wine/-/wikis/Wine-Mono#versions
ARG WINE_MONO_VERSION=8.1.0
# Must match WindowsTargetPlatformVersion of the projects.
ARG SDK_VERSION=10.0.28000
# WDK 10.0.28000.2526, from https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk
ARG WDK_INSTALLER_URL=https://go.microsoft.com/fwlink/?LinkId=2371554
ARG MSVC_WINE_COMMIT=514f8ea34842cd6d831804d0e9658d3a32870ae1

ENV DEBIAN_FRONTEND=noninteractive \
    WINEDEBUG=-all \
    WINEPREFIX=/opt/wineprefix

RUN dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        wine wine64 wine32 winbind procps python3 python3-asn1crypto msitools \
        ca-certificates curl git osslsigncode openssl zip xvfb xauth && \
    apt-get clean -y && \
    rm -rf /var/lib/apt/lists/*

# Initialize the wine prefix and install wine-mono, which MSBuild runs on.
# Wait for wineserver to exit so the prefix isn't left half-written.
RUN wine wineboot --init && \
    curl -sSL -o /tmp/wine-mono.msi \
        https://github.com/madewokherd/wine-mono/releases/download/wine-mono-${WINE_MONO_VERSION}/wine-mono-${WINE_MONO_VERSION}-x86.msi && \
    wine msiexec /i /tmp/wine-mono.msi /qn && \
    rm /tmp/wine-mono.msi && \
    while pgrep wineserver > /dev/null; do sleep 1; done

RUN git clone https://github.com/mstorsjo/msvc-wine.git /opt/msvc-wine && \
    git -C /opt/msvc-wine checkout ${MSVC_WINE_COMMIT}

WORKDIR /opt/msvc-wine

RUN WDK_INSTALLERS=$(./wdk-download.sh --cache /tmp/wdk "${WDK_INSTALLER_URL}") && \
    PYTHONUNBUFFERED=1 ./vsdownload.py --accept-license --dest /opt/msvc \
        --architecture x64 --sdk-version ${SDK_VERSION} \
        --with-wdk-installers "${WDK_INSTALLERS}" \
        --with-default yes \
        Microsoft.VisualStudio.Component.VC.Runtimes.x86.x64.Spectre && \
    ./install.sh /opt/msvc && \
    rm -rf /tmp/wdk && \
    # Only x64 is built: drop the other architectures' libraries and the kit
    # parts the build doesn't use (11 GB -> 4 GB).
    cd /opt/msvc && \
    find VC/Tools/MSVC/*/lib VC/Tools/MSVC/*/atlmfc/lib "Windows Kits/10/Lib" -depth -type d \
        \( -name arm -o -name arm64 -o -name arm64ec -o -name x86 \) -prune -exec rm -rf {} + && \
    rm -rf VC/Tools/MSVC/*/bin/Hostarm64 "Windows Kits/10/Debuggers" \
        "Windows Kits/10/Testing" "Windows Kits/10/App Certification Kit" && \
    while pgrep wineserver > /dev/null; do sleep 1; done

# MSBuild 18.x ships only MSBuild.exe.config.template; the VS installer turns
# it into MSBuild.exe.config. Without it MSBuild has no toolset and aborts
# with "Value cannot be null. Parameter name: toolsPath".
RUN for d in /opt/msvc/MSBuild/Current/Bin /opt/msvc/MSBuild/Current/Bin/amd64; do \
        [ -e "$d/MSBuild.exe.config" ] || cp "$d/MSBuild.exe.config.template" "$d/MSBuild.exe.config"; \
    done

# Inno Setup compiles IntelVirtDisplayInstallerPackage.exe. Its installer
# creates (hidden) windows even with /VERYSILENT, so it needs an X server;
# ISCC.exe itself runs without one.
ARG INNO_SETUP_VERSION=6.7.3
ARG INNO_SETUP_SHA256=9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732
RUN curl -sSL -o /tmp/innosetup.exe \
        https://github.com/jrsoftware/issrc/releases/download/is-$(echo ${INNO_SETUP_VERSION} | tr . _)/innosetup-${INNO_SETUP_VERSION}.exe && \
    echo "${INNO_SETUP_SHA256}  /tmp/innosetup.exe" | sha256sum -c - && \
    xvfb-run -a wine /tmp/innosetup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART \
        /CURRENTUSER /DIR='C:\InnoSetup' && \
    while pgrep wineserver > /dev/null; do sleep 1; done && \
    test -f "$WINEPREFIX/drive_c/InnoSetup/ISCC.exe" && \
    rm /tmp/innosetup.exe

ENV PATH=/opt/msvc/bin/x64:$PATH
