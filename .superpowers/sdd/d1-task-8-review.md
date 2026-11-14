### Spec Compliance
- Spec compliant. Maintain is verbatim the brief; header comment and constant match.
- Cannot verify from diff: build/test results (report claims 496 passing, asan clean); not re-run.

### Strengths
- Maintain reuses Upkeep/FoldAll/Pass instead of duplicating fold logic (HostedSession.cpp:51-75).
- Cancel consistency checked: the StoreFold callback (HostedSession.cpp:520-523) calls RunStore::FoldConversation, which inserts the summary and moves folded_through in one transaction (RunStore.cpp:691-698). FoldOldest only calls _storeFold after SummarizeTurns returned a summary (MemoryUpkeep.cpp:190-196), so a cancel can only land before a fold, never between summary and mark. SyncHistory runs after the transaction and rebuilds from all conversations' folded entries, so it re-converges on rerun.
- The mark is per conversation: the loop skips conversations with no turns above their own foldedThrough, Pass reads that conversation's entries only (Entries uses its folded mark), FoldConversation requires toTurn <= LatestHostedTurn and > the conversation's mark. Run-wide turn numbers are used throughout.
- Tests would fail if folding did nothing: they assert summary rows with exact range/text, foldedThrough, L2 text, empty L3 (stableCount == 1), the retrieved "Turn 1" line in the later conversation, and no backend calls on an idempotent rerun.

### Issues
#### Critical (Must Fix)
None.
#### Important (Should Fix)
None.
#### Minor (Nice to Have)
1. tests/hosted/HostedFoldingTests.cpp:224-246: the cancel test sets the stop token before the call, so it only proves "cancel before any fold". It does not cover a cancel after the first fold lands (e.g. a stop requested from the responder on the 2nd call, or two ended conversations with the cancel between them), which is the consistency case the spec stresses.
2. tests/hosted/HostedFoldingTests.cpp:184-222: no test with two conversations interleaved (A turn 1, B turn 2, A turn 3) to pin that a conversation's fold mark covers only its own turns and that retrieved turns carry run-wide numbers; nor one where Maintain condenses on the way or folds in several calls. The code handles them (shared path), but they are unpinned.
3. tests/hosted/HostedFoldingTests.cpp:74: `prompt.stableCount < 1 + 2 * 5` is a loose bound; it passes after any fold. Asserting the exact count implied by foldedThrough would be tighter.
4. src/turn/HostedSession.cpp:53,182: the fold seed turn is `latest` for every ended conversation (same seed for each conversation's calls); harmless, but two conversations' fold calls share (turn, attempt) seeds. Intentional per the brief; just noting.

### Assessment
**Task quality:** Approved
**Reasoning:** Maintain is a thin, correct use of the shared upkeep; the summary and fold mark land in one transaction, so a cancel leaves nothing half-written, and the tests genuinely depend on folding. Only test-coverage polish remains.
