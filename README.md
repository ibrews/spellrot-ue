# Spellrot — UE5.7 Prototype

Wizard vs zombie horde arena brawler. The last living wizard holds back endless waves — every hit from a wizard zombie corrupts your spells, making them more powerful but pushing you closer to transformation.

**Design & studio:** [`ibrews/spellrot`](https://github.com/ibrews/spellrot) — game concept, design docs, 49-agent Claude Code studio  
**Engine:** Unreal Engine 5.7  
**Base:** Third Person Template + ECABridge MCP plugin

## Things to Try

1. Open `ThirdPersonClass.uproject` in UE 5.7 and hit Play — the Third Person character is fully set up with chase AI, kill zone, and PrintString HUD from the test teach session
2. Run ECABridge (`ECABridge.GenerateClientConfig ClaudeCode` in the editor console) then open Claude Code in this directory to drive the editor via MCP
3. Check `Content/ThirdPerson/` for the character Blueprint (`BP_ThirdPersonCharacter`) and `Content/LevelPrototyping/` for the arena level
4. Look at the AnimBP (`ABP_Unarmed`) — it has a Layered Blend Per Bone node that gives enemies an upper-body zombie pose while the player moves normally
5. Open Claude Code and run `/prototype wave-combat` to begin building the Spellrot wave spawner and infection mechanic on top of this base

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

## What's Already Built (from test teach session 2026-05-20)

| Mechanic | File | Notes |
|---|---|---|
| Chase AI (zombie prototype) | `BP_MiniMe` | `SimpleMoveToActor` on Tick, MaxWalkSpeed=180 |
| Kill zone | Level | Box trigger, `Trigger` profile, `ApplyDamage` relay |
| Ragdoll death + restart | `BP_ThirdPersonCharacter` | `AnyDamage → SetSimulatePhysics → RestartLevel` |
| Layered Blend Per Bone | `ABP_Unarmed` | `spine_01` split, upper body zombie arms |
| PrintString HUD | Level BP | `Key="KOTHScore"`, no UMG needed for prototype |
| NavMeshBoundsVolume | Level | Already placed — required for any AI movement |

## ECABridge Patterns

See the design repo for the full pattern catalog: [`docs/engine-reference/unreal/ecabridge-patterns.md`](https://github.com/ibrews/spellrot/blob/main/docs/engine-reference/unreal/ecabridge-patterns.md)
