@echo off
setlocal enabledelayedexpansion

REM Usage: generate_version_header.bat <version_file_path> <output_folder>

if "%~1"=="" (
    echo ERROR: Version file path not specified.
    exit /b 1
)

if "%~2"=="" (
    echo ERROR: Output folder not specified.
    exit /b 1
)

set "versionFile=%~1"
set "outputDir=%~2"
set "headerFile=%outputDir%\version.h"

if not exist "%versionFile%" (
    echo ERROR: Version file "%versionFile%" not found!
    exit /b 1
)

if not exist "%outputDir%" (
    echo ERROR: Output folder "%outputDir%" not found!
    exit /b 1
)

set "version="

REM Parse version from the "<Name>_Version = x.y.z" line. findstr picks the
REM line so comment lines never reach the loop: Wine's cmd, used by the Linux
REM CI build, doesn't skip ';' lines in for /f and can't parse chained ifs.
for /f "tokens=1,* delims== " %%a in ('findstr /c:"_Version =" "%versionFile%"') do (
    set "val=%%b"

    REM Trim spaces
    for /f "tokens=* delims= " %%x in ("!val!") do set "val=%%x"

    set "version=!val!"
    goto :gotVersion
)

:gotVersion

if not defined version (
    echo ERROR: Version not found in file!
    exit /b 1
)

REM Get Jenkins build number if defined
if defined BUILD_NUMBER (
    set "buildNumber=%BUILD_NUMBER%"
) else (
    set "buildNumber="
)

REM Compose full version string
if defined buildNumber (
    set "fullVersion=%version%.%buildNumber%"
) else (
    set "fullVersion=%version%"
)

echo Generating version header at "%headerFile%"
REM One redirect per line: Wine's cmd doesn't redirect parenthesized blocks
> "%headerFile%" echo #pragma once
>> "%headerFile%" echo #define FILE_VERSION %fullVersion:.=, %
>> "%headerFile%" echo #define PRODUCT_VERSION %fullVersion:.=, %
>> "%headerFile%" echo #define FILE_VERSION_STR "%fullVersion%\0"
>> "%headerFile%" echo #define PRODUCT_VERSION_STR "%fullVersion%\0"

echo Done.
endlocal
exit /b 0
