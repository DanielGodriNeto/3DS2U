[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][string]$ProjectRoot,
    [Parameter(Mandatory)][string]$InputPath,
    [Parameter(Mandatory)][string]$OutputName
)

$ErrorActionPreference = 'Stop'

try {
    $root = (Resolve-Path -LiteralPath $ProjectRoot).ProviderPath.TrimEnd([char[]]'\/')
    $inputFile = (Resolve-Path -LiteralPath $InputPath).ProviderPath
    if (-not (Test-Path -LiteralPath (Join-Path $root 'build_3ds2u_app.cmd') -PathType Leaf)) {
        throw 'Project root does not contain build_3ds2u_app.cmd.'
    }
    if ([IO.Path]::GetFileName($OutputName) -cne $OutputName -or
        [IO.Path]::GetExtension($OutputName) -ine '.rpx' -or
        $OutputName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0) {
        throw 'Output must be an RPX filename without a directory.'
    }

    function Get-CleanupPath([string]$RelativePath) {
        $path = [IO.Path]::GetFullPath((Join-Path $root $RelativePath))
        if (-not $path.StartsWith($root + [IO.Path]::DirectorySeparatorChar,
                                   [StringComparison]::OrdinalIgnoreCase)) {
            throw "Cleanup path escapes the project: $RelativePath"
        }
        # Refuse junctions/symlinks in the target or its ancestors.
        $ancestor = $path
        while ($ancestor -ine $root) {
            if (Test-Path -LiteralPath $ancestor) {
                $item = Get-Item -LiteralPath $ancestor -Force
                if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                    throw "Cleanup path contains a junction or symlink: $ancestor"
                }
            }
            $ancestor = [IO.Path]::GetDirectoryName($ancestor)
        }
        return $path
    }

    $stem = [IO.Path]::GetFileNameWithoutExtension($OutputName)
    $relativeFiles = @(
        'extracted.code', 'extracted.exheader', 'extracted.romfs',
        'extracted_romfs.cpp', 'output_translated.cpp',
        'code.bin', 'translated_game.cpp',
        'wiiu_runtime/src/translated_game.cpp',
        'wiiu_runtime/src/generated_romfs.cpp',
        "build/$OutputName", "build/$stem.romfs"
    )
    $targets = @($relativeFiles | ForEach-Object { Get-CleanupPath $_ })
    $runtime = Get-CleanupPath 'build/runtime'
    if (Test-Path -LiteralPath $runtime -PathType Container) {
        # The runtime Makefile emits flat objects, dependencies, ELF and RPX files.
        $targets += @(Get-ChildItem -LiteralPath $runtime -File -Force |
            Where-Object { $_.Name -match '\.(o|obj|d|elf|rpx|map)$' } |
            ForEach-Object { Get-CleanupPath ('build/runtime/' + $_.Name) })
    }

    # Validate every target before deleting anything. Preserve source ROMs and tools.
    foreach ($path in $targets) {
        if ($path -ieq $inputFile) { throw 'Input file is also a generated cleanup target.' }
        if (Test-Path -LiteralPath $path -PathType Container) {
            throw "Expected a generated file, found a directory: $path"
        }
    }
    $removed = 0
    foreach ($path in $targets) {
        if ((Test-Path -LiteralPath $path -PathType Leaf) -and
            $PSCmdlet.ShouldProcess($path, 'Remove generated build artifact')) {
            Remove-Item -LiteralPath $path -Force -Confirm:$false
            ++$removed
        }
    }
    Write-Host "[3DS2U] Cleaned $removed generated files; input ROM, keys, sources and tool executables preserved."
} catch {
    [Console]::Error.WriteLine('[3DS2U] ERROR: Cleanup failed: ' + $_.Exception.Message)
    exit 1
}
