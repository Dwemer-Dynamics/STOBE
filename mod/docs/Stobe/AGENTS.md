# Stobe: instructions for agents

Stobe is an x64 native Kenshi plugin loaded by RE_Kenshi. It captures game context and sends requests to a separate PHP server.

- Client source: [Dwemer-Dynamics/STOBE](https://github.com/Dwemer-Dynamics/STOBE).
- Server source: [Dwemer-Dynamics/StobeServer](https://github.com/Dwemer-Dynamics/StobeServer).
- Read [how it works and diagnostics](agent-guide.md) before choosing a component.
- Read [building and packaging](building.md) before changing or rebuilding the client.

## Establish what you have

In an installed mod, inspect `mod.info`, `RE_Kenshi.json`, `Stobe.ini`, `StobeCustom.ini`, and current logs. An installed DLL is not a source checkout. Record the installed version and compare it with the intended source revision; upstream default-branch documentation may describe a newer release.

In a source checkout, paths in these guides are relative to the repository root. The canonical packaged guides are in `mod/docs/Stobe/`. Confirm Git state and follow repository instructions before making changes.

For a running server, follow that server's own `AGENTS.md`. Preserve its database, credentials, profiles, memories, and user files. An instruction to inspect a mod does not authorize reinstalling it, running migrations, or starting the game.

## Preserve the game and protocol boundaries

- Keep `Stobe.ini` as shipped defaults and `StobeCustom.ini` as user/runtime overrides. Runtime settings saves belong in the custom file.
- Keep KenshiLib headers, import library, runtime DLL, and RE_Kenshi/RVA compatibility aligned. Do not replace dependencies to silence one build or loader error.
- Preserve squad and NPC identity across requests; names alone are not reliable identity. Verify corresponding server handling when changing request fields or streamed responses.
- Keep capture, network work, response application, and audio lifecycle distinct. Validate cancellation and stale responses when changing dialogue flow.
- Build the game plugin with the required x64 v100 toolchain. Portable test success does not establish game ABI compatibility.
- Diagnose with current evidence. Report source checks, builds, deployment, and in-game validation separately.
