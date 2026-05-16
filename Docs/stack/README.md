# ForgeEditorBridge Cognitive Stack

## Getting Started

This stack ships with Tiers 0-2 pre-built. Tier 3 (vector search) and Tier 4 (UE knowledge graph) are regenerable on first use.

### Prerequisites
- Python 3.10+
- ~2 GB free disk for the venv + indexes (much more if rebuilding Tier 4)
- For Tier 4 only: a local UE 5.7 install (configurable in `ue_kg_manifest.json`)
- For Tier 3 only: network access for a one-time ~580 MB HuggingFace model download

### First-run setup

**Windows (PowerShell or cmd):**

```powershell
cd Docs\stack
python -m venv .venv
.\.venv\Scripts\python -m pip install --upgrade pip
.\.venv\Scripts\python -m pip install tree-sitter tree-sitter-cpp networkx pyyaml pyparsing sentence-transformers sqlite-vec einops
```

**macOS / Linux:**

```bash
cd Docs/stack
python3 -m venv .venv
./.venv/bin/python -m pip install --upgrade pip
./.venv/bin/python -m pip install tree-sitter tree-sitter-cpp networkx pyyaml pyparsing sentence-transformers sqlite-vec einops
```

Quick smoke test (Windows path shown; on macOS/Linux use `./.venv/bin/python` instead):

```powershell
.\.venv\Scripts\python bridge-stack status
```

You should see Tiers 0-2 live and Tiers 3-4 pending.

### Build the optional tiers when you want them

```powershell
# Tier 3 - semantic search over actions/recipes/domains
.\.venv\Scripts\python bridge-stack rebuild --tier vector --force

# Tier 4 - UE 5.7 engine knowledge graph (~2 hours on CPU first build)
.\.venv\Scripts\python bridge-stack rebuild --tier ue_kg --ue-version 5.7
```

After Tier 3 finishes you can run `bridge-stack search "<question>"`. After Tier 4 finishes you can run `bridge-stack ue-kg query "<question>"`.

## 1. What this is

The cognitive stack is a 4-tier index over the ForgeEditorBridge plugin that lets an LLM answer "which handler, which action, which callsite, which engine API" questions without loading the entire source tree into context. Tier 0 is the hand-authored Map plus an inventory-generated domain catalog. Tier 1 is a recipes library of goal-to-actions playbooks. Tier 2 is a code graph over the plugin (Graphify), Tier 3 is a semantic vector index (nomic-embed-text-v2-moe + sqlite-vec), and Tier 4 is a UE engine knowledge graph (hybrid tree-sitter + regex + sqlite-vec over in-use engine modules). The stack exists so upgrades to the plugin, the engine, or an adapter remain cheap - the Map stays schema-stable while everything below it is regenerated on demand.

## 2. File layout

```
Docs/
  _bridge_map.md            # Tier 0 Map (hand-authored + regen zones)
  _inventory.json           # Handler/action inventory (source of truth)
  _stack_status.md          # One-screen phase + tier dashboard
  recipes/                  # Tier 1 playbooks
    _template.yml
    wire_pbr_material_channel_packed.yml
    [etc]
  stack/
    bridge-stack            # Python orchestrator CLI
    bridge-stack.bat        # Windows wrapper
    adapters/               # graph_adapter.py, vector_adapter.py, ue_kg_adapter.py
    README.md               # This file
    .venv/                  # Python venv for vector/ue_kg deps (gitignored)
    .cache/                 # Tier 2 incremental graph cache (gitignored, safe to delete)
  .vectors/                 # Tier 3 artifact (gitignored, rebuild required)
  ue_kg/                    # Tier 4 artifact (gitignored, rebuild required)
    5.7/
    5.8/
```

## 3. The scalability contract

