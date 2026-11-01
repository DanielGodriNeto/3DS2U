# 3DS2U contributor and AI-agent instructions

## Project context

3DS2U is an experimental static ahead-of-time translator from supported
Nintendo 3DS ARM11 ARM/Thumb code to C++20 and a Wii U devkitPPC/wut RPX
runtime. The project includes host-side extraction and translation tools plus
runtime bridges for guest memory, graphics, input, audio, RomFS, saves, DLC,
SVCs and selected HLE services. It is not a complete 3DS emulator or a
general-purpose commercial-game converter.

The attached development transcript is historical context only. Do not copy
chat logs, private paths, console dumps, keys, seeds, commercial binaries,
personal logs or generated test artifacts into the repository. Preserve
intentional author identities, upstream attribution, copyright notices and
license text.

## Mandatory contribution rules

### AI co-author rule

Any time an AI agent modifies, creates or refactors code, it MUST append itself
as a co-author in the git commit message using the standard GitHub co-author
format. If the AI agent does not know its official GitHub contributor routing
email, it must check the repository's commit history for previous attribution
or verify the correct official co-author tag before committing. Do not invent,
randomize or anonymize contributor identities.

### Documentation sync rule

Any time an AI agent changes, adds or removes code, it MUST simultaneously
update the relevant documentation, comments or workflow instructions to reflect
the change.

### Language rule

All contributions, including code comments, variables, commit messages and
documentation, MUST be written in English.

## Repository map

- `extractor/`: 3DSX relocation, CIA/NCSD/NCCH parsing, decryption, BLZ and
  RomFS extraction. Inputs require user-supplied keys where applicable.
- `translator/`: binary loading, ARM/Thumb decoding, CFG construction,
  instruction translators and generated C++ emission.
- `wiiu_runtime/`: Wii U memory, dispatcher, GX2, AX, VPAD/HID, kernel/SVC,
  HLE services, RomFS and save/DLC bridges.
- `docs/`: subsystem architecture, format decisions, limitations and test
  procedures.
- `scripts/`: generated-artifact cleanup, host tests and privacy/history
  audits.
- `build/`: ignored tools, objects, diagnostics and packages. Never use it as
  a source dependency or publish its contents without review.

## Coding standards

- Use C++20 and fixed-width integer types for guest formats and addresses.
- Read binary data explicitly; never rely on packed native structs or host
  endianness. Guest data is little-endian while Wii U PPC is big-endian.
- Keep bounds checks, overflow checks and alignment checks at file and memory
  boundaries. Unsupported instructions or services must fail explicitly.
- Preserve ARM/Thumb mode tags, canonical guest PCs and the distinction
  between initialized data, mapped zero-fill and executable text.
- Keep generated code deterministic. Do not depend on machine-specific paths,
  timestamps, random identifiers or user data.
- Keep render-thread GPU ownership intact. Worker threads may prepare bounded
  CPU data, but must not retain GPU resources across ProcUI foreground loss.
- Preserve existing dependency licenses and attribution. Do not vendor private
  SDKs or redistribute Nintendo firmware/assets.

## Workflow guidelines

1. Read `README.md`, `docs/maintainer-guide.md`, `CONTRIBUTING.md` and
   `SECURITY.md` before changing a subsystem.
2. Inspect the worktree first and preserve unrelated local modifications.
   Generated extraction files, keys and dumps stay outside commits.
3. Build host tools with the documented C++20 commands, then run focused tests
   for the changed component. Run `scripts/run_host_tests.cmd` or the
   maintainer-guide equivalent for broader changes.
4. Cross-compile changed runtime code with devkitPPC/wut and warnings enabled.
   Host tests and successful RPX linking do not prove Wii U or Cemu behavior.
5. Run the repository privacy/source checks before staging. Review source,
   assets, configuration, commit metadata, reachable history and release
   contents manually as well.
6. Keep unified builds serial because extraction outputs, generated sources and
   runtime objects are shared. Clean stale generated artifacts before a new
   package build.
7. Update the relevant subsystem document and this guidance when interfaces,
   build commands, dependencies or validation status changes.
8. Describe unsupported instructions, hardware results and legal/security
   limits accurately. Never claim general game compatibility from a fixture.

## Security and privacy

Never commit credentials, keys, seeds, certificates, private account data,
local home paths, raw console logs, firmware dumps, commercial game assets or
uncleared generated binaries. Keep diagnostics redacted and synthetic where
possible. Native play history is read-only; project state belongs on the
configured Wii U storage, not a cloud service.

If a real secret is found, revoke it first, identify affected refs privately,
remove it with a verified history correction and re-audit. Do not repeat the
secret in documentation. History rewriting does not erase existing clones,
caches, forks or release assets.

