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
 */

#include "xam_lan_lobby.h"

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
                      "ReXGlue instances (must be open in the firewall)");
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
  const std::string& override_tag = systemlink::gamertag_override();
  const std::string name =
      override_tag.empty() ? REX_KERNEL_STATE()->user_profile()->name() : override_tag;
  std::strncpy(tag, name.c_str(), sizeof(tag) - 1);
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
  systemlink::xnaddr_from_ip(from_ip, &invite.host_info.hostAddress);
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
      return false;
    }
    InviteInfo invite{};
    invite.title_id = target->title_id;
    invite.xuid_inviter = target->host_xuid;
    invite.xuid_invitee = 0;
    std::memcpy(invite.gamertag, target->gamertag, sizeof(invite.gamertag));
    invite.host_info = target->info;
    accepted_invite = invite;
  }
  REXKRNL_INFO("LAN lobby: joining '{}' at {} (session {:016X})", target->gamertag,
               systemlink::ip_to_string(target->host_ip), session_id(target->info));
  REX_KERNEL_STATE()->BroadcastNotification(0x02000002, 0);  // XN_LIVE_INVITE_ACCEPTED
  return true;
}

void invite_all() {
  const uint64_t inviter = systemlink::online_xuid(REX_KERNEL_STATE()->user_profile()->xuid());
  for (const auto& player : RemotePlayers()) {
    SendInvite(player.xuid, inviter);
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
    close_socket(s);
    return;
  }
  lobby_socket.store(uint64_t(s));
  const uint32_t self = systemlink::local_ip();
  const auto peers = peer_ips();
  REXKRNL_INFO("LAN lobby: listening on UDP {} (broadcast {}, {} extra peer(s))", port,
               systemlink::ip_to_string(systemlink::broadcast_ip()), peers.size());

  auto next_beacon = std::chrono::steady_clock::now();
  std::vector<uint8_t> packet(1500);
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_beacon) {
      next_beacon = now + kBeaconInterval;
      maybe_auto_join();
      const uint32_t title_id = REX_KERNEL_STATE()->title_id();
      for (const auto& hosted : session::HostedSessions()) {
        const auto beacon = encode(hosted, title_id);
        send_to(s, beacon, systemlink::broadcast_ip(), port);
        for (uint32_t peer : peers) {
          send_to(s, beacon, peer, port);
        }
      }
      std::lock_guard lock(mutex);
      for (auto it = remotes.begin(); it != remotes.end();) {
        if (now - it->second.last_seen > kExpiry) {
          REXKRNL_INFO("LAN lobby: session {:016X} from {} expired", it->first,
                       systemlink::ip_to_string(it->second.host_ip));
          it = remotes.erase(it);
        } else {
          ++it;
        }
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
    if (from_ip == self) {
      continue;
    }
    if (n >= 4 && Reader{packet.data(), size_t(n)}.u32() == kInviteMagic) {
      handle_invite(packet.data(), size_t(n), from_ip);
      continue;
    }
    Advert advert;
    if (!decode(packet.data(), size_t(n), &advert) ||
        advert.title_id != REX_KERNEL_STATE()->title_id()) {
      continue;
    }
    // Trust the address the packet came from over the one it claims.
    advert.host_ip = from_ip;
    systemlink::xnaddr_from_ip(from_ip, &advert.info.hostAddress);
    advert.last_seen = std::chrono::steady_clock::now();
    const uint64_t id = session_id(advert.info);
    std::lock_guard lock(mutex);
    if (!remotes.count(id)) {
      REXKRNL_INFO("LAN lobby: found session {:016X} hosted by '{}' at {} ({}+{} of {}+{} slots)",
                   id, advert.gamertag, systemlink::ip_to_string(from_ip), advert.used_public,
                   advert.used_private, advert.max_public, advert.max_private);
    }
    remotes[id] = std::move(advert);
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
  const uint64_t sock = lobby_socket.load();
  const auto hosted = session::HostedSessions();
  if (sock == ~0ull || hosted.empty()) {
    REXKRNL_WARN("LAN lobby: cannot send invite (lobby {}, {} hosted session(s))",
                 sock == ~0ull ? "not running" : "running", hosted.size());
    return false;
  }
  uint32_t target_ip = 0;
  {
    std::lock_guard lock(mutex);
    for (const auto& [id, a] : remotes) {
      if (a.host_xuid == invitee_xuid) target_ip = a.host_ip;
    }
  }
  if (!target_ip) {
    REXKRNL_WARN("LAN lobby: invitee {:016X} is not on the LAN", invitee_xuid);
    return false;
  }
  Writer w;
  w.u32(kInviteMagic);
  w.u8(kVersion);
  w.u32(REX_KERNEL_STATE()->title_id());
  w.u64(inviter_xuid);
  w.u64(invitee_xuid);
  char tag[16] = {};
  const std::string& override_tag = systemlink::gamertag_override();
  const std::string name =
      override_tag.empty() ? REX_KERNEL_STATE()->user_profile()->name() : override_tag;
  std::strncpy(tag, name.c_str(), sizeof(tag) - 1);
  w.bytes(tag, sizeof(tag));
  w.bytes(&hosted.front().info, sizeof(XSESSION_INFO));
  for (int i = 0; i < 3; ++i) {  // UDP: send a few copies
    send_to(sock_t(sock), w.buf, target_ip, uint16_t(REXCVAR_GET(lan_lobby_port)));
  }
  REXKRNL_INFO("LAN lobby: sent game invite for session {:016X} to {}",
               session_id(hosted.front().info), systemlink::ip_to_string(target_ip));
  return true;
}

bool TakeAcceptedInvite(InviteInfo* out) {
  std::lock_guard lock(mutex);
  if (!accepted_invite) return false;
  *out = *accepted_invite;
  return true;
}

}  // namespace rex::kernel::xam::lobby
