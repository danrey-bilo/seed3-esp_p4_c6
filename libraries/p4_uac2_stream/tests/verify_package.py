"""Verify the exported source snapshot and the pinned DWC2 patch, without USB."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def normalized(path):
    return path.read_text(encoding="utf-8").encode("utf-8")


def verify_snapshot(root, reference):
    snapshot = json.loads((root / "snapshot.json").read_text(encoding="utf-8"))
    reference_changes = snapshot.get("reference_sha256_lf_utf8", {})
    for name, expected in snapshot["source_sha256_lf_utf8"].items():
        actual = normalized(root / name)
        if hashlib.sha256(actual).hexdigest() != expected:
            raise AssertionError(f"Snapshot mismatch: {name}")
        if reference is not None:
            reference_digest = hashlib.sha256(normalized(reference / name)).hexdigest()
            if reference_digest != reference_changes.get(name, expected):
                raise AssertionError(f"Working component differs from recorded source: {name}")
    count = len(snapshot["source_sha256_lf_utf8"])
    print(f"PASS: {count} source hashes")
    if reference:
        print(f"PASS: reference comparison ({count - len(reference_changes)} identical, "
              f"{len(reference_changes)} documented packaging changes)")


def verify_links(root):
    count = 0
    # Documentation uses ordinary relative Markdown links; no generated HTML.
    for doc in root.rglob("*.md"):
        if "build" in doc.relative_to(root).parts or "managed_components" in doc.relative_to(root).parts:
            continue
        for target in re.findall(r"\[[^\]]*\]\(([^)]+)\)", doc.read_text(encoding="utf-8")):
            if "://" in target or target.startswith("#"):
                continue
            target = target.split("#", 1)[0]
            if not (doc.parent / target).exists():
                raise AssertionError(f"Broken documentation link in {doc.name}: {target}")
            count += 1
    print(f"PASS: {count} local documentation links")


def verify_patch(root, dcd, reference):
    original = normalized(dcd)
    with tempfile.TemporaryDirectory(prefix="p4-uac2-patch-test-") as scratch:
        scratch = Path(scratch)
        outputs = []
        for name, content in (("lf", original), ("crlf", original.replace(b"\n", b"\r\n"))):
            source, output = scratch / f"{name}.c", scratch / f"patched-{name}.c"
            source.write_bytes(content)
            result = subprocess.run([sys.executable, str(root / "port/patch_dwc2.py"),
                                     str(source), str(output)], capture_output=True, text=True, timeout=15)
            if result.returncode:
                raise AssertionError(result.stderr or result.stdout)
            outputs.append(normalized(output))
        if outputs[0] != outputs[1]:
            raise AssertionError("LF and CRLF inputs produced different DCD patches")
        if reference:
            source, output = scratch / "lf.c", scratch / "original-patch-output.c"
            result = subprocess.run([sys.executable, str(reference / "port/patch_dwc2.py"),
                                     str(source), str(output)], capture_output=True, text=True, timeout=15)
            if result.returncode or normalized(output) != outputs[0]:
                raise AssertionError("Export patch differs from tested firmware DCD output")
            print("PASS: generated DCD equals the original working component")
        for token in (b"p4_dwc2_faulted", b"p4_incomplete_iso_out", b"DIEPTSIZ_MULCNT_Pos", b"Copyright"):
            if token not in outputs[0]:
                raise AssertionError(f"Expected patch content missing: {token!r}")
        source, output = scratch / "unknown.c", scratch / "must-not-exist.c"
        source.write_bytes(original + b"\n// Deliberate version drift for the negative test.\n")
        result = subprocess.run([sys.executable, str(root / "port/patch_dwc2.py"),
                                 str(source), str(output)], capture_output=True, text=True, timeout=15)
        if result.returncode == 0 or output.exists() or "Unexpected dcd_dwc2.c SHA256" not in result.stderr:
            raise AssertionError("Unknown DCD source was not rejected before generation")
    print("PASS: DWC2 LF/CRLF reproducibility and unknown-version rejection")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, help="Optional original component folder")
    parser.add_argument("--dcd", type=Path, help="Pinned TinyUSB src/portable/synopsys/dwc2/dcd_dwc2.c")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    verify_snapshot(root, args.reference)
    verify_links(root)
    if args.dcd:
        verify_patch(root, args.dcd, args.reference)
    else:
        print("SKIP: DWC2 patch check (supply --dcd to enable it)")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
