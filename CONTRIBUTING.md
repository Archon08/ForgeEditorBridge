# Contributing

Thanks for helping improve ForgeEditorBridge.

## Before You Start

- Read `README.md`.
- Read `Docs/OPERATING_GUIDE.md`.
- For LLM-oriented changes, read `Docs/LLM_OPERATOR_GUIDE.md`.
- For stack changes, read `Docs/stack/README.md`.

## Development Rules

- Keep the plugin usable without the optional cognitive stack.
- Prefer runtime discovery through `system.capabilities` and `system.describe`.
- Keep handler domains stable when possible.
- Add or update recipes for multi-step workflows.
- Do not commit generated Unreal build artifacts or optional stack artifacts.
- Do not commit live `bridge-status.json` files or local context captures.

## Adding A Handler

1. Derive from `UBridgeHandlerBase`.
2. Implement `GetDomainName()`.
3. Implement `GetSupportedActions()`.
4. Implement `HandleCommand()`.
5. Add `GetActionSchemas()` when practical.
6. Use `MakeSuccess`, `MakeError`, and `MakeUnknownAction` for consistent results.
7. Prefer editor transactions for write operations.
8. Run the stack map/index refresh if docs need to reflect the new surface.

## Documentation Expectations

Public-facing behavior should be documented in:

- `README.md` for high-level use.
- `Docs/OPERATING_GUIDE.md` for human operation.
- `Docs/LLM_OPERATOR_GUIDE.md` for LLM operation.
- `Docs/_bridge_map.md` and `Docs/_inventory.json` for domain/action discovery.

## Verification

Before proposing a release change:

1. Build the plugin in the target Unreal Engine version.
2. Start the editor and confirm `bridge-status.json`.
3. Call `system.ping`.
4. Call `system.health_check`.
5. Call `system.capabilities`.
6. Test at least one relevant non-system action.
7. Confirm no generated files are staged.
