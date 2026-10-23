/**
 * @file        ui/overlay/lobby_panel.cpp
 * @brief       Game-driven lobby panel (see rex/ui/overlay/lobby_panel.h).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/overlay/lobby_panel.h>
#include <imgui.h>
#include <mutex>
#include <sstream>
#include <string>

namespace rex::ui {

namespace {

std::mutex g_view_mutex;
std::string g_view;

const ImVec4 kSelected{1.00f, 0.85f, 0.30f, 1.00f};
const ImVec4 kNormal{1.00f, 1.00f, 1.00f, 1.00f};
const ImVec4 kDisabled{0.50f, 0.50f, 0.50f, 1.00f};

}  // namespace

LobbyPanelDialog::LobbyPanelDialog(ImGuiDrawer* imgui_drawer) : ImGuiDialog(imgui_drawer) {}
LobbyPanelDialog::~LobbyPanelDialog() {}

void LobbyPanelDialog::OnDraw(ImGuiIO& io) {
  std::string view;
  {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    view = g_view;
  }
  if (view.empty()) return;

  std::istringstream lines(view);
  std::string line, title = "Lobby";
  if (std::getline(lines, line) && !line.empty() && line[0] == 'T') title = line.substr(1);
  else lines.seekg(0);

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.92f);
  const std::string id = title + "###rex_lobby_panel";
  if (!ImGui::Begin(id.c_str(), nullptr,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::End();
    return;
  }
  while (std::getline(lines, line)) {
    if (line.empty()) continue;
    const char kind = line[0];
    const std::string text = line.substr(1);
    switch (kind) {
      case 'N': ImGui::TextWrapped("%s", text.c_str()); break;
      case 'S': ImGui::SeparatorText(text.c_str()); break;
      case 'L': ImGui::TextUnformatted(text.c_str()); break;
      case 'D': ImGui::TextDisabled("%s", text.c_str()); break;
      case 'F':
        ImGui::Separator();
        ImGui::TextDisabled("%s", text.c_str());
        break;
      case 'R':
        if (text.size() >= 2) {
          const bool enabled = text[0] == '1', selected = text[1] == '1';
          ImGui::TextColored(!enabled ? kDisabled : selected ? kSelected : kNormal, "%s %s",
                             selected ? ">" : " ", text.c_str() + 2);
        }
        break;
      default: break;
    }
  }
  ImGui::End();
}

}  // namespace rex::ui

extern "C" void rex_ui_set_lobby_view(const char* view) {
  std::lock_guard<std::mutex> lock(rex::ui::g_view_mutex);
  rex::ui::g_view = view ? view : "";
}
