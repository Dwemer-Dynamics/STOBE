# Autonomy Phase 0 Safety Spike

Status: Validated on the target build; probe disabled  
Last updated: 2026-07-14

## Purpose

This harness validates Kenshi's public AI order APIs before Stobe adds the autonomy control plane or LLM planner. It is disabled by default, operates only from the main game update hook, and never calls StobeServer or an LLM.

The harness can:

- Latch exactly one player-faction character by runtime serial.
- Log current goal, first order, player-order state, jobs state, permanent jobs, position, destination, and path state.
- Issue one direct `IDLE` or `MOVE_CUS_ORDERED` order.
- Reject mutation when the character already has an order.
- Verify that jobs settings and permanent jobs did not change during dispatch.

It does not clear orders, goals, jobs, permanent jobs, standing orders, follow state, or dialogue state.

## Validation Record

Validated on 2026-07-14 against the deployed v100 build using Herika runtime
serial `604980480`.

- Read-only observation bound one configured player character and emitted goal,
  order, jobs, permanent-job, movement, position, destination, and path state.
- `IDLE` dispatched once with `clear_old=0`, preserved `Splint Rigging`, and
  preserved every other loaded player character's order state.
- A travel attempt while IDLE remained active was rejected with
  `reason=existing_order`.
- After the player manually cancelled IDLE, `MOVE_CUS_ORDERED` moved only Herika
  approximately 10 world units and completed normally.
- Kenshi cleared the travel order at the requested coordinates while
  `destination_reached` remained false. Production completion detection must
  use requested-target distance plus order/path state instead of that flag alone.
- Both issued commands completed their invariant watches with
  `jobs_preserved=1` and `violation=0`.
- Save/load unbound the target on `world_unstable`, rebound the same serial after
  stability, and did not replay consumed command nonce `4`.
- Portable policy tests cover disabled, unbound, non-player, dead, unconscious,
  missing receiver, unavailable, existing-order, and unarmed-travel rejection.
- The probe was returned to `Enabled=0` after the run.

No live character was killed, knocked unconscious, or removed from the player
faction during this validation. Those engine-state transitions remain a
pre-release regression test; the corresponding command guards are unit tested.

## Configuration

The recommended control surface is:

```powershell
.\scripts\test-autonomy-phase0.ps1 -Action Status
.\scripts\test-autonomy-phase0.ps1 -Action Enable
.\scripts\test-autonomy-phase0.ps1 -Action Observe
.\scripts\test-autonomy-phase0.ps1 -Action Idle
.\scripts\test-autonomy-phase0.ps1 -Action Travel -TravelX 100 -TravelY 0 -TravelZ 200
.\scripts\test-autonomy-phase0.ps1 -Action Analyze
.\scripts\test-autonomy-phase0.ps1 -Action Disable
```

The script preserves existing `StobeCustom.ini` settings, increments one-shot
command nonces, and refuses incomplete travel coordinates. On RE_Kenshi installs,
it reads packaged defaults from `mods\STOBE\Stobe.ini` and controls the live
`RE_Kenshi\mods\Stobe\StobeCustom.ini` and `stobe.log` files.

### Manual Configuration

Edit the deployed `mods\Stobe\StobeCustom.ini` section while Kenshi is running:

```ini
[AutonomySafetyProbe]
Enabled=0
TargetSerial=0
TelemetryIntervalMs=2000
Command=OBSERVE
CommandNonce=0
TravelDestinationSet=0
TravelX=0
TravelY=0
TravelZ=0
```

The probe reloads this section every 500 milliseconds.

`TargetSerial=0` latches the currently selected player-faction character when the probe is enabled. A non-zero value resolves that exact loaded character serial. Selection changes after binding do not retarget the probe.

Commands are one-shot. Change `Command`, then increment `CommandNonce`. The initial nonce observed at plugin startup is consumed without execution so a stale command cannot repeat after restarting Kenshi.

## Log Records

Inspect `mods\Stobe\stobe.log` for these prefixes:

