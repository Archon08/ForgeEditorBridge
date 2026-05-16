# Security Policy

ForgeEditorBridge is local editor automation tooling. It is designed for trusted use on the same machine as Unreal Editor.

## Supported Surface

- Localhost HTTP server only (`127.0.0.1`).
- Per-session `X-Bridge-Token` read from `bridge-status.json` at startup.
- Editor-only Unreal Engine plugin.

## Important Guidance

- Do not expose the bridge port to a LAN or the public internet.
- Do not commit or share live `bridge-status.json` files - the auth token is rotated per session and treating it like a session secret is essential.
- Treat actions that touch source control, packaging, Python execution, C++ files, console commands, or project settings as privileged operations.
- Run the plugin in a disposable test project before using new automation workflows on production content.

## Reporting a Vulnerability

Please report security vulnerabilities **privately** via GitHub Security Advisories on this repository. Do not file a public issue or publish a proof-of-concept first.

1. Go to the repository on GitHub.
2. Click the **Security** tab.
3. Click **Report a vulnerability** (this opens a private advisory only visible to repo maintainers and you).
4. Fill in the form.

When reporting, please include:

- Unreal Engine version
- Plugin version (`VersionName` from `ForgeEditorBridge.uplugin`)
- Whether the issue requires local access, network access, or AI-driven automation
- Exact request payload (if safe to share)
- Expected behavior
- Observed behavior

You will receive an acknowledgement within a reasonable window once the advisory is reviewed.

## Out of Scope

- Issues that require an attacker to already have local code-execution on the developer's machine.
- Reports about default Unreal Engine vulnerabilities that the plugin merely passes through.
- Issues that depend on a non-default plugin configuration we have explicitly recommended against (e.g. binding the HTTP server to a non-loopback interface).
