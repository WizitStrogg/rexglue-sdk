/**
 ******************************************************************************
 * ReXGlue : LAN lobby, host-side view                                        *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * What the LAN lobby knows about other ReXGlue players on the network, for
 * host UI such as the co-op overlay. Everything here is safe to call from any
 * thread; it starts the lobby thread on first use when networking is on.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex::kernel::xam::lobby {

struct LanPeer {
  uint32_t ip = 0;  // host byte order, the address their packets come from
  std::string address;
  std::string gamertag;
  uint64_t xuid = 0;        // online XUID
  bool in_world = false;    // they are hosting a session (in the game world)
  bool joinable = false;    // that session has a free slot
  int seconds_since_seen = 0;
};

struct LobbyStatus {
  bool networking_enabled = false;  // network_mode = systemlink
  bool running = false;             // lobby socket is open
  bool in_world = false;            // we are hosting a session
  uint16_t port = 0;
  std::string game_address;         // address the game uses (XNADDR)
  std::vector<std::string> adapters;  // "name ip/broadcast", announced on
  uint64_t online_xuid = 0;
  std::string last_event;           // last join/invite result, for display
};

std::vector<LanPeer> GetLanPeers();
LobbyStatus GetLobbyStatus();

// Join the game hosted by the player at `ip` (same as F9, but for one
// player). Returns false if they are not hosting a joinable session.
bool JoinLanPeer(uint32_t ip);
// Invite the player at `ip` into the session we host (same as F10).
bool InviteLanPeer(uint32_t ip);

}  // namespace rex::kernel::xam::lobby
