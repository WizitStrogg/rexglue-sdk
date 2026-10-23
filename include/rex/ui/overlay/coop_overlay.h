/**
 * @file        rex/ui/overlay/coop_overlay.h
 *
 * @brief       ImGui co-op overlay: player name, System Link on/off, LAN
 *              players with join / invite, save-to-config.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <filesystem>
#include <string>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class CoopDialog : public ImGuiDialog {
 public:
  // config_path: where "Save to config" writes (e.g. exe_dir / "app.toml")
  CoopDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path);
  ~CoopDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::filesystem::path config_path_;
  char name_buf_[16] = {};
  char interface_buf_[64] = {};
  char peers_buf_[256] = {};
  std::string saved_note_;
};

}  // namespace rex::ui
