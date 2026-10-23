/**
 ******************************************************************************
 * ReXGlue : System Link (LAN) networking support                             *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * Host-side identity for System Link play: which host adapter the guest
 * uses, the guest-visible XNADDR, the per-machine network XUID and gamertag,
 * and the System Link session key the title is currently using.
 *
 * Design notes:
 *  - There is no matchmaking server. Every peer is reachable directly by its
 *    LAN IPv4 address, so the XNADDR <-> IN_ADDR translation is an identity
 *    mapping. The 6-byte "MAC" in an XNADDR is synthesized from the IPv4
 *    address (02:58:a.b.c.d, a locally administered unicast MAC), so a peer's
 *    XNADDR can be rebuilt from its address alone.
 *  - Content (saves) stays keyed on the profile's legacy XUID so switching
 *    networking on never hides existing saves; only the XUID the guest sees
 *    through XamUser* changes (see network_xuid()).
 *
 * Portions adapted from the Xenia Canary netplay branch
 * (https://github.com/AdrianCassar/xenia-canary), BSD license,
 * Copyright 2013-2025 Ben Vanik and Xenia contributors.
 */

#pragma once

#include <cstdint>
#include <string>

#include <rex/types.h>

namespace rex::kernel::xam {

// Guest XNADDR. All in_addr fields are stored in network byte order.
struct XNADDR {
  uint32_t ina;                   // LAN IP (network byte order)
  uint32_t inaOnline;             // Online IP (network byte order)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
};
static_assert(sizeof(XNADDR) == 36, "XNADDR size");

struct XNKID {
  uint8_t ab[8];
};

struct XNKEY {
  uint8_t ab[16];
};

// XSESSION_INFO as laid out in guest memory.
struct XSESSION_INFO {
  XNKID sessionID;
  XNADDR hostAddress;
  XNKEY keyExchangeKey;
};
static_assert(sizeof(XSESSION_INFO) == 60, "XSESSION_INFO size");

namespace systemlink {

// XNKID high-byte flags.
constexpr uint8_t kXnkidSystemLink = 0x00;
constexpr uint8_t kXnkidOnlinePeer = 0x80;

constexpr uint16_t kSystemLinkPort = 3074;

// True when the runtime is configured for LAN play (cvar network_mode).
bool enabled();

// Selected host adapter, host byte order. Loopback when offline or when no
// usable adapter was found.
uint32_t local_ip();
uint32_t broadcast_ip();
std::string ip_to_string(uint32_t host_order_ip);

// Fills the guest-visible XNADDR of this machine.
void get_local_xnaddr(XNADDR* out);
// Rebuilds a peer's XNADDR from its IPv4 address (host byte order).
void xnaddr_from_ip(uint32_t host_order_ip, XNADDR* out);
// Recovers the IPv4 address (host byte order) from a peer's XNADDR.
uint32_t ip_from_xnaddr(const XNADDR& addr);
bool is_local_xnaddr(const XNADDR& addr);

// Cryptographically unimportant but per-machine random bytes; replaces the
// fixed 0xBB fill so two machines never generate the same session ids.
void random_bytes(void* out, size_t length);

// New System Link session key (XNKID) and key exchange key.
void create_key(XNKID* kid, XNKEY* key);

// Session key the title registered most recently (0 if none).
uint64_t registered_session_id();
void set_registered_session_id(uint64_t id);

// XUID the guest sees through XamUser* APIs (cvar profile_xuid).
uint64_t network_xuid(uint64_t legacy_xuid);
// XUID other players see (online XUID, XSession members, LAN lobby). With
// networking on it is derived from this PC's name unless profile_xuid is an
// explicit hex value; the offline XUID (network_xuid) keeps owning the saves.
uint64_t online_xuid(uint64_t legacy_xuid);
// Gamertag override (cvar profile_gamertag), empty if unset.
const std::string& gamertag_override();

}  // namespace systemlink
}  // namespace rex::kernel::xam
