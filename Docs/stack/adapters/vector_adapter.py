"""vector_adapter - Tier 3 semantic vector index over the ForgeEditorBridge
plugin's action catalog, recipe corpus, and domain overviews.

Backend: sentence-transformers + sqlite-vec.

Model choice (recorded here and in _bridge_map.md / _stack_status.md):
    Primary candidate was `nomic-ai/nomic-embed-code` (Phase 2 spec). That model
    is a ~7B-parameter decoder (approx. 14 GB weights), which is too heavy for
    our corpus of ~751 short chunks; encode throughput would be bottlenecked
    by a resource the index does not reward. We stepped down the fallback
    ladder to `nomic-ai/nomic-embed-text-v2-moe`: 305M active params (MoE),
    768-dim output, strong on both natural-language and code-adjacent text,
    and verified to load cleanly under sentence-transformers 5.x with
    `trust_remote_code=True` plus `einops`. Swapping models is a single-line
    change in `_DEFAULT_MODEL_ID`; `rebuild_index(force=True)` will re-embed
    everything.

Chunks come from three sources:
  - _bridge_index.json actions (one chunk per action, ~677)
  - recipes/*.yml (one chunk per recipe, ~5)
  - domain overviews (one chunk per 69 domains)

Each chunk is stored as a row in `chunks` plus a matching row in the sqlite-vec
virtual table `vec_chunks`. Cosine distance is used for ranking (lower = more
similar). Optional `WHERE kind = ?` / `WHERE domain = ?` filters are applied
via an INNER JOIN so the vec MATCH scan is not skipped.

Incremental rebuild: each chunk carries an md5 hash of its `text`. When the
bridge index / inventory / recipes change, chunks whose hash no longer matches
are re-embedded; untouched chunks are reused. `rebuild_index(force=True)`
nukes everything.
"""

from __future__ import annotations

import datetime as _dt
import hashlib
import json
import os
import sqlite3
import struct
from pathlib import Path
from typing import Optional

try:
    import yaml  # type: ignore
    _HAVE_YAML = True
except ImportError:
    yaml = None  # type: ignore
    _HAVE_YAML = False

try:
    import sqlite_vec  # type: ignore
    _HAVE_VEC = True
except ImportError:
    _HAVE_VEC = False

try:
    from sentence_transformers import SentenceTransformer  # type: ignore
    _HAVE_ST = True
except ImportError:
    SentenceTransformer = None  # type: ignore
    _HAVE_ST = False


_DEFAULT_MODEL_ID = "nomic-ai/nomic-embed-text-v2-moe"
_DEFAULT_MODEL_DIM = 768

_SCHEMA_DDL = [
    """
    CREATE TABLE IF NOT EXISTS chunks (
        id          INTEGER PRIMARY KEY AUTOINCREMENT,
        kind        TEXT NOT NULL,
        domain      TEXT,
        action      TEXT,
        recipe_id   TEXT,
        file_path   TEXT,
        line        INTEGER,
        text        TEXT NOT NULL,
        source_hash TEXT NOT NULL,
        created_at  TEXT NOT NULL
    )
    """,
    "CREATE INDEX IF NOT EXISTS idx_chunks_kind   ON chunks(kind)",
    "CREATE INDEX IF NOT EXISTS idx_chunks_domain ON chunks(domain)",
    "CREATE INDEX IF NOT EXISTS idx_chunks_recipe ON chunks(recipe_id)",
    """
    CREATE TABLE IF NOT EXISTS meta (
        key   TEXT PRIMARY KEY,
        value TEXT NOT NULL
    )
    """,
]


def _vec_table_ddl(dim: int) -> str:
    return (f"CREATE VIRTUAL TABLE IF NOT EXISTS vec_chunks USING vec0("
            f"chunk_id INTEGER PRIMARY KEY, "
            f"embedding float[{dim}])")


def _now_iso() -> str:
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _md5(s: str) -> str:
    return hashlib.md5(s.encode("utf-8")).hexdigest()


def _norm_ws(s: str) -> str:
    return " ".join((s or "").split())


