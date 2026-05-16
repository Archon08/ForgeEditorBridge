#!/usr/bin/env python3
"""Resume the partial UE KG embedding build.

Reads classes from index.sqlite that are not yet present in
embeddings.sqlite/ue_entities, and embeds them in batches.

Run from inside the Phase 2/3 venv:
    .venv\\Scripts\\python.exe hooks\\resume_ue_kg_embeddings.py
"""
import json
import os
import sqlite3
import struct
import sys
import time
from pathlib import Path

import sqlite_vec
from sentence_transformers import SentenceTransformer

UE_VERSION = "5.7"
STACK_ROOT = Path(__file__).resolve().parent.parent
KG_DIR = STACK_ROOT.parent / "ue_kg" / UE_VERSION
INDEX_DB = KG_DIR / "index.sqlite"
EMBED_DB = KG_DIR / "embeddings.sqlite"
MODEL_ID = "nomic-ai/nomic-embed-text-v2-moe"
BATCH = 32


def floats_to_blob(arr):
    return struct.pack(f"{len(arr)}f", *(float(x) for x in arr))


def main():
    if not INDEX_DB.exists() or not EMBED_DB.exists():
        print(f"[error] missing DBs: {INDEX_DB} / {EMBED_DB}", file=sys.stderr)
        return 2

    # Open both DBs.
    idx = sqlite3.connect(str(INDEX_DB))
    idx.row_factory = sqlite3.Row

    emb = sqlite3.connect(str(EMBED_DB))
    emb.enable_load_extension(True)
    sqlite_vec.load(emb)

    # Get current state.
    n_existing = emb.execute("SELECT COUNT(*) FROM ue_entities").fetchone()[0]
    max_id_existing = emb.execute("SELECT COALESCE(MAX(id), 0) FROM ue_entities").fetchone()[0]
    existing_qnames = {
        row[0] for row in emb.execute("SELECT qualified_name FROM ue_entities")
    }
    print(f"[resume] {n_existing} entities already embedded (max id={max_id_existing})")

    # Read all classes ordered by ROWID (insertion order in the index).
    classes = list(idx.execute(
        "SELECT qualified_name, kind, module, file, line, bases, comment "
        "FROM classes ORDER BY rowid"
    ))
    n_total = len(classes)
    print(f"[resume] {n_total} total classes in index")

    # Filter to classes that don't yet have embeddings.
    pending = [c for c in classes if c["qualified_name"] not in existing_qnames]
    n_pending = len(pending)
    print(f"[resume] {n_pending} classes still need embedding")
    if n_pending == 0:
        print("[resume] nothing to do")
        return 0

    # Load model (this triggers HF cache hit).
    print(f"[resume] loading model {MODEL_ID} ...")
    model = SentenceTransformer(MODEL_ID, trust_remote_code=True)
    print("[resume] model ready")

    next_id = max_id_existing + 1
    t0 = time.time()
    encoded = 0

    # Pre-fetch members for all pending classes in one query (faster than per-class).
    pending_qnames = [c["qualified_name"] for c in pending]
    members_by_class: dict[str, list[tuple[str, str, str]]] = {q: [] for q in pending_qnames}
    placeholders = ",".join("?" * len(pending_qnames))
    if placeholders:
        # Chunk if too many params.
        CHUNK = 800
        for i in range(0, len(pending_qnames), CHUNK):
            sub = pending_qnames[i:i + CHUNK]
            ph = ",".join("?" * len(sub))
            for r in idx.execute(
                f"SELECT class_qualified, name, kind, access FROM members "
                f"WHERE class_qualified IN ({ph}) AND access = 'public'", sub
            ):
                members_by_class.setdefault(r[0], []).append((r[1], r[2], r[3]))

    # Build embedding texts.
    texts = []
    rows = []
    for c in pending:
        bases = ", ".join(json.loads(c["bases"]) or []) or "-"
        members = members_by_class.get(c["qualified_name"], [])
        method_names = [m[0] for m in members if m[1] == "method"][:20]
        prop_names = [m[0] for m in members if m[1] == "property"][:20]
        text = (
            f"Class: {c['qualified_name']}\n"
            f"Kind: {c['kind']}\n"
            f"Module: {c['module']}\n"
            f"File: {c['file']}\n"
            f"Bases: {bases}\n"
            f"Public methods: {', '.join(method_names) if method_names else '-'}\n"
            f"Public properties: {', '.join(prop_names) if prop_names else '-'}\n"
            f"Doc: {c['comment'] or '-'}\n"
        )
        texts.append(text)
        rows.append({
            "qualified_name": c["qualified_name"],
            "kind": c["kind"],
            "module": c["module"],
            "file": c["file"],
            "line": c["line"],
            "text": text,
        })

    # Batch encode + insert.
    n = len(texts)
    for start in range(0, n, BATCH):
        end = min(start + BATCH, n)
        chunk_texts = texts[start:end]
        chunk_rows = rows[start:end]
        embs = model.encode(
            chunk_texts, batch_size=BATCH, show_progress_bar=False,
            convert_to_numpy=True, normalize_embeddings=True,
        )
        for r, vec in zip(chunk_rows, embs):
            emb.execute(
                "INSERT INTO ue_entities(id, qualified_name, kind, module, header_path, line, text) "
                "VALUES(?, ?, ?, ?, ?, ?, ?)",
                (next_id, r["qualified_name"], r["kind"], r["module"],
                 r["file"], r["line"], r["text"]),
            )
            emb.execute(
                "INSERT INTO ue_vec(entity_id, embedding) VALUES (?, ?)",
                (next_id, floats_to_blob(vec)),
            )
            next_id += 1
            encoded += 1
        emb.commit()
        if start % (BATCH * 10) == 0:
            elapsed = time.time() - t0
            rate = encoded / elapsed if elapsed > 0 else 0
            eta = (n - encoded) / rate if rate > 0 else 0
            print(f"[resume] {encoded}/{n} encoded, {rate:.1f}/s, eta {eta/60:.1f}m",
                  flush=True)

    elapsed = time.time() - t0
    final_n = emb.execute("SELECT COUNT(*) FROM ue_entities").fetchone()[0]
    print(f"[resume] DONE - encoded {encoded} new entities in {elapsed:.1f}s "
          f"(total now {final_n})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
