# Task 2 report: lore scopes and conditions (data)

Implemented: core::LoreCondition (+Holds in World.cpp), LoreEntry::scopes/condition, Actor::knows read by ReadActors
(fault at /actors/<i>/knows), host data scopes/requires read (faults at /lore/<i>/scopes, /requires, /requires/<key>),
written only when present (hash 35856ce76ac0af2e unchanged), HostedWorld ctor checks (always-on with scopes; pointer
well-formed). Holds throws ContractError on a malformed pointer.

TDD: RED = tests built against stashed src: C4430/C2146 compile errors in LoreConditionTests.cpp (LoreCondition
missing). GREEN: `morpheus_tests.exe "[lore]"` -> All tests passed (67 assertions in 8 test cases);
`ctest --preset tests` -> 100% tests passed out of 736 (the [llama]/[server] ones skipped).
ASCII and control-byte greps: 0 and 0 for all eight touched files.

Deviations: numeric `55_json` literals do not exist in nlohmann; tests use nlohmann::json(55). RED was a compile
failure, not an assertion failure. Pointer-form fault message is "lore at /<i>/requires/pointer: '<p>' is not a JSON
pointer: <nlohmann text>" (tested by prefix).
Concerns: none. Retrieval/prompt use is Task 3.
