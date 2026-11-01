# Contributing

Read `README.md`, `AGENTS.md`, `docs/maintainer-guide.md` and `SECURITY.md`
first. Use synthetic fixtures rather than commercial binaries or private
hardware logs. Preserve existing authorship/identity and third-party attribution.
The repository root contains the current MIT license for the project code;
dependency and upstream license notices remain authoritative for their files.
Do not infer a broader license for Nintendo-derived assets or remove attribution.

## Development workflow

1. Use a C++20 compiler for host tools and devkitPPC/wut for target code. Configure
   local paths through the environment. Do not commit absolute workspace paths.
2. Build host tools as documented in README after changing their sources. The
   unified script invokes existing executables and does not rebuild them.
3. Add meaningful boundary/behavior tests for parser or instruction changes. Test
   generated C++ execution, not just string spelling, when possible. Keep host
   builds serial: shared object outputs otherwise race.
4. Run the portable test workflow in the maintainer guide. Cross-compile changed
   runtime code with warnings treated as errors. Hardware/Cemu tests supplement
   host mocks; neither proves general game compatibility.
5. Review the actual diff and run privacy checks for working/staged files before
   committing. Include validation and remaining limitations in change descriptions.

Keep code, logs and comments in English. Use explicit fixed-width integers for
guest formats, bounded byte reads and unsigned wrap semantics for guest address
arithmetic. Do not cast file bytes to native packed structs or depend on host
endianness. Explain reserved encodings, overflow checks and protocol aliases.

## Safe debugging

Reproduce with the smallest synthetic input. Distinguish extraction, translation,
host C++ generation, target compilation, startup mapping, guest execution and
presentation failures. Record the failing guest PC/opcode and mode without private
paths or raw game assets. Unsupported instructions must not be described as
implemented simply because source emission or linking succeeds.

Do not bypass Device Guard, Defender, execution policy, or approval controls to
run tests. Document the blocked executable/check and use an administrator-approved
toolchain or signed/allowed binaries. Never rename a blocked binary to evade policy.

## Handoff checklist

Update the relevant subsystem document and maintainer guide when interfaces,
build outputs or invariants change. Record decisions and known gaps in the repo.
Separate verified results from assumptions and suggested future work. Do not
invent performance, compatibility or security claims. Keep generated data and
private testing material out of commits and release packages.
