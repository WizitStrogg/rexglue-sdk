/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    2026 - System Link: real XNADDRs, XNet keys, link status and
 *              working (overlapped) sockets. Parts adapted from the Xenia
 *              Canary netplay branch (AdrianCassar/xenia-canary, BSD).
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <cstring>

#if REX_PLATFORM_MAC
#include <sys/select.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xam/systemlink.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

namespace {

// Looks up a guest socket handle, setting WSAENOTSOCK when it is invalid.
object_ref<XSocket> lookup_socket(uint32_t handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
  }
  return socket;
}

// Records the host socket error as the guest's last error and returns -1.
uint32_t socket_failed() {
  XThread::SetLastError(XSocket::LastError());
  return ~0u;
}

std::string guest_ip_string(uint32_t host_order_ip) {
  return systemlink::ip_to_string(host_order_ip);
}

}  // namespace

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {};

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }
  REXKRNL_INFO("XNetStartup (System Link {})",
               systemlink::enabled() ? guest_ip_string(systemlink::local_ip()) : "off");
  return 0;
}

u32 NetDll_XNetStartupEx_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params, u32 version) {
  return NetDll_XNetStartup_entry(caller, params);
}

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

u32 NetDll_XNetSetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr, u32 buffer_size) {
  REXKRNL_DEBUG("NetDll_XNetSetOpt({}, {}) ignored", option_id, buffer_size);
  return 0;
}

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  // Two machines must never produce the same nonces/session ids.
  systemlink::random_bytes(buffer_ptr, length);
  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  return ret;
}

u32 NetDll_WSAStartupEx_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr,
                              u32 versionReq) {
  return NetDll_WSAStartup_entry(caller, version, data_ptr);
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers_ptr,
                             u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                             ppc_ptr_t<XSOCKADDR_IN> from_addr, mapped_u32 fromlen_ptr,
                             ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                             mapped_void completion_routine_ptr) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  int ret = socket->WSARecvFrom(buffers_ptr, buffer_count, num_bytes_recv, flags_ptr, from_addr,
                                fromlen_ptr, overlapped_ptr);
  if (ret < 0) {
    return socket_failed();
  }
  return 0;
}

u32 NetDll_WSARecv_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers_ptr,
                         u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                         ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                         mapped_void completion_routine_ptr) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  int ret = socket->WSARecvFrom(buffers_ptr, buffer_count, num_bytes_recv, flags_ptr, nullptr,
                                nullptr, overlapped_ptr);
  if (ret < 0) {
    return socket_failed();
  }
  return 0;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  int ret = socket->WSASendTo(buffers, num_buffers, num_bytes_sent, flags, to_ptr, to_len,
                              overlapped);
  if (ret < 0) {
    return socket_failed();
  }
  return 0;
}

u32 NetDll_WSASend_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                         u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                         ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  int ret = socket->WSASendTo(buffers, num_buffers, num_bytes_sent, flags, nullptr, 0,
                              overlapped);
  if (ret < 0) {
    return socket_failed();
  }
  return 0;
}

u32 NetDll_WSAGetOverlappedResult_entry(u32 caller, u32 socket_handle,
                                        ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                                        mapped_u32 bytes_transferred, u32 wait,
                                        mapped_u32 flags_ptr) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return 0;
  }
  if (!socket->WSAGetOverlappedResult(overlapped_ptr, bytes_transferred, wait != 0, flags_ptr)) {
    XThread::SetLastError(XSocket::LastError());
    return 0;
  }
  return 1;
}

u32 NetDll_WSACancelOverlappedIO_entry(u32 caller, u32 socket_handle) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  return socket->WSACancelOverlappedIO();
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                    timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  // WSA_WAIT_EVENT_0 + index / WSA_WAIT_TIMEOUT share NTSTATUS values.
  return result;
}

