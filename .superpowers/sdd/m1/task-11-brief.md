### Task 11: Milestone verification

**Files:**
- No new files. Fix anything these steps uncover in the file that caused it, and commit the fix.

**Interfaces:**
- Consumes: everything above.
- Produces: confirmation that the milestone builds and passes under AddressSanitizer, and that the prototype app still builds with the new CMake layout.

- [ ] **Step 1: Run the full suite under AddressSanitizer**

Run: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: `100% tests passed`, with no `AddressSanitizer` report in the output.

- [ ] **Step 2: Confirm the app configuration still builds**

Run: `cmake --preset dev && cmake --build --preset dev`
Expected: the configure step fetches llama.cpp, SDL3 and ImGui, and the build produces `build/dev/bin/Debug/morpheus.exe` plus `morpheus_tests.exe`. This takes a while on the first run. Do not change `src/main.cpp` to fix warnings: that target is not built with `/WX`, and milestone 4 replaces it.

- [ ] **Step 3: Run the tests from the dev build**

Run: `ctest --preset dev`
Expected: `100% tests passed`.

- [ ] **Step 4: Commit fixes, if any**

If Steps 1-3 required changes, commit the changed files:

```bash
git add <changed files>
git commit -m "Fix issues found in milestone 1 verification" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

If nothing changed, there is nothing to commit.
