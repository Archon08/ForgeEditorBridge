# ForgeEditorBridge Operating Guide

This guide explains how to operate the project as a human developer or technical user. For LLM-specific behavior, see `Docs/LLM_OPERATOR_GUIDE.md`.

## Mental Model

ForgeEditorBridge has two separable parts:

1. The Unreal Editor plugin.
   - Required for editor automation.
   - Lives under `Source/ForgeEditorBridge`.
   - Runs in Unreal Editor.
   - Exposes `POST /command` and `POST /batch` on localhost.

2. The cognitive stack.
   - Optional documentation and search infrastructure.
   - Lives under `Docs`.
   - Helps humans and LLMs find domains, actions, recipes, callsites, and UE API references.
   - Does not need to run for the plugin to work.

If your goal is to use the bridge, install the plugin and call the HTTP server. If your goal is to understand or extend the bridge, use the docs and stack.

## Install The Plugin

1. Copy the folder containing `ForgeEditorBridge.uplugin` (this repo's root) into:
   - `<YourProject>/Plugins/ForgeEditorBridge/` (typical), or
   - an Unreal Engine editor plugin directory (advanced).
   Rename the folder to `ForgeEditorBridge` if it isn't already.

2. Open the Unreal project.

3. Enable `Forge Editor Bridge`.

4. Restart the editor.

5. Open `Project Settings > Plugins > Forge Editor Bridge`.

6. Confirm:
   - `HttpPort`: usually `8765`
   - `ContextDirectory`: usually `Forge/ue-context`
   - `bAutoStart`: enabled unless you want manual startup

## Start And Verify

With auto-start enabled, the bridge starts after the editor subsystem initializes.

Verify with:

1. Open `{ProjectDir}/Forge/ue-context/bridge-status.json`.
2. Confirm `"active": true`.
3. Copy the `auth_token`.
4. Call `system.ping`.

Example:

```powershell
$statusPath = Join-Path (Get-Location) "Forge\ue-context\bridge-status.json"
$status = Get-Content $statusPath | ConvertFrom-Json
$headers = @{ "X-Bridge-Token" = $status.auth_token }
$body = @{ domain = "system"; action = "ping"; params = @{} } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$($status.port)/command" -Headers $headers -ContentType "application/json" -Body $body
```

## Discover Capabilities

Start with system commands:

```json
{ "domain": "system", "action": "capabilities", "params": {} }
```

Then inspect schemas:

```json
{ "domain": "system", "action": "describe", "params": { "domain": "blueprint" } }
```

If no domain is supplied to `system.describe`, the bridge returns schemas for all domains that provide them.

Useful system actions to run first in a live editor session:

```json
{ "domain": "system", "action": "ping", "params": {} }
```

```json
{ "domain": "system", "action": "health_check", "params": {} }
```

```json
{ "domain": "system", "action": "capabilities", "params": {} }
```

```json
{ "domain": "system", "action": "describe", "params": { "domain": "blueprint" } }
```

System action reference:

| Action | Purpose |
|---|---|
| `ping` | Verify the server is alive |
| `capabilities` | List registered domains and actions |
| `describe` | Return action schemas |
| `describe_all` | Return all available schemas |
| `health_check` | Check server, output directory, captures, and runtime state |
| `get_editor_state` | Read level, PIE (Play In Editor), selection, dirty package state |
| `export_all_captures` | Write context captures to disk |
| `save_all` | Save dirty maps and assets |
| `undo` / `redo` | Use editor transaction history |

## Send Commands

Every command has:

- `domain`: handler domain such as `actor`, `asset`, `blueprint`, `material`, or `system`
- `action`: operation inside that domain
- `params`: JSON object consumed by the handler

Example:

```json
{
  "domain": "system",
  "action": "get_editor_state",
  "params": {}
}
```

## Send Batches

Use `/batch` for a top-level array:

```json
[
  { "domain": "system", "action": "get_editor_state", "params": {} },
  { "domain": "system", "action": "health_check", "params": {} }
]
```

The batch endpoint wraps the whole sequence in a single editor transaction named `ForgeEditorBridge Atomic Batch`. Every action is dispatched in order; a failing action does not stop or roll back the batch - successful mutations are kept and failures are reported per-action in the `results` array. The response returns `ok` (true only if all succeeded), a `results` array with one entry per action, and a `summary` with `total` / `success` / `fail` counts. To undo the whole batch, call `system.undo` once after the response returns.

You can also call `system.batch` through `/command`:

```json
{
  "domain": "system",
  "action": "batch",
  "params": {
    "commands": [
      { "domain": "system", "action": "ping", "params": {} }
    ]
  }
}
```

## Context Output

The output directory defaults to:

```text
{ProjectDir}/Forge/ue-context
```

Common files include:

- `bridge-status.json` - server state, token, port, domains.
- bridge result files written by `UBridgeResultWriter`.
- capture outputs written by the read-side capture classes.

Run:

```json
{ "domain": "system", "action": "export_all_captures", "params": {} }
```

This exports available audit/capture JSON for assets, blueprints, materials, Niagara, input, network, world generation, performance, and related domains.

## When To Use The Stack

Do not start with the stack when you only need to operate the plugin.

Use the stack when you need to answer:

- Which domain owns a capability?
- Which action or recipe should be used for a compound task?
- Where is an action dispatched in source?
- What code calls a specific function?
- Which UE API member or deprecation is relevant?

Main files:

- `Docs/_bridge_map.md` - first-stop overview.
- `Docs/_inventory.json` - action inventory.
- `Docs/_bridge_index.json` - graph index.
- `Docs/recipes/*.yml` - task playbooks.
- `Docs/stack/README.md` - stack setup and command reference.

Basic stack commands. These require the one-time venv setup in `Docs/stack/README.md`; after that:

```powershell
cd Docs\stack
.\.venv\Scripts\python bridge-stack status
.\.venv\Scripts\python bridge-stack verify
.\.venv\Scripts\python bridge-stack graph list-domains
.\.venv\Scripts\python bridge-stack graph list-actions blueprint
.\.venv\Scripts\python bridge-stack graph query-action blueprint create_blueprint
```

Optional vector setup (run from `Docs/stack/`):

```powershell
cd Docs\stack
python -m venv .venv
.\.venv\Scripts\python -m pip install --upgrade pip
.\.venv\Scripts\python -m pip install tree-sitter tree-sitter-cpp networkx pyyaml pyparsing sentence-transformers sqlite-vec einops
.\.venv\Scripts\python bridge-stack rebuild --tier vector --force
```

Optional UE knowledge graph setup requires a local UE 5.7 install and the manifest in `Docs/stack/ue_kg_manifest.json`.

## Troubleshooting

Bridge status file missing:

- Confirm the plugin is enabled.
- Confirm the module compiled.
- Confirm `bAutoStart` is enabled. If you intentionally disabled it, restart the editor with auto-start on. C++ extension developers can invoke `UForgeAISubsystem::StartBridge()` from their own module - this is not exposed as a console command or HTTP action.

HTTP 403 Forbidden:

- Use `127.0.0.1` or `localhost`.
- Do not call from a remote host.
- Include the current `X-Bridge-Token`.

Unknown domain:

- Call `system.capabilities`.
- Confirm the handler class is compiled and derives from `UBridgeHandlerBase`.

Unknown action:

- Call `system.describe` for the domain.
- Check `Docs/_bridge_map.md` or `Docs/_inventory.json`.

PIE (Play In Editor) required:

- Some runtime actions require PIE to be running.
- Start PIE in the editor, then retry the action.

Some packages failed to save:

- Check Unreal Output Log.
- Look for locked assets, source-control checkout requirements, or invalid package paths.

## Release Checklist

- Build the plugin in the target Unreal Engine version.
- Start the editor and confirm `bridge-status.json`.
- Run `system.ping`.
- Run `system.health_check`.
- Run `system.capabilities`.
- Run one read-only command from a non-system domain.
- Run one harmless write command in a test project.
- Run `system.undo` after a transactional edit.
- Run `system.export_all_captures`.
- Verify `Docs/_bridge_map.md` matches the current handler set.
- Confirm `VersionName` in `.uplugin` matches `Docs/_inventory.json`, `Docs/_bridge_index.json`, capture JSON outputs, and startup logs (see `Docs/MAINTAINERS.md` for the full version-bump checklist).
