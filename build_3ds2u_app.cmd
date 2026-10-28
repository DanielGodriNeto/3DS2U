@echo off
setlocal EnableExtensions DisableDelayedExpansion
if "%~1"=="" goto usage
if "%~2"=="" goto usage
if not "%~3"=="" goto usage
rem Resolve input relative to the caller, then build from the repository root.
set "INPUT_ROM=%~f1"
set "OUTPUT_NAME=%~2"
set "OUTPUT_STEM=%~n2"
if not "%~nx2"=="%~2" goto bad_output
if /I not "%~x2"==".rpx" goto bad_output
pushd "%~dp0"
if errorlevel 1 exit /b 1
if not defined DEVKITPRO set "DEVKITPRO=%SystemDrive%/devkitPro"
if "%DEVKITPRO%"=="/opt/devkitpro" set "DEVKITPRO=%SystemDrive%/devkitPro"
if not defined DEVKITPPC set "DEVKITPPC=%DEVKITPRO%/devkitPPC"
if "%DEVKITPPC%"=="/opt/devkitpro/devkitPPC" set "DEVKITPPC=%SystemDrive%/devkitPro/devkitPPC"
if not defined WUT_ROOT set "WUT_ROOT=%DEVKITPRO%/wut"
if not defined MAKE_EXE set "MAKE_EXE=%DEVKITPRO%/msys2/usr/bin/make.exe"
set "DEVKITPRO=%DEVKITPRO:\=/%"
set "DEVKITPPC=%DEVKITPPC:\=/%"
set "WUT_ROOT=%WUT_ROOT:\=/%"
echo [3DS2U] [1/6] Checking input and toolchain
echo [3DS2U] Input: "%INPUT_ROM%"
echo [3DS2U] Output: "build\%OUTPUT_NAME%"
if not exist "%INPUT_ROM%" goto missing_input
if not exist "build\extractor.exe" goto missing_tools
if not exist "build\3ds2u_translator.exe" goto missing_tools
if not exist "%DEVKITPPC%/bin/powerpc-eabi-g++.exe" goto missing_toolchain
if not exist "%WUT_ROOT%/share/wut.specs" goto missing_toolchain
if not exist "%WUT_ROOT%/include/gx2/state.h" goto missing_toolchain
if not exist "%WUT_ROOT%/lib/libwut.a" goto missing_toolchain
if not exist "%DEVKITPRO%/tools/bin/elf2rpl.exe" goto missing_toolchain
if not exist "%MAKE_EXE%" goto missing_toolchain
if not exist "%DEVKITPRO%/msys2/usr/bin/sh.exe" goto missing_toolchain
echo [3DS2U] [2/6] Extracting and relocating 3DSX segments and RomFS
"build\extractor.exe" "%INPUT_ROM%"
if errorlevel 1 goto extraction_failed
if not exist "extracted.code" goto extraction_failed
if not exist "extracted.exheader" goto extraction_failed
if not exist "extracted.romfs" goto extraction_failed
if not exist "extracted_romfs.cpp" goto extraction_failed
echo [3DS2U] [3/6] Translating executable text to C++
rem Metadata prevents rodata, data and BSS from being decoded as instructions.
"build\3ds2u_translator.exe" "extracted.code" "output_translated.cpp" --exheader "extracted.exheader"
if errorlevel 1 goto translation_failed
echo [3DS2U] [4/6] Installing translated code and embedded RomFS
copy /Y "output_translated.cpp" "wiiu_runtime\src\translated_game.cpp" >nul
if errorlevel 1 goto copy_failed
copy /Y "extracted_romfs.cpp" "wiiu_runtime\src\generated_romfs.cpp" >nul
if errorlevel 1 goto copy_failed
echo [3DS2U] [5/6] Building with devkitPPC and wut
"%MAKE_EXE%" --no-print-directory -C "wiiu_runtime" all
if errorlevel 1 goto build_failed
if not exist "build\runtime\wiiu_runtime.rpx" goto build_failed
echo [3DS2U] [6/6] Publishing RPX and RomFS sidecar
copy /Y "extracted.romfs" "build\%OUTPUT_STEM%.romfs" >nul
if errorlevel 1 goto publish_failed
move /Y "build\runtime\wiiu_runtime.rpx" "build\%OUTPUT_NAME%" >nul
if errorlevel 1 goto publish_failed
echo [3DS2U] Build complete: "%CD%\build\%OUTPUT_NAME%"
echo [3DS2U] RomFS is embedded; sidecar: "%CD%\build\%OUTPUT_STEM%.romfs"
popd
exit /b 0
:missing_input
echo [3DS2U] ERROR: Input file does not exist. 1>&2
goto failed
:missing_tools
echo [3DS2U] ERROR: Build extractor.exe and 3ds2u_translator.exe first; see README.md. 1>&2
goto failed
:missing_toolchain
echo [3DS2U] ERROR: Missing devkitPPC, wut, elf2rpl, make or the MSYS shell. Check toolchain environment variables. 1>&2
goto failed
:extraction_failed
echo [3DS2U] ERROR: Extraction failed; translation and compilation were skipped. 1>&2
goto failed
:translation_failed
echo [3DS2U] ERROR: Translation failed; compilation was skipped. 1>&2
goto failed
:copy_failed
echo [3DS2U] ERROR: Could not install generated runtime sources. 1>&2
goto failed
:build_failed
echo [3DS2U] ERROR: Runtime build failed; no RPX was published. 1>&2
goto failed
:publish_failed
echo [3DS2U] ERROR: Could not publish RPX or RomFS sidecar. 1>&2
:failed
popd
exit /b 1
:bad_output
echo [3DS2U] ERROR: Output must be a filename ending in .rpx without a directory. 1>&2
exit /b 1
:usage
echo Usage: build_3ds2u_app.cmd ^<input.3dsx^> ^<output.rpx^>
exit /b 1
