/**
 ******************************************************************************
 * ReXGlue : LAN session discovery                                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * Beacon wire format (little endian, version 1):
 *   u32 magic 'RXLB'  u8 version  u32 title_id
 *   XSESSION_INFO (60 bytes, guest layout)  u32 host_ip
 *   u32 flags  u32 max_public  u32 max_private  u32 used_public  u32 used_private
 *   u64 host_xuid  char gamertag[16]
 *   u8 context_count  { u32 id, u32 value } * n
 *   u8 property_count { u32 id, u8 size, u8 data[8] } * n
 *
 * Hello wire format (sent every second, hosting or not, so players find each
 * other before either is in the game world):
 *   u32 magic 'RXLH'  u8 version  u32 title_id  u32 game_ip
 *   u64 online_xuid  char gamertag[16]  u8 in_world
 *
 * Data wire format (keyed values for co-op features, unicast to each peer):
 *   u32 magic 'RXLD'  u8 version  u32 title_id  char key[16]  u32 revision
 *   u32 total_size  u16 part  u16 part_count  u16 size  u8 data[size]
 *
 * Announcements go to every adapter's directed broadcast (from a socket bound
 * to that adapter, so virtual LANs such as ZeroTier get their own copy), to
 * 255.255.255.255, to lan_peers, and by unicast to every peer heard from, so
 * one direction of broadcast getting through is enough for both sides.
 */

#include "xam_lan_lobby.h"

#include <rex/kernel/xam/lan_lobby.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/xam/systemlink.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

#include "xam_session.h"

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_UINT32(lan_lobby_port, 3075, "Network",
                      "UDP port used to announce and discover hosted sessions between "
                      "ReXGlue instances (must be open in the firewall)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(lan_peers, "", "Network",
                      "Comma-separated IPv4 addresses that also receive session announcements, "
                      "for networks where broadcast does not reach the other PC");

REXCVAR_DEFINE_BOOL(lan_auto_accept_invites, true, "Network",
                    "Accept game invites from LAN players immediately (there is no Xbox "
                    "guide to accept them from)");

REXCVAR_DEFINE_STRING(lan_join, "", "Network",
                      "Join a LAN player's game automatically once both games are in the "
                      "world: their IPv4 address, or 'auto' for the first one found. Empty = "
                      "only join with the hotkey (F9)");

