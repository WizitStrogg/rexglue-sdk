/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/logging.h>
#include <rex/system/xenumerator.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/thread.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "../xam_lan_lobby.h"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

namespace {

std::string HexBytes(const uint8_t* p, size_t n) {
  std::string out;
  char b[4];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(b, sizeof(b), "%02X", p[i]);
    out += b;
    if ((i & 3) == 3 && i + 1 < n) out += ' ';
  }
  return out;
}

}  // namespace

// Unknown messages are often polled every frame. Log the first few calls per
// message with a dump of the argument block (and of the blocks its first
// words point at) so the layout can be worked out, then stay quiet.
void XLiveBaseApp::LogUnimplemented(uint32_t message, uint32_t buffer_ptr,
                                    uint32_t buffer_length) {
  static std::mutex mutex;
  static std::map<uint32_t, uint32_t> counts;
  uint32_t seen;
  {
    std::lock_guard<std::mutex> lock(mutex);
    seen = counts[message]++;
  }
  if (seen >= 4) {
    if (seen == 4) {
      REXKRNL_WARN("XLIVEBASE message {:08X}: further calls not logged", message);
    }
    return;
  }
  REXKRNL_ERROR("Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, arg2={:08X}",
                app_id(), message, buffer_ptr, buffer_length);
  if (!buffer_ptr) {
    return;
  }
  auto block = memory_->TranslateVirtual<uint8_t*>(buffer_ptr);
  REXKRNL_ERROR("  arg1 @{:08X}: {}", buffer_ptr, HexBytes(block, 0x40));
  if (buffer_length == 0x28) {
    // Async LIVE task: dump the XLIVE_ASYNC_TASK and its marshalled request.
    const uint32_t task_ptr = memory::load_and_swap<uint32_t>(block);
    auto task_heap = task_ptr ? memory_->LookupHeap(task_ptr) : nullptr;
    if (task_heap) {
      auto* task = memory_->TranslateVirtual<uint8_t*>(task_ptr);
      REXKRNL_ERROR("  task @{:08X}: {}", task_ptr, HexBytes(task, 0x40));
      const uint32_t request_ptr = memory::load_and_swap<uint32_t>(task + 0x18);
      const uint32_t request_size = memory::load_and_swap<uint32_t>(task + 0x1C);
      if (request_ptr && memory_->LookupHeap(request_ptr)) {
        REXKRNL_ERROR("  request @{:08X} ({} bytes): {}", request_ptr, request_size,
                      HexBytes(memory_->TranslateVirtual<uint8_t*>(request_ptr),
                               std::min<uint32_t>(std::max<uint32_t>(request_size, 0x20), 0x80)));
      }
    }
  }
  for (uint32_t i = 0; i < 8; ++i) {
    uint32_t ptr = memory::load_and_swap<uint32_t>(block + i * 4);
    auto heap = ptr ? memory_->LookupHeap(ptr) : nullptr;
    uint32_t protect = 0;
    if (heap && heap->QueryProtect(ptr, &protect) && (protect & memory::kMemoryProtectRead) &&
        heap->QueryProtect(ptr + 0x1F, &protect) && (protect & memory::kMemoryProtectRead)) {
      REXKRNL_ERROR("  arg1[{}] -> @{:08X}: {}", i, ptr,
                    HexBytes(memory_->TranslateVirtual<uint8_t*>(ptr), 0x20));
    }
  }
}