u32 NetDll_WSACreateEvent_entry() {
  auto ev = object_ref<XEvent>(new XEvent(REX_KERNEL_STATE()));
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  if (!systemlink::enabled()) {
    // Offline: loopback with a non-zero MAC. RakNet's 360 build derives
    // "random" ids from abEnet and fails on zero (58411436).
    std::memset(static_cast<XNADDR*>(addr_ptr), 0, sizeof(XNADDR));
    addr_ptr->ina = htonl(INADDR_LOOPBACK);
    std::memset(addr_ptr->abEnet, 0xCC, 6);
    return XnAddrStatus::XNET_GET_XNADDR_STATIC;
  }
  systemlink::get_local_xnaddr(addr_ptr);
  return XnAddrStatus::XNET_GET_XNADDR_STATIC | XnAddrStatus::XNET_GET_XNADDR_GATEWAY |
         XnAddrStatus::XNET_GET_XNADDR_DNS;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  std::memset(static_cast<XNADDR*>(addr_ptr), 0, sizeof(XNADDR));

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetGetXnAddrPlatform_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr,
                                       mapped_u32 platform_ptr) {
  if (platform_ptr) {
    *platform_ptr = 1;  // XNET_XNADDR_PLATFORM_XBOX360
  }
  return 0;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr,
                                       mapped_u64 id_ptr) {
  if (!systemlink::enabled() || !addr_ptr) {
    // Tell the caller we're not signed in to live (non-zero ret)
    return 1;
  }
  // Stable per-peer id derived from its synthesized MAC.
  uint64_t id = 0xFA00000000000000ull;
  for (int i = 0; i < 6; ++i) {
    id |= uint64_t(addr_ptr->abEnet[i]) << (8 * (5 - i));
  }
  *id_ptr = id;
  return 0;
}

void NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                     u32 string_size) {
  rex::string::copy_truncating(string_out, guest_ip_string(in_addr), string_size);
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    mapped_u32 in_addr) {
  if (!systemlink::enabled() || !xn_addr || !in_addr) {
    return 1;
  }
  uint32_t ip = systemlink::is_local_xnaddr(*xn_addr) ? systemlink::local_ip()
                                                      : systemlink::ip_from_xnaddr(*xn_addr);
  *in_addr = ip;  // stored big-endian == network byte order
  REXKRNL_DEBUG("XNetXnAddrToInAddr -> {}", guest_ip_string(ip));
  return 0;
}

// Does the reverse of the above.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, u32 in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                    mapped_void xid) {
  if (!systemlink::enabled()) {
    return 1;
  }
  uint32_t ip = in_addr;
  if (ip == 0x7F000001u || ip == 0xFFFFFFFFu || ip == 0) {
    ip = systemlink::local_ip();
  }
  if (xn_addr) {
    systemlink::xnaddr_from_ip(ip, xn_addr);
  }
  if (xid) {
    const uint64_t session = systemlink::registered_session_id();
    memory::store_and_swap<uint64_t>(static_cast<uint8_t*>(static_cast<void*>(xid)), session);
  }
  return 0;
}

u32 NetDll_XNetUnregisterInAddr_entry(u32 caller, u32 in_addr) {
  return 0;
}

u32 NetDll_XNetConnect_entry(u32 caller, u32 in_addr) {
  return 0;
}

u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 in_addr) {
  // XNET_CONNECT_STATUS_CONNECTED: there is no key exchange to wait for.
  return 2;
}

u32 NetDll_XNetCreateKey_entry(u32 caller, ppc_ptr_t<XNKID> kid, ppc_ptr_t<XNKEY> key) {
  if (!kid || !key) {
    return uint32_t(X_WSA_ERROR::X_WSAEFAULT);
  }
  systemlink::create_key(kid, key);
  REXKRNL_INFO("XNetCreateKey -> {:016X}",
               memory::load_and_swap<uint64_t>(static_cast<XNKID*>(kid)->ab));
  return 0;
}