namespace rex::kernel::xam::lobby {

namespace {

constexpr uint32_t kMagic = 0x424C5852;  // 'RXLB'
constexpr uint8_t kVersion = 1;
constexpr auto kBeaconInterval = std::chrono::milliseconds(1000);
constexpr auto kExpiry = std::chrono::seconds(6);
constexpr size_t kMaxValues = 32;

struct Property {
  uint32_t id = 0;
  uint8_t size = 0;
  uint8_t data[8] = {};  // guest (big endian) bytes
};

struct Advert {
  XSESSION_INFO info = {};
  uint32_t host_ip = 0;
  uint32_t title_id = 0;
  uint32_t flags = 0;
  uint32_t max_public = 0, max_private = 0, used_public = 0, used_private = 0;
  uint64_t host_xuid = 0;
  char gamertag[16] = {};
  std::vector<std::pair<uint32_t, uint32_t>> contexts;
  std::vector<Property> properties;
  std::chrono::steady_clock::time_point last_seen;
};

std::mutex mutex;
std::map<uint32_t, uint32_t> local_contexts;
std::map<uint32_t, Property> local_properties;
std::map<uint64_t, Advert> remotes;  // keyed by session id
std::atomic<bool> started{false};

constexpr uint32_t kHelloMagic = 0x484C5852;  // 'RXLH'

struct Peer {
  uint32_t ip = 0;          // observed source address
  uint32_t game_ip = 0;     // address their game claims (their XNADDR)
  uint64_t xuid = 0;
  char gamertag[16] = {};
  bool in_world = false;
  std::chrono::steady_clock::time_point last_seen;
};
std::map<uint32_t, Peer> peers;  // keyed by observed ip, guarded by mutex
std::string last_event;          // guarded by mutex

void set_event(std::string text) {
  std::lock_guard lock(mutex);
  last_event = std::move(text);
}

uint64_t session_id(const XSESSION_INFO& info) {
  return memory::load_and_swap<uint64_t>(info.sessionID.ab);
}

// --- serialization --------------------------------------------------------

struct Writer {
  std::vector<uint8_t> buf;
  void bytes(const void* p, size_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    buf.insert(buf.end(), b, b + n);
  }
  void u8(uint8_t v) { buf.push_back(v); }
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(uint8_t(v >> (i * 8)));
  }
  void u64(uint64_t v) {
    u32(uint32_t(v));
    u32(uint32_t(v >> 32));
  }
};

struct Reader {
  const uint8_t* p;
  size_t left;
  bool ok = true;
  bool bytes(void* out, size_t n) {
    if (!ok || left < n) return ok = false;
    std::memcpy(out, p, n);
    p += n;
    left -= n;
    return true;
  }
  uint8_t u8() {
    uint8_t v = 0;
    bytes(&v, 1);
    return v;
  }
  uint32_t u32() {
    uint8_t b[4] = {};
    bytes(b, 4);
    return b[0] | (b[1] << 8) | (b[2] << 16) | (uint32_t(b[3]) << 24);
  }
  uint64_t u64() {
    uint64_t lo = u32();
    return lo | (uint64_t(u32()) << 32);
  }
};

std::vector<uint8_t> encode(const session::HostedSession& s, uint32_t title_id) {
  Writer w;
  w.u32(kMagic);
  w.u8(kVersion);
  w.u32(title_id);
  w.bytes(&s.info, sizeof(s.info));
  w.u32(systemlink::local_ip());
  w.u32(s.flags);
  w.u32(s.max_public);
  w.u32(s.max_private);
  w.u32(s.used_public);
  w.u32(s.used_private);
  w.u64(s.host_xuid);
  char tag[16] = {};
  std::strncpy(tag, systemlink::display_name().c_str(), sizeof(tag) - 1);
  w.bytes(tag, sizeof(tag));
  std::lock_guard lock(mutex);
  w.u8(uint8_t(std::min(local_contexts.size(), kMaxValues)));
  size_t n = 0;
  for (const auto& [id, value] : local_contexts) {
    if (n++ == kMaxValues) break;
    w.u32(id);
    w.u32(value);
  }
  w.u8(uint8_t(std::min(local_properties.size(), kMaxValues)));
  n = 0;
  for (const auto& [id, prop] : local_properties) {
    if (n++ == kMaxValues) break;
    w.u32(prop.id);
    w.u8(prop.size);
    w.bytes(prop.data, sizeof(prop.data));
  }
  return w.buf;
}

bool decode(const uint8_t* data, size_t size, Advert* out) {
  Reader r{data, size};
  if (r.u32() != kMagic || r.u8() != kVersion) return false;
  out->title_id = r.u32();
  r.bytes(&out->info, sizeof(out->info));
  out->host_ip = r.u32();
  out->flags = r.u32();
  out->max_public = r.u32();
  out->max_private = r.u32();
  out->used_public = r.u32();
  out->used_private = r.u32();
  out->host_xuid = r.u64();
  r.bytes(out->gamertag, sizeof(out->gamertag));
  out->gamertag[15] = 0;
  uint8_t count = r.u8();
  for (uint8_t i = 0; i < count && r.ok; ++i) {
    uint32_t id = r.u32();
    out->contexts.emplace_back(id, r.u32());
  }
  count = r.u8();
  for (uint8_t i = 0; i < count && r.ok; ++i) {
    Property p;
    p.id = r.u32();
    p.size = std::min<uint8_t>(r.u8(), 8);
    r.bytes(p.data, sizeof(p.data));
    out->properties.push_back(p);
  }
  return r.ok;
}

// --- network thread -------------------------------------------------------

#if REX_PLATFORM_WIN32
using sock_t = SOCKET;
using socklen_t = int;
constexpr sock_t kBadSocket = INVALID_SOCKET;
void close_socket(sock_t s) { closesocket(s); }
#else
using sock_t = int;
constexpr sock_t kBadSocket = -1;
void close_socket(sock_t s) { close(s); }
#endif

std::vector<uint32_t> peer_ips() {
  std::vector<uint32_t> out;
  std::stringstream ss(REXCVAR_GET(lan_peers));
  std::string item;
  while (std::getline(ss, item, ',')) {
    item.erase(std::remove_if(item.begin(), item.end(),
                              [](unsigned char c) { return std::isspace(c) != 0; }),
               item.end());
    in_addr addr{};
    if (!item.empty() && inet_pton(AF_INET, item.c_str(), &addr) == 1) {
      out.push_back(ntohl(addr.s_addr));
    }
  }
  return out;
}

void send_to(sock_t s, const std::vector<uint8_t>& packet, uint32_t ip, uint16_t port) {
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = htonl(ip);
  sendto(s, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
         reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// One sending socket per local adapter, bound to its address, so broadcasts
// leave through that adapter (Windows sends 255.255.255.255 and other
// adapters' broadcasts out of the default route only). Owned by the lobby
// thread.
struct AdapterSocket {
  systemlink::LocalAdapter adapter;
  sock_t s = kBadSocket;
};
std::vector<AdapterSocket> adapter_sockets;
std::atomic<size_t> adapter_socket_count{0};

void refresh_adapter_sockets() {
  const auto current = systemlink::adapters();
  for (auto it = adapter_sockets.begin(); it != adapter_sockets.end();) {
    const bool still_up = std::any_of(current.begin(), current.end(), [&](const auto& a) {
      return a.ip == it->adapter.ip && a.broadcast == it->adapter.broadcast;
    });
    if (still_up) {
      ++it;
      continue;
    }
    REXKRNL_INFO("LAN lobby: adapter '{}' {} went away", it->adapter.name,
                 systemlink::ip_to_string(it->adapter.ip));
    close_socket(it->s);
    it = adapter_sockets.erase(it);
  }
  for (const auto& a : current) {
    if (std::any_of(adapter_sockets.begin(), adapter_sockets.end(),
                    [&](const AdapterSocket& as) { return as.adapter.ip == a.ip; })) {
      continue;
    }
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadSocket) {
      continue;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(a.ip);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      close_socket(s);
      continue;
    }
    REXKRNL_INFO("LAN lobby: announcing on '{}' {} (broadcast {})", a.name,
                 systemlink::ip_to_string(a.ip), systemlink::ip_to_string(a.broadcast));
    adapter_sockets.push_back({a, s});
  }
  adapter_socket_count.store(adapter_sockets.size());
}

bool is_own_address(uint32_t ip) {
  if (ip == systemlink::local_ip()) {
    return true;
  }
  return std::any_of(adapter_sockets.begin(), adapter_sockets.end(),
                     [&](const AdapterSocket& as) { return as.adapter.ip == ip; });
}

// Sends `packet` everywhere a player might be: each adapter's directed and
// limited broadcast, configured lan_peers and every peer heard from.
void announce(sock_t main, const std::vector<uint8_t>& packet, uint16_t port,
              const std::vector<uint32_t>& unicast) {
  for (const auto& as : adapter_sockets) {
    send_to(as.s, packet, as.adapter.broadcast, port);
    send_to(as.s, packet, 0xFFFFFFFFu, port);
  }
  for (uint32_t ip : unicast) {
    send_to(main, packet, ip, port);
  }
}

uint64_t own_online_xuid() {
  return systemlink::online_xuid(REX_KERNEL_STATE()->user_profile()->xuid());
}

std::vector<uint8_t> encode_hello(uint32_t title_id) {
  Writer w;
  w.u32(kHelloMagic);
  w.u8(kVersion);
  w.u32(title_id);
  w.u32(systemlink::local_ip());
  w.u64(own_online_xuid());
  char tag[16] = {};
  std::strncpy(tag, systemlink::display_name().c_str(), sizeof(tag) - 1);
  w.bytes(tag, sizeof(tag));
  w.u8(session::HostedSessions().empty() ? 0 : 1);
  return w.buf;
}

// Records a packet's sender as a peer. game_ip is the address their game
// claims to be at (0 if the packet does not say).
void note_peer(uint32_t from_ip, uint32_t game_ip, const Peer* hello) {
  if (game_ip) {
    systemlink::note_peer_address(game_ip, from_ip);
  }
  bool added = false;
  {
    std::lock_guard lock(mutex);
    auto [it, inserted] = peers.try_emplace(from_ip);
    Peer& p = it->second;
    p.ip = from_ip;
    if (game_ip) p.game_ip = game_ip;
    if (hello) {
      p.xuid = hello->xuid;
      std::memcpy(p.gamertag, hello->gamertag, sizeof(p.gamertag));
      p.in_world = hello->in_world;
    }
    p.last_seen = std::chrono::steady_clock::now();
    added = inserted;
  }
  if (added) {
    REXKRNL_INFO("LAN lobby: heard from player at {}", systemlink::ip_to_string(from_ip));
  }
}

void handle_hello(const uint8_t* data, size_t size, uint32_t from_ip) {
  Reader r{data, size};
  if (r.u32() != kHelloMagic || r.u8() != kVersion) return;
  const uint32_t title_id = r.u32();
  const uint32_t game_ip = r.u32();
  Peer hello;
  hello.xuid = r.u64();
  r.bytes(hello.gamertag, sizeof(hello.gamertag));
  hello.gamertag[15] = 0;
  hello.in_world = r.u8() != 0;
  if (!r.ok || title_id != REX_KERNEL_STATE()->title_id()) return;
  note_peer(from_ip, game_ip, &hello);
}

// --- keyed data channel ---------------------------------------------------

constexpr uint32_t kDataMagic = 0x444C5852;  // 'RXLD'
constexpr size_t kDataChunk = 1200;          // payload bytes per datagram
constexpr auto kDataResend = std::chrono::seconds(3);

struct LocalData {
  uint32_t revision = 0;
  std::string value;
};
std::map<std::string, LocalData> local_data;  // guarded by mutex
uint32_t next_revision = 0;                     // guarded by mutex

struct Assembly {
  uint32_t revision = 0;
  uint32_t total = 0;
  std::vector<std::string> parts;
  size_t received = 0;
};
struct ReceivedData {
  uint32_t revision = 0;
  std::string value;
  std::string gamertag;  // copied from the peer, which may expire before this
  uint64_t xuid = 0;
  std::chrono::steady_clock::time_point when;
};
// Keyed by (sender ip, key); guarded by mutex.
std::map<std::pair<uint32_t, std::string>, Assembly> data_assembly;
std::map<std::pair<uint32_t, std::string>, ReceivedData> received_data;

std::vector<std::vector<uint8_t>> encode_data(const std::string& key, const LocalData& d) {
  std::vector<std::vector<uint8_t>> out;
  const size_t total = d.value.size();
  const uint16_t count = uint16_t(std::max<size_t>(1, (total + kDataChunk - 1) / kDataChunk));
  for (uint16_t i = 0; i < count; ++i) {
    const size_t offset = size_t(i) * kDataChunk;
    const size_t size = std::min(kDataChunk, total - std::min(total, offset));
    Writer w;
    w.u32(kDataMagic);
    w.u8(kVersion);
    w.u32(REX_KERNEL_STATE()->title_id());
    char k[16] = {};
    std::strncpy(k, key.c_str(), sizeof(k) - 1);
    w.bytes(k, sizeof(k));
    w.u32(d.revision);
    w.u32(uint32_t(total));
    w.u8(uint8_t(i));
    w.u8(uint8_t(i >> 8));
    w.u8(uint8_t(count));
    w.u8(uint8_t(count >> 8));
    w.u8(uint8_t(size));
    w.u8(uint8_t(size >> 8));
    w.bytes(d.value.data() + offset, size);
    out.push_back(std::move(w.buf));
  }
  return out;
}

void handle_data(const uint8_t* data, size_t size, uint32_t from_ip) {
  Reader r{data, size};
  if (r.u32() != kDataMagic || r.u8() != kVersion) return;
  const uint32_t title_id = r.u32();
  char k[16] = {};
  r.bytes(k, sizeof(k));
  k[15] = 0;
  const uint32_t revision = r.u32();
  const uint32_t total = r.u32();
  const uint16_t part = uint16_t(r.u8() | (r.u8() << 8));
  const uint16_t count = uint16_t(r.u8() | (r.u8() << 8));
  const uint16_t chunk = uint16_t(r.u8() | (r.u8() << 8));
  if (!r.ok || title_id != REX_KERNEL_STATE()->title_id() || total > kMaxDataSize ||
      count == 0 || part >= count || chunk > r.left ||
      size_t(count) != std::max<size_t>(1, (total + kDataChunk - 1) / kDataChunk)) {
    return;
  }
  note_peer(from_ip, 0, nullptr);
  const auto id = std::make_pair(from_ip, std::string(k));
  bool completed = false;
  {
    std::lock_guard lock(mutex);
    auto have = received_data.find(id);
    if (have != received_data.end() && have->second.revision == revision) {
      have->second.when = std::chrono::steady_clock::now();
      return;  // a resend of what we already have
    }
    Assembly& a = data_assembly[id];
    if (a.revision != revision || a.parts.size() != count || a.total != total) {
      a = Assembly{};
      a.revision = revision;
      a.total = total;
      a.parts.assign(count, std::string());
    }
    if (a.parts[part].empty() && (chunk > 0 || total == 0)) {
      a.parts[part].assign(reinterpret_cast<const char*>(r.p), chunk);
      ++a.received;
    }
    if (a.received >= count) {
      ReceivedData d;
      d.revision = revision;
      for (auto& p : a.parts) d.value += p;
      d.when = std::chrono::steady_clock::now();
      if (auto peer = peers.find(from_ip); peer != peers.end()) {
        d.gamertag = peer->second.gamertag;
        d.xuid = peer->second.xuid;
      }
      if (d.value.size() == total) {
        received_data[id] = std::move(d);
        completed = true;
      }
      data_assembly.erase(id);
    }
  }
  if (completed) {
    REXKRNL_INFO("LAN lobby: received '{}' data ({} bytes, revision {:08X}) from {}", k, total,
                 revision, systemlink::ip_to_string(from_ip));
  }
}

// Sends every published value to `targets`.
void send_data(sock_t s, const std::vector<uint32_t>& targets, uint16_t port,
               const std::string* only_key = nullptr) {
  std::vector<std::vector<uint8_t>> packets;
  {
    std::lock_guard lock(mutex);
    for (const auto& [key, d] : local_data) {
      if (only_key && key != *only_key) continue;
      for (auto& p : encode_data(key, d)) packets.push_back(std::move(p));
    }
  }
  for (uint32_t ip : targets) {
    for (const auto& p : packets) send_to(s, p, ip, port);
  }
}

std::vector<uint32_t> peer_targets() {
  std::vector<uint32_t> out = peer_ips();
  std::lock_guard lock(mutex);
  for (const auto& [ip, p] : peers) {
    if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
  }
  return out;
}

constexpr uint32_t kInviteMagic = 0x494C5852;  // 'RXLI'

std::atomic<uint64_t> lobby_socket{~0ull};
std::optional<InviteInfo> accepted_invite;  // guarded by mutex

void handle_invite(const uint8_t* data, size_t size, uint32_t from_ip) {
  Reader r{data, size};
  if (r.u32() != kInviteMagic || r.u8() != kVersion) return;
  InviteInfo invite{};
  invite.title_id = r.u32();
  invite.xuid_inviter = r.u64();
  invite.xuid_invitee = r.u64();
  r.bytes(invite.gamertag, sizeof(invite.gamertag));
  invite.gamertag[15] = 0;
  r.bytes(&invite.host_info, sizeof(invite.host_info));
  if (!r.ok || invite.title_id != REX_KERNEL_STATE()->title_id()) return;
  note_peer(from_ip, systemlink::ip_from_xnaddr(invite.host_info.hostAddress), nullptr);
  systemlink::xnaddr_from_ip(from_ip, &invite.host_info.hostAddress);
  invite.from_game_invite = true;
  REXKRNL_INFO("LAN lobby: game invite from '{}' ({}) to session {:016X}", invite.gamertag,
               systemlink::ip_to_string(from_ip), session_id(invite.host_info));
  {
    std::lock_guard lock(mutex);
    accepted_invite = invite;
  }
  if (REXCVAR_GET(lan_auto_accept_invites)) {
    // XN_LIVE_INVITE_ACCEPTED for user 0.
    REX_KERNEL_STATE()->BroadcastNotification(0x02000002, 0);
    REXKRNL_INFO("LAN lobby: invite accepted automatically");
    set_event(std::string("Accepted invite from ") + invite.gamertag);
  } else {
    set_event(std::string("Invite from ") + invite.gamertag);
  }
}

// "Join session" from the Xbox guide: treat a LAN player's session as an
// accepted invite (join via presence) and tell the title.
bool join_remote(uint32_t only_ip) {
  std::optional<Advert> target;
  {
    std::lock_guard lock(mutex);
    for (const auto& [id, a] : remotes) {
      if ((!only_ip || a.host_ip == only_ip) &&
          a.used_public + a.used_private < a.max_public + a.max_private) {
        target = a;
        break;
      }
    }
    if (!target) {
      last_event = "No joinable game found";
      return false;
    }
    InviteInfo invite{};
    invite.title_id = target->title_id;
    invite.xuid_inviter = target->host_xuid;
    invite.xuid_invitee = 0;
    std::memcpy(invite.gamertag, target->gamertag, sizeof(invite.gamertag));
    invite.host_info = target->info;
    invite.from_game_invite = false;
    accepted_invite = invite;
    last_event = std::string("Joining ") + target->gamertag;
  }
  REXKRNL_INFO("LAN lobby: joining '{}' at {} (session {:016X})", target->gamertag,
               systemlink::ip_to_string(target->host_ip), session_id(target->info));
  REX_KERNEL_STATE()->BroadcastNotification(0x02000002, 0);  // XN_LIVE_INVITE_ACCEPTED
  return true;
}

// Sends our hosted session to the player at `target_ip`.
bool send_invite_to(uint32_t target_ip, uint64_t invitee_xuid, uint64_t inviter_xuid) {
  const uint64_t sock = lobby_socket.load();
  const auto hosted = session::HostedSessions();
  if (sock == ~0ull || hosted.empty()) {
    REXKRNL_WARN("LAN lobby: cannot send invite (lobby {}, {} hosted session(s))",
                 sock == ~0ull ? "not running" : "running", hosted.size());
    set_event(hosted.empty() ? "Load into the world before inviting" : "Lobby is not running");
    return false;
  }
  Writer w;
  w.u32(kInviteMagic);
  w.u8(kVersion);
  w.u32(REX_KERNEL_STATE()->title_id());
  w.u64(inviter_xuid);
  w.u64(invitee_xuid);
  char tag[16] = {};
  std::strncpy(tag, systemlink::display_name().c_str(), sizeof(tag) - 1);
  w.bytes(tag, sizeof(tag));
  w.bytes(&hosted.front().info, sizeof(XSESSION_INFO));
  for (int i = 0; i < 3; ++i) {  // UDP: send a few copies
    send_to(sock_t(sock), w.buf, target_ip, uint16_t(REXCVAR_GET(lan_lobby_port)));
  }
  REXKRNL_INFO("LAN lobby: sent game invite for session {:016X} to {}",
               session_id(hosted.front().info), systemlink::ip_to_string(target_ip));
  return true;
}

void invite_all() {
  std::vector<std::pair<uint32_t, uint64_t>> targets;
  {
    std::lock_guard lock(mutex);
    for (const auto& [ip, p] : peers) targets.emplace_back(ip, p.xuid);
  }
  size_t sent = 0;
  for (const auto& [ip, xuid] : targets) {
    sent += send_invite_to(ip, xuid, own_online_xuid()) ? 1 : 0;
  }
  if (sent) {
    set_event("Invited " + std::to_string(sent) + " player(s)");
  } else if (targets.empty()) {
    set_event("No players found to invite");
  }
}

// Guide replacements while the game window has focus: F9 joins the LAN
// player's game, F10 invites LAN players into ours.
void poll_hotkeys() {
#if REX_PLATFORM_WIN32
  static bool f9_down = false, f10_down = false;
  HWND foreground = GetForegroundWindow();
  DWORD pid = 0;
  if (foreground) GetWindowThreadProcessId(foreground, &pid);
  const bool focused = pid == GetCurrentProcessId();
  const bool f9 = focused && (GetAsyncKeyState(VK_F9) & 0x8000);
  const bool f10 = focused && (GetAsyncKeyState(VK_F10) & 0x8000);
  if (f9 && !f9_down) {
    REXKRNL_INFO("LAN lobby: F9 pressed (join)");
    if (!join_remote(0)) REXKRNL_WARN("LAN lobby: no joinable LAN game found");
  }
  if (f10 && !f10_down) {
    REXKRNL_INFO("LAN lobby: F10 pressed (invite)");
    invite_all();
  }
  f9_down = f9;
  f10_down = f10;
#endif
}

void maybe_auto_join() {
  static bool done = false;
  const std::string& target = REXCVAR_GET(lan_join);
  if (done || target.empty() || session::HostedSessions().empty()) {
    return;  // only once our own game is in the world
  }
  uint32_t ip = 0;
  if (target != "auto") {
    in_addr addr{};
    if (inet_pton(AF_INET, target.c_str(), &addr) != 1) {
      REXKRNL_ERROR("LAN lobby: lan_join '{}' is not an IPv4 address", target);
      done = true;
      return;
    }
    ip = ntohl(addr.s_addr);
  }
  done = join_remote(ip);
}

void run() {
#if REX_PLATFORM_WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  const uint16_t port = uint16_t(REXCVAR_GET(lan_lobby_port));
  sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kBadSocket) {
    REXKRNL_ERROR("LAN lobby: could not create socket");
    set_event("Could not create the lobby socket");
    return;
  }
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(port);
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
    REXKRNL_ERROR("LAN lobby: could not bind UDP port {} (is another copy running?)", port);
    set_event("UDP port " + std::to_string(port) + " is in use (another copy running?)");
    close_socket(s);
    return;
  }
  lobby_socket.store(uint64_t(s));
  const auto configured_peers = peer_ips();
  refresh_adapter_sockets();
  REXKRNL_INFO("LAN lobby: listening on UDP {} ({} adapter(s), {} configured peer(s))", port,
               adapter_sockets.size(), configured_peers.size());