// Fable II asks for the server path of its title-storage files (items.dat,
// orb settings) every frame until it gets one. Hand back the console's path
// format; the matching download then reports the file as missing.
X_HRESULT XLiveBaseApp::XStorageBuildServerPath(uint32_t buffer_ptr) {
  if (!buffer_ptr) {
    return X_E_INVALIDARG;
  }
  // { u32 user_index; u64 xuid (8-aligned); u32 facility; u32 info_ptr;
  //   u32 info_size; u32 file_name_ptr; u32 server_path_ptr; u32 server_path_length_ptr; }
  auto* args = memory_->TranslateVirtual<uint8_t*>(buffer_ptr);
  const uint64_t xuid_arg = memory::load_and_swap<uint64_t>(args + 0x08);
  const uint32_t facility = memory::load_and_swap<uint32_t>(args + 0x10);
  const uint32_t file_name_ptr = memory::load_and_swap<uint32_t>(args + 0x1C);
  const uint32_t server_path_ptr = memory::load_and_swap<uint32_t>(args + 0x20);
  const uint32_t server_path_length_ptr = memory::load_and_swap<uint32_t>(args + 0x24);

  std::string file_name;
  if (file_name_ptr) {
    auto* name = memory_->TranslateVirtual<uint8_t*>(file_name_ptr);
    for (uint32_t i = 0; i < 256; ++i) {
      const uint16_t c = memory::load_and_swap<uint16_t>(name + i * 2);
      if (!c) break;
      file_name.push_back(c < 0x80 ? char(c) : '_');
    }
  }
  const uint32_t title_id = kernel_state_->title_id();
  char path[512];
  switch (facility) {
    case 2:  // per title
      std::snprintf(path, sizeof(path), "//title.%08x/t:%08x/%s", title_id, title_id,
                    file_name.c_str());
      break;
    case 3: {  // per user, per title
      const uint64_t xuid = xuid_arg ? xuid_arg : kernel_state_->user_profile()->xuid();
      std::snprintf(path, sizeof(path), "//tuser.%08x/u:%016llx/%08x/%s", title_id,
                    static_cast<unsigned long long>(xuid), title_id, file_name.c_str());
      break;
    }
    default:
      return 0x8015C009;  // X_ONLINE_E_STORAGE_INVALID_FACILITY
  }
  const uint32_t chars = uint32_t(std::strlen(path)) + 1;
  if (server_path_length_ptr) {
    auto* length = memory_->TranslateVirtual<uint8_t*>(server_path_length_ptr);
    const uint32_t capacity = memory::load_and_swap<uint32_t>(length);
    if (server_path_ptr && capacity < chars) {
      return X_E_INVALIDARG;
    }
    memory::store_and_swap<uint32_t>(length, chars);
  }
  if (server_path_ptr) {
    auto* out = memory_->TranslateVirtual<uint8_t*>(server_path_ptr);
    for (uint32_t i = 0; i < chars; ++i) {
      memory::store_and_swap<uint16_t>(out + i * 2, uint8_t(path[i]));
    }
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) {
    REXKRNL_INFO("XStorageBuildServerPath facility={} file='{}' -> {}", facility, file_name, path);
  }
  return X_E_SUCCESS;
}

// XStorageEnumerate is an async LIVE task: the message is an
// XLIVEBASE_ASYNC_MESSAGE whose first word points at the XLIVE_ASYNC_TASK
// (results_ptr at +0x2C, results_size at +0x30). With no title storage
// online, report an empty listing so the title moves on.
X_HRESULT XLiveBaseApp::XStorageEnumerate(uint32_t buffer_ptr, uint32_t buffer_length) {
  if (!buffer_ptr || buffer_length != 0x28) {
    return X_E_INVALIDARG;
  }
  auto* message = memory_->TranslateVirtual<uint8_t*>(buffer_ptr);
  const uint32_t task_ptr = memory::load_and_swap<uint32_t>(message);
  if (!task_ptr) {
    return X_E_INVALIDARG;
  }
  auto* task = memory_->TranslateVirtual<uint8_t*>(task_ptr);
  const uint32_t results_ptr = memory::load_and_swap<uint32_t>(task + 0x2C);
  const uint32_t results_size = memory::load_and_swap<uint32_t>(task + 0x30);
  if (!results_ptr || results_size < 0xC) {
    return X_E_INVALIDARG;
  }
  // X_STORAGE_ENUMERATE_RESULTS { total_num_items, num_items_returned, items_ptr }
  auto* results = memory_->TranslateVirtual<uint8_t*>(results_ptr);
  std::memset(results, 0, results_size);
  memory::store_and_swap<uint32_t>(results + 0x8, results_ptr + 0xC);
  REXKRNL_INFO("XStorageEnumerate -> 0 items (no LIVE title storage)");
  return X_E_SUCCESS;
}