- `AUTONOMY_SPIKE:`: lifecycle and target binding.
- `AUTONOMY_SPIKE_STATE:`: read-only AI and movement snapshots.
- `AUTONOMY_SPIKE_COMMAND:`: validation and dispatch result.
- `AUTONOMY_SPIKE_INVARIANT:`: jobs or permanent-job state changed unexpectedly.

Each command writes a `before_command` and `after_command` snapshot.

## Baseline Observation

1. Start Kenshi and load a test save.
2. Select one player-faction character who has at least one permanent job configured.
3. Set `Enabled=1` and leave `TargetSerial=0`.
4. Confirm one `target bound` record appears with the expected name and serial.
5. Change `Command=OBSERVE` and increment `CommandNonce`.
6. Confirm periodic and command snapshots contain goal, order, jobs, and path fields.
7. Select another squad member and confirm subsequent snapshots retain the original serial.

Pass condition: only one serial is logged after binding, and selection changes do not retarget it.

## IDLE Probe

1. Ensure the bound character has no active player order. Permanent jobs may remain configured.
2. Record `jobs_enabled`, `permajob_count`, and `permajobs` from the latest snapshot.
3. Set `Command=IDLE` and increment `CommandNonce`.
4. Confirm `issued=1`, `clear_old=0`, `jobs_preserved=1`, and `other_player_orders_preserved=1`.
5. Confirm the same jobs values appear in the before and after snapshots.
6. Confirm no other selected or unselected squad member receives an IDLE order.

Pass condition: only the bound serial receives `IDLE`, and no jobs state changes.

If `reason=existing_order` is logged, let the current order finish or cancel it manually, then increment the nonce again. The harness never clears that order itself.

## Travel Probe

Use coordinates from a nearby reachable position in the current test area.

1. Ensure the bound character has no active player order.
2. Set `TravelX`, `TravelY`, and `TravelZ`.
3. Set `TravelDestinationSet=1`.
4. Set `Command=TRAVEL` and increment `CommandNonce`.
5. Confirm `task=MOVE_CUS_ORDERED`, `issued=1`, `clear_old=0`, `jobs_preserved=1`, and `other_player_orders_preserved=1`.
6. Watch `destination`, `path_ok`, `path_failed`, and `destination_reached` in periodic snapshots.
7. Confirm no other squad member moves because of the probe.

Pass condition: only the bound serial travels, its path state is observable, and jobs remain unchanged.

## Lifecycle Matrix

Run each case with telemetry enabled and record the resulting log lines.

| Case | Expected result |
| --- | --- |
| Save and load | Binding resets during the unstable world transition. No old nonce executes after load. |
| Character knocked unconscious | `unconscious=1`; IDLE and TRAVEL reject with `target_unconscious`. |
| Character killed | `dead=1`; IDLE and TRAVEL reject with `target_dead`. |
| Character removed from player faction | Mutation rejects with `not_player_character`. |
| Character unloaded | Probe logs that it is waiting for the bound serial and issues no command. |
| Character reloaded | The same configured or latched serial becomes observable again without replaying a command. |
| Existing player order | Mutation rejects with `existing_order`; the existing order remains intact. |
| Multiple characters selected | Only the latched primary character serial is addressed. |
| Invalid travel destination | Kenshi path state reports failure; the probe does not retry or clear jobs. |

## Exit Evidence

Phase 0 is complete only when the captured log proves all of the following:

- Read-only goal, order, jobs, and path telemetry works on the target build.
- `IDLE` and travel dispatch through `OrdersReceiver::addOrder` without an LLM.
- Save/load, knockout, death, faction change, and unload/reload do not replay commands.
- No command affects a second squad member.
- Existing orders are rejected rather than overwritten.
- Jobs-enabled state and permanent jobs are identical before and after dispatch.
- Every other loaded player character retains the same order state during dispatch.
- No `AUTONOMY_SPIKE_INVARIANT` record is emitted.

After testing, set `Enabled=0`.
