# LLM Operator Guide

This file is written for an LLM agent operating ForgeEditorBridge through local tools, HTTP, or repository context.

## Prime Directive

Use the plugin directly when the task is editor automation. The cognitive stack is optional lookup infrastructure. Do not rebuild graph/vector/UE knowledge-graph data unless the user asks for deeper discovery, missing docs, semantic search, or engine API analysis.

## Fast Orientation

Read in this order:

1. `README.md`
2. `Docs/_bridge_map.md`
3. `Docs/_inventory.json` only if action-level detail is needed
4. the relevant `Docs/recipes/*.yml` for compound tasks
5. the relevant `Source/ForgeEditorBridge/Private/Handlers/*Handler.cpp` only when parameter details or edge cases are needed

Avoid loading the entire source tree. The plugin has many handlers and hundreds of actions.

## Runtime Contract

The editor bridge listens on localhost, usually:

```text
http://127.0.0.1:8765
```

The live port and token are in:

```text
{ProjectDir}/Forge/ue-context/bridge-status.json
```

Every request must include:

```text
X-Bridge-Token: <auth_token>
```

Use:

- `POST /command` for one command
- `POST /batch` for a top-level array of commands

Single command:

```json
{
  "domain": "system",
  "action": "ping",
  "params": {}
}
```

Batch:

```json
[
  { "domain": "system", "action": "get_editor_state", "params": {} },
  { "domain": "system", "action": "health_check", "params": {} }
]
```

## First Three Calls

When operating a live editor session, start with:

1. `system.ping`
2. `system.health_check`
3. `system.get_editor_state`

Then call:

```json
{ "domain": "system", "action": "capabilities", "params": {} }
```

Use `system.describe` before calling a write action whose parameter schema you have not confirmed:

```json
{
  "domain": "system",
  "action": "describe",
  "params": { "domain": "material" }
}
```

## Routing Rules

For "what can do X?":

- Start at `Docs/_bridge_map.md`.
- Use the domain catalog to pick a domain.
- Use `system.capabilities` or `system.describe` in the live editor to confirm the current runtime surface.

For "perform X in Unreal":

- Prefer the live schema from `system.describe` (the dispatch contract and parameter list returned by the running plugin at this version, which may include actions added after `_inventory.json` was last hand-curated).
- If the task has multiple steps, check `Docs/recipes`.
- Use `/batch` for multi-step edits that should be grouped.
- Run `system.save_all` only when the user wants changes saved.

For "where is X implemented?":

- Use `Docs/_bridge_index.json` if available.
- Or use `Docs/stack/bridge-stack graph query-action <domain> <action>`.
- Then inspect only the relevant handler file.

For "what UE API should this use?":

- Use existing handler code first.
- Use Tier 4 UE KG only if it has been built or the user asks to build it.
- If Tier 4 is not built, do not imply it is required for normal plugin use.

## Using Recipes

Recipes (`Docs/recipes/*.yml`) are hand-curated playbooks that capture multi-step
editor workflows as an ordered list of `domain/action` dispatches. The plugin's
HTTP dispatcher does not parse recipe files - you do. To "run" a recipe you
read the YAML, validate the operator's inputs against the `inputs:` block,
issue each `steps[].action` through `POST /command` (or stage the run as a
single `POST /batch`), and thread the captured outputs forward. Without an
executor wrapping the loop, a recipe is reference material; with you in the
loop, it is the workflow.

### Schema fields you must respect

Every recipe declares the following top-level keys (template: `Docs/recipes/_template.yml`):

- `recipe_schema_version` - currently `"1.0"`.
- `id` - snake_case, matches the filename stem.
- `title`, `goal`, `description` - human-readable intent.
- `domain_scope` - list of plugin domains the recipe touches.
- `tested_on_ue`, `tested_on_plugin` - version pins (e.g. `["5.7"]`, `["0.2.6"]`).
- `preconditions` - free-form bullets the caller must satisfy before dispatch.
- `inputs` - user-provided parameters: `{name, type, required, description, example}`. Validate every required input before the first dispatch.
- `outputs` - values the recipe emits on success.
- `action_dependencies` - the authoritative manifest of every `domain/action` the recipe references. `bridge-stack verify` resolves each entry against `_inventory.json` offline, so by dispatch time this list is your safe-list.
- `steps` - ordered list of `{id, action, inputs, captures, on_error}`. `on_error` is one of `abort | continue | retry`. Inputs reference earlier `captures` with `$name` and recipe-level inputs with `$input_name`.
- Optional: `decision_points`, `loops`, `failure_modes`, `variants`, `notes`.

A step tagged `status: "pending_plugin_support"` marks an action whose direct handler currently returns 3003. `bridge-stack verify` downgrades the missing-action check to a warning for that recipe; you should fall back to the Python path (see Known Recipe Stubs below).

