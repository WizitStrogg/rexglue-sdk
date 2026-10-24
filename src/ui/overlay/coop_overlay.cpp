/**
 * @file        ui/overlay/coop_overlay.cpp
 * @brief       ImGui co-op overlay implementation.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/overlay/coop_overlay.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/lan_lobby.h>
#include <imgui.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace rex::ui {

namespace {

namespace lobby = rex::kernel::xam::lobby;

const ImVec4 kGood{0.45f, 1.00f, 0.55f, 1.00f};
const ImVec4 kWarn{1.00f, 0.80f, 0.30f, 1.00f};
const ImVec4 kMuted{0.60f, 0.62f, 0.66f, 1.00f};

void copy_flag(const char* name, char* buf, size_t size) {
  std::snprintf(buf, size, "%s", rex::cvar::GetFlagByName(name).c_str());
}

bool networking_requested() {
  const std::string mode = rex::cvar::GetFlagByName("network_mode");
  return mode == "systemlink" || mode == "lan" || mode == "1";
}

}  // namespace

CoopDialog::CoopDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path)
    : ImGuiDialog(imgui_drawer), config_path_(std::move(config_path)) {
  copy_flag("profile_gamertag", name_buf_, sizeof(name_buf_));
  copy_flag("network_interface", interface_buf_, sizeof(interface_buf_));
  copy_flag("lan_peers", peers_buf_, sizeof(peers_buf_));
}

CoopDialog::~CoopDialog() {}

void CoopDialog::OnDraw(ImGuiIO& /*io*/) {
  ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.90f);
  if (!ImGui::Begin("Co-op##rex", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }

  const lobby::LobbyStatus status = lobby::GetLobbyStatus();
  const bool requested = networking_requested();

  // --- You -----------------------------------------------------------------
  ImGui::SeparatorText("You");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Name");
  ImGui::SameLine(110.0f);
  ImGui::SetNextItemWidth(200.0f);
  // Shown on your orb to other players. Applies immediately.
  if (ImGui::InputText("##name", name_buf_, sizeof(name_buf_))) {
    rex::cvar::SetFlagByName("profile_gamertag", name_buf_);
  }
  ImGui::SameLine();
  ImGui::TextColored(kMuted, "(shown on your orb)");

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("System Link");
  ImGui::SameLine(110.0f);
  bool on = requested;
  if (ImGui::Checkbox("##systemlink", &on)) {
    rex::cvar::SetFlagByName("network_mode", on ? "systemlink" : "offline");
  }
  ImGui::SameLine();
  if (on != status.networking_enabled) {
    ImGui::TextColored(kWarn, "Save, then restart the game to turn it %s", on ? "on" : "off");
  } else if (status.networking_enabled) {
    ImGui::TextColored(kGood, "On");
  } else {
    ImGui::TextColored(kMuted, "Off");
  }

  bool auto_accept = rex::cvar::Query<bool>("lan_auto_accept_invites");
  ImGui::SetCursorPosX(110.0f);
  if (ImGui::Checkbox("Accept invites automatically", &auto_accept)) {
    rex::cvar::SetFlagByName("lan_auto_accept_invites", auto_accept ? "true" : "false");
  }

  // Game-specific co-op options, shown when the game defines them (the
  // overlay only knows them by name).
  auto option_checkbox = [](const char* cvar, const char* label) {
    if (rex::cvar::GetFlagByName(cvar).empty()) return;
    bool value = rex::cvar::Query<bool>(cvar);
    ImGui::SetCursorPosX(110.0f);
    if (ImGui::Checkbox(label, &value)) {
      rex::cvar::SetFlagByName(cvar, value ? "true" : "false");
    }
  };
  option_checkbox("coop_lua_runner", "Run co-op scripts (untick if the game misbehaves)");
  option_checkbox("coop_own_hero",
                  "Partners appear as their own hero (both PCs; the host dresses the partner)");
  option_checkbox("coop_own_camera",
                  "In someone else's game, my camera follows my own hero (mouse/right stick turns it)");
  auto option_metres = [](const char* cvar, const char* label, int max_m) {
    if (rex::cvar::GetFlagByName(cvar).empty()) return;
    int value = rex::cvar::Query<int32_t>(cvar);
    ImGui::SetCursorPosX(110.0f);
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::InputInt(label, &value, 1, 5)) {
      value = std::clamp(value, 0, max_m);
      rex::cvar::SetFlagByName(cvar, std::to_string(value));
    }
  };
  if (!rex::cvar::GetFlagByName("coop_leash_outdoor_m").empty()) {
    option_metres("coop_leash_outdoor_m", "m apart outdoors (host, 0 = game default)", 50);
    option_metres("coop_leash_indoor_m", "m apart indoors (host, 0 = game default)", 20);
  }
  const std::string story_sync = rex::cvar::GetFlagByName("coop_story_sync");
  if (!story_sync.empty()) {
    static const char* kChoices[] = {"ask", "always", "never"};
    int current = 0;
    for (int i = 0; i < 3; ++i) {
      if (story_sync == kChoices[i]) current = i;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Story");
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::Combo("##storysync", &current, kChoices, 3)) {
      rex::cvar::SetFlagByName("coop_story_sync", kChoices[current]);
    }
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "bring the host's story into my save (forward only)");
    const std::string pending = rex::cvar::GetFlagByName("coop_story_pending");
    if (!pending.empty()) {
      ImGui::SetCursorPosX(110.0f);
      ImGui::TextColored(kWarn, "Story waiting: %s", pending.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Apply##story")) {
        rex::cvar::InvokeCommand("coop_story_apply", "");
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Discard##story")) {
        rex::cvar::InvokeCommand("coop_story_discard", "");
      }
    }
  }

  // --- Co-op campaign (Fable-2-Recomp feature 001) ----------------------------
  if (!rex::cvar::GetFlagByName("coop_campaign").empty()) {
    ImGui::SeparatorText("Co-op campaign");
    option_checkbox("coop_campaign",
                    "New Game offers Solo / Co-op; Continue on a campaign save opens its lobby");
    const std::string campaign_status = rex::cvar::GetFlagByName("coop_campaign_status");
    if (!campaign_status.empty()) {
      ImGui::SetCursorPosX(110.0f);
      ImGui::TextColored(kMuted, "%s", campaign_status.c_str());
    }
  }

  // --- Players -------------------------------------------------------------
  ImGui::SeparatorText("Players on your network");
  if (!status.networking_enabled) {
    ImGui::TextColored(kMuted, "Turn System Link on (and restart) to find other players.");
  } else if (!status.running) {
    ImGui::TextColored(kWarn, "The LAN lobby is not running.");
  } else {
    const auto peers = lobby::GetLanPeers();
    if (peers.empty()) {
      ImGui::TextColored(kMuted, "Looking for players... (both games need System Link on)");
    } else if (ImGui::BeginTable("##players", 4,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
      ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.2f);
      ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.2f);
      ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 120.0f);
      ImGui::TableHeadersRow();
      for (const auto& peer : peers) {
        ImGui::PushID(int(peer.ip));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(peer.gamertag.empty() ? "(no name)" : peer.gamertag.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(peer.address.c_str());
        ImGui::TableNextColumn();
        if (!peer.in_world) {
          ImGui::TextColored(kMuted, "Menus");
        } else if (peer.joinable) {
          ImGui::TextColored(kGood, "In the world");
        } else {
          ImGui::TextColored(kWarn, "In the world (full)");
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!peer.joinable);
        if (ImGui::SmallButton("Join")) {
          lobby::JoinLanPeer(peer.ip);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          ImGui::SetTooltip("Join their game (you become their co-op partner)");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!status.in_world);
        if (ImGui::SmallButton("Invite")) {
          lobby::InviteLanPeer(peer.ip);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          ImGui::SetTooltip(status.in_world ? "Invite them into your game"
                                            : "Load into the world first");
        }
        ImGui::EndDisabled();
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }
  if (!status.last_event.empty()) {
    ImGui::TextColored(kWarn, "%s", status.last_event.c_str());
  }

  // --- Network details -----------------------------------------------------
  if (ImGui::CollapsingHeader("Network details")) {
    if (status.networking_enabled) {
      ImGui::Text("Game address: %s   Lobby port: UDP %u", status.game_address.c_str(),
                  unsigned(status.port));
      ImGui::Text("Online ID: %016llX", static_cast<unsigned long long>(status.online_xuid));
      ImGui::TextUnformatted("Announcing on:");
      for (const auto& adapter : status.adapters) {
        ImGui::BulletText("%s", adapter.c_str());
      }
    }
    ImGui::Spacing();
    ImGui::TextColored(kMuted,
                       "Normally leave these empty: players are found on every network "
                       "adapter automatically. Both apply after a restart.");
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputText("Network adapter IP", interface_buf_, sizeof(interface_buf_))) {
      rex::cvar::SetFlagByName("network_interface", interface_buf_);
    }
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputText("Extra player IPs", peers_buf_, sizeof(peers_buf_))) {
      rex::cvar::SetFlagByName("lan_peers", peers_buf_);
    }
  }

  // --- Footer --------------------------------------------------------------
  ImGui::Separator();
  if (ImGui::Button("Save to config")) {
    rex::cvar::SaveConfig(config_path_);
    saved_note_ = "Saved to " + config_path_.filename().string();
  }
  ImGui::SameLine();
  ImGui::TextColored(kMuted, "%s",
                     saved_note_.empty() ? "Keys: F9 join, F10 invite" : saved_note_.c_str());

  ImGui::End();
}

}  // namespace rex::ui
