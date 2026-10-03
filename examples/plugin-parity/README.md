# ParityProbe example addon

A minimal native addon for testing Stobe's [addon API](../../docs/ADDON_API.md). It registers the `ParityProbe` action bridge and answers `ExtCmdParityProbe_Ping@<target>` by reading the actor's state and reporting success. It does not change the game. Without Stobe, or with a Stobe that lacks the API, it logs one line and stays inert.

It asks for API version 2 and falls back to version 1. With version 2 it registers a control callback and, after each Ping, makes control calls with no lasting effect: it requests interaction On only when it is already On (never Off), locks the actor, reads the owner and unlocks it, and sets, reads and clears the actor's busy flag (when `STOBE_CAP_ACTOR_BUSY` is reported), all within the same callback.

With version 3 (`STOBE_CAP_AGENTS`) it then only reads agents: `ListAgents` (up to 8), `FindClosestAgent` from the player and `GetAgentRegistration` for the actor. It never registers, unregisters or refreshes an actor.

Version 4 is exercised only when `STOBE_PARITY_PROBE_V4_DEMO=1` is set in Kenshi's environment before launch (PowerShell: `$env:STOBE_PARITY_PROBE_V4_DEMO = "1"`, then start Kenshi from that shell). Off by default, because it can write NPC context or start a reply. With it set, `ExtCmdParityProbe_Ping@<op>` runs exactly one operation for that actor 10 seconds later, which leaves time for the Ping's own reply to settle. `<op>` must be one of `normal`, `whisper`, `shout`, `context` (`SendAddonMessage` in that mode), `reactexplicit`, `reacteligible` (`RequestAddonReaction` with no listener) or `state` (an `addon_state` record `probe.status={"v":1,...}`, no model call). Any other parameter leaves Ping read-only. `Report` never runs a demo.

`ParityProbe.log` shows `v4 demo <op> scheduled in 10 s`, then `v4 demo queue=1` and `v4 demo <op> serial=<s> code=<c> ticket=<t>`, followed by a `control ticket=<t> ...` line once Stobe settles the ticket. Wait about 11 seconds before you check. Limits:

- One demo at a time per addon. A Ping that arrives while one is pending logs `refused: one is already pending`.
- One attempt, no retries. If dialogue is still busy, or the world is not ready, the log shows the refusal (`code=-5` or a `REJECTED` ticket with reason `-5`).
- A load in the meantime drops the queued callback, and a stale actor reference gives `-3`. Missing capability bits give `-1`. If the callback has not run within 60 seconds, the slot is released and `did not run` is logged.

Nothing here is built or loaded by Stobe's own build or package. Test it in a separate Kenshi/mod setup, never with real saves you care about.

## Build

The addon needs only Windows and `include/StobeAddonApi.h`; it does not link KenshiLib or Stobe. Any MSVC x64 toolset works; v100 matches Stobe. From the STOBE source root in PowerShell:

```powershell
cmake -S examples/plugin-parity -B build-parity-probe -G "Visual Studio 17 2022" -A x64 -T v100
cmake --build build-parity-probe --config Release
```

Output: `build-parity-probe/Release/ParityProbe.dll` (x64, exports `?startPlugin@@YAXXZ`). Do not commit it.

## Install for a test

1. Create `<Kenshi>/mods/ParityProbe/` and copy `mod/RE_Kenshi.json`, `mod/mod.info` and `ParityProbe.dll` into it.
2. Add an empty `ParityProbe.mod` (for example saved from the Forgotten Construction Set) so Kenshi can list and enable the mod. Enable it in the launcher; Stobe must also be installed and enabled.
3. Start the game and load a disposable save. `ParityProbe.log` beside the DLL should show `connected to Stobe … register=0`, and `stobe.log` should show `ADDON_API: registered addon 'ParityProbe'` and `registered action bridge ParityProbe`.

## Server side

The client addon only executes the action. StobeServer must offer `ExtCmdParityProbe_Ping` to the model. Use the matching server example in the [StobeServer repository](https://github.com/Dwemer-Dynamics/StobeServer) under `examples/plugin-parity/` (the `parity_probe` extension registers the action and logs completions), installed on a disposable test server as that README describes.

To exercise package sync as well, build the server package from the StobeServer source root (PHP with the `zip` extension; do not commit the archive):

```sh
php examples/plugin-parity/build_package.php parity_probe-1.0.0.dwpkg 1.0.0
```

and place it at:

```text
<Kenshi>/mods/ParityProbe/Stobe/server-plugins/parity_probe/1.0.0.dwpkg
```

The folder name must be `parity_probe`, the manifest `name`; the server rejects an upload whose name differs. The client bridge stays `ParityProbe`. On the next game start with interaction On, `stobe.log` shows `SERVER_PLUGIN_SYNC: found parity_probe 1.0.0 in mod ParityProbe`, then `installed` or `already current`.

To uninstall, disable the ParityProbe mod (or delete the archive) first; otherwise Stobe uploads the package again on the next game start. Then open StobeServer's **Server Plugins** page and choose **Remove** on `parity_probe`. Removal moves the plugin folder into package storage and keeps its database tables, declared settings and data; installing the same plugin again restores them. Nothing is deleted.

Use a bridge name that starts with a letter. Stobe accepts bridges that begin with a digit, but StobeServer only registers `ExtCmd<Bridge>_<Action>` codes whose bridge and action start with a letter.

## Expected round trip

When the model selects the action for an NPC:

- `stobe.log`: `ADDON_ACTION: ExtCmdParityProbe_Ping accepted by bridge ParityProbe request=<n> actor_serial=<serial>`, then `external action ExtCmdParityProbe_Ping completed: pong serial=… state=0 flags=0x07 target=<parameter>`.
- `ParityProbe.log`: the same request and report code `1` (`STOBE_QUEUED`). With version 2, `v2 interaction=1 request=1 ticket=<t> lock=0 owner_is_self=1 unlock=0 busy=0 busy_owner_is_self=1 clear=0`, then on a later frame `control ticket=<t> kind=3 state=4 reason=0` (completed without a server call). With version 3, `v3 agents=0 count=<n> first=<name> closest=0 serial=<s> registration=0 mode=0 owner=0`. With interaction not On, no interaction request is made.
- The server receives `funcret` data `command@ExtCmdParityProbe_Ping@<parameter>@completed: pong …` and an `infoaction` line for the NPC.

`ExtCmdParityProbe_Report` is the read-only follow-up fixture. It reports `completed: green serial=<serial> state=<code>` and calls no control or agent APIs. With the server's `parity_probe` extension and a client that sends `addon_followup=1`, the action line carries `aid=<n>`. Stobe sends the result on the follow-up path (`ADDON_FOLLOWUP: aid=<n> arid=<request> sid=<serial> completed sent` in `stobe.log`), and the server answers with one spoken line for the same NPC. Ping is not registered for a follow-up and stays result-only.

An unknown action such as `ExtCmdParityProbe_Explode` is rejected by the handler and reported as failed. If the addon is not installed, Stobe reports `failed: no registered handler for bridge ParityProbe`.

For another speaker in a group or rechat response, the server must send the speaker serial (`|sid=<serial>` on the action line; see [ADDON_API.md](../../docs/ADDON_API.md#speaker-serial)). An older server, or a serial that does not match an identity Stobe sent with the request, gives `failed: speaker unresolved`. A dead, unconscious or unloaded speaker gives `failed: actor unavailable` or `failed: actor not loaded` without calling the handler.
