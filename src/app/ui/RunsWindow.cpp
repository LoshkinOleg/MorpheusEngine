#include "app/ui/Windows.h"

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/Text.h"

namespace morpheus::app {

void RunsWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(RUNS_WINDOW)) {
        // The open run.
        if (const std::optional<turn::LoadRun>& open = controller.OpenRun()) {
            ImGui::Text("Open: %s / %s", controller.Run().Meta().worldId.c_str(),
                        core::PathText(open->runFile.stem()).c_str());
        } else {
            ImGui::TextDisabled("No run open");
        }

        // The worlds. Refresh lists them again after world files were added or edited.
        ImGui::SeparatorText("Worlds");
        if (ImGui::SmallButton("Refresh")) {
            controller.RefreshWorlds();
            _selectedWorld = 0;
            _suggestName = true;
            _runsKey.clear();
        }
        const std::vector<WorldEntry>& worlds = controller.Worlds();
        if (worlds.empty()) {
            ImGui::TextDisabled("No worlds in %s", core::PathText(controller.Paths().projectsDir).c_str());
        }
        for (std::size_t i = 0; i < worlds.size(); ++i) {
            const WorldEntry& world = worlds[i];
            ImGui::PushID(static_cast<int>(i));
            if (!world.error.empty()) {
                TextWrappedColored(ERROR_COLOR, std::format("{}: {}", world.id, world.error)); // Not playable.
            } else if (ImGui::Selectable(std::format("{} ({})", world.title, world.id).c_str(), _selectedWorld == i)) {
                if (_selectedWorld != i) {
                    _selectedWorld = i;
                    _suggestName = true;
                }
            }
            ImGui::PopID();
        }
        if (_selectedWorld >= worlds.size() || !worlds[_selectedWorld].error.empty()) {
            ImGui::TextDisabled("Select a world to start or continue a run.");
        } else {
            const WorldEntry& world = worlds[_selectedWorld];

            // A new run: a name (suggested from the clock) and an optional seed.
            ImGui::SeparatorText("New run");
            if (_suggestName) {
                _runName = DefaultRunName(std::chrono::system_clock::now());
                _suggestName = false;
            }
            ImGui::InputText("Name", &_runName);
            ImGui::InputTextWithHint("Seed", "random", &_seedText);
            const std::string nameProblem = RunNameProblem(controller.Paths().savesDir, world.id, _runName);
            const SeedInput seed = ParseSeed(_seedText);
            const std::string problem = nameProblem.empty() ? seed.problem : nameProblem;
            if (!problem.empty()) {
                TextWrappedColored(ERROR_COLOR, problem);
            }
            ImGui::BeginDisabled(!controller.CanPost() || !problem.empty());
            if (ImGui::Button("Start")) {
                controller.NewRun(world, _runName, seed.seed);
                _suggestName = true;
                _seedText.clear();
            }
            ImGui::EndDisabled();

            // The world's saved runs. They are listed again when the world, the open run or its turn count changes:
            // a turn writes its run's file, which moves it to the top.
            const std::string runsKey =
                std::format("{}|{}|{}", world.id,
                            controller.OpenRun() ? core::PathText(controller.OpenRun()->runFile) : std::string(),
                            controller.Run().History().size());
            if (runsKey != _runsKey) {
                _runs = ListRuns(controller.Paths().savesDir, world.id);
                _runsKey = runsKey;
            }
            ImGui::SeparatorText("Saved runs");
            if (_runs.empty()) {
                ImGui::TextDisabled("No runs yet.");
            }
            for (const RunEntry& run : _runs) {
                ImGui::PushID(run.name.c_str());
                ImGui::BeginDisabled(!controller.CanPost());
                if (ImGui::SmallButton("Continue")) {
                    controller.ContinueRun(world, run);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextUnformatted(run.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s", LocalTimeText(run.lastWrite).c_str());
                ImGui::PopID();
            }
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