u32 NetDll_XNetRegisterKey_entry(u32 caller, ppc_ptr_t<XNKID> kid, ppc_ptr_t<XNKEY> key) {
  if (!kid) {
    return uint32_t(X_WSA_ERROR::X_WSAEFAULT);
  }
  const uint64_t id = memory::load_and_swap<uint64_t>(static_cast<XNKID*>(kid)->ab);
  systemlink::set_registered_session_id(id);
  REXKRNL_INFO("XNetRegisterKey {:016X}", id);
  return 0;
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, ppc_ptr_t<XNKID> kid) {
  if (kid) {
    const uint64_t id = memory::load_and_swap<uint64_t>(static_cast<XNKID*>(kid)->ab);
    if (systemlink::registered_session_id() == id) {
      systemlink::set_registered_session_id(0);
    }
    REXKRNL_INFO("XNetUnregisterKey {:016X}", id);
  }
  return 0;
}

u32 NetDll_XNetReplaceKey_entry(u32 caller, ppc_ptr_t<XNKID> kid_unregister,
                                ppc_ptr_t<XNKID> kid_register) {
  if (kid_register) {
    systemlink::set_registered_session_id(
        memory::load_and_swap<uint64_t>(static_cast<XNKID*>(kid_register)->ab));
  }
  return 0;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
  REXKRNL_INFO("XNetSetSystemLinkPort({})", port & 0xFFFF);
  return 0;
}

u32 NetDll_XNetGetSystemLinkPort_entry(u32 caller, mapped_u16 port_ptr) {
  if (port_ptr) {
    *port_ptr = systemlink::kSystemLinkPort;
  }
  return 0;
}

u32 NetDll_XNetGetBroadcastVersionStatus_entry(u32 caller, u32 reset) {
  return 0;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  if (!systemlink::enabled()) {
    return 0;
  }
  return XEthernetStatus::XNET_ETHERNET_LINK_ACTIVE | XEthernetStatus::XNET_ETHERNET_LINK_100MBPS |
         XEthernetStatus::XNET_ETHERNET_LINK_FULL_DUPLEX;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
  // TODO(gibbed): actually implement this
  if (pdns) {
    auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
    dns->status = 1;  // non-zero = error
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

u32 NetDll_XNetQosListen_entry(u32 caller, mapped_void id, mapped_void data, u32 data_size, u32 r7,
                               u32 flags) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  auto socket = object_ref<XSocket>(new XSocket(REX_KERNEL_STATE()));
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->ReleaseHandle();
    return socket_failed();
  }

  return socket->handle();
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }

  int ret = socket->Close();
  socket->ReleaseHandle();
  return ret < 0 ? socket_failed() : 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return -1;
  }

  int ret = socket->Shutdown(how);
  if (ret < 0) {
    socket_failed();
  }
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_u8 optval_ptr, u32 optlen) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  if (!optval_ptr && optlen) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAEFAULT));
    return ~0u;
  }
  return socket->SetOption(level, optname, optval_ptr, optlen) < 0 ? socket_failed() : 0;
}

u32 NetDll_getsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_u8 optval_ptr, mapped_u32 optlen_ptr) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  if (!optval_ptr || !optlen_ptr) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAEFAULT));
    return ~0u;
  }
  uint32_t len = *optlen_ptr;
  if (socket->GetOption(level, optname, optval_ptr, &len) < 0) {
    return socket_failed();
  }
  *optlen_ptr = len;
  return 0;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_u8 arg_ptr) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  return socket->IOControl(cmd, arg_ptr) < 0 ? socket_failed() : 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  if (!name || namelen < sizeof(XSOCKADDR_IN)) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAEFAULT));
    return ~0u;
  }
  return socket->Bind(name, namelen) < 0 ? socket_failed() : 0;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                         u32 namelen) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  if (!name || namelen < sizeof(XSOCKADDR_IN)) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAEFAULT));
    return ~0u;
  }
  REXKRNL_INFO("connect -> {}:{}", guest_ip_string(name->sin_addr), uint16_t(name->sin_port));
  return socket->Connect(name, namelen) < 0 ? socket_failed() : 0;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  return socket->Listen(backlog) < 0 ? socket_failed() : 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> addr_ptr,
                        mapped_u32 addrlen_ptr) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }

  uint32_t len = 0;
  auto new_socket = socket->Accept(addr_ptr, &len);
  if (!new_socket) {
    return socket_failed();
  }
  if (addrlen_ptr) {
    *addrlen_ptr = len;
  }
  if (addr_ptr) {
    REXKRNL_INFO("accept <- {}:{}", guest_ip_string(addr_ptr->sin_addr),
                 uint16_t(addr_ptr->sin_port));
  }
  return new_socket->handle();
}