  auto next_beacon = std::chrono::steady_clock::now();
  auto next_refresh = next_beacon + std::chrono::seconds(10);
  auto next_data = next_beacon;
  std::vector<uint8_t> packet(1500);
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_refresh) {
      next_refresh = now + std::chrono::seconds(10);
      refresh_adapter_sockets();
    }
    if (now >= next_beacon) {
      next_beacon = now + kBeaconInterval;
      maybe_auto_join();
      const uint32_t title_id = REX_KERNEL_STATE()->title_id();
      std::vector<uint32_t> unicast = configured_peers;
      {
        std::lock_guard lock(mutex);
        for (const auto& [ip, p] : peers) {
          if (std::find(unicast.begin(), unicast.end(), ip) == unicast.end()) {
            unicast.push_back(ip);
          }
        }
      }
      announce(s, encode_hello(title_id), port, unicast);
      if (now >= next_data) {
        next_data = now + kDataResend;
        send_data(s, unicast, port);
      }
      for (const auto& hosted : session::HostedSessions()) {
        announce(s, encode(hosted, title_id), port, unicast);
      }
      bool removed = false;
      {
        std::lock_guard lock(mutex);
        for (auto it = remotes.begin(); it != remotes.end();) {
          if (now - it->second.last_seen > kExpiry) {
            REXKRNL_INFO("LAN lobby: session {:016X} from {} expired", it->first,
                         systemlink::ip_to_string(it->second.host_ip));
            it = remotes.erase(it);
            removed = true;
          } else {
            ++it;
          }
        }
        for (auto it = peers.begin(); it != peers.end();) {
          if (now - it->second.last_seen > kExpiry) {
            REXKRNL_INFO("LAN lobby: player at {} went quiet",
                         systemlink::ip_to_string(it->first));
            it = peers.erase(it);
          } else {
            ++it;
          }
        }
      }
      if (removed) {
        // XN_FRIENDS_FRIEND_REMOVED / PRESENCE_CHANGED for user 0.
        REX_KERNEL_STATE()->BroadcastNotification(0x04000003, 0);
        REX_KERNEL_STATE()->BroadcastNotification(0x04000001, 0);
      }
    }

    poll_hotkeys();
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    timeval tv{0, 50 * 1000};
    if (select(int(s) + 1, &fds, nullptr, nullptr, &tv) <= 0) {
      continue;
    }
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    int n = recvfrom(s, reinterpret_cast<char*>(packet.data()), int(packet.size()), 0,
                     reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n <= 0) {
      continue;
    }
    const uint32_t from_ip = ntohl(from.sin_addr.s_addr);
    if (is_own_address(from_ip)) {
      continue;
    }
    const uint32_t magic = n >= 4 ? Reader{packet.data(), size_t(n)}.u32() : 0;
    if (magic == kInviteMagic) {
      handle_invite(packet.data(), size_t(n), from_ip);
      continue;
    }
    if (magic == kHelloMagic) {
      handle_hello(packet.data(), size_t(n), from_ip);
      continue;
    }
    if (magic == kDataMagic) {
      handle_data(packet.data(), size_t(n), from_ip);
      continue;
    }
    Advert advert;
    if (!decode(packet.data(), size_t(n), &advert) ||
        advert.title_id != REX_KERNEL_STATE()->title_id()) {
      continue;
    }
    // Trust the address the packet came from over the one it claims, and
    // route the claimed one there too (it may be an adapter we cannot reach).
    note_peer(from_ip, advert.host_ip, nullptr);
    advert.host_ip = from_ip;
    systemlink::xnaddr_from_ip(from_ip, &advert.info.hostAddress);
    advert.last_seen = std::chrono::steady_clock::now();
    const uint64_t id = session_id(advert.info);
    bool added = false;
    {
      std::lock_guard lock(mutex);
      if (!remotes.count(id)) {
        REXKRNL_INFO("LAN lobby: found session {:016X} hosted by '{}' at {} ({}+{} of {}+{} slots)",
                     id, advert.gamertag, systemlink::ip_to_string(from_ip), advert.used_public,
                     advert.used_private, advert.max_public, advert.max_private);
        added = true;
      }
      remotes[id] = std::move(advert);
    }
    if (added) {
      // XN_FRIENDS_FRIEND_ADDED / PRESENCE_CHANGED so the title re-reads the
      // friends list (and draws the orb).
      REX_KERNEL_STATE()->BroadcastNotification(0x04000002, 0);
      REX_KERNEL_STATE()->BroadcastNotification(0x04000001, 0);
    }
  }
}

