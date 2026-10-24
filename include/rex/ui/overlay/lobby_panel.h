/**
 * @file        rex/ui/overlay/lobby_panel.h
 *
 * @brief       A game-driven menu panel for menus layered on the front end
 *              (e.g. a co-op lobby). The game owns all state and input; the
 *              panel only draws. It draws in the game's own menu style: the
 *              game's bitmap font (rex_ui_set_lobby_font: a Fable II .bft file
 *              from the player's own game data) and bar rows shaped and
 *              coloured like the stock main menu, at its positions.
 *
 * View text, one item per line (empty view = hidden):
 *   T<title>   title                     N<text>   notice line
 *   S<text>    section heading           L<text>   plain line
 *   D<text>    greyed line               F<text>   footer hint
 *   R<e><s><label>   menu row: e = '1' enabled, s = '1' selected
 *   C<text>    full-screen cover card: hides everything behind it; the other
 *              lines are shown on the card
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <memory>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class ImmediateDrawer;
class ImmediateTexture;

class LobbyPanelDialog : public ImGuiDialog {
 public:
  explicit LobbyPanelDialog(ImGuiDrawer* imgui_drawer);
  ~LobbyPanelDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  bool EnsureFont();
  std::unique_ptr<ImmediateTexture> font_texture_;
  ImmediateDrawer* font_drawer_ = nullptr;  // the drawer the texture belongs to
};

}  // namespace rex::ui

// Sets the panel's view (UTF-8, format above); "" hides it. Any thread.
extern "C" void rex_ui_set_lobby_view(const char* view);
// A full-screen cover card drawn above everything, independent of the view
// (the view's own changes never clear it): "" hides it; lines after the first
// (newline-separated) are shown under it, dimmed. Any thread.
extern "C" void rex_ui_set_lobby_cover(const char* text);
// The .bft font the panel draws with ("" = ImGui's own font). Any thread.
extern "C" void rex_ui_set_lobby_font(const char* bft_path);