- The Map is schema-stable. Zone markers `<!-- BEGIN:X -->` / `<!-- END:X -->` delimit regenerable content; hand-authored text outside those markers is never touched by tooling.
- Everything below Tier 0 is regenerable from source. Lose `.vectors/` or `ue_kg/`? Re-run the relevant `bridge-stack rebuild --tier ...` and keep moving.
- A single orchestrator `bridge-stack` drives all tiers. No per-tool CLIs leak into the day-to-day loop.
- Each tier exposes a narrow adapter interface (see section 7). Swap the implementation - Graphify for another graph tool, sqlite-vec for LanceDB - without the Map or recipes caring.
- UE version is a first-class parameter. Tier 4 builds live under `ue_kg/<version>/` so 5.7 and 5.8 indexes coexist during upgrades.
- Recipes reference plugin actions by stable `domain.action` names, not file paths. When the plugin refactors, recipes survive.

## 4. Upgrade playbooks

### New handlers added to the plugin

```
bridge-stack rebuild --tier graph
bridge-stack rebuild --tier vector
bridge-stack map regen
bridge-stack verify
```

Graph and vector indexes pick up the new code; `map regen` refreshes DOMAIN_CATALOG from the updated `_inventory.json`; `verify` confirms recipe action_dependencies still resolve.

### UE 5.8 upgrade

```
# 1. Install UE 5.8 via Epic Games Launcher.
# 2. Update Build.cs module list if engine APIs moved.
bridge-stack rebuild --tier ue_kg --ue-version 5.8
bridge-stack map regen
bridge-stack verify
# 4. Review any recipes flagged as referencing deprecated engine symbols.
```

Tier 4 is the only tier that fully rebuilds on engine upgrade. The 5.7 index can stay in `ue_kg/5.7/` as a cross-reference until the port is complete.

### Adapter swap (e.g., replace Graphify)

```
# 1. Rewrite stack/adapters/graph_adapter.py against the new tool, keeping the
#    contract from section 7 intact.
bridge-stack rebuild --tier graph
bridge-stack verify
```

Recipes and the Map are unaffected because they never call the adapter directly; `bridge-stack` routes queries through it.

## 5. Tier status

| Tier | Implementation | Status |
|---|---|---|
| 0 Map | Hand-authored + `bridge-stack map regen` | shipped |
| 1 Recipes | YAML playbooks under `Docs/recipes/` | shipped |
| 2 Graph | tree-sitter-cpp dispatch + callsite index | shipped |
| 3 Vector | sentence-transformers + sqlite-vec semantic search | rebuild required |
| 4 UE KG | hybrid tree-sitter + regex + networkx + sqlite-vec | rebuild required |

See `_stack_status.md` for the live dashboard and `_bridge_map.md` for the Map itself.

### Tier 2 (graph) backend choice

Backend: `tree-sitter-cpp` (with `graphifyy` + `networkx` also installed in the venv).

Graphify's PyPI package (`graphifyy`) ships primarily as a CLI that produces an LLM-friendly natural-language knowledge graph in `graphify-out/graph.json`. Its schema is optimized for conversational traversal, not for the narrow, deterministic "domain.action -> dispatch line / callsite" lookups the stack contract actually needs. Rather than translate Graphify's JSON into our schema on every rebuild, the adapter uses `tree-sitter-cpp` directly. `tree-sitter-cpp` is the same parser Graphify uses internally for C++, so accuracy is equivalent; we just keep control of the node/edge schema end-to-end.

The `GraphAdapter` interface is the stable contract. Swapping to Graphify-proper, LanceDB-based call indexing, or any other tool in a future phase is a single-file change in `adapters/graph_adapter.py`. Fallback backends (`regex`-only) exist and trip automatically if `tree-sitter-cpp` is unavailable at runtime.

## 6. Query routing

The Map owns the routing table. Short version of the handoffs:

| Ask | Route to |
|---|---|
| "Which handler owns X?" | Map (DOMAIN_CATALOG) |
| "How do I accomplish goal G?" | Recipes |
| "Who calls function F? Where is action dispatched?" | Tier 2 graph |
| "What plugin code looks semantically like my query?" | Tier 3 vector |
| "What UE API does Y, and is member Z deprecated?" | Tier 4 UE KG (`ue-kg query` / `ue-kg deprecated`) |
| "Signature of UClass::Method?" | Tier 4 UE KG (`ue-kg sig`) |
| "Public members of UClass?" | Tier 4 UE KG (`ue-kg members`) |

