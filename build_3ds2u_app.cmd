@echo off
setlocal EnableExtensions DisableDelayedExpansion
if "%~1"=="" goto usage
if "%~2"=="" goto usage
if "%~3"=="" goto usage
if not "%~4"=="" goto usage

rem Resolve inputs before switching to the project directory.
set "INPUT_CODE=%~f1"
set "INPUT_EXHEADER=%~f2"
set "OUTPUT_NAME=%~3"
if not "%~nx3"=="%~3" goto bad_output
if /I not "%~x3"==".rpx" goto bad_output
pushd "%~dp0"
if errorlevel 1 exit /b 1

if not defined DEVKITPRO set "DEVKITPRO=%SystemDrive%/devkitPro"
rem devkitPro's MSYS defaults need native Windows paths in a batch workflow.
if "%DEVKITPRO%"=="/opt/devkitpro" set "DEVKITPRO=%SystemDrive%/devkitPro"
if not defined DEVKITPPC set "DEVKITPPC=%DEVKITPRO%/devkitPPC"
if "%DEVKITPPC%"=="/opt/devkitpro/devkitPPC" set "DEVKITPPC=%DEVKITPRO%/devkitPPC"
if not defined WUT_ROOT set "WUT_ROOT=%DEVKITPRO%/wut"
if not defined MAKE_EXE set "MAKE_EXE=%DEVKITPRO%/msys2/usr/bin/make.exe"
set "DEVKITPRO=%DEVKITPRO:\=/%"
set "DEVKITPPC=%DEVKITPPC:\=/%"
set "WUT_ROOT=%WUT_ROOT:\=/%"

echo [3DS2U] [1/5] Checking inputs and toolchain
echo [3DS2U] Input: "%INPUT_CODE%"
echo [3DS2U] ExHeader: "%INPUT_EXHEADER%"
echo [3DS2U] Output: "build\%OUTPUT_NAME%"
if not exist "%INPUT_CODE%" goto missing_input
if not exist "%INPUT_EXHEADER%" goto missing_input
if not exist "build\3ds2u_translator.exe" goto missing_translator
if not exist "%DEVKITPPC%/bin/powerpc-eabi-g++.exe" goto missing_toolchain
if not exist "%WUT_ROOT%/share/wut.specs" goto missing_toolchain
if not exist "%WUT_ROOT%/include/gx2/state.h" goto missing_toolchain
if not exist "%WUT_ROOT%/lib/libwut.a" goto missing_toolchain
if not exist "%DEVKITPRO%/tools/bin/elf2rpl.exe" goto missing_toolchain
if not exist "%MAKE_EXE%" goto missing_make
if not exist "%DEVKITPRO%/msys2/usr/bin/sh.exe" goto missing_make

echo [3DS2U] [2/5] Translating ARM code to C++
"build\3ds2u_translator.exe" "%INPUT_CODE%" "output_translated.cpp" --exheader "%INPUT_EXHEADER%"
if errorlevel 1 goto translation_failed

echo [3DS2U] [3/5] Installing generated runtime source
copy /Y "output_translated.cpp" "wiiu_runtime\src\translated_game.cpp" >nul
if errorlevel 1 goto copy_failed

echo [3DS2U] [4/5] Compiling and packaging with devkitPPC/wut
"%MAKE_EXE%" --no-print-directory -C "wiiu_runtime" all
if errorlevel 1 goto build_failed
if not exist "build\runtime\wiiu_runtime.rpx" goto build_failed

echo [3DS2U] [5/5] Publishing RPX
move /Y "build\runtime\wiiu_runtime.rpx" "build\%OUTPUT_NAME%" >nul
if errorlevel 1 goto publish_failed
echo [3DS2U] Build complete: "%CD%\build\%OUTPUT_NAME%"
popd
exit /b 0

:missing_input
echo [3DS2U] ERROR: Input .code or ExHeader file does not exist. 1>&2
goto failed
:missing_translator
echo [3DS2U] ERROR: Build build\3ds2u_translator.exe first; see README.md. 1>&2
goto failed
:missing_toolchain
echo [3DS2U] ERROR: Missing devkitPPC, wut, or elf2rpl. Check DEVKITPRO, DEVKITPPC and WUT_ROOT. 1>&2
goto failed
:missing_make
echo [3DS2U] ERROR: GNU make or the devkitPro MSYS shell is missing. Check MAKE_EXE and DEVKITPRO. 1>&2
goto failed
:translation_failed
echo [3DS2U] ERROR: Translation failed; runtime compilation was skipped. 1>&2
goto failed
:copy_failed
echo [3DS2U] ERROR: Could not install translated_game.cpp. 1>&2
goto failed
:build_failed
echo [3DS2U] ERROR: Runtime build failed; no RPX was published. 1>&2
goto failed
:publish_failed
echo [3DS2U] ERROR: Could not move the RPX to the requested output. 1>&2
:failed
popd
exit /b 1
:bad_output
echo [3DS2U] ERROR: Output must be a filename ending in .rpx, without a directory. 1>&2
exit /b 1
:usage
echo Usage: build_3ds2u_app.cmd ^<input.code^> ^<input.exheader^> ^<output.rpx^>
exit /b 1
