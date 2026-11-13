# Final fixes report (commit 9858910)
1. morpheus_target_defaults: COMPILE_WARNING_AS_ERROR property, flags kept; morpheus_sqlite3 set OFF. Verified: scratch consumer configure with -DCMAKE_COMPILE_WARNING_AS_ERROR=ON -> no TreatWarningAsError in any generated Morpheus project (only the host consumer_check.vcxproj has true); tests build: morpheus_core.vcxproj has TreatWarningAsError>true (4), morpheus_sqlite3.vcxproj none.
2. FetchContent name morpheus_sqlite, ${morpheus_sqlite_SOURCE_DIR}.
3. README: SYSTEM in snippet and consumer-check declaration, nlohmann-before-Morpheus order, -Werror note.
4. README Linux: status wording, git package, CMAKE_CUDA_ARCHITECTURES=86 paragraph.
5. dev-preset mentions: CLAUDE.md main.cpp, spec llama row, README morpheus_play, plus README "models in-process" and "Play in the app" lines.
6. "GPU backend" in README and CLAUDE.md.
7. README line 5: "The C# implementation this engine replaces is gone: `master` is the C++ engine."
Verification: tests preset 315/315 pass; consumer check from scratch prints "consumer_check: ok (rolled 15, total 18)", exit 0; dev app built (RelWithDebInfo), --frames 120 exit 0; ASCII check of three docs clean.
Note: files are CRLF in the working tree (CMakeLists, CLAUDE.md); edits preserved endings.
