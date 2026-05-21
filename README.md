# Spellrot — UE5.7 Prototype

Wizard vs zombie horde arena brawler. The last living wizard holds back endless waves — every hit from a wizard zombie corrupts your spells, making them more powerful but pushing you closer to transformation.

**Design & studio:** [`ibrews/spellrot`](https://github.com/ibrews/spellrot) — game concept, design docs, 49-agent Claude Code studio
**Engine:** Unreal Engine 5.7
**Base:** Third Person Template + ECABridge MCP plugin

## Things to Try

1. Open `ThirdPersonClass.uproject` in UE 5.7 and hit Play (`Lvl_ThirdPerson`). Enemies (`BP_Enemy`) spawn from `BP_WaveSpawner` actors and chase you. Each enemy that reaches you applies 100 damage which raises `CorruptionLevel` by 0.25. After 4 hits you ragdoll and the level restarts after 3s.
2. Walk into a `BP_KOTHZone` (yellow cube). On overlap it calls `ApplyCleanse` on you, resetting `CorruptionLevel` to 0. Walk out to deactivate.
3. Left-click to fire a line trace. If it hits `BP_Enemy`, `ApplyDamage` runs the enemy's `ReceiveAnyDamage` which casts back to you and subtracts 0.1 from `CorruptionLevel`, then ragdoll-and-recovers the enemy.
4. Inspect the `WBP_HUD` widget — it has a `CorruptionText` field and a `CurrentCorruption` float variable. The Tick event reads `CurrentCorruption` and writes the formatted string to `CorruptionText`. To wire up display in PIE, add a `CreateWidget(WBP_HUD)` → `AddToViewport` chain in `BP_PlatformingGameMode` BeginPlay (currently empty — TODO).
5. Run ECABridge (`ECABridge.GenerateClientConfig ClaudeCode` in the editor console) then open Claude Code in this directory to drive the editor via MCP.

## Setup

1. Unreal Engine 5.7 required (Epic Games Launcher)
2. Open `ThirdPersonClass.uproject` — accept any "missing modules" prompt
3. For ECABridge (AI-assisted development):
   ```
   # In UE editor console:
   ECABridge.GenerateClientConfig ClaudeCode
   ```
   Then in terminal:
   ```bash
   claude mcp list   # should show unreal-ecabridge
   ```

## Mechanics Implemented (2026-05-21)

| Mechanic | File | Notes |
|---|---|---|
| Corruption accumulation | `BP_PlatformingCharacter` | `ReceiveAnyDamage` → `CorruptionLevel += 0.25`; branch on `>= 1.0` → ragdoll + delay 3s + `OpenLevel("/Game/ThirdPerson/Lvl_ThirdPerson")` |
| Cleanse zone reset | `BP_KOTHZone` | `BeginOverlap` casts to `BP_PlatformingCharacter`, sets `IsActive=true`, calls `ApplyCleanse` on player. `EndOverlap` clears `IsActive`. |
| Fireball | `BP_PlatformingCharacter` | `IA_Fire` (`Started`) → `LineTraceByChannel` → branch on hit → cast to `BP_Enemy` → `ApplyDamage(100)` |
| Enemy damage relay | `BP_Enemy` | `ReceiveAnyDamage` → cast to player → `CorruptionLevel -= 0.1` → `RagdollAndRecover` (2s ragdoll then restore) |
| Trail color shift | `BP_PlatformingCharacter` | `BeginPlay` activates `Trail_L`/`Trail_R` Niagara; `Tick` lerps `User.Color` parameter from base to magenta as corruption rises |
| Wave spawning | `BP_WaveSpawner` | Timer calls `SpawnWave` every `SpawnInterval` seconds, spawns `BP_Enemy` at spawner transform. 3 spawner instances in level. |
| HUD widget | `WBP_HUD` | Static `ScoreText` + dynamic `CorruptionText` bound to `CurrentCorruption` member float via Tick. Wiring to gameplay still TODO. |

## Known TODOs

- **HUD spawn**: Add `CreateWidget(WBP_HUD) → AddToViewport` to `BP_PlatformingGameMode` BeginPlay. `CreateWidget` is a K2 macro and cannot be added through ECABridge's `add_blueprint_function_node` — needs manual editor work.
- **HUD ↔ player wiring**: When `CorruptionLevel` changes on player, push value to `HUDWidget.CurrentCorruption`. Requires `HUDWidget` reference variable on player + setter calls.
- **Wave scaling**: Increment `WaveCount` each `SpawnWave` and use it to spawn additional enemies. `lisp_to_blueprint` keeps dropping the actor class on `BeginDeferredActorSpawnFromClass` when nested in branches — needs manual surgery.
- **Enemy color tint**: Visual distinction for fast/tanky variants.
- **Score display on death**: Kills + time survived on game-over screen.

## ECABridge Patterns

See the design repo for the full pattern catalog: [`docs/engine-reference/unreal/ecabridge-patterns.md`](https://github.com/ibrews/spellrot/blob/main/docs/engine-reference/unreal/ecabridge-patterns.md)

### Patterns learned this session

- **`lisp_to_blueprint` cross-BP variable set**: `(set CorruptionLevel ...)` inside a `(cast)` body does NOT properly target the cast result — it tries to resolve on the casting BP. Workaround: define a custom event on the target BP that does the set, then call it from the casting BP using `add_blueprint_function_node` with `target_class` pointing to the target's `_C` class.
- **`(call FunctionName)` inside `(cast)`**: Breaks the cast's Object pin (becomes wildcard) and the compile fails. Use surgical `add_blueprint_function_node` + `connect_blueprint_nodes` instead.
- **`(print float)` in lisp**: Auto-resolves to `PrintString` expecting a string, fails with float-to-string compile error. Workaround: drop the print or pre-convert.
- **`CreateWidget` not addable via function_node**: It's a `K2Node_CreateWidget` macro, not a regular function. ECABridge has no surface for it yet — must add in editor manually.
- **Always reload from git after a broken `lisp_to_blueprint` rebuild**: `delete_loaded_asset` unloads the asset and orphans the in-memory state. `git checkout` + `scan_paths_synchronous(force_rescan=True)` restores cleanly.