// --- search results -------------------------------------------------------

struct XSESSION_SEARCHRESULT {
  XSESSION_INFO info;
  rex::be<uint32_t> open_public_slots;
  rex::be<uint32_t> open_private_slots;
  rex::be<uint32_t> filled_public_slots;
  rex::be<uint32_t> filled_private_slots;
  rex::be<uint32_t> properties_count;
  rex::be<uint32_t> contexts_count;
  rex::be<uint32_t> properties_ptr;
  rex::be<uint32_t> contexts_ptr;
};
static_assert(sizeof(XSESSION_SEARCHRESULT) == 0x5C, "XSESSION_SEARCHRESULT size");

struct XUSER_CONTEXT {
  rex::be<uint32_t> context_id;
  rex::be<uint32_t> value;
};

struct XUSER_PROPERTY {
  rex::be<uint32_t> property_id;
  uint32_t pad;
  uint8_t type;  // XUSER_DATA
  uint8_t pad2[7];
  uint8_t value[8];
};
static_assert(sizeof(XUSER_PROPERTY) == 0x18, "XUSER_PROPERTY size");

constexpr X_HRESULT kInsufficientBuffer = 0x8007007A;

// Writes the header, results and their contexts/properties into the
// caller's buffer, dropping results that do not fit.
X_HRESULT write_results(const std::vector<Advert>& found, uint32_t results_ptr,
                        uint32_t buffer_size) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!results_ptr || buffer_size < 8) {
    return kInsufficientBuffer;
  }
  auto* base = memory->TranslateVirtual<uint8_t*>(results_ptr);
  std::memset(base, 0, buffer_size);

  auto extra_size = [](const Advert& a) {
    return uint32_t(a.contexts.size() * sizeof(XUSER_CONTEXT) +
                    a.properties.size() * sizeof(XUSER_PROPERTY));
  };
  uint32_t count = 0;
  uint32_t needed = 8;
  for (const auto& a : found) {
    const uint32_t more = sizeof(XSESSION_SEARCHRESULT) + extra_size(a);
    if (needed + more > buffer_size) break;
    needed += more;
    ++count;
  }

  memory::store_and_swap<uint32_t>(base + 0, count);
  memory::store_and_swap<uint32_t>(base + 4, count ? results_ptr + 8 : 0);
  auto* results = reinterpret_cast<XSESSION_SEARCHRESULT*>(base + 8);
  uint32_t extra = 8 + count * sizeof(XSESSION_SEARCHRESULT);
  for (uint32_t i = 0; i < count; ++i) {
    const Advert& a = found[i];
    auto& r = results[i];
    r.info = a.info;
    r.open_public_slots = a.max_public - std::min(a.used_public, a.max_public);
    r.open_private_slots = a.max_private - std::min(a.used_private, a.max_private);
    r.filled_public_slots = a.used_public;
    r.filled_private_slots = a.used_private;

    r.contexts_count = uint32_t(a.contexts.size());
    r.contexts_ptr = a.contexts.empty() ? 0 : results_ptr + extra;
    for (const auto& [id, value] : a.contexts) {
      auto* c = reinterpret_cast<XUSER_CONTEXT*>(base + extra);
      c->context_id = id;
      c->value = value;
      extra += sizeof(XUSER_CONTEXT);
    }
    r.properties_count = uint32_t(a.properties.size());
    r.properties_ptr = a.properties.empty() ? 0 : results_ptr + extra;
    for (const auto& prop : a.properties) {
      auto* p = reinterpret_cast<XUSER_PROPERTY*>(base + extra);
      p->property_id = prop.id;
      p->type = uint8_t(prop.id >> 28);
      std::memcpy(p->value, prop.data, prop.size);
      extra += sizeof(XUSER_PROPERTY);
    }
  }
  return X_E_SUCCESS;
}