u32 NetDll_getpeername_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> addr_ptr,
                             mapped_u32 addrlen_ptr) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  if (!addr_ptr || !addrlen_ptr) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAEFAULT));
    return ~0u;
  }
  uint32_t len = 0;
  if (socket->GetPeerName(addr_ptr, &len) < 0) {
    return socket_failed();
  }
  *addrlen_ptr = len;
  return 0;
}

u32 NetDll_getsockname_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> addr_ptr,
                             mapped_u32 addrlen_ptr) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  if (!addr_ptr || !addrlen_ptr) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAEFAULT));
    return ~0u;
  }
  uint32_t len = 0;
  if (socket->GetSockName(addr_ptr, &len) < 0) {
    return socket_failed();
  }
  *addrlen_ptr = len;
  return 0;
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    this->count = std::min<uint32_t>(guest_set->fd_count, 64);
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      if (!socket) {
        this->count = i;
        break;
      }
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  int MaxNative() const {
    int max_fd = -1;
    for (uint32_t i = 0; i < this->count; ++i) {
      max_fd = std::max(max_fd, static_cast<int>(this->sockets[i]->native_handle()));
    }
    return max_fd;
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  // Some titles pass the same set as read, write and except to poll for
  // data; treating it as writable would report ready forever.
  const bool use_writefds =
      writefds && writefds.guest_address() != readfds.guest_address();
  const bool use_exceptfds = exceptfds &&
                             exceptfds.guest_address() != readfds.guest_address() &&
                             exceptfds.guest_address() != writefds.guest_address();

  int max_fd = -1;
  host_set host_readfds = {};
  fd_set native_readfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
    max_fd = std::max(max_fd, host_readfds.MaxNative());
  }
  host_set host_writefds = {};
  fd_set native_writefds = {};
  if (use_writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
    max_fd = std::max(max_fd, host_writefds.MaxNative());
  }
  host_set host_exceptfds = {};
  fd_set native_exceptfds = {};
  if (use_exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
    max_fd = std::max(max_fd, host_exceptfds.MaxNative());
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    auto* t = static_cast<rex::be<int32_t>*>(static_cast<void*>(timeout_ptr));
    timeout = {static_cast<int32_t>(t[0]), static_cast<int32_t>(t[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  // nfds is ignored by Winsock; POSIX needs the highest native fd + 1.
  int ret = select(max_fd + 1, readfds ? &native_readfds : nullptr,
                   use_writefds ? &native_writefds : nullptr,
                   use_exceptfds ? &native_exceptfds : nullptr, timeout_in);
  if (ret < 0) {
    socket_failed();
  }
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (use_writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (use_exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  return ret;
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_u8 buf_ptr, u32 buf_len, u32 flags) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  int ret = socket->Recv(buf_ptr, buf_len, flags);
  return ret < 0 ? socket_failed() : static_cast<uint32_t>(ret);
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_u8 buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  XSOCKADDR_IN from = {};
  uint32_t from_len = sizeof(from);
  int ret = socket->RecvFrom(buf_ptr, buf_len, flags, &from, &from_len);
  if (ret < 0) {
    return socket_failed();
  }
  if (from_ptr) {
    *static_cast<XSOCKADDR_IN*>(from_ptr) = from;
  }
  if (fromlen_ptr) {
    *fromlen_ptr = from_len;
  }
  return static_cast<uint32_t>(ret);
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_u8 buf_ptr, u32 buf_len, u32 flags) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  int ret = socket->Send(buf_ptr, buf_len, flags);
  return ret < 0 ? socket_failed() : static_cast<uint32_t>(ret);
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_u8 buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = lookup_socket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  int ret = socket->SendTo(buf_ptr, buf_len, flags, to_ptr ? &*to_ptr : nullptr, to_len);
  return ret < 0 ? socket_failed() : static_cast<uint32_t>(ret);
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

#define NET_EXPORT(name) REX_EXPORT(__imp__##name, rex::kernel::xam::name##_entry)

NET_EXPORT(NetDll_XNetStartup)
NET_EXPORT(NetDll_XNetStartupEx)
NET_EXPORT(NetDll_XNetCleanup)
NET_EXPORT(NetDll_XNetGetOpt)
NET_EXPORT(NetDll_XNetSetOpt)
NET_EXPORT(NetDll_XNetRandom)
NET_EXPORT(NetDll_WSAStartup)
NET_EXPORT(NetDll_WSAStartupEx)
NET_EXPORT(NetDll_WSACleanup)
NET_EXPORT(NetDll_WSAGetLastError)
NET_EXPORT(NetDll_WSARecvFrom)
NET_EXPORT(NetDll_WSARecv)
NET_EXPORT(NetDll_WSASendTo)
NET_EXPORT(NetDll_WSASend)
NET_EXPORT(NetDll_WSAGetOverlappedResult)
NET_EXPORT(NetDll_WSACancelOverlappedIO)
NET_EXPORT(NetDll_WSAWaitForMultipleEvents)
NET_EXPORT(NetDll_WSACreateEvent)
NET_EXPORT(NetDll_WSACloseEvent)
NET_EXPORT(NetDll_WSAResetEvent)
NET_EXPORT(NetDll_WSASetEvent)
NET_EXPORT(NetDll_XNetGetTitleXnAddr)
NET_EXPORT(NetDll_XNetGetDebugXnAddr)
NET_EXPORT(NetDll_XNetGetXnAddrPlatform)
NET_EXPORT(NetDll_XNetXnAddrToMachineId)
NET_EXPORT(NetDll_XNetInAddrToString)
NET_EXPORT(NetDll_XNetXnAddrToInAddr)
NET_EXPORT(NetDll_XNetInAddrToXnAddr)
NET_EXPORT(NetDll_XNetUnregisterInAddr)
NET_EXPORT(NetDll_XNetConnect)
NET_EXPORT(NetDll_XNetGetConnectStatus)
NET_EXPORT(NetDll_XNetCreateKey)
NET_EXPORT(NetDll_XNetRegisterKey)
NET_EXPORT(NetDll_XNetUnregisterKey)
NET_EXPORT(NetDll_XNetReplaceKey)
NET_EXPORT(NetDll_XNetSetSystemLinkPort)
NET_EXPORT(NetDll_XNetGetSystemLinkPort)
NET_EXPORT(NetDll_XNetGetBroadcastVersionStatus)
NET_EXPORT(NetDll_XNetGetEthernetLinkStatus)
NET_EXPORT(NetDll_XNetDnsLookup)
NET_EXPORT(NetDll_XNetDnsRelease)
NET_EXPORT(NetDll_XNetQosServiceLookup)
NET_EXPORT(NetDll_XNetQosRelease)
NET_EXPORT(NetDll_XNetQosListen)
NET_EXPORT(NetDll_inet_addr)
NET_EXPORT(NetDll_socket)
NET_EXPORT(NetDll_closesocket)
NET_EXPORT(NetDll_shutdown)
NET_EXPORT(NetDll_setsockopt)
NET_EXPORT(NetDll_getsockopt)
NET_EXPORT(NetDll_ioctlsocket)
NET_EXPORT(NetDll_bind)
NET_EXPORT(NetDll_connect)
NET_EXPORT(NetDll_listen)
NET_EXPORT(NetDll_accept)
NET_EXPORT(NetDll_getpeername)
NET_EXPORT(NetDll_getsockname)
NET_EXPORT(NetDll_select)
NET_EXPORT(NetDll_recv)
NET_EXPORT(NetDll_recvfrom)
NET_EXPORT(NetDll_send)
NET_EXPORT(NetDll_sendto)
NET_EXPORT(NetDll___WSAFDIsSet)
NET_EXPORT(NetDll_WSASetLastError)

REX_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_EXPORT_STUB(__imp__NetDll_UpnpStartup);
REX_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
REX_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
REX_EXPORT_STUB(__imp__NetDll_XNetQosLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetServerToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