def _floats_to_blob(vec) -> bytes:
    """Pack a 1-D float vector into sqlite-vec's float32 little-endian blob."""
    return struct.pack(f"<{len(vec)}f", *[float(x) for x in vec])


# ---------------------------------------------------------------------------
# Chunk builders
# ---------------------------------------------------------------------------


def _iter_action_chunks(bridge_index: dict, inventory: dict) -> list[dict]:
    """One chunk per action from _bridge_index.json.

    Pulls the action-level info from the graph index and the domain-level
    `purpose` from _inventory.json (the inventory is hand-curated).
    """
    inv_dom = {d.get("domain"): d for d in inventory.get("domains", [])}
    out: list[dict] = []
    for d in bridge_index.get("domains", []):
        domain = d.get("domain", "")
        handler_file = d.get("handler_file", "")
        file_abs = d.get("handler_file_abs", handler_file)
        purpose = (inv_dom.get(domain) or {}).get("purpose", "")
        for a in d.get("actions", []) or []:
            action = a.get("name") or ""
            aliases = a.get("aliases") or []
            handler_fn = a.get("handler_fn") or "(inline)"
            line = a.get("dispatch_line")
            alias_str = ", ".join(aliases) if aliases else "(none)"
            text_lines = [
                f"Domain: {domain}",
                f"Action: {action}",
                f"Purpose (domain): {purpose}" if purpose else "",
                f"Aliases: {alias_str}",
                f"Handler function: {handler_fn}",
                f"File: {handler_file}:{line}" if line else f"File: {handler_file}",
            ]
            text = "\n".join(t for t in text_lines if t)
            out.append({
                "kind": "action",
                "domain": domain,
                "action": action,
                "recipe_id": None,
                "file_path": file_abs,
                "line": int(line) if isinstance(line, (int, float)) else None,
                "text": text,
            })
    return out


def _parse_recipe_simple(path: Path) -> dict:
    """PyYAML if available, else a shallow regex grab of the top-level
    scalar fields + action_dependencies list."""
    import re
    text = path.read_text(encoding="utf-8")
    if _HAVE_YAML:
        try:
            data = yaml.safe_load(text) or {}
            if not isinstance(data, dict):
                return {"_raw": text}
            data["_raw"] = text
            return data
        except Exception:
            return {"_raw": text}
    out: dict = {"_raw": text}
    for key in ("id", "title", "goal", "description"):
        m = re.search(rf"^{key}\s*:\s*(.+?)\s*$", text, re.MULTILINE)
        if m:
            v = m.group(1).strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in ("'", '"'):
                v = v[1:-1]
            out[key] = v
    # multi-line description block: `description: |` then indented lines
    m = re.search(r"^description\s*:\s*[|>][-+]?\s*\n((?:[ \t]+[^\n]*\n?)+)", text, re.MULTILINE)
    if m:
        block = "\n".join(ln.strip() for ln in m.group(1).splitlines())
        out["description"] = block
    dm = re.search(r"^action_dependencies\s*:\s*\n((?:[ \t]+-[^\n]*\n?)+)",
                   text, re.MULTILINE)
    if dm:
        deps = []
        for line in dm.group(1).splitlines():
            m = re.match(r"\s*-\s*(.+?)\s*$", line)
            if m:
                deps.append(m.group(1).strip().strip('"').strip("'"))
        out["action_dependencies"] = deps
    sm = re.search(r"^domain_scope\s*:\s*\[([^\]]+)\]", text, re.MULTILINE)
    if sm:
        out["domain_scope"] = [t.strip().strip('"').strip("'")
                               for t in sm.group(1).split(",") if t.strip()]
    return out