std::vector<Advert> snapshot_remotes() {
  std::lock_guard lock(mutex);
  std::vector<Advert> out;
  for (const auto& [id, a] : remotes) out.push_back(a);
  return out;
}

}  // namespace

void Start() {
  if (!systemlink::enabled() || started.exchange(true)) {
    return;
  }
  std::thread(run).detach();
}

void SetContext(uint32_t context_id, uint32_t value) {
  std::lock_guard lock(mutex);
  local_contexts[context_id] = value;
}

void SetProperty(uint32_t property_id, const uint8_t* data, uint32_t size) {
  if (!data || size == 0 || size > 8) {
    return;  // strings and blobs are not advertised
  }
  Property p;
  p.id = property_id;
  p.size = uint8_t(size);
  std::memcpy(p.data, data, size);
  std::lock_guard lock(mutex);
  local_properties[property_id] = p;
}

X_HRESULT Search(uint8_t* buffer, [[maybe_unused]] uint32_t buffer_length) {
  Start();
  const uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
  const uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
  const uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
  const uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
  const uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 24);
  const uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);

  // Drop sessions whose advertised contexts contradict the search's.
  std::vector<std::pair<uint32_t, uint32_t>> wanted;
  if (ctx_ptr && num_ctx) {
    auto* ctx = REX_KERNEL_MEMORY()->TranslateVirtual<XUSER_CONTEXT*>(ctx_ptr);
    for (uint16_t i = 0; i < num_ctx; ++i) wanted.emplace_back(ctx[i].context_id, ctx[i].value);
  }
  std::vector<Advert> found;
  for (auto& a : snapshot_remotes()) {
    bool match = a.used_public + a.used_private < a.max_public + a.max_private;
    for (const auto& [id, value] : wanted) {
      for (const auto& [aid, avalue] : a.contexts) {
        if (aid == id && avalue != value) match = false;
      }
    }
    if (match && found.size() < num_results) found.push_back(std::move(a));
  }
  REXKRNL_INFO("XSessionSearch proc={} wants {} ctx, max {} -> {} LAN session(s)", proc_index,
               wanted.size(), num_results, found.size());
  return write_results(found, results_ptr, results_size);
}

