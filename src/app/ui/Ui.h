#pragma once

#include "app/model/AppController.h"
#include "app/ui/Windows.h"

namespace morpheus::app {

// Every window, docked over the main viewport, with a status bar along the bottom. Draw once per frame between
// ImGui::NewFrame and ImGui::Render, after AppController::Frame.
class Ui {
private:
    RunsWindow _runs = {};
    PlayWindow _play = {};

public:
    void Draw(AppController& controller);
};

} // namespace morpheus::app
