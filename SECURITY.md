# Security and privacy

This is an experimental local toolchain. It has no security audit, isolation
boundary, or commercial-game compatibility guarantee. Treat ROMs, metadata,
command buffers and generated C++ as untrusted inputs. Run builds without
administrator privileges and keep dependencies current.

## Private inputs and release contents

- Never commit or distribute keys, tickets with credentials, ROM dumps, extracted
  game code/RomFS, save data, account files, certificates, or unredacted logs.
- Keep `keys.txt` local and ignored. Never print its values or copy them into
  generated sources. Crypto fixtures use public standard vectors or synthetic
  values; these are not console keys.
- Generated RPX files embed the supplied executable and RomFS. They can therefore
  contain copyrighted/private content. An ignored output is not automatically
  suitable for redistribution. Review artifacts and applicable permissions before
  release; distribute synthetic demos only when provenance is clear.
- Logs can reveal file paths, hardware, identifiers and game details. Report
  minimal redacted excerpts. Do not include entire local logs in commits/issues.
- Preserve intentional author credits, copyright notices and project identity.
  Do not rewrite author/committer names or addresses during content cleanup.

## Checks before committing or releasing

From the repository root, with Python 3 and Git on PATH:

```text
python scripts/audit_repository.py --scope working
python scripts/audit_repository.py --scope staged
python scripts/audit_repository.py --scope history
git diff --cached --check
```

The scanner prints categories/counts, not matched values. Exit 1 means review is
required. Optional `--report build/privacy-audit.json` writes relative paths and
blob IDs into an ignored report. Its heuristic patterns cover selected token
formats, private-key/certificate markers, user-profile paths and private filenames,
including UTF-16 logs. It cannot prove absence of secrets, identify every personal
detail, or determine copyright ownership. Review diffs and release payloads too.

Enable the repository's staged-file check explicitly:

```text
git config core.hooksPath .githooks
```

Hooks are local and bypassable; they supplement review. Do not use force-add to
publish ignored data. A conventional configurable SDK example path is not a
developer's personal machine path. Use environment variables for actual paths.

## Reporting and incident response

No private reporting channel is configured in this repository. Contact the
maintainer through a verified private channel before disclosing sensitive details;
do not assume a public issue is private. Rotate exposed credentials first when
applicable. Stop publication while investigating.

History removal must cover every branch/tag/ref, preserve current source work,
and remove recovery refs/reflogs before local garbage collection. Do not retain a
backup of tainted Git history in the repository. Re-scan reachable and unreachable
objects after rewriting. Other clones, backups, forks and hosting caches need
separate cleanup; force-pushing alone cannot remove them. Follow the hosting
provider's sensitive-data removal process. Never silently overwrite remote work.

`--ignore-hash` is an explicit extraction experiment only. It does not disable
bounds/decompression checks, recover missing keys, validate signatures, or make
corrupt inputs safe. The unified pipeline never enables it automatically.