X_HRESULT SearchById(uint8_t* buffer, [[maybe_unused]] uint32_t buffer_length) {
  Start();
  // { u32 user_index; XNKID id; u32 results_buffer_size; u32 results_ptr; }
  const uint64_t id = memory::load_and_swap<uint64_t>(buffer + 4);
  const uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 12);
  const uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
  std::vector<Advert> found;
  for (auto& a : snapshot_remotes()) {
    if (session_id(a.info) == id) found.push_back(std::move(a));
  }
  REXKRNL_INFO("XSessionSearchByID {:016X} -> {}", id, found.size());
  return write_results(found, results_ptr, results_size);
}

X_HRESULT SearchByIds(uint8_t* buffer, [[maybe_unused]] uint32_t buffer_length) {
  Start();
  // { u32 user_index; u32 count; u32 ids_ptr; u32 results_buffer_size; u32 results_ptr; ... }
  const uint32_t count = memory::load_and_swap<uint32_t>(buffer + 4);
  const uint32_t ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
  const uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 12);
  const uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
  auto* ids = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(ids_ptr);
  std::vector<Advert> found;
  const auto all = snapshot_remotes();
  for (uint32_t i = 0; ids && i < std::min<uint32_t>(count, 100); ++i) {
    const uint64_t id = memory::load_and_swap<uint64_t>(ids + i * 8);
    for (const auto& a : all) {
      if (session_id(a.info) == id) found.push_back(a);
    }
  }
  REXKRNL_INFO("XSessionSearchByIds {} id(s) -> {}", count, found.size());
  return write_results(found, results_ptr, results_size);
}

