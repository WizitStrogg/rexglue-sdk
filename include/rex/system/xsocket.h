#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    2026 - Overlapped WSASendTo/WSARecvFrom, option/ioctl
 *              translation and System Link broadcast handling, adapted from
 *              the Xenia Canary netplay branch (AdrianCassar/xenia-canary).
 */

#include <atomic>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <set>

#include <rex/math.h>
#include <rex/system/xobject.h>
#include <rex/types.h>

namespace rex::system {

enum class X_WSA_ERROR : uint32_t {
  X_WSA_NO_ERROR = 0,
  X_WSA_INVALID_PARAMETER = 87,
  X_WSA_OPERATION_ABORTED = 995,
  X_WSA_IO_INCOMPLETE = 996,
  X_WSA_IO_PENDING = 997,
  X_WSAEINTR = 10004,
  X_WSAEBADF = 10009,
  X_WSAEACCES = 10013,
  X_WSAEFAULT = 10014,
  X_WSAEINVAL = 10022,
  X_WSAEMFILE = 10024,
  X_WSAEWOULDBLOCK = 10035,
  X_WSAEINPROGRESS = 10036,
  X_WSAEALREADY = 10037,
  X_WSAENOTSOCK = 10038,
  X_WSAEDESTADDRREQ = 10039,
  X_WSAEMSGSIZE = 10040,
  X_WSAEPROTOTYPE = 10041,
  X_WSAENOPROTOOPT = 10042,
  X_WSAEPROTONOSUPPORT = 10043,
  X_WSAESOCKTNOSUPPORT = 10044,
  X_WSAEOPNOTSUPP = 10045,
  X_WSAEPFNOSUPPORT = 10046,
  X_WSAEAFNOSUPPORT = 10047,
  X_WSAEADDRINUSE = 10048,
  X_WSAEADDRNOTAVAIL = 10049,
  X_WSAENETDOWN = 10050,
  X_WSAENETUNREACH = 10051,
  X_WSAENETRESET = 10052,
  X_WSAECONNABORTED = 10053,
  X_WSAECONNRESET = 10054,
  X_WSAENOBUFS = 10055,
  X_WSAEISCONN = 10056,
  X_WSAENOTCONN = 10057,
  X_WSAESHUTDOWN = 10058,
  X_WSAETIMEDOUT = 10060,
  X_WSAECONNREFUSED = 10061,
  X_WSAEHOSTUNREACH = 10065,
  X_WSASYSNOTREADY = 10091,
  X_WSAVERNOTSUPPORTED = 10092,
  X_WSANOTINITIALISED = 10093,
  X_WSAECANCELLED = 10103,
  X_WSASYSCALLFAILURE = 10107,
};

// Xbox-only socket options.
constexpr uint32_t X_SO_MARKINSECURE = 0x5801;
constexpr uint32_t X_SO_PRIVATE = 0x5802;
constexpr uint32_t X_SO_GRANTINSECURE = 0x5803;

struct XSOCKADDR {
  rex::be<uint16_t> address_family;
  char sa_data[14];
};
static_assert(sizeof(XSOCKADDR) == 0x10, "XSOCKADDR size");

// Guest sockaddr_in. Port and address are big-endian, i.e. network byte
// order, exactly like the host's sockaddr_in fields.
struct XSOCKADDR_IN {
  rex::be<uint16_t> sin_family;
  rex::be<uint16_t> sin_port;  // host-order value (byte-swapped by be<>)
  rex::be<uint32_t> sin_addr;  // host-order value (byte-swapped by be<>)
  char x_sin_zero[8];
};
static_assert(sizeof(XSOCKADDR_IN) == 0x10, "XSOCKADDR_IN size");

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};
static_assert(sizeof(XWSABUF) == 0x8, "XWSABUF size");

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;       // status
  rex::be<uint32_t> internal_high;  // bytes transferred
  rex::be<uint32_t> offset;         // flags
  rex::be<uint32_t> offset_high;
  rex::be<uint32_t> event_handle;
};
static_assert(sizeof(XWSAOVERLAPPED) == 0x14, "XWSAOVERLAPPED size");