// XFriendsCreateEnumerator. buffer_length points at an X_ARGUMENT_LIST
// (32 entries of { u32 size; u32 pad; u64 value_ptr }, count at +0x200):
// user index, starting index, amount, buffer-size out, handle out.
// The list holds the players heard from on the LAN, shown as online friends
// in this title so Fable II can draw their orbs.
X_HRESULT XLiveBaseApp::XFriendsCreateEnumerator(uint32_t buffer_ptr, uint32_t args_ptr) {
  if (!buffer_ptr || !args_ptr || !memory_->LookupHeap(args_ptr)) {
    return X_E_INVALIDARG;
  }
  auto* args = memory_->TranslateVirtual<uint8_t*>(args_ptr);
  if (memory::load_and_swap<uint32_t>(args + 0x200) != 5) {
    return X_E_INVALIDARG;
  }
  auto arg_ptr = [&](int i) {
    return static_cast<uint32_t>(memory::load_and_swap<uint64_t>(args + i * 0x10 + 8));
  };
  auto read_arg = [&](int i) {
    const uint32_t ptr = arg_ptr(i);
    return ptr ? memory::load_and_swap<uint32_t>(memory_->TranslateVirtual<uint8_t*>(ptr)) : 0u;
  };
  const uint32_t user_index = read_arg(0);
  const uint32_t starting_index = read_arg(1);
  const uint32_t amount = read_arg(2);
  const uint32_t buffer_size_ptr = arg_ptr(3);
  const uint32_t handle_ptr = arg_ptr(4);
  if (!handle_ptr || !buffer_size_ptr) {
    return X_E_INVALIDARG;
  }
  memory::store_and_swap<uint32_t>(memory_->TranslateVirtual<uint8_t*>(handle_ptr), 0);
  memory::store_and_swap<uint32_t>(memory_->TranslateVirtual<uint8_t*>(buffer_size_ptr), 0);
  if (user_index != 0 || amount == 0 || amount > 100 || starting_index >= 100) {
    return X_E_INVALIDARG;
  }

  constexpr size_t kFriendSize = 0xC4;  // X_ONLINE_FRIEND
  auto e = object_ref<XStaticUntypedEnumerator>(
      new XStaticUntypedEnumerator(kernel_state_, amount, kFriendSize));
  auto result = e->Initialize(0xFF, app_id(), 0x58021, 0x58022, 0);
  if (XFAILED(result)) {
    return result;
  }
  const auto players = lobby::RemotePlayers();
  for (size_t i = starting_index; i < players.size(); ++i) {
    const auto& p = players[i];
    uint8_t* item = e->AppendItem();
    std::memset(item, 0, kFriendSize);
    memory::store_and_swap<uint64_t>(item + 0x00, p.xuid);
    std::memcpy(item + 0x08, p.gamertag, 15);
    // XONLINE_FRIENDSTATE_FLAG_ONLINE | PLAYING | (JOINABLE)
    uint32_t state = 0x1 | 0x2 | (p.joinable ? 0x10 : 0);
    memory::store_and_swap<uint32_t>(item + 0x18, state);
    std::memcpy(item + 0x1C, p.session_id, 8);
    memory::store_and_swap<uint32_t>(item + 0x24, p.title_id);
  }
  memory::store_and_swap<uint32_t>(memory_->TranslateVirtual<uint8_t*>(buffer_size_ptr),
                                   uint32_t(amount * kFriendSize));
  memory::store_and_swap<uint32_t>(memory_->TranslateVirtual<uint8_t*>(handle_ptr), e->handle());
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8 || !players.empty()) {
    REXKRNL_INFO("XFriendsCreateEnumerator start={} amount={} -> {} LAN player(s)", starting_index,
                 amount, players.size() > starting_index ? players.size() - starting_index : 0);
  }
  return X_E_SUCCESS;
}

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      return 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058020: {
      return XFriendsCreateEnumerator(buffer_ptr, buffer_length);
    }
    case 0x00058023: {
      REXKRNL_DEBUG(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058035: {
      return XStorageBuildServerPath(buffer_ptr);
    }
    case 0x00050009: {
      // XStorageDownloadToMemory: there is no LIVE title storage to fetch from.
      REXKRNL_DEBUG("XStorageDownloadToMemory({:08X}) -> file not found", buffer_ptr);
      return 0x8015C004;  // X_ONLINE_E_STORAGE_FILE_NOT_FOUND
    }
    case 0x0005000A: {
      return XStorageEnumerate(buffer_ptr, buffer_length);
    }
    case 0x00058037: {
      REXKRNL_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  LogUnimplemented(message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
