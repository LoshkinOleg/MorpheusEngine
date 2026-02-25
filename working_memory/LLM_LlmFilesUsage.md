# LLM Files Usage (project memory system)

This workspace can contain multiple repos/projects. Because LLM context windows are limited, we use **repo-native, versioned “memory files”** to preserve high-signal context across sessions and across different models.

These files are written for **humans and LLMs**:
- Humans: skimmable, operationally useful, reviewable in PRs.
- LLMs: compact, structured, evidence-backed “state checkpoints”.

> **Hard rule:** LLM memory files must be **truthy and evidence-backed**, not “plausible-sounding”.
> If you can’t point to evidence, write it under **To verify**, not under **Facts**.

---

## 1) Naming, scope, and where these files live

### File naming
- Memory files are named: `LLM_<Subject>.md`
- `<Subject>` should be stable and broad enough to avoid file explosion.
  - Examples: `LLM_BackEnd.md`, `LLM_Deployments.md`, `LLM_Security.md`, `LLM_Analytics.md`, `LLM_Dashboards.md`, `LLM_<GameTitle>.md`.

### Where they live
- Place `LLM_*.md` files under working_memory/llm_files/ .
- Keep the location consistent so new models can reliably find them.

### What goes into an LLM file
LLM files capture **durable, expensive-to-rediscover context**, such as:
- Architecture and invariants (“must always be true”)
- Integration contracts (endpoints, event names, payload shapes)
- Operational workflows (deploy, rotate keys, add a new game)
- Environment mapping (prod vs dev, domains, buckets)
- Known pitfalls (CORS, caching, WebGL quirks, auth gotchas)
- Postmortem-style learnings (what broke, how to prevent recurrence)

### What does NOT go into an LLM file
- Secrets (tokens, passwords, connection strings, private keys)
- One-off discussion-style prose
- Long rationale essays (those belong in LLM_Changelog's; see below)
- Claims that are not grounded in evidence

---

## 2) The “facts vs hypotheses” write barrier (critical)

LLM files are only useful if they stay accurate.

### 2.1 Evidence-backed facts
Any statement in **Facts / Contracts / Workflows / Gotchas** must include evidence:
- A **file pointer** (path + identifier)
  - Example: `BackEnd/src/Auth/RequestSigner.cs::Validate()`
- A **command** that reproduces behavior
  - Example: `npm run build && npm run preview`
- A **configuration key name** (not its secret value)
  - Example: `AZURE_STORAGE_ACCOUNT_NAME`, not the account name itself
- A **commit/PR reference** (if available)

If evidence cannot be provided, do not write it as fact.

### 2.2 “To verify” for uncertainty
If you suspect something is true but cannot confirm, write it in:
- `## To verify`
- include what would confirm it (file, test, log line, portal setting, etc.)

---

## 3) Consistent internal structure (make files skimmable)

All `LLM_*.md` files should be short, structured, and consistent.
Prefer **bullets and checklists** over paragraphs.

Recommended sections (in this order):
1. **Purpose**
2. **Last verified** (YYYY-MM-DD)
3. **Owner / Source of truth**
4. **Invariants / Contracts (Facts)**
5. **Key entry points** (folders, projects, key files)
6. **Operational workflows** (checklists)
7. **Gotchas / footguns**
8. **Recent changes** (optional, short)
9. **To verify**
10. **References / Links** (internal file pointers, LLM_Changelog links)

---

## 4) Avoiding duplication and contradictions

### 4.1 One source of truth per topic
To prevent drift:
- Each topic should have **one owning file** that is the source of truth.
- Other files should link to it instead of duplicating content.

Example:
- `LLM_Security.md` owns request signing, auth flows, key rotation.
- `LLM_BackEnd.md` links to `LLM_Security.md#Request-signing` rather than restating it.

### 4.2 Cross-links are preferred
If a workflow touches multiple areas:
- put the full workflow in the owning file
- add short “See also” links elsewhere

---

## 5) “Memory” vs “Decision history” (use LLM_Changelog's)

LLM files are **current truth** on **how to operate**. Code source files are **current truth** on **how systems function**.

When documenting *why a choice was made*, use LLM_Changelog's:
- `working_memory/changelogs/LLM_Changelog_####-short-title.md`
- LLM_Changelog's should be mostly immutable once accepted.

LLM files may reference LLM_Changelog's:
- “We do X (see LLM_Changelog_0012 for rationale).”

---

## 6) When to create a new LLM subject file

Create a new file when:
- The topic spans multiple repos/projects (e.g., signing touches Unity + BackEnd + Dashboards).
- The topic is operationally critical (deployments, incident response, key rotation).
- The topic frequently breaks (CORS, caching, WebGL hosting).
- The topic is a “product area” with recurring work (analytics pipeline, dashboards auth).

Avoid creating a file when:
- It’s a one-off incident (document in a LLM_Changelog instead).
- It duplicates an existing file’s scope.

---

## 7) Size and maintainability constraints

To keep files usable:
- Prefer each file to stay under **~200–400 lines**.
- If it grows:
  - extract a subtopic into a new owning file
  - leave a link + a short summary in the original

---

## 8) Update protocol (how LLMs should edit memory)

When you (LLM) finish work that changed behavior:
1. Identify which LLM file owns the topic.
2. Update only the relevant sections (don’t rewrite the whole doc).
3. Add/refresh **Last verified** date.
4. Add evidence pointers for each new/updated claim.
5. If unsure, add to **To verify**, not Facts.
6. If a decision/rationale changed, add or update a LLM_Changelog and link it.

**Do not**:
- “clean up” aggressively unless asked (risk of introducing errors).
- change facts without evidence.
- merge sections that make scanning harder.

---

## 9) Secrets and sensitive data policy

Never include:
- passwords, tokens, connection strings
- private keys / certificates
- full PII datasets
- anything that would compromise security if committed

Allowed:
- names of env vars and configuration keys
- high-level descriptions of secret sources (Key Vault, GitHub Secrets, etc.)
- placeholders: `<SET_IN_AZURE_PORTAL>`, `<SEE_KEY_VAULT_SECRET_NAME>`

---

## 10) Reading order (what an LLM should do first)

When starting work in this repo:
1. Read **this** file: `LLM_LlmFilesUsage.md`.
2. Read `LLM_Overview.md` (workspace routing / index).
3. Read the owning subject file(s) for your task.
4. Only then: inspect code / make changes.

---

## Appendix A: Suggested core file taxonomy (keep small)

Common “core” files that tend to stay stable and useful:
- `LLM_Overview.md` (router / index)
- `LLM_Deployments.md` (environments, domains, build + release steps)
- `LLM_Security.md` (auth, signing, secrets handling, key rotation)
- `LLM_<GameProject>.md` per game project (only if the game has unique integrations/workflows)

Keep “nice-to-have” topics as sections inside existing files until they justify a split.

---

## Appendix B: Copy-paste template

When creating a new `LLM_*.md` file, use:
- `LLM_LlmFileTemplate.md`