Always start at Tier 0. Fall through only when the cheaper tier cannot answer.

## 6b. Command reference

### Core
```
bridge-stack status                # dashboard
bridge-stack verify                # sanity checks across live tiers
bridge-stack map regen             # regenerate Map zones from inventory + recipes
bridge-stack rebuild --tier graph  # rebuild Tier 2 (also chains `map regen`)
```

### Tier 2 graph (shipped)
```
bridge-stack graph list-domains            # all handler domains with action counts
bridge-stack graph list-actions <domain>   # actions in one domain, with aliases + dispatch lines
bridge-stack graph query-action <d> <a>    # locate domain.action in source
bridge-stack graph query-caller <fn>       # callsites of a function (bare name or Class::Method)
bridge-stack graph stats                   # backend, counts, inventory cross-check
```

Example:
```
bridge-stack graph query-action actor spawn_actor
bridge-stack graph list-actions pcg_graph
bridge-stack graph query-caller ResolveClass
```

The Tier 2 index lives at `Docs/_bridge_index.json`. Delete it and run `bridge-stack rebuild --tier graph` to rebuild from scratch. An incremental graph cache builds at `Docs/stack/.cache/` on first rebuild and is safe to delete at any time.

### Tier 3 vector (rebuild required)

```
bridge-stack rebuild --tier vector            # incremental re-embed (hash-based reuse)
bridge-stack rebuild --tier vector --force    # nuke + full re-embed
bridge-stack search "<query>"                 # semantic search, k=5 by default
bridge-stack search "<query>" --k 10          # top-10 hits
bridge-stack search "<query>" --kind action   # restrict to action chunks
bridge-stack search "<query>" --domain pcg_graph
bridge-stack vector stats                     # DB size, chunk counts, model info
```

Example:
```
bridge-stack search "wire 13 textures into a PBR material"
bridge-stack search "set texture compression for a normal map" --kind action
bridge-stack search "spawn with retry"
```

The Tier 3 DB lives at `Docs/.vectors/chunks.sqlite` (gitignored). Delete it and run `bridge-stack rebuild --tier vector` to rebuild from scratch. The HF model cache (~580 MB) lives outside the project at `~/.cache/huggingface/`.

### Tier 4 UE KG (rebuild required)
```
bridge-stack rebuild --tier ue_kg --ue-version 5.7   # full extraction + embed
bridge-stack rebuild --tier ue_kg                    # default UE version from manifest
bridge-stack ue-kg query "<question>" [--k N]        # free-form semantic query
bridge-stack ue-kg deprecated [--class C] [--module M]
bridge-stack ue-kg sig <Class::Method>               # full signature lookup
bridge-stack ue-kg members <ClassName> [--scope public|protected|private|all]
bridge-stack ue-kg callers <Class::Method>           # best-effort engine call sites
bridge-stack ue-kg stats [--ue-version 5.7]
```

Example:
```
bridge-stack ue-kg query "what replaces UStaticMesh::NaniteSettings"
bridge-stack ue-kg deprecated --class UStaticMesh
bridge-stack ue-kg sig UGameplayAbility::GetAssetTags
bridge-stack ue-kg members UPCGSettings --scope public
bridge-stack ue-kg deprecated --module GameplayAbilities
```

The Tier 4 artifacts live at `Docs/ue_kg/<ue_version>/` (gitignored):
- `kg.pickle` - NetworkX DiGraph (modules / headers / classes / members / deprecation markers)
- `index.sqlite` - O(log n) lookup index for class + member queries
- `embeddings.sqlite` - sqlite-vec table over class/struct embeddings (768-dim, MoE model from Phase 2)

`Docs/stack/ue_kg_manifest.json` is the input: a list of 87 UE modules (resolved from `ForgeEditorBridge.Build.cs`) with their header roots and module-types. Generated by `Docs/stack/build_manifest.py`. Total scope: ~9237 headers, ~1.78M LOC.

