$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$cleaner = Join-Path $repo 'scripts/clean_generated.ps1'
$fixture = Join-Path $repo ('build/cleanup-tests-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path (Join-Path $fixture 'wiiu_runtime/src') -Force
$null = New-Item -ItemType Directory -Path (Join-Path $fixture 'build/runtime') -Force
$generated = @(
    'extracted.code', 'extracted.exheader', 'extracted.romfs', 'extracted_romfs.cpp',
    'output_translated.cpp', 'code.bin', 'translated_game.cpp',
    'wiiu_runtime/src/translated_game.cpp', 'wiiu_runtime/src/generated_romfs.cpp',
    'build/nes_remix_wiiu.rpx', 'build/nes_remix_wiiu.romfs',
    'build/runtime/translated_game.o', 'build/runtime/main.d',
    'build/runtime/wiiu_runtime.elf', 'build/runtime/wiiu_runtime.rpx'
)
$preserved = @(
    'build_3ds2u_app.cmd', 'nes_remix.3ds', 'keys.txt', 'wiiu_runtime/src/main.cpp',
    'wiiu_runtime/src/packaged_romfs.cpp', 'build/extractor.exe',
    'build/3ds2u_translator.exe', 'build/other_game.rpx', 'build/runtime/notes.txt'
)
foreach ($path in ($generated + $preserved)) {
    Set-Content -LiteralPath (Join-Path $fixture $path) -Value 'fixture' -Encoding Ascii
}
$rom = Join-Path $fixture 'nes_remix.3ds'

& powershell.exe -NoProfile -NonInteractive -File $cleaner -ProjectRoot $fixture -InputPath $rom -OutputName nes_remix_wiiu.rpx -WhatIf *> (Join-Path $fixture 'preview.log')
if ($LASTEXITCODE -ne 0) { throw 'Cleanup preview failed.' }
foreach ($path in $generated) {
    if (-not (Test-Path -LiteralPath (Join-Path $fixture $path))) { throw 'Preview deleted a file.' }
}

# Invalid output and input/target collisions must fail before deleting any files.
$ErrorActionPreference = 'Continue'
& powershell.exe -NoProfile -NonInteractive -File $cleaner -ProjectRoot $fixture -InputPath $rom -OutputName '../escape.rpx' *> (Join-Path $fixture 'invalid.log')
$rejectedOutput = $LASTEXITCODE -ne 0
& powershell.exe -NoProfile -NonInteractive -File $cleaner -ProjectRoot $fixture -InputPath (Join-Path $fixture 'code.bin') -OutputName nes_remix_wiiu.rpx *> (Join-Path $fixture 'collision.log')
$rejectedInput = $LASTEXITCODE -ne 0
$ErrorActionPreference = 'Stop'
if (-not $rejectedOutput) { throw 'Output path traversal was accepted.' }
if (-not $rejectedInput) { throw 'Input/generated-file collision was accepted.' }
foreach ($path in $generated) {
    if (-not (Test-Path -LiteralPath (Join-Path $fixture $path))) { throw 'Validation failure deleted a file.' }
}

& powershell.exe -NoProfile -NonInteractive -File $cleaner -ProjectRoot $fixture -InputPath $rom -OutputName nes_remix_wiiu.rpx
if ($LASTEXITCODE -ne 0) { throw 'Cleanup failed.' }
foreach ($path in $generated) {
    if (Test-Path -LiteralPath (Join-Path $fixture $path)) { throw "Generated artifact survived: $path" }
}
foreach ($path in $preserved) {
    if ((Get-Content -LiteralPath (Join-Path $fixture $path) -Raw).Trim() -ne 'fixture') {
        throw "Preserved file changed: $path"
    }
}
& powershell.exe -NoProfile -NonInteractive -File $cleaner -ProjectRoot $fixture -InputPath $rom -OutputName nes_remix_wiiu.rpx
if ($LASTEXITCODE -ne 0) { throw 'Repeated cleanup failed.' }
Write-Host 'Cleanup tests passed: preview, bounds, input protection, artifact removal, preserved tools/ROM/keys/sources, repeatability.'