std::vector<RemotePlayer> RemotePlayers() {
  std::vector<RemotePlayer> out;
  for (const auto& a : snapshot_remotes()) {
    if (std::any_of(out.begin(), out.end(),
                    [&](const RemotePlayer& p) { return p.xuid == a.host_xuid; })) {
      continue;
    }
    RemotePlayer p{};
    p.xuid = a.host_xuid;
    std::memcpy(p.gamertag, a.gamertag, sizeof(p.gamertag));
    std::memcpy(p.session_id, a.info.sessionID.ab, sizeof(p.session_id));
    p.title_id = a.title_id;
    p.joinable = a.used_public + a.used_private < a.max_public + a.max_private;
    out.push_back(p);
  }
  return out;
}

bool SendInvite(uint64_t invitee_xuid, uint64_t inviter_xuid) {
  uint32_t target_ip = 0;
  {
    std::lock_guard lock(mutex);
    for (const auto& [id, a] : remotes) {
      if (a.host_xuid == invitee_xuid) target_ip = a.host_ip;
    }
    for (const auto& [ip, p] : peers) {
      if (!target_ip && p.xuid == invitee_xuid) target_ip = ip;
    }
  }
  if (!target_ip) {
    REXKRNL_WARN("LAN lobby: invitee {:016X} is not on the LAN", invitee_xuid);
    return false;
  }
  return send_invite_to(target_ip, invitee_xuid, inviter_xuid);
}

