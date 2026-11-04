@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Run from an x64 C++20 MSVC developer command prompt. All builds are serial.
where cl >nul 2>nul
if errorlevel 1 (
    echo ERROR: Run from an x64 Native Tools developer command prompt. 1>&2
    exit /b 1
)
pushd "%~dp0.."
if errorlevel 1 exit /b 1
if not exist build mkdir build
set "CXX=cl /nologo /std:c++20 /EHsc /W4 /WX /I extractor\include /I translator\include /I wiiu_runtime\include /Fo:build\"
set "CRYPTO=extractor\src\crypto_engine.cpp extractor\src\key_store.cpp"
set "CONTAINERS=extractor\src\ncch_extractor.cpp translator\src\binary_loader.cpp wiiu_runtime\src\romfs_reader.cpp"
set "TRANSLATORS=translator\src\vfp_translator.cpp translator\src\cpp_emitter.cpp translator\src\cfg_builder.cpp translator\src\thumb_translator.cpp translator\src\alu_translator.cpp translator\src\branch_translator.cpp translator\src\cond_evaluator.cpp translator\src\mem_translator.cpp"

%CXX% extractor\tests\crypto_tests.cpp extractor\src\crypto_engine.cpp /Fe:build\crypto_tests.exe
if errorlevel 1 goto failed
build\crypto_tests.exe
if errorlevel 1 goto failed
%CXX% extractor\tests\cia_tests.cpp extractor\src\cia_extractor.cpp %CRYPTO% %CONTAINERS% /Fe:build\cia_tests.exe
if errorlevel 1 goto failed
build\cia_tests.exe
if errorlevel 1 goto failed
%CXX% extractor\tests\ncsd_tests.cpp extractor\src\ncsd_extractor.cpp %CRYPTO% %CONTAINERS% /Fe:build\ncsd_tests.exe
if errorlevel 1 goto failed
build\ncsd_tests.exe
if errorlevel 1 goto failed

%CXX% translator\tests\binary_loader_tests.cpp translator\src\binary_loader.cpp translator\src\decoder.cpp translator\src\cfg_builder.cpp translator\src\mem_translator.cpp translator\src\thumb_translator.cpp translator\src\cond_evaluator.cpp /Fe:build\binary_loader_tests.exe
if errorlevel 1 goto failed
build\binary_loader_tests.exe build\exefs-loader-tests
if errorlevel 1 goto failed
%CXX% translator\tests\entry_dispatch_tests.cpp %TRANSLATORS% translator\src\binary_loader.cpp translator\src\decoder.cpp /Fe:build\entry_dispatch_tests.exe
if errorlevel 1 goto failed
build\entry_dispatch_tests.exe build\entry-dispatch-tests
if errorlevel 1 goto failed
%CXX% translator\tests\memory_addressing_tests.cpp %TRANSLATORS% /Fe:build\memory_addressing_tests.exe
if errorlevel 1 goto failed
build\memory_addressing_tests.exe build\memory_addressing_execution.cpp build\memory_addressing_blocks.cpp
if errorlevel 1 goto failed
%CXX% build\memory_addressing_execution.cpp /Fe:build\memory_addressing_execution.exe
if errorlevel 1 goto failed
build\memory_addressing_execution.exe
if errorlevel 1 goto failed

%CXX% translator\tests\vfp_tests.cpp %TRANSLATORS% /Fe:build\vfp_tests.exe
if errorlevel 1 goto failed
build\vfp_tests.exe
if errorlevel 1 goto failed
%CXX% /fp:strict build\vfp_execution.cpp /Fe:build\vfp_execution.exe
if errorlevel 1 goto failed
build\vfp_execution.exe
if errorlevel 1 goto failed
%CXX% translator\tests\thumb32_tests.cpp %TRANSLATORS% /Fe:build\thumb32_tests.exe
if errorlevel 1 goto failed
build\thumb32_tests.exe
if errorlevel 1 goto failed
%CXX% build\thumb32_execution.cpp /Fe:build\thumb32_execution.exe
if errorlevel 1 goto failed
build\thumb32_execution.exe
if errorlevel 1 goto failed
echo Host parser and generated-code execution tests passed.
popd
exit /b 0
:failed
echo ERROR: Host test pipeline stopped. Review the failing check; do not bypass execution controls. 1>&2
popd
exit /b 1