### When to reach for a recipe vs hand-roll

Use a recipe when:

- The task crosses three or more domains (e.g. texture + asset + material).
- A `failure_modes` entry already documents the gotcha you would otherwise hit (e.g. degenerate terrain normal in `scan_mesh_bounds_and_place`).
- The operator asks for a named workflow that matches a recipe `title`.

Hand-roll when:

- The task is a single action or a two-step `/batch`.
- The operator is exploring and wants you to inspect state between calls.
- No recipe matches and authoring a new one would be premature.

### Verifying recipe step success

The recipe schema does not define a `verify:` field. Verification is your discipline:

1. Inspect the standard response envelope (`ok`, `error_code`, `recovery_hint`) after every dispatch.
2. If `recovery_hint` is set, follow it before deviating from the recipe.
3. Honor `on_error: abort | continue | retry` literally - it is a contract with the operator, not a suggestion.
4. Evaluate `only_if:` predicates against the latest captured outputs. The bridge does not enforce these.
5. After consequential terminal steps (compile, save, spawn-loop completion), call the corresponding read action (`get_tree_topology`, `get_widget_tree`, `list_emitters`, `get_topology`) to confirm the world matches the recipe's expected state.

### Aliases vs recipe step `action:` names

Recipe `steps[].action` values must use canonical names (e.g. `actor/spawn_actor`, not `actor/create_actor`). The HTTP dispatcher routes by canonical name only - the `aliases` field in `_inventory.json` exists for search and operator-correction (see Personalization: Action Aliases in the README), not for dispatch. When you update an alias because the operator corrected your naming, do not also rewrite recipe step references; recipes remain canonical.

### Known Recipe Stubs (Niagara)

Four actions in `niagara_emitter_from_static_mesh.yml` return `error_code 3003`:

- **BLOCKED (no future C++ wiring planned)**: `niagara/add_module`, `niagara/set_module_property`, `niagara/add_renderer`. The relevant UE 5.7 surface is private or internal-only. Dispatch a `python_agent` snippet to drive the equivalent via the `unreal` Python API.
- **DEFERRED (planned for a future plugin release)**: `niagara/set_renderer_property`. Will be wired via reflection in a later release. Until then, use the Python fallback.

The recipe's `failure_modes:` block contains the explicit fallback pattern. Do not surprise the operator: when you hit a BLOCKED action, state which one and pivot to Python before dispatching further steps.

## Safety Rules

- Treat all non-read commands as editor mutations.
- Prefer `system.get_editor_state` before writes.
- Prefer `system.describe` before unknown actions.
- Use batches for related multi-step writes.
- After a write, inspect the returned `ok`, `message`, `error_code`, `affected_path`, and `recovery_hint`.
- If a write fails, follow `recovery_hint` before inventing a workaround.
- For destructive actions, know that action names containing `delete`, `remove`, or `destroy` are globally intercepted for quarantine before reaching the underlying handler.
- Do not call source-control, packaging, Python execution, C++ writing, or console commands unless the user intent clearly requires them.
- Do not run `system.save_all` automatically after exploratory edits.

## Response Interpretation

Success:

```json
{
  "ok": true,
  "message": "...",
  "domain": "...",
  "action": "...",
  "error_code": 0,
  "data": {}
}
```

Failure:

```json
{
  "ok": false,
  "message": "...",
  "domain": "...",
  "action": "...",
  "error_code": 1001,
  "recovery_hint": "..."
}
```

Common error codes:

- `1000` - missing required parameter
- `1001` - unknown action
- `2000` - asset not found
- `2003` - asset not loaded
- `3000` - engine API failure
- `3003` - module or world unavailable
- `3004` - PIE (Play In Editor) required
- `5000` - domain not registered

## Choosing Between Docs And Live Runtime

Prefer live runtime for action availability:

```json
{ "domain": "system", "action": "capabilities", "params": {} }
```

Prefer docs for planning:

- `Docs/_bridge_map.md` for domain selection
- `Docs/recipes` for multi-step recipes
- `Docs/_inventory.json` for aliases and dispatch-line metadata

Prefer source for exact behavior:

- `Source/ForgeEditorBridge/Private/Handlers/<Domain>Handler.cpp`

## Optional Stack Usage

The stack is useful but not mandatory.

Do not tell users they need graph/vector/UE KG data to use the plugin. They only need Unreal Editor, the plugin, localhost access, and the session token.

Use stack commands when useful (assumes the one-time venv setup from `Docs/stack/README.md`):

```powershell
cd Docs\stack
.\.venv\Scripts\python bridge-stack status
.\.venv\Scripts\python bridge-stack verify
.\.venv\Scripts\python bridge-stack graph list-domains
.\.venv\Scripts\python bridge-stack graph list-actions blueprint
.\.venv\Scripts\python bridge-stack graph query-action material create_material
.\.venv\Scripts\python bridge-stack graph query-caller ResolveClass
```

