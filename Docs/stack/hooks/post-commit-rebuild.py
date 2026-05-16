#!/usr/bin/env python3
"""
Post-commit hook helper for the ForgeEditorBridge cognitive stack.

Runs incremental rebuilds when plugin source or recipes change.
Designed to be invoked from a git post-commit hook OR run manually after edits.

Decision matrix (looks at files changed since last index build):
  - Any *.cpp or *.h under Source/ changed  -> rebuild graph + vector
  - Any recipes/*.yml changed               -> rebuild vector only
  - Build.cs changed                        -> WARN that Tier 4 may be stale
  - Nothing relevant changed                -> exit 0

Tier 4 (UE KG) is NEVER rebuilt automatically -- it depends on UE version,
not plugin churn. Rebuild it only when upgrading UE or when this script
prints the Build.cs warning.

Usage:
  python post-commit-rebuild.py [--dry-run] [--since <git-ref>]

  --dry-run    Print decisions without invoking bridge-stack.
  --since      Diff against this ref. Default: HEAD~1.

Exit codes:
  0  no rebuild needed, or rebuild succeeded
  1  rebuild failed
  2  argument or environment error
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path


STACK_ROOT = Path(__file__).resolve().parent.parent
DOCS_ROOT = STACK_ROOT.parent
PLUGIN_ROOT = DOCS_ROOT.parent
SRC_DIR = PLUGIN_ROOT / "Source"
BRIDGE_STACK = STACK_ROOT / "bridge-stack"
VENV_PYTHON = STACK_ROOT / ".venv" / "Scripts" / "python.exe"


def changed_files(since_ref: str) -> list[str]:
    """Return paths changed between since_ref and HEAD, relative to repo root."""
    try:
        out = subprocess.check_output(
            ["git", "diff", "--name-only", since_ref, "HEAD"],
            cwd=PLUGIN_ROOT,
            text=True,
            stderr=subprocess.STDOUT,
        )
    except subprocess.CalledProcessError as e:
        # Plugin may live in a sub-repo or not be a git root at all -- fall back
        # to mtime-based detection (anything modified in last 24h).
        print(f"[note] git diff failed ({e.output.strip()}); using mtime fallback")
        cutoff = (
            subprocess.check_output(["date", "+%s"], text=True).strip()
            if os.name != "nt"
            else None
        )
        return _changed_by_mtime()
    return [line.strip() for line in out.splitlines() if line.strip()]


def _changed_by_mtime(window_hours: int = 24) -> list[str]:
    """Fallback: list files under the plugin modified in the last N hours."""
    import time

    cutoff = time.time() - window_hours * 3600
    out = []
    for root in [SRC_DIR, DOCS_ROOT / "recipes"]:
        if not root.exists():
            continue
        for p in root.rglob("*"):
            try:
                if p.is_file() and p.stat().st_mtime >= cutoff:
                    out.append(str(p.relative_to(PLUGIN_ROOT)))
            except OSError:
                pass
    return out


def categorize(paths: list[str]) -> dict:
    """Classify changed paths into rebuild buckets."""
    cpp_changed = False
    recipes_changed = False
    build_cs_changed = False
    for p in paths:
        norm = p.replace("\\", "/").lower()
        if norm.startswith("source/") and (norm.endswith(".cpp") or norm.endswith(".h")):
            cpp_changed = True
        if "/docs/recipes/" in norm and norm.endswith(".yml"):
            recipes_changed = True
        if norm.endswith("forgeeditorbridge.build.cs"):
            build_cs_changed = True
    return {
        "cpp_changed": cpp_changed,
        "recipes_changed": recipes_changed,
        "build_cs_changed": build_cs_changed,
    }


def run_stack(args: list[str], dry_run: bool) -> int:
    cmd = [str(VENV_PYTHON), str(BRIDGE_STACK)] + args
    if dry_run:
        print(f"[dry-run] would run: {' '.join(cmd)}")
        return 0
    print(f"[run] {' '.join(cmd)}")
    return subprocess.call(cmd, cwd=str(STACK_ROOT))


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--since", default="HEAD~1", help="git ref to diff against (default HEAD~1)")
    a = p.parse_args()

    if not BRIDGE_STACK.exists():
        print(f"[error] bridge-stack CLI not found at {BRIDGE_STACK}", file=sys.stderr)
        return 2
    if not VENV_PYTHON.exists() and not a.dry_run:
        print(f"[error] venv python missing at {VENV_PYTHON}", file=sys.stderr)
        return 2

    paths = changed_files(a.since)
    cat = categorize(paths)
    print(f"[info] {len(paths)} changed path(s); cpp={cat['cpp_changed']} recipes={cat['recipes_changed']} build_cs={cat['build_cs_changed']}")

    rc = 0
    if cat["cpp_changed"]:
        rc = run_stack(["rebuild", "--tier", "graph"], a.dry_run) or rc
        rc = run_stack(["rebuild", "--tier", "vector"], a.dry_run) or rc
    elif cat["recipes_changed"]:
        rc = run_stack(["rebuild", "--tier", "vector", "--force"], a.dry_run) or rc

    if cat["build_cs_changed"]:
        print("[WARN] Build.cs changed. UE KG (Tier 4) may be stale. To rebuild:")
        print("       bridge-stack rebuild --tier ue_kg --ue-version 5.7")

    if cat["cpp_changed"] or cat["recipes_changed"] or cat["build_cs_changed"]:
        rc = run_stack(["map", "regen"], a.dry_run) or rc
        rc = run_stack(["verify"], a.dry_run) or rc
    else:
        print("[info] no relevant changes; nothing to rebuild")

    return rc


if __name__ == "__main__":
    sys.exit(main())
