# Task 12 report: documentation

Commit abf81cb. No code, no tests run. Byte checks (>0x7F, control bytes): 0 and 0 for all five files.

Files: README.md (new section after "Play in the terminal", Layout lines, [server] tests sentence), CHANGELOG.md (new),
CLAUDE.md (CRLF kept), architecture spec (server row, events omitted, Hosted runs sentences), hosted-world spec (section 6 bullet, section 1 pointer).

Adjustments: README binary paths are real (build/tests/bin/Debug/, build/dev/bin/<config>/). Example session gained an
events step (`{"type": "heard", ...}`, matching the fixtures README) and an end step. README notes the review behaviours:
run_id differing only in case is refused (400), open after shutdown begins is 503. HostedSession's StoreError for a
too-large stored events_omitted and HttpServer's TCP_NODELAY are not documented (internal; no brief text touches them).
No measured figures written. Concern: none.

## Fix round
Architecture spec: schema version 4 (two places); slot sentence marked as D1's contract. Hosted-world spec: slot bullets
marked D1's, pointer that D2 moved the store to version 4 (version 3 history kept). README: refused action shape, token
count, `\` continuations restored, measured figure added.
