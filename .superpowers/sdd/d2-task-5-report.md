# Task 5 report: SentenceSplitter
Implemented server::SentenceSplitter verbatim from the brief; added SentenceSplitter.cpp to src/server/CMakeLists.txt.
RED: build with test file but cpp not in CMake -> LNK2019 unresolved SentenceSplitter::Add/Finish (the brief's include error did not occur because the header was already written; link failure is the equivalent).
GREEN: `morpheus_tests.exe "[sentences]"` -> All tests passed (14 assertions in 6 test cases); ctest --preset tests: 100% passed out of 527. No warnings.
Byte greps (all three files): both print 0; `\xE2` escapes still literal (tests 5 lines, cpp 2 lines).
Deviation: first write via heredoc failed on shell quoting; used Write tool, which preserved escapes. Needed `git add` for new files before commit.
