# Idea: import character cards into a world

Date: 2026-11-06. Status: parked idea, not scheduled. Not an approved design; brainstorm it with the owner before writing a spec.

## Goal
- Let people bring the characters they like into a world.
- Characters come as community character cards (the SillyTavern formats), which are widespread.
- The engine itself keeps working only with the world format: world.json, rules.md, lore.csv, state_schema.json and initial_state.json.

## Owner decisions so far
- **A separate tool, not engine support.** It reads a folder of cards and folds them into a world's initial files. The engine never learns what a card is.
- **Cards seed a world; they do not create one.** A world is still authored, or generated, as a world. Cards only add characters to it.

## Today
A character has no single home in a world. It is split across up to three places:
- **Mechanics:** an entity in initial_state.json of a schema type. Sandcrawler's `npc` type has `role`, `disposition`, `location`, `conditions` and `health`, and no free-text description.
- **Description:** a lore.csv row whose subject is the name. It is retrieved when relevant, or always included with `always_on`.
- **Behavior:** prose in rules.md.
- **During play:** the model creates NPCs through `create` on a creatable type.

## Card formats
- **V1/V2:** JSON, or a PNG with the JSON base64-encoded in a tEXt chunk named `chara`.
- **V3:** a `ccv3` chunk, or a `.charx` zip.
- **Fields:**
  - name, description, personality, scenario
  - first_mes, mes_example
  - system_prompt and post_history_instructions
  - an optional character_book: a lorebook whose entries have trigger keys and content
  - an avatar image
- Reading a PNG card only means finding the text chunk. The image itself is never decoded.

## Sketch
- **Usage:** `morpheus_cards <cards folder> <world folder>`, a tool beside tools/play and tools/probe.
- **Entities.** Each card becomes an entity in initial_state.json of a type chosen per import, e.g. `--type npc`.
  - Fields start from the schema's defaults, since cards carry no stats.
  - An optional mapping file sets values per card, e.g. `disposition: wary`.
- **Lore.** Each card becomes a lore.csv row keyed by the character's name, holding:
  - the description and personality;
  - the scenario, when it describes the character rather than a chat setup;
  - a few lines of mes_example, as a voice sample.
- **Character books.** character_book entries become further lore rows. Their trigger keys can choose the subject; the other keys are dropped, since retrieval is by embedding similarity.
- **Ignored:** system_prompt, post_history_instructions and first_mes. These are chat-persona instructions that would fight rules.md.
- **Avatar.** Copied beside the world for the portraits in the narrative visualization spec (docs/superpowers/specs/2026-10-27-narrative-visualization-design.md).
- **Validation.** Before anything is written, the result is loaded with core's own world loader, the way the app's Settings Apply checks settings.json.
  - A name that clashes with an existing lore subject or entity id stops the import with an error naming the card.
  - Nothing is written on failure.
- **Re-running.** The tool records which rows and entities came from which card, e.g. in a manifest file in the world folder. Importing again updates them instead of duplicating them, and never touches hand-written entries.

## Open questions
1. **Long cards.** Card descriptions often run 1,000 to 3,000 tokens, but retrieved lore shares `retrieval_cap`, which is 768 in the example settings. The options:
   - cut to a token limit;
   - summarize with a model, which makes the tool need the `dev` preset and a loaded model;
   - give the characters on stage their own budget layer.
2. **Entity ids.** Should they be derived from card names (with what normalization), or given in the mapping file?
3. **Content.** Cards vary widely in quality and content. Should the tool warn about, or filter, anything?
4. **Portrait source.** Should the avatar feed the visualization spec's portraits directly, or only as a reference image?
