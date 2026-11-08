# Runtime validation record

## User-reported Cemu run

The following is a redacted record of a user-supplied build transcript and Cemu
log. The source revision and hashes of the input and output were not recorded, so
this observation cannot be tied to a reproducible artifact.

- A local, decrypted NCSD image completed extraction, translation and RPX
  linking. The translator reported 455,281 instructions in 62,198 basic blocks
  across 115 units.
- The build printed repeated warnings for unsupported VFPv2 instructions and
  their fallback emission. The exact count and instruction addresses were not
  retained.
- Cemu 2.6 launched the RPX directly in standalone mode and reported missing or
  incorrect title metadata, creating a placeholder Title ID. It mounted the
  packaged RomFS, then reported a fatal missing translated block at PC
  `0x004FEAB0` in ARM mode and exited with `coreinit.exit(1)`.
- The user described the visible result as an infinite loading screen. The log
  records process termination; it does not establish whether the display
  remained static before exit.

This confirms one failed Cemu run, not the cause. The standalone title-metadata
warning co-occurred with the failure but has not been shown to cause it. The
missing-block address could not be tied to a specific loader, CFG, mode-range or
dynamic-code problem from the available evidence. The VFP warnings are also not
established as causal: execution stopped at the missing-block error, and there
is no evidence that a warned VFP instruction was reached. The diagnostics and
fallback policy remain tracked in [issue #3](https://github.com/Wiirizon/3DS2U/issues/3);
the VFP fallback behavior is tracked separately in
[issue #11](https://github.com/Wiirizon/3DS2U/issues/11).

## Validation status and privacy

This run does not demonstrate a successful Cemu boot, visual output, input
handling, gameplay or Wii U hardware behavior. A successful build and RomFS
mount are not runtime compatibility evidence.

The source revision, exact Cemu launch metadata, hardware details, local paths,
game identity, Title ID, input/output hashes and raw logs were not supplied in
the report and are intentionally not included here. Do not commit ROMs, keys,
extracted content, generated RPX files or unredacted logs. See
[SECURITY.md](../SECURITY.md) before sharing additional diagnostics.

## Next validation

1. Record the repository commit and tool versions for each new run; record a
   hash of the generated RPX if artifact comparison is needed.
2. First launch the repository's synthetic `triangle_demo.rpx` in Cemu and
   record whether it renders, to distinguish a runtime/package baseline from
   title-specific translation behavior.
3. If an authorized local title is tested, keep the input and logs local and
   share only a redacted summary with reproducible tool/version details.
4. Treat unsupported VFPv2 warnings as a separate correctness risk: the
   translator now emits an explicit runtime trap for them instead of a no-op.
   A warning alone does not prove that the instruction executed; a trap message
   at run time means it did.
