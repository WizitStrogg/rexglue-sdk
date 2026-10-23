/**
 ******************************************************************************
 * ReXGlue : LAN session discovery                                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * Stands in for Xbox LIVE matchmaking between ReXGlue instances. Every
 * machine broadcasts a small beacon per session it hosts (with the host's
 * contexts and properties) on the lobby port, and XSessionSearch* answer from
 * the beacons heard from other machines.
 */

#pragma once

#include <cstdint>
#include <vector>

#include <rex/kernel/xam/systemlink.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xam::lobby {

// Records the local user's XGIUserSetContextEx / XGIUserSetPropertyEx values
// so they can be advertised with hosted sessions. `data` is guest memory.
void SetContext(uint32_t context_id, uint32_t value);
void SetProperty(uint32_t property_id, const uint8_t* data, uint32_t size);

// XGI search messages. `buffer` points at the guest message struct.
X_HRESULT Search(uint8_t* buffer, uint32_t buffer_length);         // 0x000B0016 / 0x000B001C
X_HRESULT SearchById(uint8_t* buffer, uint32_t buffer_length);     // 0x000B001B
X_HRESULT SearchByIds(uint8_t* buffer, uint32_t buffer_length);    // 0x000B0060

// Players heard from on the LAN, one per hosting machine, for the friends
// list (Fable II draws orbs for online friends).
struct RemotePlayer {
  uint64_t xuid;
  char gamertag[16];
  uint8_t session_id[8];
  uint32_t title_id;
  bool joinable;
};
std::vector<RemotePlayer> RemotePlayers();

// Game invites between LAN players. SendInvite delivers our hosted
// session to the invitee's machine; the receiver keeps it as the accepted
// invite (and raises XN_LIVE_INVITE_ACCEPTED when lan_auto_accept_invites).
struct InviteInfo {
  uint64_t xuid_inviter;
  uint64_t xuid_invitee;
  uint32_t title_id;
  char gamertag[16];
  XSESSION_INFO host_info;
  bool from_game_invite;  // false for "join session" (join via presence)
};
bool SendInvite(uint64_t invitee_xuid, uint64_t inviter_xuid);
bool TakeAcceptedInvite(InviteInfo* out);

// Starts the beacon thread (no-op when networking is off or already running).
void Start();

}  // namespace rex::kernel::xam::lobby
