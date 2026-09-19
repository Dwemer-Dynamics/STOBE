# Stobe agent entry point

This repository builds the x64 Kenshi client. The PHP backend is [StobeServer](https://github.com/Dwemer-Dynamics/StobeServer).

Read the canonical [agent instructions](mod/docs/Stobe/AGENTS.md), [architecture and diagnostics](mod/docs/Stobe/agent-guide.md), and [build guide](mod/docs/Stobe/building.md). These files live in `mod/` so the same guidance ships with the installed plugin; edit them there instead of maintaining duplicate copies.

Confirm the remote, branch, working tree, and requested base before editing. Preserve unrelated changes and use an isolated worktree when needed. Keep binaries, personal configuration, logs, caches, and release archives out of source PRs. Use focused draft PRs unless explicitly asked otherwise; deployment, version changes, merging, and releases require their own authorization.