def _iter_recipe_chunks(recipes_dir: Path) -> list[dict]:
    out: list[dict] = []
    if not recipes_dir.exists():
        return out
    for rp in sorted(recipes_dir.glob("*.yml")):
        if rp.name == "_template.yml":
            continue
        data = _parse_recipe_simple(rp)
        rid = data.get("id") or rp.stem
        title = data.get("title", "")
        goal = data.get("goal", "")
        description = data.get("description", "")
        domain_scope = data.get("domain_scope") or []
        inputs = data.get("inputs") or []
        input_lines = []
        if isinstance(inputs, list):
            for i in inputs:
                if isinstance(i, dict):
                    nm = i.get("name", "")
                    tp = i.get("type", "")
                    ds = _norm_ws(str(i.get("description", "")))[:200]
                    input_lines.append(f"- {nm} ({tp}): {ds}")
        deps = data.get("action_dependencies") or []
        if isinstance(deps, list):
            dep_line = ", ".join(str(x) for x in deps)
        else:
            dep_line = ""
        scope_line = ", ".join(domain_scope) if domain_scope else ""

        step_actions: list[str] = []
        steps = data.get("steps") or []
        if isinstance(steps, list):
            for s in steps:
                if isinstance(s, dict):
                    a = s.get("action")
                    if isinstance(a, str) and a and a != "noop":
                        step_actions.append(a)
        loops = data.get("loops") or []
        if isinstance(loops, list):
            for lp in loops:
                if isinstance(lp, dict):
                    for s in (lp.get("steps") or []):
                        if isinstance(s, dict):
                            a = s.get("action")
                            if isinstance(a, str) and a and a != "noop":
                                step_actions.append(a)
        steps_line = ", ".join(step_actions) if step_actions else ""

        variant_lines: list[str] = []
        variants = data.get("variants") or []
        if isinstance(variants, list):
            for v in variants:
                if isinstance(v, dict):
                    nm = v.get("name", "")
                    df = _norm_ws(str(v.get("differs_by", "")))[:200]
                    if nm:
                        variant_lines.append(f"- {nm}: {df}" if df else f"- {nm}")

        failure_lines: list[str] = []
        failures = data.get("failure_modes") or []
        if isinstance(failures, list):
            for f in failures:
                if isinstance(f, dict):
                    tg = _norm_ws(str(f.get("trigger", "")))[:160]
                    sm = _norm_ws(str(f.get("symptom", "")))[:200]
                    if tg:
                        failure_lines.append(f"- {tg}" + (f" - {sm}" if sm else ""))

        text = "\n".join(t for t in [
            f"Recipe: {rid}",
            f"Title: {title}" if title else "",
            f"Goal: {goal}" if goal else "",
            f"Description: {_norm_ws(str(description))}" if description else "",
            f"Domain scope: {scope_line}" if scope_line else "",
            "Inputs:" if input_lines else "",
            *input_lines,
            f"Action dependencies: {dep_line}" if dep_line else "",
            f"Steps: {steps_line}" if steps_line else "",
            "Variants:" if variant_lines else "",
            *variant_lines,
            "Failure modes:" if failure_lines else "",
            *failure_lines,
        ] if t)
        out.append({
            "kind": "recipe",
            "domain": None,
            "action": None,
            "recipe_id": rid,
            "file_path": str(rp),
            "line": None,
            "text": text,
        })
    return out


def _iter_domain_chunks(bridge_index: dict, inventory: dict) -> list[dict]:
    inv_dom = {d.get("domain"): d for d in inventory.get("domains", [])}
    out: list[dict] = []
    for d in bridge_index.get("domains", []):
        domain = d.get("domain", "")
        handler_file = d.get("handler_file", "")
        file_abs = d.get("handler_file_abs", handler_file)
        purpose = (inv_dom.get(domain) or {}).get("purpose", "")
        actions = d.get("actions", []) or []
        key_actions = [a.get("name", "") for a in actions[:10]]
        text_lines = [
            f"Domain: {domain}",
            f"Handler file: {handler_file}",
            f"Purpose: {purpose}" if purpose else "",
            f"Action count: {len(actions)}",
            f"Key actions: {', '.join(key_actions)}" if key_actions else "",
        ]
        text = "\n".join(t for t in text_lines if t)
        out.append({
            "kind": "domain_overview",
            "domain": domain,
            "action": None,
            "recipe_id": None,
            "file_path": file_abs,
            "line": None,
            "text": text,
        })
    return out