#### Backend rationale (why hybrid, not pure LightRAG)
- HKU's `lightrag-hku` library uses an LLM for entity-relation extraction. Running an LLM over 9000+ engine headers is slow, expensive, and non-deterministic.
- UE engine code is highly structured C++. Class boundaries, member declarations, `UE_DEPRECATED(...)` markers, base classes, and `#include` graphs all parse out cleanly without an LLM.
- Tree-sitter-cpp alone is not enough: UE's `UCLASS(...)` macro on top of multi-base inheritance (`class UStaticMesh : public X, public Y, public Z, public W`) plus `#if WITH_EDITOR` blocks puts tree-sitter into ERROR nodes around a large fraction of the engine's UCLASS-decorated types (UStaticMesh was the canary). The adapter uses a regex sweep + balanced-brace scanner to find class boundaries, then per-line extraction within each body for members. Tree-sitter-cpp is loaded but optional.
- The result: deterministic, cheap, incremental, and version-keyed. LightRAG remains an option for free-form `.md` engine docs in a future revision; for the structured C++ surface the hybrid path is the right tool.

#### KG schema
Nodes:
- `module::<name>` - one per module from the manifest; carries `module_type` (Runtime / Editor / Plugin / etc.).
- `header::<absolute_path>` - one per `.h` walked.
- `class::<qualified_name>` - one per class or struct with a body. Carries `kind`, `module`, `file`, `line`.
- `class::<base_name>` (as `class_ref`) - referenced base classes that may not have a parsed body.
- `member::<qualified>::<name>@<line>` - one per method or property. Carries `kind` (method / property), `access` (public / protected / private / internal), `deprecated`.
- `deprecation::<version>` - one per UE version that deprecated something (e.g. `deprecation::5.7`).

Edges:
- `contains_header` (module -> header)
- `declares_class` (header -> class)
- `contains_class` (module -> class)
- `inherits` (class -> class_ref)
- `has_member` (class -> member)
- `deprecated_in` (member -> deprecation_marker)

#### UE upgrade playbook (parameterized by --ue-version)
When UE 5.8 lands:
```
# 1. Install UE 5.8 via Epic Games Launcher.
# 2. Update Build.cs module list if engine APIs moved.
# 3. Regenerate the manifest against the 5.8 engine root:
python Docs/stack/build_manifest.py     # first edit the ENGINE_ROOT constant at the top of build_manifest.py (default: C:\Program Files\Epic Games\UE_5.7\Engine) so it points at your UE 5.8 install

# 4. Build the UE 5.8 KG. This creates Docs/ue_kg/5.8/ alongside the existing 5.7 index.
bridge-stack rebuild --tier ue_kg --ue-version 5.8

# 5. Map regen + verify chain.
bridge-stack map regen
bridge-stack verify

# 6. Query both versions side by side to find broken APIs:
bridge-stack ue-kg sig <Class::Method> --ue-version 5.7
bridge-stack ue-kg sig <Class::Method> --ue-version 5.8
bridge-stack ue-kg deprecated --module <YourModule> --ue-version 5.8
```

The 5.7 index can stay in `ue_kg/5.7/` as a cross-reference until the port is complete. Delete it when you're ready: `rm -rf Docs/ue_kg/5.7/`.

#### Backend rationale
- **Embedding model**: `nomic-ai/nomic-embed-text-v2-moe` (768-dim, MoE, 305M active params). The Phase 2 spec called for `nomic-embed-code` but at ~7B params / ~14 GB weights it is poorly matched to a corpus of ~751 short structured chunks - encode throughput is bottlenecked on a resource the index does not reward. The MoE model is rock-solid on natural-language and code-adjacent text and downloads cleanly under sentence-transformers. Swap by editing `_DEFAULT_MODEL_ID` in `adapters/vector_adapter.py` and running `--force`.
- **Vector store**: `sqlite-vec` loaded as a SQLite extension. Chosen over LanceDB / FAISS because the entire index fits in a single 3.5 MB file, the dependency surface is tiny, and the schema is dimension-agnostic. The vec0 virtual table sits next to a plain `chunks` table; filters apply via JOIN, not vec scan.
- **Distance**: Cosine. Embeddings are L2-normalized at encode time so the returned distance maps cleanly to cosine; `score = 1 - distance/2` lands in `[0, 1]` for display.

