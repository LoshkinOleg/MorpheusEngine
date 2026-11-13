# Model seat final fixes report

Commit 3830693 (one commit, only my files; staged .gitmodules left out).

1. Tab to front without focus (ModelSeatWindow.cpp, UiTests.cpp). Deviation from the brief: NextSelectedTabId on a
   dock node's tab bar makes DockNodeUpdateTabBar call FocusWindow on that tab (imgui.cpp ~19742), i.e. it steals
   focus. So the window sets the tab bar's SelectedTabId (not Next...) directly. The request check also moved BEFORE
   ImGui::Begin: a window in a hidden tab returns false from Begin, so the old in-body check never ran while hidden.
   Limitation: if the focused window is in the same dock node, ImGui forces that tab (NavWindow wins); fine.
   Test "A call that arrives brings the Model seat tab to the front without taking the keyboard focus": Play focused,
   State tab selected in the seat's node, a Human call arrives; seat tab selected, NavWindow still Play. GREEN.
   (RED earlier: tab never selected / my first test draft; the real RED was the missing feature.)
2. Keep the typed reply (same label + prompt under a new id). Window stores _label/_prompt. Test "keeps the typed
   reply...": types into the reply via mouse click + AddInputCharacters, withdraws, same call republished -> Submit
   returns "typed"; a call with another prompt -> reply empty. RED: ImGui assertion (IM_ASSERT throws) because the
   reply string was reset while its box was active. Found a real bug: any reset while the reply box is active would
   assert in the app. Fix: ImGui::ClearActiveID() on reset (verified the test fails without it). GREEN.
3. Grammar drawn as a read-only InputTextMultiline (GRAMMAR_LINES = 8).
4. ModelSeatBackendTests: sleep replaced by CHECK_FALSE(worker.WaitEvent(300ms)), comment updated.
5. CheckGrammar: MORPHEUS_REQUIRE on the end token + add_space_prefix comment.
6. ModelSeat.h Await comment, architecture spec (lines 16, 18, CheckGrammar bullet), README Model seat paragraph.

Verification
- `[model_seat],[ui],[app_controller]` x3: all passed (3693/3694/3694 assertions, 38 cases).
- `ctest --preset tests`: 100% passed, 313 tests.
- `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests`: built; `morpheus.exe --frames 120`
  exit 0.
- dev `[llama]` CheckGrammar test with the Qwen/nomic models: passed (8 assertions).

## Fix round 2

1. ClearActiveID only for this window's items. Test "A new call leaves another window's active input alone"
   (scratch window input active, a call arrives): RED `CHECK(ActiveID == scratchId)` 0 == 1199209431; GREEN after fix.
   The suggested ids (##reply/##thinking) were not enough: after pressing Submit the active item is a button of the
   seat window and a reset then still tripped the IM_ASSERT in the keep test. So the check is
   `g.ActiveIdWindow->RootWindow == seat window`.
2. Answered call forgotten (`_prompt = {}` on Submit). Test "A Draft call that repeats an answered call starts from
   its draft": RED reply "" == "the draft"; GREEN.
3. Tab test asserts DockNode->VisibleWindow == pSeat; spec line 18 reworded.
Verification: [model_seat],[ui],[app_controller] x3 pass (3708/3708/3709 assertions, 40 cases); ctest 100%;
dev morpheus builds, --frames 120 exit 0.
