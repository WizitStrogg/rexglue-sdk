/**
 ******************************************************************************
 * ReXGlue : XSession bookkeeping for System Link                             *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xam_session.h"
#include "xam_lan_lobby.h"

#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <rex/kernel/xam/systemlink.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

namespace rex::kernel::xam::session {

namespace {

constexpr X_HRESULT X_ONLINE_E_SESSION_NOT_FOUND = static_cast<X_HRESULT>(0x80155200L);
constexpr X_HRESULT X_ONLINE_E_SESSION_FULL = static_cast<X_HRESULT>(0x80155202L);
constexpr X_HRESULT X_ONLINE_E_SESSION_WRONG_STATE = static_cast<X_HRESULT>(0x80155206L);

enum SessionFlags : uint32_t {
  kHost = 0x01,
  kPresence = 0x02,
  kStats = 0x04,
  kMatchmaking = 0x08,
  kArbitration = 0x10,
  kPeerNetwork = 0x20,
  kJoinInProgressDisabled = 0x0400,
};

enum class SessionState : uint32_t { kLobby, kRegistration, kInGame, kReporting, kDeleted };

enum MemberFlags : uint32_t { kPrivateSlot = 0x01, kZombie = 0x02 };

// Natural alignment: the u64 nonce sits at 0x30, giving the SDK's 0x80 size.
struct XSESSION_LOCAL_DETAILS {
  rex::be<uint32_t> UserIndexHost;
  rex::be<uint32_t> GameType;
  rex::be<uint32_t> GameMode;
  rex::be<uint32_t> Flags;
  rex::be<uint32_t> MaxPublicSlots;
  rex::be<uint32_t> MaxPrivateSlots;
  rex::be<uint32_t> AvailablePublicSlots;
  rex::be<uint32_t> AvailablePrivateSlots;
  rex::be<uint32_t> ActualMemberCount;
  rex::be<uint32_t> ReturnedMemberCount;
  rex::be<uint32_t> eState;
  rex::be<uint64_t> Nonce;
  XSESSION_INFO sessionInfo;
  XNKID xnkidArbitration;
  rex::be<uint32_t> SessionMembers_ptr;
};
static_assert(offsetof(XSESSION_LOCAL_DETAILS, Nonce) == 0x30, "Nonce offset");
static_assert(sizeof(XSESSION_LOCAL_DETAILS) == 0x80, "XSESSION_LOCAL_DETAILS size");

struct XSESSION_MEMBER {
  rex::be<uint64_t> OnlineXUID;
  rex::be<uint32_t> UserIndex;
  rex::be<uint32_t> Flags;
};
static_assert(sizeof(XSESSION_MEMBER) == 0x10, "XSESSION_MEMBER size");

struct XGI_SESSION_CREATE {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> flags;
  rex::be<uint32_t> num_slots_public;
  rex::be<uint32_t> num_slots_private;
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> session_info_ptr;
  rex::be<uint32_t> nonce_ptr;
};

struct XGI_SESSION_STATE {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> flags;
  rex::be<uint64_t> session_nonce;
};

struct XGI_SESSION_MANAGE {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> array_count;
  rex::be<uint32_t> xuid_array_ptr;
  rex::be<uint32_t> indices_array_ptr;
  rex::be<uint32_t> private_slots_array_ptr;
};

struct XGI_SESSION_MODIFY {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> flags;
  rex::be<uint32_t> maxPublicSlots;
  rex::be<uint32_t> maxPrivateSlots;
};

struct XGI_SESSION_DETAILS {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> details_buffer_size_ptr;
  rex::be<uint32_t> session_details_ptr;
  rex::be<uint32_t> reserved1;
  rex::be<uint32_t> reserved2;
  rex::be<uint32_t> reserved3;
};

struct XGI_SESSION_MIGRATE {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> session_info_ptr;
  rex::be<uint32_t> user_index;
};

struct Member {
  uint64_t xuid = 0;
  uint32_t user_index = 0xFE;
  bool is_private = false;
};

struct Session {
  bool created = false;
  bool host = false;
  uint32_t flags = 0;
  uint32_t user_index_host = 0;
  uint32_t max_public = 0;
  uint32_t max_private = 0;
  SessionState state = SessionState::kLobby;
  uint64_t nonce = 0;
  XSESSION_INFO info = {};
  std::vector<Member> members;

  uint32_t used_public() const {
    uint32_t n = 0;
    for (const auto& m : members) n += !m.is_private;
    return n;
  }
  uint32_t used_private() const {
    uint32_t n = 0;
    for (const auto& m : members) n += m.is_private;
    return n;
  }
};

std::mutex mutex;
std::map<uint32_t, uint32_t> handle_objects;  // handle -> guest object
std::map<uint32_t, Session> sessions;          // guest object -> state
uint32_t next_handle = 0xCAFE0001;

memory::Memory* guest_memory() {
  return REX_KERNEL_MEMORY();
}

template <typename T>
T* guest(uint32_t address) {
  return address ? guest_memory()->TranslateVirtual<T*>(address) : nullptr;
}

uint64_t local_xuid() {
  return systemlink::online_xuid(REX_KERNEL_STATE()->user_profile()->xuid());
}

uint64_t xnkid_value(const XNKID& kid) {
  return memory::load_and_swap<uint64_t>(kid.ab);
}

Session* find(uint32_t obj_ptr) {
  auto it = sessions.find(obj_ptr);
  return it == sessions.end() ? nullptr : &it->second;
}

X_HRESULT add_member(Session& s, uint64_t xuid, uint32_t user_index, bool want_private) {
  for (const auto& m : s.members) {
    if (m.xuid == xuid) {
      return X_E_SUCCESS;  // already joined
    }
  }
  bool use_private = want_private && s.used_private() < s.max_private;
  if (!use_private && s.used_public() >= s.max_public) {
    if (s.used_private() < s.max_private) {
      use_private = true;
    } else {
      return X_ONLINE_E_SESSION_FULL;
    }
  }
  s.members.push_back({xuid, user_index, use_private});
  return X_E_SUCCESS;
}

}  // namespace

uint32_t CreateHandle() {
  std::lock_guard lock(mutex);
  const uint32_t handle = next_handle++;
  // Titles only ever pass the object back to us, but it must be a distinct
  // guest address per session (the console's object is 4 bytes).
  const uint32_t object = guest_memory()->SystemHeapAlloc(16);
  std::memset(guest<uint8_t>(object), 0, 16);
  memory::store_and_swap<uint32_t>(guest<uint8_t>(object), handle);
  handle_objects[handle] = object;
  sessions[object] = Session{};
  return handle;
}

uint32_t ObjectForHandle(uint32_t handle) {
  std::lock_guard lock(mutex);
  auto it = handle_objects.find(handle);
  if (it != handle_objects.end()) {
    return it->second;
  }
  // Unknown handle: hand out a fresh object so the title can't crash on it.
  const uint32_t object = guest_memory()->SystemHeapAlloc(16);
  std::memset(guest<uint8_t>(object), 0, 16);
  handle_objects[handle] = object;
  sessions[object] = Session{};
  return object;
}

X_HRESULT Create(uint8_t* buffer) {
  auto* data = reinterpret_cast<XGI_SESSION_CREATE*>(buffer);
  std::lock_guard lock(mutex);
  Session& s = sessions[data->obj_ptr];
  if (s.created && s.state != SessionState::kDeleted) {
    return X_ONLINE_E_SESSION_WRONG_STATE;
  }
  s = Session{};

  auto* info = guest<XSESSION_INFO>(data->session_info_ptr);
  auto* nonce = guest<rex::be<uint64_t>>(data->nonce_ptr);

  s.flags = data->flags;
  s.max_public = data->num_slots_public;
  s.max_private = data->num_slots_private;
  s.user_index_host = data->user_index;

  // Hosts (and purely local/offline sessions) mint the session; everyone
  // else joins the session info the title received from the host.
  // Sessions without peer networking (presence/stats only) are local.
  const bool hosting = (s.flags & kHost) || !(s.flags & kPeerNetwork) || !info;
  if (hosting) {
    s.host = true;
    uint64_t id = systemlink::registered_session_id();
    if (!id) {
      XNKID kid;
      systemlink::create_key(&kid, &s.info.keyExchangeKey);
      id = xnkid_value(kid);
      systemlink::set_registered_session_id(id);
    } else {
      systemlink::random_bytes(s.info.keyExchangeKey.ab, sizeof(s.info.keyExchangeKey.ab));
    }
    memory::store_and_swap<uint64_t>(s.info.sessionID.ab, id);
    systemlink::get_local_xnaddr(&s.info.hostAddress);
    systemlink::random_bytes(&s.nonce, sizeof(s.nonce));
    if (info) {
      // Keep a key the title may already have put there (XNetCreateKey).
      if (xnkid_value(info->sessionID) == id) {
        s.info.keyExchangeKey = info->keyExchangeKey;
      }
      *info = s.info;
    }
    if (nonce) {
      *nonce = s.nonce;
    }
  } else {
    s.info = *info;
    s.nonce = nonce ? uint64_t(*nonce) : 0;
    systemlink::set_registered_session_id(xnkid_value(s.info.sessionID));
  }
  s.created = true;
  lobby::Start();

  REXKRNL_INFO("XSessionCreate obj={:08X} flags={:08X} slots={}+{} {} session {:016X} host {}",
               uint32_t(data->obj_ptr), s.flags, s.max_public, s.max_private,
               s.host ? "HOSTING" : "JOINING", xnkid_value(s.info.sessionID),
               systemlink::ip_to_string(systemlink::ip_from_xnaddr(s.info.hostAddress)));
  return X_E_SUCCESS;
}

X_HRESULT Delete(uint8_t* buffer) {
  auto* data = reinterpret_cast<XGI_SESSION_STATE*>(buffer);
  std::lock_guard lock(mutex);
  Session* s = find(data->obj_ptr);
  if (!s) {
    return X_E_SUCCESS;
  }
  REXKRNL_INFO("XSessionDelete obj={:08X} session {:016X}", uint32_t(data->obj_ptr),
               xnkid_value(s->info.sessionID));
  if (systemlink::registered_session_id() == xnkid_value(s->info.sessionID)) {
    systemlink::set_registered_session_id(0);
  }
  s->state = SessionState::kDeleted;
  s->members.clear();
  return X_E_SUCCESS;
}

X_HRESULT Join(uint8_t* buffer) {
  auto* data = reinterpret_cast<XGI_SESSION_MANAGE*>(buffer);
  std::lock_guard lock(mutex);
  Session* s = find(data->obj_ptr);
  if (!s || !s->created) {
    return X_ONLINE_E_SESSION_NOT_FOUND;
  }
  if (s->state == SessionState::kDeleted) {
    return X_ONLINE_E_SESSION_WRONG_STATE;
  }
  if (s->state == SessionState::kInGame && (s->flags & (kArbitration | kJoinInProgressDisabled))) {
    return X_ONLINE_E_SESSION_FULL;
  }
  const bool local = data->xuid_array_ptr == 0;
  auto* xuids = guest<rex::be<uint64_t>>(data->xuid_array_ptr);
  auto* indices = guest<rex::be<uint32_t>>(data->indices_array_ptr);
  auto* privates = guest<rex::be<uint32_t>>(data->private_slots_array_ptr);
  for (uint32_t i = 0; i < data->array_count; ++i) {
    const uint32_t user_index = local && indices ? uint32_t(indices[i]) : 0xFE;
    const uint64_t xuid = local ? local_xuid() : uint64_t(xuids[i]);
    const bool want_private = privates && privates[i];
    X_HRESULT result = add_member(*s, xuid, user_index, want_private);
    REXKRNL_INFO("XSessionJoin{} obj={:08X} xuid {:016X}{}", local ? "Local" : "Remote",
                 uint32_t(data->obj_ptr), xuid, result == X_E_SUCCESS ? "" : " -> FULL");
    if (result != X_E_SUCCESS) {
      return result;
    }
  }
  return X_E_SUCCESS;
}

X_HRESULT Leave(uint8_t* buffer) {
  auto* data = reinterpret_cast<XGI_SESSION_MANAGE*>(buffer);
  std::lock_guard lock(mutex);
  Session* s = find(data->obj_ptr);
  if (!s) {
    return X_ONLINE_E_SESSION_NOT_FOUND;
  }
  const bool local = data->xuid_array_ptr == 0;
  auto* xuids = guest<rex::be<uint64_t>>(data->xuid_array_ptr);
  for (uint32_t i = 0; i < data->array_count; ++i) {
    const uint64_t xuid = local ? local_xuid() : uint64_t(xuids[i]);
    std::erase_if(s->members, [xuid](const Member& m) { return m.xuid == xuid; });
    REXKRNL_INFO("XSessionLeave{} obj={:08X} xuid {:016X}", local ? "Local" : "Remote",
                 uint32_t(data->obj_ptr), xuid);
  }
  return X_E_SUCCESS;
}

X_HRESULT Start(uint8_t* buffer) {
  auto* data = reinterpret_cast<XGI_SESSION_STATE*>(buffer);
  std::lock_guard lock(mutex);
  Session* s = find(data->obj_ptr);
  if (!s || s->state == SessionState::kDeleted) {
    return X_ONLINE_E_SESSION_WRONG_STATE;
  }
  s->state = SessionState::kInGame;
  REXKRNL_INFO("XSessionStart obj={:08X}", uint32_t(data->obj_ptr));
  return X_E_SUCCESS;
}

X_HRESULT End(uint8_t* buffer) {
  auto* data = reinterpret_cast<XGI_SESSION_STATE*>(buffer);
  std::lock_guard lock(mutex);
  Session* s = find(data->obj_ptr);
  if (!s || s->state == SessionState::kDeleted) {
    return X_ONLINE_E_SESSION_WRONG_STATE;
  }
  s->state = SessionState::kLobby;
  REXKRNL_INFO("XSessionEnd obj={:08X}", uint32_t(data->obj_ptr));
  return X_E_SUCCESS;
}

X_HRESULT Modify(uint8_t* buffer) {
  auto* data = reinterpret_cast<XGI_SESSION_MODIFY*>(buffer);
  std::lock_guard lock(mutex);
  Session* s = find(data->obj_ptr);
  if (!s) {
    return X_ONLINE_E_SESSION_NOT_FOUND;
  }
  // Only the modifier flags (join-in-progress etc.) may change.
  s->flags = (s->flags & 0x3F) | (data->flags & ~0x3Fu);
  s->max_public = data->maxPublicSlots;
  s->max_private = data->maxPrivateSlots;
  REXKRNL_INFO("XSessionModify obj={:08X} flags={:08X} slots={}+{}", uint32_t(data->obj_ptr),
               s->flags, s->max_public, s->max_private);
  return X_E_SUCCESS;
}

X_HRESULT GetDetails(uint8_t* buffer) {
  auto* data = reinterpret_cast<XGI_SESSION_DETAILS*>(buffer);
  std::lock_guard lock(mutex);
  Session* s = find(data->obj_ptr);
  auto* size_ptr = guest<rex::be<uint32_t>>(data->details_buffer_size_ptr);
  auto* details = guest<XSESSION_LOCAL_DETAILS>(data->session_details_ptr);
  if (!s) {
    return X_ONLINE_E_SESSION_NOT_FOUND;
  }
  if (!size_ptr || !details) {
    return X_E_INVALIDARG;
  }
  const uint32_t member_bytes = uint32_t(s->members.size() * sizeof(XSESSION_MEMBER));
  const uint32_t needed = sizeof(XSESSION_LOCAL_DETAILS) + member_bytes;
  const uint32_t size = *size_ptr;
  if (size < sizeof(XSESSION_LOCAL_DETAILS)) {
    *size_ptr = needed;
    return X_HRESULT_FROM_WIN32(X_ERROR_INSUFFICIENT_BUFFER);
  }

  std::memset(details, 0, sizeof(*details));
  details->UserIndexHost = s->user_index_host;
  details->Flags = s->flags;
  details->MaxPublicSlots = s->max_public;
  details->MaxPrivateSlots = s->max_private;
  details->AvailablePublicSlots = s->max_public - std::min(s->max_public, s->used_public());
  details->AvailablePrivateSlots = s->max_private - std::min(s->max_private, s->used_private());
  details->eState = static_cast<uint32_t>(s->state);
  details->Nonce = s->nonce;
  details->sessionInfo = s->info;

  const uint32_t room = (size - sizeof(XSESSION_LOCAL_DETAILS)) / sizeof(XSESSION_MEMBER);
  auto* members = reinterpret_cast<XSESSION_MEMBER*>(details + 1);
  uint32_t returned = 0;
  for (const auto& m : s->members) {
    if (returned >= room) break;
    members[returned].OnlineXUID = m.xuid;
    members[returned].UserIndex = m.user_index;
    members[returned].Flags = m.is_private ? kPrivateSlot : 0;
    ++returned;
  }
  details->ActualMemberCount = uint32_t(s->members.size());
  details->ReturnedMemberCount = returned;
  details->SessionMembers_ptr = returned ? data->session_details_ptr + sizeof(*details) : 0;
  return X_E_SUCCESS;
}

X_HRESULT MigrateHost(uint8_t* buffer) {
  auto* data = reinterpret_cast<XGI_SESSION_MIGRATE*>(buffer);
  std::lock_guard lock(mutex);
  Session* s = find(data->obj_ptr);
  if (!s) {
    return X_ONLINE_E_SESSION_NOT_FOUND;
  }
  auto* info = guest<XSESSION_INFO>(data->session_info_ptr);
  if (data->user_index != 0xFE) {
    // We become the host.
    s->host = true;
    s->user_index_host = data->user_index;
    systemlink::get_local_xnaddr(&s->info.hostAddress);
    if (info) {
      *info = s->info;
    }
  } else if (info) {
    s->host = false;
    s->info = *info;
  }
  REXKRNL_INFO("XSessionMigrateHost obj={:08X} -> {}", uint32_t(data->obj_ptr),
               s->host ? "local" : "remote");
  return X_E_SUCCESS;
}

std::vector<HostedSession> HostedSessions() {
  std::lock_guard lock(mutex);
  std::vector<HostedSession> out;
  for (const auto& [obj, s] : sessions) {
    if (!s.created || !s.host || s.state == SessionState::kDeleted ||
        !(s.flags & kPeerNetwork)) {
      continue;
    }
    out.push_back({s.info, s.flags, s.max_public, s.max_private, s.used_public(), s.used_private(),
                   local_xuid()});
  }
  return out;
}

}  // namespace rex::kernel::xam::session
