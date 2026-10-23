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
#include <rex/thread.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>

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
      // 0x00058004 is called right before this.
      // We should create a XamEnumerate-able empty list here, but I'm not
      // sure of the format.
      // buffer_length seems to be the same ptr sent to 0x00058004.
      REXKRNL_DEBUG("CXLiveFriends::Enumerate({:08X}, {:08X}) unimplemented", buffer_ptr,
                    buffer_length);
      return X_E_FAIL;
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