If a venv is not available, you can still read `Docs/_bridge_index.json` directly - it ships pre-built and is plain JSON.

Only build vector search when semantic lookup is needed (run from `Docs/stack/` with the venv active):

```powershell
.\.venv\Scripts\python bridge-stack rebuild --tier vector --force
.\.venv\Scripts\python bridge-stack search "wire channel packed textures into material"
```

### Using Semantic Search

The vector tier is **optional**. Do not require it for normal operation, and do not assume it is built - check `bridge-stack status` first, or just call `bridge-stack search` and treat the "not built" error as a signal to fall back to name lookup.

When to issue a `bridge-stack search` query (vector tier):

- The operator's task is phrased by **intent** rather than by API name, and a quick scan of `Docs/_bridge_map.md` does not surface an obvious handler.
- You need to find a **recipe that does X** but you do not remember its id (e.g. "the texture import one with sRGB rules").
- You hit a 3xxx error and want to find the matching `failure_modes` entry across all recipes without grepping each file.
- The operator describes a **variant** of a known workflow (e.g. "low count CPU emitter") and you want to land on the right `variants[]` entry.

When NOT to issue a vector query:

- You already know the canonical `domain/action` name. Use the graph tier (`bridge-stack graph query-action <domain> <action>`) or read the handler file directly.
- The task is a one-call action or a two-step `/batch`. The overhead of a search round-trip is not worth it.
- The vector tier reports "not built" and the operator has not opted into building it. Fall back to grep over `Docs/_bridge_index.json` or `Docs/_bridge_map.md`.

What the vector tier indexes (so you can predict what it can answer): per-action chunks with domain purpose + aliases + handler location; per-recipe chunks with goal, description, inputs, `action_dependencies`, the ordered `steps[].action` chain, every `variants[]` differentiator, and every `failure_modes[]` trigger / symptom; per-domain overview chunks with purpose + first 10 action names.

What it does NOT index: raw C++ handler source bodies, individual recipe step input shapes, runtime editor state. For those, read the handler file or call `system.describe` / `system.get_editor_state`.

Only build UE KG when engine API lookup is needed (run from `Docs/stack/` with the venv active; first build is ~2 hours CPU and requires a local UE 5.7 install):

```powershell
.\.venv\Scripts\python bridge-stack rebuild --tier ue_kg --ue-version 5.7
.\.venv\Scripts\python bridge-stack ue-kg sig UGameplayAbility::GetAssetTags
```

## Good Operating Loop

1. Identify intent: read, write, batch write, inspect docs, or inspect source.
2. Check editor state if live automation is involved.
3. Select a domain from map/capabilities.
4. Confirm schema for unfamiliar actions.
5. Execute the smallest useful command or batch.
6. Read the result object.
7. Verify the editor state or affected asset.
8. Save only when asked or when the workflow requires persistence.

## Human Escalation Points

Ask the human before:

- changing project-wide settings
- saving all dirty packages
- running packaging/cook/build actions
- running arbitrary Python or console commands
- invoking source-control submit/revert actions
- building the UE knowledge graph if it requires a long local UE scan
- changing the license

## Action Naming And Aliases (Personalization)

Each action in `Docs/_inventory.json` carries an `aliases` field, currently empty. The HTTP dispatcher routes by canonical name only; aliases improve `bridge-stack search` and inventory lookup that LLM consumers use during discovery.

If the user corrects your action naming - for example you called `actor/create` and the user replied "the canonical name is `actor/spawn`" - that is a signal an alias would help. Suggest to the user:

> I called `actor/create` but the canonical name is `actor/spawn`. Would you like me to add `create` as an alias to `actor/spawn` in `_inventory.json` so future searches find it on the first try?

If they confirm, append the alternate name to that action's `aliases` array. Note that the HTTP dispatcher still requires the canonical name; aliases pay off in discovery (search/inventory lookup), not dispatch.

## Verifying Symbol Capture Is Wired

The C++ symbol capture (`UForgeSymbolCapture`) writes `{ProjectDir}/Forge/ue-context/symbols/project_symbols.json` describing the host project's UClasses. It auto-detects the host project's `/Script/<ProjectName>` package via `FApp::GetProjectName()`.

After the first capture run, verify:

1. The file exists.
2. `class_count > 0` in the JSON.

If `class_count` is zero, the host project's gameplay code is loaded under a module name that does not match `/Script/<ProjectName>`. Open `Source/ForgeEditorBridge/Private/Capture/ForgeSymbolCapture.cpp` and extend `GetTargetPrefixes()` to add the project's additional module prefixes.

## Known Release Audit Notes

- `Docs/stack/README.md` contains the deepest explanation of the optional cognitive stack.
- `Docs/_stack_status.md` is the quick stack dashboard.