bool TakeAcceptedInvite(InviteInfo* out) {
  std::lock_guard lock(mutex);
  if (!accepted_invite) return false;
  *out = *accepted_invite;
  return true;
}

std::vector<LanPeer> GetLanPeers() {
  Start();
  const auto now = std::chrono::steady_clock::now();
  std::vector<LanPeer> out;
  std::lock_guard lock(mutex);
  for (const auto& [ip, p] : peers) {
    LanPeer peer;
    peer.ip = ip;
    peer.address = systemlink::ip_to_string(ip);
    peer.gamertag = p.gamertag;
    peer.xuid = p.xuid;
    peer.in_world = p.in_world;
    peer.seconds_since_seen =
        int(std::chrono::duration_cast<std::chrono::seconds>(now - p.last_seen).count());
    for (const auto& [id, a] : remotes) {
      if (a.host_ip == ip) {
        peer.in_world = true;
        peer.joinable = a.used_public + a.used_private < a.max_public + a.max_private;
        if (peer.gamertag.empty()) peer.gamertag = a.gamertag;
        if (!peer.xuid) peer.xuid = a.host_xuid;
      }
    }
    out.push_back(std::move(peer));
  }
  return out;
}

LobbyStatus GetLobbyStatus() {
  Start();
  LobbyStatus status;
  status.networking_enabled = systemlink::enabled();
  status.running = lobby_socket.load() != ~0ull;
  status.in_world = !session::HostedSessions().empty();
  status.joined_host_ip = session::JoinedHostIp();
  status.port = uint16_t(REXCVAR_GET(lan_lobby_port));
  if (status.networking_enabled) {
    status.game_address = systemlink::ip_to_string(systemlink::local_ip());
    status.online_xuid = own_online_xuid();
    for (const auto& a : systemlink::adapters()) {
      status.adapters.push_back(a.name + "  " + systemlink::ip_to_string(a.ip));
    }
  }
  std::lock_guard lock(mutex);
  status.last_event = last_event;
  return status;
}

bool JoinLanPeer(uint32_t ip) {
  REXKRNL_INFO("LAN lobby: co-op menu join {}", systemlink::ip_to_string(ip));
  return join_remote(ip);
}

bool InviteLanPeer(uint32_t ip) {
  uint64_t xuid = 0;
  std::string name;
  {
    std::lock_guard lock(mutex);
    auto it = peers.find(ip);
    if (it == peers.end()) {
      last_event = "That player is no longer on the network";
      return false;
    }
    xuid = it->second.xuid;
    name = it->second.gamertag;
  }
  REXKRNL_INFO("LAN lobby: co-op menu invite {}", systemlink::ip_to_string(ip));
  if (!send_invite_to(ip, xuid, own_online_xuid())) {
    return false;
  }
  set_event("Invited " + (name.empty() ? systemlink::ip_to_string(ip) : name));
  return true;
}

bool PublishData(const std::string& key, const std::string& value) {
  if (key.empty() || key.size() > 15 || value.size() > kMaxDataSize) {
    REXKRNL_WARN("LAN lobby: not publishing '{}' ({} bytes): key or value too large", key,
                 value.size());
    return false;
  }
  {
    std::lock_guard lock(mutex);
    if (!next_revision) {
      // Seed from the clock so a restarted game never reuses a revision the
      // other side already has.
      next_revision = uint32_t(std::chrono::system_clock::now().time_since_epoch().count());
    }
    LocalData& d = local_data[key];
    if (d.revision && d.value == value) {
      return true;
    }
    d.revision = ++next_revision ? next_revision : ++next_revision;
    d.value = value;
  }
  REXKRNL_INFO("LAN lobby: publishing '{}' ({} bytes)", key, value.size());
  Start();
  const uint64_t sock = lobby_socket.load();
  if (sock != ~0ull) {
    send_data(sock_t(sock), peer_targets(), uint16_t(REXCVAR_GET(lan_lobby_port)), &key);
  }
  return true;
}

std::vector<PeerData> GetPeerData(const std::string& key) {
  const auto now = std::chrono::steady_clock::now();
  std::vector<PeerData> out;
  std::lock_guard lock(mutex);
  for (const auto& [id, d] : received_data) {
    if (id.second != key) continue;
    PeerData pd;
    pd.ip = id.first;
    pd.address = systemlink::ip_to_string(id.first);
    pd.revision = d.revision;
    pd.value = d.value;
    pd.seconds_since_received =
        int(std::chrono::duration_cast<std::chrono::seconds>(now - d.when).count());
    pd.gamertag = d.gamertag;
    pd.xuid = d.xuid;
    if (auto it = peers.find(id.first); it != peers.end() && it->second.xuid) {
      pd.gamertag = it->second.gamertag;
      pd.xuid = it->second.xuid;
    }
    out.push_back(std::move(pd));
  }
  return out;
}

}  // namespace rex::kernel::xam::lobby
