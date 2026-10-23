/**
 * @file        rex/ui/overlay/lobby_panel.h
 *
 * @brief       A centred panel drawn from a text view the game sets, for game
 *              menus layered on the front end (e.g. a co-op lobby). The game
 *              owns all state and input; the panel only draws.
 *
 * View text, one item per line (empty view = hidden):
 *   T<title>   window title          N<text>   notice line (wrapped)
 *   S<text>    section heading       L<text>   plain line
 *   D<text>    greyed line           F<text>   footer hint
 *   R<e><s><label>   menu row: e = '1' enabled, s = '1' selected
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class LobbyPanelDialog : public ImGuiDialog {
 public:
  explicit LobbyPanelDialog(ImGuiDrawer* imgui_drawer);
  ~LobbyPanelDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace rex::ui

// Sets the panel's view (UTF-8, format above); "" hides it. Any thread.
extern "C" void rex_ui_set_lobby_view(const char* view);
