# LLM_<Subject> (project memory)

## Purpose
- One sentence: what this file is for and what it owns.
- What it explicitly does *not* own (link to owner files if relevant).

## Last verified
- YYYY-MM-DD (update when facts/workflows are confirmed)

## Owner / source of truth
- This file is the source of truth for: <topic list>
- Related owner files:
  - `LLM_<Other>.md#<Section>`
  - `docs/adr/ADR-####-...md` (rationale)

---

## Invariants / contracts (Facts)
> Only include evidence-backed statements here.

- **Invariant:** <what must always be true>
  - Evidence: `<path/to/file.ext>::<symbol>` or `<command>` or `<config key name>`
- **Contract:** <API/event/schema contract>
  - Evidence: `<openapi.yaml#...>` / `<code>` / `<docs link>`

---

## Key entry points
- Repos / folders:
  - `<path/>` — what it contains
- Critical files:
  - `<path/to/file>` — why it matters
- Key services/components:
  - `<component>` — responsibility, boundaries

---

## Operational workflows (checklists)

### Workflow: <name>
**Goal:** <what this achieves>  
**Preconditions:** <required access, env vars, tools>  

1. <step>
2. <step>
3. <validation step: how to know it worked>

**Rollback / recovery**
- <rollback step>
- <how to verify rollback>

**Evidence pointers**
- `<path/to/script>` / `<pipeline config>` / `<portal setting name>`

---

## Gotchas / footguns
- <Symptom> → <Cause> → <Fix>
  - Evidence: `<file pointer>` / `<issue link>` / `<commit>`

---

## Recent changes (optional; keep short)
- YYYY-MM-DD — <change summary> (evidence: <pointer>)

---

## To verify (hypotheses / open questions)
> Things suspected but not yet confirmed.

- <claim/question>
  - How to verify: <file to check / command to run / log to inspect>

---

## References / links
- Internal:
  - `<path/to/doc>` / `<path/to/config>` / `<ADR link>`
- External (only if stable and essential):
  - <vendor docs link>