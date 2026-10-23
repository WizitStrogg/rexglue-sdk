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
  uint32_t joined_host_ip = 0;      // in someone else's game: their address (else 0)
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

// Keyed data shared with every LAN player, for co-op features layered on top
// of the game (e.g. "hero" = the joiner's hero profile, "story" = the host's
// story progress). The latest value per key is sent to each player as soon as
// it is published and again every few seconds, split into datagrams when
// large (up to kMaxDataSize bytes). Received values are kept after the sender
// goes quiet, so a value sent at the end of a session can still be read.
constexpr size_t kMaxDataSize = 60 * 1024;
struct PeerData {
  uint32_t ip = 0;  // host byte order, the address their packets come from
  std::string address;
  std::string gamertag;
  uint64_t xuid = 0;        // their online XUID (0 if not heard yet)
  uint32_t revision = 0;    // changes whenever the sender publishes again
  int seconds_since_received = 0;
  std::string value;
};
// Keys are up to 15 characters. Returns false if the key or value is too big.
bool PublishData(const std::string& key, const std::string& value);
std::vector<PeerData> GetPeerData(const std::string& key);

}  // namespace rex::kernel::xam::lobby