#### Chunk strategy
Three chunk kinds, all built from the same sources Tier 0 / 1 / 2 already use:

- **action** (677 chunks): one per `_bridge_index.json` action. Includes domain, action name, aliases, handler function, file:line, plus the inventory-curated domain `purpose`. Carries enough context to match queries like "where is the X-y-Z action".
- **recipe** (10 chunks): one per `recipes/*.yml` (skipping `_template.yml`). Includes id, title, goal, normalized description, domain_scope, inputs (name + type + description), action_dependencies. High-quality matches for compound-task queries.
- **domain_overview** (69 chunks): one per domain. Handler file, curated purpose, action count, top 10 action names. Useful when the right answer is "look at the whole domain".

Total = **756 chunks**. Each chunk text is well under 500 tokens; whitespace is normalized; an md5 hash gates incremental rebuild.

## 6c. Authoring a new recipe

Recipes are hand-curated YAML files under `Docs/recipes/`. To add one:

1. Copy `Docs/recipes/_template.yml` to a new file named `<snake_case_id>.yml`.
2. Fill in the required top-level keys:
   - `recipe_schema_version: "1.0"`
   - `id` (snake_case, must match the filename stem)
   - `title` (one-line human description)
   - `goal` (one-paragraph what-and-why)
   - `description` (block scalar, longer explanation)
   - `domain_scope: [...]` (list of handler domains the recipe touches)
   - `tested_on_ue: ["5.7"]`
   - `tested_on_plugin: ["<current plugin version>"]`
   - `preconditions` (free-form list)
   - `inputs` (list of `{name, type, description, required}`)
   - `outputs` (list of `{name, type, description}`)
   - `action_dependencies` (list of `domain/action` strings)
   - `steps` (ordered list of dispatchable actions)
3. Optional keys: `decision_points`, `loops`, `failure_modes`, `variants`, `notes`.
4. Run `bridge-stack verify` - it parses every recipe, resolves each `action_dependencies` entry against `_inventory.json`, and reports any unresolved references.
5. Run `bridge-stack rebuild --tier graph` to refresh the graph index, then `bridge-stack map regen` to rewrite the RECIPE_CATALOG zone in `_bridge_map.md`.

If an action depends on a verb that has not been implemented yet, mark the recipe with the string `pending_plugin_support` anywhere in the YAML - `verify` will downgrade the missing-action check to a warning rather than an error for that recipe.

## 6d. Regenerating `_inventory.json`

`_inventory.json` is **hand-curated**, not auto-generated. It is the source of truth for:

- The `purpose` field shown in the DOMAIN_CATALOG zone of `_bridge_map.md`.
- The `aliases` field per action (a personalization hook - see the LLM_OPERATOR_GUIDE for details).
- The headline `counts` shown by `bridge-stack status`.

When a new handler ships in C++ source:

1. Run `bridge-stack rebuild --tier graph` first - this writes `_bridge_index.json` from the C++ dispatch table.
2. Open `Docs/_inventory.json` and add a new entry under `domains[]` mirroring the new handler's domain. Fill in:
   - `domain`: canonical name returned by the handler's `GetDomainName()` override.
   - `handler_file`: short filename (e.g. `MyNewHandler.cpp`).
   - `purpose`: one-sentence description of what this handler does.
   - `actions[]`: one entry per action, each with `name`, `dispatch_line` (copy from the graph index), and `aliases: []`.
3. Update the top-level `counts.handlers` and `counts.actions_distinct` to match the graph index.
4. Run `bridge-stack map regen` to push the new domain into DOMAIN_CATALOG.
5. Run `bridge-stack verify` to confirm consistency.

