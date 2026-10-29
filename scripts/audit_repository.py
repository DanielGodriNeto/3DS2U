"""Scan Git history, working files or staged blobs without displaying matched values."""
import argparse
import collections
import json
import pathlib
import re
import subprocess

PATTERNS = {
    "private_key_or_certificate": re.compile(rb"-----BEGIN (?:[A-Z0-9 ]*PRIVATE KEY|CERTIFICATE)-----"),
    "personal_absolute_path": re.compile(rb"(?i)(?:[A-Z]:[\\/]+Users[\\/]+[^\\/\s\"'<>]+|/(?:home|Users)/[^/\s\"'<>]+)"),
    "service_token": re.compile(rb"(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|sk-proj-[A-Za-z0-9_-]{30,}|AKIA[0-9A-Z]{16})"),
}
PRIVATE_NAMES = re.compile(r"(?i)(?:^|/)(?:keys[^/]*\.txt|log\.txt|wiiunx2(?:-recent-\d+\.txt|\.log)|\.env(?:\..*)?|.*\.(?:pfx|p12|pem|key|cia|cci|3ds|3dsx|cxi|romfs|exheader))$")
# Authored metadata fixture reproduced by generate_triangle_test.cpp, not a dump.
SYNTHETIC_FILES = {"triangle_test.exheader"}

def git(*args, **kwargs):
    return subprocess.run(["git", *args], check=True, **kwargs)

def categories(data, names):
    # PowerShell transcripts are often UTF-16; do not miss their embedded paths.
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        data = data.decode("utf-16", "replace").encode("utf-8")
    result = [name for name, pattern in PATTERNS.items() if pattern.search(data)]
    if any(PRIVATE_NAMES.search(path) and path not in SYNTHETIC_FILES and
           pathlib.PurePosixPath(path).name != ".env.example" for path in names):
        result.append("private_filename")
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scope", choices=("history", "working", "staged"), default="history")
    parser.add_argument("--report", type=pathlib.Path, help="Write a redacted JSON report")
    args = parser.parse_args()
    findings = []
    if args.scope != "history":
        if args.scope == "staged":
            names = git("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z", stdout=subprocess.PIPE).stdout
        else:
            names = git("ls-files", "--cached", "--others", "--exclude-standard", "-z", stdout=subprocess.PIPE).stdout
        for raw in set(names.split(b"\0")) - {b""}:
            path = raw.decode("utf-8", "replace")
            if args.scope == "staged":
                data = git("show", ":" + path, stdout=subprocess.PIPE).stdout
            else:
                item = pathlib.Path(path)
                if not item.is_file() or item.is_symlink():
                    continue
                data = item.read_bytes()
            matches = categories(data, [path])
            if matches:
                findings.append({"paths": [path], "categories": matches})
        return finish(args, findings, len(names.split(b"\0")) - 1)
    objects = git("rev-list", "--objects", "--all", stdout=subprocess.PIPE).stdout.splitlines()
    paths = collections.defaultdict(set)
    for row in objects:
        oid, _, path = row.partition(b" ")
        if path:
            paths[oid.decode()].add(path.decode("utf-8", "replace"))
    process = subprocess.Popen(["git", "cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        for oid, names in paths.items():
            process.stdin.write((oid + "\n").encode())
            process.stdin.flush()
            fields = process.stdout.readline().split()
            data = process.stdout.read(int(fields[2]))
            if process.stdout.read(1) != b"\n":
                raise RuntimeError("Invalid Git batch response")
            if fields[1] != b"blob":
                continue
            matches = categories(data, names)
            if matches:
                findings.append({"blob": oid, "paths": sorted(names), "categories": matches})
    finally:
        process.stdin.close()
        process.wait()
    messages = git("log", "--all", "--format=%B", "-z", stdout=subprocess.PIPE).stdout
    matches = categories(messages, [])
    if matches:
        findings.append({"paths": ["<commit messages>"], "categories": matches})
    return finish(args, findings, len(objects))

def finish(args, findings, checked):
    report = {"scope": args.scope, "checked": checked, "findings": findings}
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    counts = collections.Counter(category for item in findings for category in item["categories"])
    print(json.dumps({"scope": args.scope, "checked": checked, "finding_counts": counts}))
    return 1 if findings else 0

if __name__ == "__main__":
    raise SystemExit(main())
