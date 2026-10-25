# Guildleves: what it takes

Research notes (2026-10-23) on getting guildleves working with the native client and the Garlemald
server.

## What Garlemald already has

- **Leve catalog:** `gamedata_guildleves` (624 rows) and `gamedata_regional_leves` in its database,
  with per-character state in `characters_quest_guildleve_local` / `_regional`.
- **Rust support:** `map-server/src/leve/` (catalog, resolver, per-quest progress view) and
  `map-server/src/director/guildleve*.rs` (the guildleve director and its work state).
  - Fieldcraft leves count successful gathers of the target item.
  - Battlecraft leves count kills of the target monster class (`onKillBNpc`).
  - Garlemald says these two types are its own design; no Project Meteor port existed to copy.
  - Tradecraft (local) leves follow Meteor's `crafting_and_localleves` branch.
- **Lua:**
  - Leve-giver NPCs: `PopulaceGuildlevePublisher.lua` (menu, card grid, detail, accept). Accepting a
    battlecraft card resolves it to a catalog leve and adds it to the journal.
  - Leve content: `directors/Guildleve/` (Hunt, Chase, Detect, Gather, Orb, Round, Survive, Sweep and
    Tutorial directors) and `GuildleveWarpPoint.lua`.
  - GM commands: `addguildleve <id>` and `removeguildleve`.
- **Recent work:** the "regional-leve accept" pipeline landed recently (commit 638f3c8), so this is
  active, partly finished work upstream.

## What is needed to play one

1. **Get past the city tutorials.** Leve publishers stand in the cities after the opening quest
   (`man0l0`/`man0g0`/`man0u0`) and the first city quest (`man0l1`/`man0g1`/`man0u1`). Today the
   openings work except for the tutorial-talk lockup (click an NPC twice while a tutorial box is up
   and the client waits for good) and the broken cutscenes, which can be skipped.
2. **Client side (ours).** A leve uses things the tutorial already exercised: event menus driven by
   `callClientFunction`, the journal, battle, and a content zone with its own director (like the boat
   battle). New pieces to check:
   - the card-grid menu art (2D UI);
   - the leve's private battle area and its warp point;
   - the leve timer and "orb"/objective UI;
   - leve allowances.
   Nothing here needs new Windows APIs as far as I can tell; the risk is in rendering and in event
   flows like the ones that lock up today.
3. **Server side.** Leve rewards, faction (Grand Company) evaluation and some leve types are partial.
   Expect to fix flows the same way as the tutorial: watch the map-server log for each
   `RunEventFunction` / `EndEvent` pair and compare with the client's behavior.

## Fastest test

1. Use a character that has finished the city quest (or skip ahead with `setseq`).
2. Add a battlecraft leve with the GM console: `addguildleve <id>` (ids from `gamedata_guildleves`).
3. Start it at a guildleve warp point or aetheryte.
4. Watch the client and the map-server log through accept, start, kill count and completion.