If you skip the inventory step, the graph still has the new actions, but the map's DOMAIN_CATALOG row for the new handler will render with a blank `purpose` column.

## 6e. What `bridge-stack map regen` rewrites

The map has three machine-managed zones, each delimited by `<!-- BEGIN:NAME -->` / `<!-- END:NAME -->` markers. `bridge-stack map regen` rewrites exactly these zones; everything outside them is hand-authored and preserved verbatim:

- `DOMAIN_CATALOG`: handler-by-handler table built from `_bridge_index.json` (source of truth) joined to `_inventory.json` (for the `purpose` column).
- `RECIPE_CATALOG`: id/title/goal table built from `Docs/recipes/*.yml`.
- `INDEX_FRESHNESS`: per-tier last-built timestamps and rebuild commands.

If you need to refresh the catalog after adding a handler or recipe, run `bridge-stack rebuild --tier graph` (which auto-chains `map regen`) or just `bridge-stack map regen` directly when only the inventory or recipes changed.

## 6f. What `bridge-stack verify` checks

`verify` is a fast sanity sweep. It runs all of these and prints `PASS` / `FAIL` with per-check status:

- **Inventory**: parses `_inventory.json`, confirms it has at least 50 domains, builds a domain-action index.
- **Map**: confirms `_bridge_map.md` exists and starts with a valid YAML frontmatter block.
- **Recipes**: parses every `*.yml` in `Docs/recipes/`, confirms `id` and `goal` are present, and resolves every `action_dependencies` entry against the inventory index. Missing actions are errors unless the recipe is tagged `pending_plugin_support` (then they become warnings).
- **Graph index**: if `_bridge_index.json` exists, parses it and confirms domains were extracted.
- **Vector index**: if `Docs/.vectors/chunks.sqlite` exists, opens it and confirms `chunks_total` matches `actions + recipes + domain_overviews` from the graph index. Mismatches are warnings (incremental rebuild needed).
- **UE KG**: if `Docs/ue_kg/<version>/` exists, confirms the index files are readable and reports class/method/deprecation counts.

`verify` exits non-zero on any error and zero on warnings-only.

## 7. Contracts (adapter interfaces)

Each adapter lives in `stack/adapters/<name>.py` and exposes exactly the functions listed here. Adapters are the only place tool-specific code lives.

### graph_adapter (Tier 2, shipped)

```python
class GraphAdapter:
    def __init__(self, plugin_src_root: str, index_cache_path: str): ...

    def rebuild(self) -> dict:
        """Rebuilds the graph from source. Writes cache. Returns stats."""

    def query_caller(self, fn_name: str) -> list[dict]:
        """[{file, line, caller_fn, context}] for all callers of fn_name.
        Matches either a bare name or a Class::Method qualified form."""

    def query_action_line(self, domain: str, action: str) -> tuple[str, int] | None:
        """(handler_file_abs, dispatch_line) for a given domain.action."""

    def list_domains(self) -> list[str]:
        """Sorted domain names extracted from dispatch tables + inventory carry-forward."""

    def list_actions(self, domain: str) -> list[dict]:
        """[{action, dispatch_line, handler_fn}] for a domain."""

    def who_uses_ue_api(self, api_symbol: str) -> list[dict]:
        """[{file, line, domain, action, caller}] for plugin call sites of a UE API."""

    def graph_stats(self) -> dict:
        """{nodes, edges, files_indexed, domains, actions, last_build_iso, backend}"""
```

Backends (in preference order): `tree-sitter` (primary, live), `regex` (lossy fallback if tree-sitter-cpp unavailable). The backend used in the most recent build is recorded in `_bridge_index.json` under `graph_backend`.

### vector_adapter (Tier 3, rebuild required)