class XSocket : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Socket;

  enum AddressFamily {
    X_AF_INET = 2,
  };

  enum Type {
    X_SOCK_STREAM = 1,
    X_SOCK_DGRAM = 2,
  };

  enum Protocol {
    X_IPPROTO_TCP = 6,
    X_IPPROTO_UDP = 17,

    // LIVE Voice and Data Protocol
    // Format: [cbGameData][GameData(encrypted)][VoiceData(unencrypted)]
    // Every peer here is another ReXGlue instance, so VDP is carried as plain
    // UDP with the payload passed through unchanged.
    X_IPPROTO_VDP = 254,
  };

  XSocket(KernelState* kernel_state);
  ~XSocket();

  uint64_t native_handle() const { return native_handle_; }
  uint16_t bound_port() const { return bound_port_; }
  bool is_vdp() const { return vdp_; }
  bool is_tcp() const { return proto_ == X_IPPROTO_TCP; }

  X_STATUS Initialize(AddressFamily af, Type type, Protocol proto);
  int Close();

  int GetOption(uint32_t level, uint32_t optname, uint8_t* optval_ptr, uint32_t* optlen);
  int SetOption(uint32_t level, uint32_t optname, const uint8_t* optval_ptr, uint32_t optlen);
  int IOControl(uint32_t cmd, uint8_t* arg_ptr);

  int Connect(const XSOCKADDR_IN* name, int name_len);
  int Bind(const XSOCKADDR_IN* name, int name_len);
  int Listen(int backlog);
  object_ref<XSocket> Accept(XSOCKADDR_IN* name, uint32_t* name_len);
  int Shutdown(int how);
  int GetPeerName(XSOCKADDR_IN* name, uint32_t* name_len);
  int GetSockName(XSOCKADDR_IN* name, uint32_t* name_len);

  int Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags);
  int Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags);
  int RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, XSOCKADDR_IN* from,
               uint32_t* from_len);
  int SendTo(const uint8_t* buf, uint32_t buf_len, uint32_t flags, const XSOCKADDR_IN* to,
             uint32_t to_len);

  int WSASendTo(const XWSABUF* buffers, uint32_t num_buffers, rex::be<uint32_t>* num_bytes_sent,
                uint32_t flags, const XSOCKADDR_IN* to, uint32_t to_len,
                XWSAOVERLAPPED* overlapped);
  int WSARecvFrom(const XWSABUF* buffers, uint32_t num_buffers,
                  rex::be<uint32_t>* num_bytes_recv, rex::be<uint32_t>* flags_ptr,
                  XSOCKADDR_IN* from, rex::be<uint32_t>* from_len, XWSAOVERLAPPED* overlapped);
  bool WSAGetOverlappedResult(XWSAOVERLAPPED* overlapped, rex::be<uint32_t>* bytes_transferred,
                              bool wait, rex::be<uint32_t>* flags_ptr);
  int WSACancelOverlappedIO();

  // Last host socket error, translated to the guest's WSA error space.
  static uint32_t LastError();
  static void SetLastError(X_WSA_ERROR error);

 private:
  XSocket(KernelState* kernel_state, uint64_t native_handle);

  struct SendRequest {
    std::vector<XWSABUF> buffers;
    bool has_to = false;
    XSOCKADDR_IN to = {};
    uint32_t to_len = 0;
    XWSAOVERLAPPED* overlapped = nullptr;
  };

  struct RecvRequest {
    XWSABUF buffer = {};
    rex::be<uint32_t>* num_bytes_recv = nullptr;
    rex::be<uint32_t>* flags = nullptr;
    XSOCKADDR_IN* from = nullptr;
    rex::be<uint32_t>* from_len = nullptr;
    XWSAOVERLAPPED* overlapped = nullptr;
  };

  // >0 ready, 0 not ready, <0 error/cancelled.
  int SelectReady(bool for_write, bool wait);
  int DoSend(const SendRequest& request, bool wait, uint32_t* bytes_sent);
  int DoRecv(const RecvRequest& request, bool wait);
  void CompleteOverlapped(XWSAOVERLAPPED* overlapped, uint32_t status, uint32_t bytes,
                          uint32_t flags);
  void NoteImplicitBind();

  // Rewrites a guest destination for the host network: limited broadcast is
  // sent to the selected adapter's directed broadcast, loopback to self.
  void TranslateDestination(const XSOCKADDR_IN& guest, void* host_sockaddr_in) const;

  uint64_t native_handle_ = ~0ull;
  bool closed_ = false;

  AddressFamily af_ = X_AF_INET;
  Type type_ = X_SOCK_DGRAM;
  Protocol proto_ = X_IPPROTO_UDP;
  bool vdp_ = false;
  bool secure_ = true;
  bool broadcast_ = false;
  bool non_blocking_ = false;

  uint16_t bound_port_ = 0;  // host byte order

  std::mutex send_mutex_;
  std::mutex recv_mutex_;
  std::map<XWSAOVERLAPPED*, std::future<int>> send_tasks_;
  std::map<XWSAOVERLAPPED*, std::future<int>> recv_tasks_;
  std::atomic<bool> cancel_ = false;
};

}  // namespace rex::system
