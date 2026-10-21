#!/usr/bin/env python3
"""Audit project documentation and the explicit publication surface."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]

def audit(build, output):
    documents = [ROOT / "README.md", *sorted((ROOT / "docs").rglob("*.md"))]
    errors = []
    for path in documents:
        if not path.is_file():
            errors.append("missing " + str(path.relative_to(ROOT)))
            continue
        text = path.read_text()
        if any(word in text for word in ("/Users/", "notes/release-", "notes/milestone", "certified ASD", "untouched holdout")):
            errors.append("private path or unsupported wording: " + str(path.relative_to(ROOT)))
        for target in re.findall(r"\]\(([^)]+)\)", text):
            target = target.split("#", 1)[0]
            if not target or re.match(r"[a-z]+://", target):
                continue
            if not (path.parent / target).resolve().exists():
                errors.append(f"broken link: {path.relative_to(ROOT)} -> {target}")
    actual = subprocess.check_output([str(pathlib.Path(build).resolve() / "book_demo")])
    expected = ROOT / "docs/demo.txt"
    if not expected.exists() or expected.read_bytes() != actual:
        errors.append("docs/demo.txt differs from book_demo")
    # This is an inspectable candidate list, not an instruction to stage it.
    listing = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT).split(b"\0")
    paths = [p.decode() for p in listing if p and (ROOT/p.decode()).is_file()]
    forbidden = [p for p in paths if p.startswith(("data/", "notes/", "results/", ".venv/")) or p.endswith((".itch", ".mdb", ".mdb-lock", ".DS_Store")) and not p.startswith("tests/itch/") or ".env" in pathlib.Path(p).parts]
    if forbidden:
        errors.append("private or unrelated publication candidates: " + repr(forbidden))
    # The Docker build context must exclude every private path by default.
    ignore = (ROOT / ".dockerignore").read_text().splitlines()
    if not ignore or ignore[0] != "**" or any(x.startswith("!data") or x.startswith("!notes") or x == "!.git" for x in ignore):
        errors.append("Docker context is not restricted to the source allowlist")
    license_text=(ROOT/'LICENSE').read_text()
    if 'Apache License' not in license_text or 'Version 2.0' not in license_text:
        errors.append('missing original source Apache-2.0 license')
    if (ROOT/'lmdb/mdb.c').exists() or (ROOT/'include/msim/grpc_storage.hpp').exists():
        errors.append('unverified legacy dependency remains on the publication surface')
    secret_patterns=[rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',rb'AKIA[0-9A-Z]{16}',
                     rb'ghp_[A-Za-z0-9]{30,}',rb'github_pat_[A-Za-z0-9_]{30,}',rb'xox[baprs]-[A-Za-z0-9-]{20,}']
    allowed_dirs={'.github','include','src','tests','tools','bench','fuzz','scripts','docs'}
    allowed_root={'CMakeLists.txt','Dockerfile.verify','README.md','DRAW.md','NOTICE','LICENSE','.gitignore','.dockerignore','.clang-format','.gitattributes'}
    for name in paths:
        relative=pathlib.Path(name)
        if (len(relative.parts)==1 and name not in allowed_root) or (len(relative.parts)>1 and relative.parts[0] not in allowed_dirs):
            errors.append('unreviewed publication path: '+name)
        data=(ROOT/name).read_bytes()
        # Split signatures so the scanner does not identify its own patterns.
        if any(re.search(pattern,data) for pattern in secret_patterns):errors.append('credential-like content: '+name)
        if name.startswith('docs/results/') and b'/Users/' in data:errors.append('private host path in public result: '+name)
    manifest = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in documents if p.is_file()}
    for path in sorted((ROOT / "docs/results").rglob("*")):
        if path.is_file():
            manifest[str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()
    result = dict(status="FAIL" if errors else "PASS", errors=errors, documentation_sha256=manifest, publication_candidates=paths, checks=["local links", "deterministic demo", "private artifact exclusions", "restricted Docker context", "source license", "credential signatures", "explicit source inventory"])
    pathlib.Path(output).write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"status": result["status"], "errors": errors, "candidate_count": len(paths)}))
    return bool(errors)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    raise SystemExit(audit(args.build, args.out))
