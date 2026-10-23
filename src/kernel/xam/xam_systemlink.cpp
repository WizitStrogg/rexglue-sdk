/**
 ******************************************************************************
 * ReXGlue : System Link (LAN) networking support                             *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <rex/kernel/xam/systemlink.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_STRING(network_mode, "offline", "Network",
                      "Guest networking: 'offline' (no network, the default) or 'systemlink' "
                      "(LAN play between ReXGlue instances)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(network_interface, "",
                      "Network",
                      "IPv4 address of the host adapter used for System Link (e.g. a ZeroTier "
                      "or Hamachi address). Empty = pick the first private LAN adapter; the LAN "
                      "lobby still announces on every adapter")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(profile_xuid, "", "Network",
                      "Player identity. Empty or 'auto' = keep the built-in offline XUID (which "
                      "owns the saves) and, with networking on, derive a per-PC online XUID "
                      "from this PC's name. A 16-digit hex value replaces both, so existing "
                      "saves will not load")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(profile_gamertag, "", "Network",
                      "Gamertag shown to other players. Empty = built-in default");

namespace rex::kernel::xam::systemlink {

namespace {

struct Adapter {
  uint32_t ip = 0;    // host byte order
  uint32_t mask = 0;  // host byte order
  std::string name;
  bool has_gateway = false;
};

constexpr uint32_t kLoopback = 0x7F000001;

bool is_private(uint32_t ip) {
  return (ip >> 24) == 10 || (ip >> 20) == (172 << 4 | 1) || (ip >> 16) == (192 << 8 | 168) ||
         (ip >> 22) == (100 << 2 | 1);  // 100.64/10 (CGNAT, used by Tailscale)
}

[[maybe_unused]] uint32_t prefix_to_mask(unsigned prefix) {
  return prefix == 0 ? 0 : prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1);
}

std::vector<Adapter> enumerate_adapters() {
  std::vector<Adapter> result;
#if REX_PLATFORM_WIN32
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buffer;
  ULONG ret = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 4 && ret == ERROR_BUFFER_OVERFLOW; ++attempt) {
    buffer.resize(size);
    ret = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  }
  if (ret != NO_ERROR) {
    REXKRNL_WARN("SystemLink: GetAdaptersAddresses failed ({})", ret);
    return result;
  }
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
      continue;
    }
    std::string name;
    if (a->FriendlyName) {
      for (const wchar_t* c = a->FriendlyName; *c; ++c) {
        name.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
      }
    }
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (u->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }
      auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
      Adapter adapter;
      adapter.ip = ntohl(sin->sin_addr.s_addr);
      adapter.mask = prefix_to_mask(u->OnLinkPrefixLength);
      adapter.name = name;
      adapter.has_gateway = a->FirstGatewayAddress != nullptr;
      result.push_back(adapter);
    }
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) {
    return result;
  }
  for (ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
        (i->ifa_flags & IFF_LOOPBACK)) {
      continue;
    }
    Adapter adapter;
    adapter.ip = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
    adapter.mask =
        i->ifa_netmask ? ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr)
                       : 0xFFFFFF00u;
    adapter.name = i->ifa_name ? i->ifa_name : "";
    adapter.has_gateway = true;
    result.push_back(adapter);
  }
  freeifaddrs(list);
#endif
  return result;
}

bool parse_ipv4(std::string_view text, uint32_t* out) {
  in_addr addr{};
  std::string copy(text);
  if (inet_pton(AF_INET, copy.c_str(), &addr) != 1) {
    return false;
  }
  *out = ntohl(addr.s_addr);
  return true;
}

struct State {
  bool enabled = false;
  uint32_t ip = kLoopback;
  uint32_t broadcast = 0xFFFFFFFFu;
};

const State& state() {
  static const State s = [] {
    State st;
    const std::string& mode = REXCVAR_GET(network_mode);
    st.enabled = mode == "systemlink" || mode == "lan" || mode == "1";
    if (!st.enabled) {
      if (!mode.empty() && mode != "offline" && mode != "0") {
        REXKRNL_WARN("SystemLink: unknown network_mode '{}', staying offline", mode);
      }
      return st;
    }

    const auto adapters = enumerate_adapters();
    for (const auto& a : adapters) {
      REXKRNL_INFO("SystemLink: adapter '{}' {} mask {}{}", a.name, ip_to_string(a.ip),
                   ip_to_string(a.mask), a.has_gateway ? " (gateway)" : "");
    }

    const Adapter* chosen = nullptr;
    uint32_t wanted = 0;
    const std::string& iface = REXCVAR_GET(network_interface);
    if (!iface.empty()) {
      if (!parse_ipv4(iface, &wanted)) {
        REXKRNL_ERROR("SystemLink: network_interface '{}' is not an IPv4 address", iface);
      }
      for (const auto& a : adapters) {
        if (a.ip == wanted) {
          chosen = &a;
        }
      }
      if (!chosen && wanted) {
        REXKRNL_ERROR("SystemLink: no adapter has address {}", iface);
      }
    }
    // Prefer a private address with a default gateway (the home LAN), then
    // any private address (VPN-style virtual LANs), then anything.
    for (int pass = 0; pass < 3 && !chosen; ++pass) {
      for (const auto& a : adapters) {
        const bool ok = pass == 0   ? is_private(a.ip) && a.has_gateway
                        : pass == 1 ? is_private(a.ip)
                                    : (a.ip >> 16) != (169 << 8 | 254);
        if (ok) {
          chosen = &a;
          break;
        }
      }
    }

    if (chosen) {
      st.ip = chosen->ip;
      const uint32_t mask = chosen->mask ? chosen->mask : 0xFFFFFF00u;
      st.broadcast = (chosen->ip & mask) | ~mask;
      REXKRNL_INFO("SystemLink: enabled on '{}' {} (broadcast {})", chosen->name,
                   ip_to_string(st.ip), ip_to_string(st.broadcast));
    } else {
      REXKRNL_ERROR("SystemLink: no usable network adapter, using loopback only");
    }
    return st;
  }();
  return s;
}

std::mutex random_mutex;
std::mt19937_64& rng() {
  static std::mt19937_64 engine = [] {
    std::random_device rd;
    std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd()};
    return std::mt19937_64(seq);
  }();
  return engine;
}

std::atomic<uint64_t> registered_session{0};

bool is_usable(const Adapter& a) {
  return a.ip != kLoopback && (a.ip >> 16) != (169 << 8 | 254) && (a.ip >> 24) != 0;
}

std::mutex adapters_mutex;
std::vector<LocalAdapter> cached_adapters;
std::chrono::steady_clock::time_point adapters_time;

std::mutex alias_mutex;
std::map<uint32_t, uint32_t> peer_aliases;  // claimed ip -> observed ip

uint64_t fnv1a64(std::string_view text) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char c : text) {
    h = (h ^ c) * 0x100000001B3ull;
  }
  return h;
}

std::string host_name() {
  char name[256] = {};
#if REX_PLATFORM_WIN32
  DWORD size = sizeof(name);
  if (!GetComputerNameA(name, &size)) {
    return "rexglue";
  }
#else
  if (gethostname(name, sizeof(name) - 1) != 0) {
    return "rexglue";
  }
#endif
  return name;
}

}  // namespace

bool enabled() {
  return state().enabled;
}

uint32_t local_ip() {
  return state().ip;
}

uint32_t broadcast_ip() {
  return state().broadcast;
}

std::string ip_to_string(uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xFF) + "." +
         std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF);
}

std::vector<LocalAdapter> adapters() {
  std::lock_guard lock(adapters_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (cached_adapters.empty() || now - adapters_time > std::chrono::seconds(5)) {
    std::vector<LocalAdapter> fresh;
    for (const auto& a : enumerate_adapters()) {
      if (!is_usable(a)) {
        continue;
      }
      LocalAdapter out;
      out.ip = a.ip;
      out.mask = a.mask ? a.mask : 0xFFFFFF00u;
      out.broadcast = (a.ip & out.mask) | ~out.mask;
      out.name = a.name;
      fresh.push_back(out);
    }
    cached_adapters = std::move(fresh);
    adapters_time = now;
  }
  return cached_adapters;
}

uint32_t local_ip_for_peer(uint32_t peer_ip) {
  for (const auto& a : adapters()) {
    if ((a.ip & a.mask) == (peer_ip & a.mask)) {
      return a.ip;
    }
  }
  return local_ip();
}

void note_peer_address(uint32_t claimed_ip, uint32_t observed_ip) {
  if (!claimed_ip || claimed_ip == observed_ip) {
    return;
  }
  std::lock_guard lock(alias_mutex);
  auto [it, inserted] = peer_aliases.insert_or_assign(claimed_ip, observed_ip);
  if (inserted) {
    REXKRNL_INFO("SystemLink: peer at {} announces itself as {}; routing to {}",
                 ip_to_string(observed_ip), ip_to_string(claimed_ip), ip_to_string(observed_ip));
  }
}

void xnaddr_from_ip(uint32_t ip, XNADDR* out) {
  std::memset(out, 0, sizeof(XNADDR));
  out->ina = htonl(ip);
  out->abEnet[0] = 0x02;  // locally administered, unicast
  out->abEnet[1] = 0x58;  // 'X'
  out->abEnet[2] = static_cast<uint8_t>(ip >> 24);
  out->abEnet[3] = static_cast<uint8_t>(ip >> 16);
  out->abEnet[4] = static_cast<uint8_t>(ip >> 8);
  out->abEnet[5] = static_cast<uint8_t>(ip);
}

void get_local_xnaddr(XNADDR* out) {
  xnaddr_from_ip(local_ip(), out);
}

uint32_t ip_from_xnaddr(const XNADDR& addr) {
  uint32_t ip = 0;
  if (addr.ina) {
    ip = ntohl(addr.ina);
  } else if (addr.abEnet[0] == 0x02 && addr.abEnet[1] == 0x58) {
    ip = uint32_t(addr.abEnet[2]) << 24 | uint32_t(addr.abEnet[3]) << 16 |
         uint32_t(addr.abEnet[4]) << 8 | addr.abEnet[5];
  } else {
    ip = ntohl(addr.inaOnline);
  }
  std::lock_guard lock(alias_mutex);
  auto it = peer_aliases.find(ip);
  return it != peer_aliases.end() ? it->second : ip;
}

bool is_local_xnaddr(const XNADDR& addr) {
  XNADDR local;
  get_local_xnaddr(&local);
  return std::memcmp(local.abEnet, addr.abEnet, sizeof(local.abEnet)) == 0;
}

void random_bytes(void* out, size_t length) {
  std::lock_guard lock(random_mutex);
  auto* bytes = static_cast<uint8_t*>(out);
  for (size_t i = 0; i < length; i += 8) {
    const uint64_t v = rng()();
    std::memcpy(bytes + i, &v, std::min<size_t>(8, length - i));
  }
}

void create_key(XNKID* kid, XNKEY* key) {
  random_bytes(kid->ab, sizeof(kid->ab));
  kid->ab[0] = kXnkidSystemLink | (kid->ab[0] & 0x3F);
  if (key) {
    random_bytes(key->ab, sizeof(key->ab));
  }
}

uint64_t registered_session_id() {
  return registered_session.load();
}

void set_registered_session_id(uint64_t id) {
  registered_session.store(id);
}

uint64_t network_xuid(uint64_t legacy_xuid) {
  static const uint64_t xuid = [legacy_xuid] {
    const std::string& value = REXCVAR_GET(profile_xuid);
    if (value.empty()) {
      return legacy_xuid;
    }
    uint64_t result = 0;
    if (value == "auto") {
      // 'auto' only changes the online XUID (online_xuid); saves are owned by
      // the offline XUID, which stays the default.
      return legacy_xuid;
    } else {
      try {
        result = std::stoull(value, nullptr, 16);
      } catch (...) {
        REXKRNL_ERROR("SystemLink: profile_xuid '{}' is not hex, using default", value);
        return legacy_xuid;
      }
    }
    // 58410A1F-style titles reject XUIDs with bits 54-55 set.
    result &= ~(3ull << 54);
    REXKRNL_INFO("SystemLink: profile XUID {:016X}", result);
    return result;
  }();
  return xuid;
}

uint64_t online_xuid(uint64_t legacy_xuid) {
  if (!enabled()) {
    return network_xuid(legacy_xuid);
  }
  static const uint64_t xuid = [legacy_xuid] {
    const std::string& value = REXCVAR_GET(profile_xuid);
    uint64_t result = 0;
    if (!value.empty() && value != "auto") {
      result = network_xuid(legacy_xuid);  // explicit hex XUID wins
    } else {
      // Online XUID range (0x0009...), stable per PC name, so every PC in a
      // game is a different player without touching the save's owner.
      result = 0x0009000000000000ull | (fnv1a64(host_name() + "/online") & 0x0000FFFFFFFFFFFFull);
    }
    REXKRNL_INFO("SystemLink: online XUID {:016X} (saves stay with {:016X})", result,
                 network_xuid(legacy_xuid));
    return result;
  }();
  return xuid;
}

const std::string& gamertag_override() {
  return REXCVAR_GET(profile_gamertag);
}

std::string display_name() {
  const std::string& override_tag = gamertag_override();
  return override_tag.empty() ? REX_KERNEL_STATE()->user_profile()->name() : override_tag;
}

}  // namespace rex::kernel::xam::systemlink
