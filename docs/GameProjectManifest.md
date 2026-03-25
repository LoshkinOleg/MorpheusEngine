# Game Project Manifest Reference

This document defines the expected `game_projects/<id>/manifest.json` structure.

## Required fields

- `id: string`
- `title: string`
- `engineVersion: string`
- `entryState: { playerId: string; anchorId: string }`
- `moduleBindings: Record<string, string>`

Runtime contract source: `apps/backend/src/gameProject.ts` (`GameProjectManifest`).

## Module bindings

The backend router resolves module endpoints from `moduleBindings` in this order:

1. If the binding value is a full URL (`http://` or `https://`), use it directly.
2. Otherwise, treat the binding as a logical key and check env overrides:
   - role-specific: `MODULE_BINDING_<BINDING>_<ROLE>_URL`
   - generic: `MODULE_BINDING_<BINDING>_URL`
3. If no override exists, fall back to role defaults:
   - intent: `MODULE_INTENT_URL` (default `http://localhost:8791`)
   - intent validator: `MODULE_INTENT_VALIDATOR_URL` (default `http://localhost:8796`)
   - simulator: `MODULE_DEFAULT_SIMULATOR_URL` (default `http://localhost:8793`)
   - proser: `MODULE_PROSER_URL` (default `http://localhost:8794`)
   - arbiter: `MODULE_ARBITER_URL` (default `http://localhost:8795`)

`<BINDING>` is uppercased and non-alphanumeric characters are normalized to `_`.

Example:

- binding value: `llm.default`
- role: `intent`
- resolved env key: `MODULE_BINDING_LLM_DEFAULT_INTENT_URL`

## Example

```json
{
  "id": "sandcrawler",
  "title": "Sandcrawler Captain",
  "engineVersion": "0.1.x",
  "entryState": {
    "playerId": "player.captain",
    "anchorId": "anchor.bridge"
  },
  "moduleBindings": {
    "intent_extractor": "llm.default",
    "intent_validator": "llm.default",
    "default_simulator": "llm.default",
    "arbiter": "llm.default",
    "proser": "llm.default"
  }
}
```