def _build_all_chunks(bridge_index_path: Path, inventory_path: Path,
                      recipes_dir: Path) -> list[dict]:
    bi = json.loads(bridge_index_path.read_text(encoding="utf-8"))
    inv = json.loads(inventory_path.read_text(encoding="utf-8"))
    out: list[dict] = []
    out.extend(_iter_action_chunks(bi, inv))
    out.extend(_iter_recipe_chunks(recipes_dir))
    out.extend(_iter_domain_chunks(bi, inv))
    # Normalize `text` whitespace and attach source_hash.
    for c in out:
        c["text"] = c["text"].strip()
        c["source_hash"] = _md5(c["text"])
    return out


# ---------------------------------------------------------------------------
# Adapter
# ---------------------------------------------------------------------------


class VectorAdapter:
    """Tier 3 semantic index. Backend: sentence-transformers + sqlite-vec."""

    def __init__(self, stack_root: str, db_path: str,
                 model_id: str = _DEFAULT_MODEL_ID) -> None:
        self.stack_root = Path(stack_root).resolve()
        # Docs dir is one level up from stack/.
        self.docs_dir = self.stack_root.parent
        self.db_path = Path(db_path).resolve()
        self.model_id = model_id
        self._model = None  # lazy
        self._db: Optional[sqlite3.Connection] = None
        self._model_dim = _DEFAULT_MODEL_DIM

    # -- lazy resources ----------------------------------------------------

    def _ensure_model(self) -> None:
        if self._model is not None:
            return
        if not _HAVE_ST:
            raise RuntimeError(
                "sentence-transformers is not installed. Run "
                "`<stack>/.venv/Scripts/pip install sentence-transformers`.")
        self._model = SentenceTransformer(self.model_id, trust_remote_code=True)
        # Probe dim.
        try:
            dim = int(self._model.get_sentence_embedding_dimension() or _DEFAULT_MODEL_DIM)
            if dim > 0:
                self._model_dim = dim
        except Exception:
            pass

    def _ensure_db(self) -> sqlite3.Connection:
        if self._db is not None:
            return self._db
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        con = sqlite3.connect(str(self.db_path))
        if not _HAVE_VEC:
            con.close()
            raise RuntimeError(
                "sqlite-vec is not installed. Run "
                "`<stack>/.venv/Scripts/pip install sqlite-vec`.")
        con.enable_load_extension(True)
        sqlite_vec.load(con)
        con.enable_load_extension(False)
        for ddl in _SCHEMA_DDL:
            con.execute(ddl)
        # vec table depends on model dim; probe model before creating so the
        # column width is right.
        self._ensure_model()
        con.execute(_vec_table_ddl(self._model_dim))
        con.commit()
        self._db = con
        return con

    # -- meta helpers ------------------------------------------------------

    def _meta_get(self, key: str) -> Optional[str]:
        con = self._ensure_db()
        r = con.execute("SELECT value FROM meta WHERE key = ?", (key,)).fetchone()
        return r[0] if r else None

    def _meta_set(self, key: str, value: str) -> None:
        con = self._ensure_db()
        con.execute(
            "INSERT INTO meta(key, value) VALUES(?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
            (key, value),
        )
        con.commit()

    # -- public contract ---------------------------------------------------

    def rebuild_index(self, force: bool = False) -> dict:
        """Re-embed all chunks. When force=False, chunks whose source_hash
        already exists in the DB are reused."""
        self._ensure_model()
        con = self._ensure_db()

        chunks = _build_all_chunks(
            self.docs_dir / "_bridge_index.json",
            self.docs_dir / "_inventory.json",
            self.docs_dir / "recipes",
        )

        if force:
            con.execute("DELETE FROM vec_chunks")
            con.execute("DELETE FROM chunks")
            con.commit()
            existing_hashes: dict = {}
        else:
            existing_hashes = {
                row[0]: row[1]
                for row in con.execute("SELECT source_hash, id FROM chunks").fetchall()
            }

        desired_hashes = {c["source_hash"] for c in chunks}

        # Delete rows whose hash is no longer desired (incremental prune).
        if not force:
            stale_ids = [
                rid for h, rid in existing_hashes.items() if h not in desired_hashes
            ]
            if stale_ids:
                qmarks = ",".join("?" * len(stale_ids))
                con.execute(f"DELETE FROM vec_chunks WHERE chunk_id IN ({qmarks})", stale_ids)
                con.execute(f"DELETE FROM chunks WHERE id IN ({qmarks})", stale_ids)
                con.commit()

        new_chunks = [c for c in chunks if c["source_hash"] not in existing_hashes]

        encoded = 0
        if new_chunks:
            texts = [c["text"] for c in new_chunks]
            # Encode in small batches to play nice with CPU runtime.
            embs = self._model.encode(
                texts,
                batch_size=16,
                show_progress_bar=False,
                convert_to_numpy=True,
                normalize_embeddings=True,
            )
            now = _now_iso()
            for c, emb in zip(new_chunks, embs):
                cur = con.execute(
                    "INSERT INTO chunks"
                    " (kind, domain, action, recipe_id, file_path, line,"
                    "  text, source_hash, created_at)"
                    " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (c["kind"], c["domain"], c["action"], c["recipe_id"],
                     c["file_path"], c["line"], c["text"], c["source_hash"], now),
                )
                cid = cur.lastrowid
                con.execute(
                    "INSERT INTO vec_chunks(chunk_id, embedding) VALUES(?, ?)",
                    (cid, _floats_to_blob(emb)),
                )
                encoded += 1
            con.commit()

        self._meta_set("model_id", self.model_id)
        self._meta_set("model_dim", str(self._model_dim))
        self._meta_set("last_build_iso", _now_iso())

        return {
            "chunks_total": con.execute("SELECT COUNT(*) FROM chunks").fetchone()[0],
            "newly_embedded": encoded,
            "reused": len(chunks) - encoded,
            "force": force,
            "model_id": self.model_id,
            "model_dim": self._model_dim,
        }

    def rebuild_incremental(self, changed_files: list[str]) -> dict:
        """Re-embed only chunks touching the changed files.

        Since our chunks are derived from _bridge_index.json / _inventory.json
        / recipes/, any change to those sources already triggers a hash diff
        via `rebuild_index(force=False)`. This method is a thin wrapper that
        also prunes any chunk whose file_path was passed in (useful when a
        recipe is deleted and the hash diff alone would not catch it).
        """
        con = self._ensure_db()
        if not changed_files:
            return self.rebuild_index(force=False)
        # Normalize.
        norm = {str(Path(f).resolve()) for f in changed_files}
        stale_ids = [
            row[0]
            for row in con.execute("SELECT id, file_path FROM chunks").fetchall()
            if row[1] and str(Path(row[1]).resolve()) in norm
        ]
        if stale_ids:
            qmarks = ",".join("?" * len(stale_ids))
            con.execute(f"DELETE FROM vec_chunks WHERE chunk_id IN ({qmarks})", stale_ids)
            con.execute(f"DELETE FROM chunks WHERE id IN ({qmarks})", stale_ids)
            con.commit()
        return self.rebuild_index(force=False)

    def search(self, query: str, k: int = 5,
               filter: Optional[dict] = None) -> list[dict]:
        """Top-k cosine-similarity hits. `filter` can constrain `kind` and/or
        `domain`. Returns [{kind, domain, action, recipe_id, file, line,
        score, snippet}] in descending similarity order."""
        if not query or not query.strip():
            return []
        self._ensure_model()
        con = self._ensure_db()
        q_emb = self._model.encode(
            [query], show_progress_bar=False, convert_to_numpy=True,
            normalize_embeddings=True,
        )[0]
        blob = _floats_to_blob(q_emb)

        # Filters are applied post-ANN to keep vec MATCH efficient. Grab extra
        # candidates so filters still yield k hits in most cases.
        filt = filter or {}
        want_kind = filt.get("kind")
        want_domain = filt.get("domain")
        overshoot = k * (4 if (want_kind or want_domain) else 1) + 5

        rows = con.execute(
            "SELECT v.chunk_id, v.distance, c.kind, c.domain, c.action, c.recipe_id,"
            "       c.file_path, c.line, c.text "
            "FROM vec_chunks v JOIN chunks c ON c.id = v.chunk_id "
            "WHERE v.embedding MATCH ? AND k = ? "
            "ORDER BY v.distance ASC",
            (blob, overshoot),
        ).fetchall()

        out: list[dict] = []
        for r in rows:
            _cid, distance, kind, domain, action, recipe_id, fp, line, text = r
            if want_kind and kind != want_kind:
                continue
            if want_domain and domain != want_domain:
                continue
            # Cosine distance -> similarity: with normalized embeddings, sqlite-vec
            # returns distance in [0, 2]. score = 1 - distance/2 maps to [0, 1].
            score = max(0.0, 1.0 - (float(distance) / 2.0))
            snippet = _norm_ws(text)[:240]
            out.append({
                "kind": kind,
                "domain": domain,
                "action": action,
                "recipe_id": recipe_id,
                "file": fp,
                "line": line,
                "score": score,
                "snippet": snippet,
            })
            if len(out) >= k:
                break
        return out

    def stats(self) -> dict:
        """Return DB counts and metadata."""
        con = self._ensure_db()
        by_kind: dict = {}
        for kind, cnt in con.execute(
                "SELECT kind, COUNT(*) FROM chunks GROUP BY kind").fetchall():
            by_kind[kind] = cnt
        total = con.execute("SELECT COUNT(*) FROM chunks").fetchone()[0]
        size_mb = 0.0
        if self.db_path.exists():
            size_mb = self.db_path.stat().st_size / (1024.0 * 1024.0)
        return {
            "chunks_total": total,
            "by_kind": by_kind,
            "model_id": self._meta_get("model_id") or self.model_id,
            "model_dim": int(self._meta_get("model_dim") or self._model_dim),
            "db_size_mb": round(size_mb, 2),
            "last_build_iso": self._meta_get("last_build_iso") or "",
            "db_path": str(self.db_path),
        }

    def verify(self) -> dict:
        """Sanity checks. Returns {ok: bool, issues: [str], info: {...}}."""
        issues: list[str] = []
        con = self._ensure_db()
        s = self.stats()
        # Check that vec_chunks and chunks are in sync.
        vc = con.execute("SELECT COUNT(*) FROM vec_chunks").fetchone()[0]
        if vc != s["chunks_total"]:
            issues.append(f"vec_chunks count ({vc}) != chunks count ({s['chunks_total']})")
        # Expected chunk count from source.
        bi = json.loads((self.docs_dir / "_bridge_index.json").read_text(encoding="utf-8"))
        n_actions = sum(len(d.get("actions", []) or []) for d in bi.get("domains", []))
        n_domains = len(bi.get("domains", []))
        recipes_dir = self.docs_dir / "recipes"
        n_recipes = 0
        if recipes_dir.exists():
            n_recipes = sum(1 for p in recipes_dir.glob("*.yml") if p.name != "_template.yml")
        expected = n_actions + n_recipes + n_domains
        if s["chunks_total"] != expected:
            issues.append(
                f"chunks_total ({s['chunks_total']}) != expected ({expected}) "
                f"[{n_actions} actions + {n_recipes} recipes + {n_domains} domains]")
        # Model dim match.
        stored_dim = int(self._meta_get("model_dim") or 0)
        if stored_dim and stored_dim != _DEFAULT_MODEL_DIM and stored_dim != self._model_dim:
            issues.append(f"stored model_dim {stored_dim} does not match loaded {self._model_dim}")
        return {
            "ok": not issues,
            "issues": issues,
            "info": {
                "expected_chunks": expected,
                "actual_chunks": s["chunks_total"],
                "by_kind": s["by_kind"],
                "model_id": s["model_id"],
                "model_dim": s["model_dim"],
            },
        }