```python
class VectorAdapter:
    def __init__(self, stack_root: str, db_path: str, model_id: str): ...

    def rebuild_index(self, force: bool = False) -> dict:
        """Re-embed all chunks. Returns {chunks_total, newly_embedded, reused,
        force, model_id, model_dim}."""

    def rebuild_incremental(self, changed_files: list[str]) -> dict:
        """Drop chunks whose file_path is in changed_files, then run a normal
        hash-aware rebuild. Returns rebuild stats."""

    def search(self, query: str, k: int = 5,
               filter: dict | None = None) -> list[dict]:
        """Top-k semantic hits. Each hit:
        {kind, domain, action, recipe_id, file, line, score, snippet}.
        `filter` accepts {kind: 'action'|'recipe'|'domain_overview',
        domain: '<domain>'}."""

    def stats(self) -> dict:
        """{chunks_total, by_kind, model_id, model_dim, db_size_mb,
        last_build_iso, db_path}."""
```

Backend: `sentence-transformers` (encoding) + `sqlite-vec` (ANN over cosine distance). The model id is recorded in the `meta` table at build time so a query-time mismatch is detectable.

### ue_kg_adapter (Tier 4, rebuild required)

```python
class UEKGAdapter:
    def __init__(self, manifest_path: str, ue_version: str, kg_root: str): ...

    def rebuild(self, force: bool = False) -> dict:
        """Build/rebuild the KG from the manifest. Returns
        {classes, members, methods, headers_indexed, modules_indexed,
         deprecated_count, embeddings_indexed, graph_nodes, graph_edges,
         elapsed_sec, embed_elapsed_sec}."""

    def query(self, q: str, k: int = 5) -> list[dict]:
        """Free-form semantic query against class/struct embeddings.
        Each hit: {qualified_name, kind, module, file, line, score,
        members, deprecated_members, snippet}."""

    def deprecated(self, class_name: str | None = None,
                   module: str | None = None) -> list[dict]:
        """List deprecated members. Optional filter by class or module.
        Each row: {qualified_name, class, name, kind, access, deprecated_in,
        message, replacement_hint, file, line, module}."""

    def signature(self, qualified_name: str) -> dict:
        """Class or member lookup. For Class::Method form returns
        {found, qualified_name, class, name, kind, access, signature,
        file, line, deprecated, deprecated_in, deprecated_message,
        replacement_hint, decorators, flags}.
        For a bare class name, returns the class header info."""

    def members(self, class_name: str, scope: str = "public") -> list[dict]:
        """List members of a class. scope = public|protected|private|all.
        Each row: {name, kind, access, signature, file, line, deprecated}."""

    def callers_in_engine(self, qualified_name: str) -> list[dict]:
        """Best-effort engine call-site search (regex over header roots).
        Plugin-side callers should go through the Tier 2 graph adapter."""

    def stats(self) -> dict:
        """{ue_version, nodes, edges, classes, members, methods,
        deprecated_count, modules_indexed, embeddings, last_build_iso, ...}."""
```

Backend: `tree-sitter-cpp` for opportunistic body parsing, regex + balanced-brace scanner for class detection (UE's `UCLASS(...)` macros + multi-base inheritance + `#if WITH_EDITOR` blocks defeat tree-sitter's grammar around a large fraction of UCLASS-decorated types). NetworkX DiGraph for structural relationships, sqlite-vec for class-level embeddings (reuses the Tier 3 nomic-embed-text-v2-moe model).

All adapters accept an optional `ue_version` in their module-level config; Tier 4 requires it. Index lives at `Docs/ue_kg/<ue_version>/` so 5.7 and 5.8 indexes coexist during upgrades.

## 8. Non-goals

- No feedback loop lives in this stack. Session logging, brain-data correlation, and learning-from-failure are handled by the external system; the stack is read-only infrastructure for query routing.
- No recipe auto-synthesis. Recipes are hand-curated; the stack only validates them.
- No multi-project scope. This stack indexes ForgeEditorBridge and its engine dependencies only. Sibling projects get their own stacks if they need them.
- No in-editor UI. Queries run from the CLI or from an LLM shell. The editor integration lives in the plugin itself, not here.
- No background daemons or watchers. Rebuilds are explicit, triggered by `bridge-stack rebuild ...`.
