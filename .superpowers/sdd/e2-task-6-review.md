### Spec Compliance
- Spec compliant (fix round 9292990, docs only). Every item of the previous review is addressed.

### Strengths
- Architecture spec rows now match the schema: `summaries` no longer a `superseded` column (RunStore.cpp:192), `conversations` keeps id and folded through (RunStore.cpp:193-194, 198), `conversation_changes` holds start/join/leave/end with participants and ended flag (RunStore.cpp:167-174), `pending_shares` describes applied rows and `applied_clock`/`unapplied_by` (RunStore.cpp:1184-1211).
- The corrected version sentence "a version 2, 3, 4 or 5 run is upgraded in place; any other version is refused" matches RunStore.cpp:541 and the comment above it.
- `clock?` in the open rows (README, server spec) matches HostedWorld.cpp:72-76; README's `rewound`/`hidden_turns`/`restored_turns` text matches HostedSession.h:54-56; server spec api_version and CHANGELOG sentences and the per-speaker spec notes are accurate and point to the right sections.

### Issues
#### Critical (Must Fix)
None.

#### Important (Should Fix)
None.

#### Minor (Nice to Have)
- Architecture spec version note: the earlier sentence "A version 2 run is upgraded in place on open..." now sits next to the new "A version 2, 3, 4 or 5 run is upgraded in place" sentence, and the parenthesis still says "an older run is upgraded in place"; slightly redundant, not wrong.
- Per-speaker spec line 170 (`core::Summary` ... `superseded` (false)) is still E1's description; `Summary::superseded` remains a derived field in code (RunStore.cpp:208-209), so it is correct.

### Assessment
**Task quality:** Approved
**Reasoning:** The stale table rows and version sentence now agree with the version 6 schema and the upgrade code; the remaining nits are cosmetic.
