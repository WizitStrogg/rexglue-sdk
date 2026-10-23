/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    2026 - Rewritten around the Xenia Canary netplay XSocket
 *              (AdrianCassar/xenia-canary, BSD): guest/host byte order,
 *              option and ioctl translation, overlapped WSASendTo /
 *              WSARecvFrom, and System Link broadcast routing.
 */

#include <rex/system/xsocket.h>

#include <chrono>
#include <cstring>
#include <vector>

#include <rex/kernel/xam/systemlink.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace rex::system {

namespace {

namespace systemlink = rex::kernel::xam::systemlink;

constexpr uint64_t kInvalidSocket = ~0ull;

#if REX_PLATFORM_WIN32
using native_socket_t = SOCKET;
#else
using native_socket_t = int;
#endif

native_socket_t to_native(uint64_t handle) {
  return static_cast<native_socket_t>(handle);
}

// Guest (Xbox, Winsock-compatible) socket option ids.
constexpr uint32_t X_SOL_SOCKET = 0xFFFF;
constexpr uint32_t X_IPPROTO_TCP_LEVEL = 6;
constexpr uint32_t X_SO_REUSEADDR = 0x0004;
constexpr uint32_t X_SO_KEEPALIVE = 0x0008;
constexpr uint32_t X_SO_BROADCAST = 0x0020;
constexpr uint32_t X_SO_LINGER = 0x0080;
[[maybe_unused]] constexpr uint32_t X_SO_DONTLINGER = ~0x0080u;
[[maybe_unused]] constexpr uint32_t X_SO_EXCLUSIVEADDRUSE = ~0x0004u;
constexpr uint32_t X_SO_SNDBUF = 0x1001;
constexpr uint32_t X_SO_RCVBUF = 0x1002;
constexpr uint32_t X_SO_SNDTIMEO = 0x1005;
constexpr uint32_t X_SO_RCVTIMEO = 0x1006;
constexpr uint32_t X_SO_ERROR = 0x1007;
constexpr uint32_t X_SO_TYPE = 0x1008;
constexpr uint32_t X_TCP_NODELAY = 0x0001;

constexpr uint32_t X_FIONBIO = 0x8004667E;
constexpr uint32_t X_FIONREAD = 0x4004667F;

bool translate_option(uint32_t level, uint32_t optname, int* native_level, int* native_name) {
  if (level == X_SOL_SOCKET) {
    *native_level = SOL_SOCKET;
    switch (optname) {
      case X_SO_REUSEADDR:
        *native_name = SO_REUSEADDR;
        return true;
      case X_SO_KEEPALIVE:
        *native_name = SO_KEEPALIVE;
        return true;
      case X_SO_BROADCAST:
        *native_name = SO_BROADCAST;
        return true;
      case X_SO_LINGER:
        *native_name = SO_LINGER;
        return true;
      case X_SO_SNDBUF:
        *native_name = SO_SNDBUF;
        return true;
      case X_SO_RCVBUF:
        *native_name = SO_RCVBUF;
        return true;
      case X_SO_SNDTIMEO:
        *native_name = SO_SNDTIMEO;
        return true;
      case X_SO_RCVTIMEO:
        *native_name = SO_RCVTIMEO;
        return true;
      case X_SO_ERROR:
        *native_name = SO_ERROR;
        return true;
      case X_SO_TYPE:
        *native_name = SO_TYPE;
        return true;
#if REX_PLATFORM_WIN32
      case X_SO_DONTLINGER:
        *native_name = SO_DONTLINGER;
        return true;
      case X_SO_EXCLUSIVEADDRUSE:
        *native_name = SO_EXCLUSIVEADDRUSE;
        return true;
#endif
    }
    return false;
  }
  if (level == X_IPPROTO_TCP_LEVEL) {
    *native_level = IPPROTO_TCP;
    if (optname == X_TCP_NODELAY) {
      *native_name = TCP_NODELAY;
      return true;
    }
  }
  return false;
}

// Guest option values are big-endian. Convert one in place for the host.
void swap_option_value(uint32_t optname, uint8_t* value, uint32_t length) {
  if (optname == X_SO_LINGER && length >= 4) {
    // struct linger { u_short l_onoff; u_short l_linger; }
    std::swap(value[0], value[1]);
    std::swap(value[2], value[3]);
    return;
  }
  if (length == 4) {
    std::swap(value[0], value[3]);
    std::swap(value[1], value[2]);
  } else if (length == 2) {
    std::swap(value[0], value[1]);
  }
}

void to_host_sockaddr(const XSOCKADDR_IN& guest, sockaddr_in* host) {
  std::memset(host, 0, sizeof(*host));
  host->sin_family = AF_INET;
  host->sin_port = htons(guest.sin_port);
  host->sin_addr.s_addr = htonl(guest.sin_addr);
}

void to_guest_sockaddr(const sockaddr_in& host, XSOCKADDR_IN* guest) {
  guest->sin_family = host.sin_family == AF_INET ? XSocket::X_AF_INET : host.sin_family;
  guest->sin_port = ntohs(host.sin_port);
  guest->sin_addr = ntohl(host.sin_addr.s_addr);
  std::memset(guest->x_sin_zero, 0, sizeof(guest->x_sin_zero));
}

// POSIX: guest-specific errors set through XSocket::SetLastError are parked
// in errno offset by this base so they survive until read back.
constexpr int kGuestErrnoBase = 100000;

uint32_t translate_error(int error) {
  if (error >= kGuestErrnoBase) {
    return static_cast<uint32_t>(error - kGuestErrnoBase);
  }
#if REX_PLATFORM_WIN32
  // Winsock error numbers are the guest's error numbers.
  return static_cast<uint32_t>(error);
#else
  switch (error) {
    case 0:
      return 0;
    case EWOULDBLOCK:
      return uint32_t(X_WSA_ERROR::X_WSAEWOULDBLOCK);
    case EINPROGRESS:
      return uint32_t(X_WSA_ERROR::X_WSAEINPROGRESS);
    case EALREADY:
      return uint32_t(X_WSA_ERROR::X_WSAEALREADY);
    case ENOTSOCK:
      return uint32_t(X_WSA_ERROR::X_WSAENOTSOCK);
    case EMSGSIZE:
      return uint32_t(X_WSA_ERROR::X_WSAEMSGSIZE);
    case EADDRINUSE:
      return uint32_t(X_WSA_ERROR::X_WSAEADDRINUSE);
    case EADDRNOTAVAIL:
      return uint32_t(X_WSA_ERROR::X_WSAEADDRNOTAVAIL);
    case ENETDOWN:
      return uint32_t(X_WSA_ERROR::X_WSAENETDOWN);
    case ENETUNREACH:
      return uint32_t(X_WSA_ERROR::X_WSAENETUNREACH);
    case ECONNABORTED:
      return uint32_t(X_WSA_ERROR::X_WSAECONNABORTED);
    case ECONNRESET:
      return uint32_t(X_WSA_ERROR::X_WSAECONNRESET);
    case ENOBUFS:
      return uint32_t(X_WSA_ERROR::X_WSAENOBUFS);
    case EISCONN:
      return uint32_t(X_WSA_ERROR::X_WSAEISCONN);
    case ENOTCONN:
      return uint32_t(X_WSA_ERROR::X_WSAENOTCONN);
    case ETIMEDOUT:
      return uint32_t(X_WSA_ERROR::X_WSAETIMEDOUT);
    case ECONNREFUSED:
      return uint32_t(X_WSA_ERROR::X_WSAECONNREFUSED);
    case EHOSTUNREACH:
      return uint32_t(X_WSA_ERROR::X_WSAEHOSTUNREACH);
    case EACCES:
      return uint32_t(X_WSA_ERROR::X_WSAEACCES);
    case EFAULT:
      return uint32_t(X_WSA_ERROR::X_WSAEFAULT);
    case EINVAL:
      return uint32_t(X_WSA_ERROR::X_WSAEINVAL);
    case EINTR:
      return uint32_t(X_WSA_ERROR::X_WSAEINTR);
    default:
      return uint32_t(X_WSA_ERROR::X_WSASYSCALLFAILURE);
  }
#endif
}

}  // namespace

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  if (!closed_) {
    Close();
  }
}

uint32_t XSocket::LastError() {
#if REX_PLATFORM_WIN32
  return translate_error(WSAGetLastError());
#else
  return translate_error(errno);
#endif
}

void XSocket::SetLastError(X_WSA_ERROR error) {
#if REX_PLATFORM_WIN32
  WSASetLastError(static_cast<int>(error));
#else
  errno = kGuestErrnoBase + static_cast<int>(error);
#endif
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;
  vdp_ = false;

  if (!type) {
    type_ = (proto == X_IPPROTO_TCP) ? X_SOCK_STREAM : X_SOCK_DGRAM;
  }
  if (!proto) {
    proto_ = type_ == X_SOCK_STREAM ? X_IPPROTO_TCP : X_IPPROTO_UDP;
  } else if (proto == X_IPPROTO_VDP) {
    proto_ = X_IPPROTO_UDP;
    vdp_ = true;
  }

  const int native_type = type_ == X_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM;
  const int native_proto = proto_ == X_IPPROTO_TCP ? IPPROTO_TCP : IPPROTO_UDP;
  native_socket_t s = ::socket(AF_INET, native_type, native_proto);
#if REX_PLATFORM_WIN32
  if (s == INVALID_SOCKET) {
    return X_STATUS_UNSUCCESSFUL;
  }
#else
  if (s < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
#endif
  native_handle_ = static_cast<uint64_t>(s);

#if REX_PLATFORM_WIN32
  if (native_type == SOCK_DGRAM) {
    // Don't let an ICMP port-unreachable from a peer that left surface as
    // WSAECONNRESET on the next recvfrom (the console never reports it).
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &report, sizeof(report),
             nullptr, 0, &returned, nullptr, nullptr);
  }
#endif

  REXKRNL_DEBUG("XSocket: created {} socket{}", is_tcp() ? "TCP" : "UDP", vdp_ ? " (VDP)" : "");
  return X_STATUS_SUCCESS;
}

int XSocket::Close() {
  WSACancelOverlappedIO();
  if (closed_ || native_handle_ == kInvalidSocket) {
    closed_ = true;
    return 0;
  }
#if REX_PLATFORM_WIN32
  int ret = ::closesocket(to_native(native_handle_));
#else
  int ret = ::close(to_native(native_handle_));
#endif
  if (ret == 0) {
    closed_ = true;
  }
  return ret;
}

int XSocket::GetOption(uint32_t level, uint32_t optname, uint8_t* optval_ptr, uint32_t* optlen) {
  int native_level = 0;
  int native_name = 0;
  if (!translate_option(level, optname, &native_level, &native_name)) {
    REXKRNL_WARN("XSocket::GetOption: unsupported option {:04X}/{:04X}", level, optname);
    SetLastError(X_WSA_ERROR::X_WSAENOPROTOOPT);
    return -1;
  }
  socklen_t len = *optlen;
  int ret = ::getsockopt(to_native(native_handle_), native_level, native_name,
                         reinterpret_cast<char*>(optval_ptr), &len);
  if (ret < 0) {
    return -1;
  }
  *optlen = static_cast<uint32_t>(len);
  swap_option_value(optname, optval_ptr, *optlen);
  return 0;
}

int XSocket::SetOption(uint32_t level, uint32_t optname, const uint8_t* optval_ptr,
                       uint32_t optlen) {
  if (level == X_SOL_SOCKET &&
      (optname == X_SO_MARKINSECURE || optname == X_SO_PRIVATE ||
       optname == X_SO_GRANTINSECURE)) {
    // XNet security is not emulated; peers talk plain UDP/TCP.
    secure_ = false;
    return 0;
  }

  int native_level = 0;
  int native_name = 0;
  if (!translate_option(level, optname, &native_level, &native_name)) {
    REXKRNL_WARN("XSocket::SetOption: ignoring unsupported option {:04X}/{:04X}", level,
                 optname);
    return 0;
  }

  std::vector<uint8_t> value(optval_ptr, optval_ptr + optlen);
  swap_option_value(optname, value.data(), optlen);

#if !REX_PLATFORM_WIN32
  // Winsock timeouts are DWORD milliseconds; POSIX wants a timeval.
  if (native_name == SO_SNDTIMEO || native_name == SO_RCVTIMEO) {
    uint32_t ms = 0;
    std::memcpy(&ms, value.data(), std::min<size_t>(sizeof(ms), value.size()));
    timeval tv{static_cast<time_t>(ms / 1000), static_cast<suseconds_t>((ms % 1000) * 1000)};
    return ::setsockopt(to_native(native_handle_), native_level, native_name, &tv, sizeof(tv));
  }
  // A one-byte BOOL is legal on Winsock but not POSIX.
  if (value.size() == 1) {
    int v = value[0];
    return ::setsockopt(to_native(native_handle_), native_level, native_name, &v, sizeof(v));
  }
#endif

  int ret = ::setsockopt(to_native(native_handle_), native_level, native_name,
                         reinterpret_cast<const char*>(value.data()), optlen);
  if (ret < 0) {
    REXKRNL_WARN("XSocket::SetOption: {:04X}/{:04X} failed ({})", level, optname, LastError());
    return -1;
  }
  if (level == X_SOL_SOCKET && optname == X_SO_BROADCAST) {
    broadcast_ = optlen && value[0] | (optlen > 1 ? value[1] : 0) |
                               (optlen > 3 ? value[2] | value[3] : 0);
  }
  return 0;
}

int XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  auto* arg = reinterpret_cast<rex::be<uint32_t>*>(arg_ptr);
  switch (cmd) {
    case X_FIONBIO: {
      const bool enable = *arg != 0;
#if REX_PLATFORM_WIN32
      u_long param = enable ? 1 : 0;
      int ret = ::ioctlsocket(to_native(native_handle_), FIONBIO, &param);
#else
      int flags = ::fcntl(to_native(native_handle_), F_GETFL, 0);
      int ret = ::fcntl(to_native(native_handle_), F_SETFL,
                        enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
      if (ret == 0) {
        non_blocking_ = enable;
      }
      return ret < 0 ? -1 : 0;
    }
    case X_FIONREAD: {
#if REX_PLATFORM_WIN32
      u_long param = 0;
      int ret = ::ioctlsocket(to_native(native_handle_), FIONREAD, &param);
#else
      int param = 0;
      int ret = ::ioctl(to_native(native_handle_), FIONREAD, &param);
#endif
      if (ret < 0) {
        return -1;
      }
      *arg = static_cast<uint32_t>(param);
      return 0;
    }
  }
  REXKRNL_WARN("XSocket::IOControl: unsupported command {:08X}", cmd);
  SetLastError(X_WSA_ERROR::X_WSAEINVAL);
  return -1;
}

void XSocket::TranslateDestination(const XSOCKADDR_IN& guest, void* host_ptr) const {
  auto* host = static_cast<sockaddr_in*>(host_ptr);
  to_host_sockaddr(guest, host);
  if (!systemlink::enabled()) {
    return;
  }
  const uint32_t ip = guest.sin_addr;
  if (ip == 0xFFFFFFFFu) {
    // Limited broadcast only leaves through the default route on Windows.
    // Direct it at the selected adapter's subnet instead.
    host->sin_addr.s_addr = htonl(systemlink::broadcast_ip());
  } else if (ip == 0) {
    host->sin_addr.s_addr = htonl(systemlink::local_ip());
  }
}

void XSocket::NoteImplicitBind() {
  if (bound_port_) {
    return;
  }
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(to_native(native_handle_), reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
    bound_port_ = ntohs(addr.sin_port);
  }
}

int XSocket::Connect(const XSOCKADDR_IN* name, int /*name_len*/) {
  sockaddr_in addr;
  TranslateDestination(*name, &addr);
  int ret = ::connect(to_native(native_handle_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  NoteImplicitBind();
  return ret < 0 ? -1 : 0;
}

int XSocket::Bind(const XSOCKADDR_IN* name, int /*name_len*/) {
  sockaddr_in addr;
  to_host_sockaddr(*name, &addr);
  // Titles bind to INADDR_ANY or to their own XNADDR address. Anything
  // else (e.g. a stale address from another PC) would fail on the host.
  const uint32_t ip = name->sin_addr;
  if (ip != 0 && ip != systemlink::local_ip() && ip != 0x7F000001u) {
    REXKRNL_WARN("XSocket::Bind: guest asked for {}, binding to any address instead",
                 systemlink::ip_to_string(ip));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  }
  int ret = ::bind(to_native(native_handle_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (ret < 0) {
    REXKRNL_WARN("XSocket::Bind: port {} failed ({})", uint16_t(name->sin_port), LastError());
    return -1;
  }
  bound_port_ = name->sin_port;
  NoteImplicitBind();
  REXKRNL_INFO("XSocket: bound {} port {}", is_tcp() ? "TCP" : (vdp_ ? "VDP" : "UDP"),
               bound_port_);
  return 0;
}

int XSocket::Listen(int backlog) {
  return ::listen(to_native(native_handle_), backlog) < 0 ? -1 : 0;
}

object_ref<XSocket> XSocket::Accept(XSOCKADDR_IN* name, uint32_t* name_len) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  native_socket_t s = ::accept(to_native(native_handle_), reinterpret_cast<sockaddr*>(&addr), &len);
#if REX_PLATFORM_WIN32
  if (s == INVALID_SOCKET) {
    return nullptr;
  }
#else
  if (s < 0) {
    return nullptr;
  }
#endif
  if (name) {
    to_guest_sockaddr(addr, name);
  }
  if (name_len) {
    *name_len = sizeof(XSOCKADDR_IN);
  }
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, static_cast<uint64_t>(s)));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;
  socket->vdp_ = vdp_;
  socket->NoteImplicitBind();
  return socket;
}

int XSocket::Shutdown(int how) {
  return ::shutdown(to_native(native_handle_), how);
}

int XSocket::GetPeerName(XSOCKADDR_IN* name, uint32_t* name_len) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getpeername(to_native(native_handle_), reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    return -1;
  }
  to_guest_sockaddr(addr, name);
  *name_len = sizeof(XSOCKADDR_IN);
  return 0;
}

int XSocket::GetSockName(XSOCKADDR_IN* name, uint32_t* name_len) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(to_native(native_handle_), reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    return -1;
  }
  to_guest_sockaddr(addr, name);
  // Report the System Link address rather than 0.0.0.0, as XNet would.
  if (!addr.sin_addr.s_addr && systemlink::enabled()) {
    name->sin_addr = systemlink::local_ip();
  }
  *name_len = sizeof(XSOCKADDR_IN);
  return 0;
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return ::recv(to_native(native_handle_), reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  int ret = ::send(to_native(native_handle_), reinterpret_cast<const char*>(buf), buf_len, flags);
  NoteImplicitBind();
  return ret;
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  int ret = ::recvfrom(to_native(native_handle_), reinterpret_cast<char*>(buf), buf_len, flags,
                       reinterpret_cast<sockaddr*>(&addr), &len);
  if (ret >= 0) {
    if (is_tcp()) {
      // The console reports the peer even for stream sockets.
      len = sizeof(addr);
      ::getpeername(to_native(native_handle_), reinterpret_cast<sockaddr*>(&addr), &len);
    }
    if (from) {
      to_guest_sockaddr(addr, from);
    }
    if (from_len) {
      *from_len = sizeof(XSOCKADDR_IN);
    }
  }
  return ret;
}

int XSocket::SendTo(const uint8_t* buf, uint32_t buf_len, uint32_t flags, const XSOCKADDR_IN* to,
                    uint32_t /*to_len*/) {
  int ret;
  if (to) {
    sockaddr_in addr;
    TranslateDestination(*to, &addr);
    ret = ::sendto(to_native(native_handle_), reinterpret_cast<const char*>(buf), buf_len, flags,
                   reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  } else {
    ret = ::sendto(to_native(native_handle_), reinterpret_cast<const char*>(buf), buf_len, flags,
                   nullptr, 0);
  }
  NoteImplicitBind();
  return ret;
}

//-----------------------------------------------------------------------------
// Overlapped I/O
//
// Like Xenia, completion is emulated with a host task per pending request
// that polls the socket and then completes the guest XWSAOVERLAPPED (status,
// byte count, flags) and signals its event.
//-----------------------------------------------------------------------------

int XSocket::SelectReady(bool for_write, bool wait) {
  using namespace std::chrono_literals;
  while (true) {
    if (cancel_) {
      return -1;
    }
    fd_set set;
    FD_ZERO(&set);
    native_socket_t s = to_native(native_handle_);
    FD_SET(s, &set);
    timeval tv{0, wait ? 50 * 1000 : 0};
#if REX_PLATFORM_WIN32
    int nfds = 0;
#else
    int nfds = s + 1;
#endif
    int ret = ::select(nfds, for_write ? nullptr : &set, for_write ? &set : nullptr, nullptr, &tv);
    if (ret != 0 || !wait) {
      return ret;
    }
  }
}

void XSocket::CompleteOverlapped(XWSAOVERLAPPED* overlapped, uint32_t status, uint32_t bytes,
                                 uint32_t flags) {
  overlapped->internal_high = bytes;
  overlapped->offset = flags;
  overlapped->internal = status;
  if (overlapped->event_handle) {
    rex::kernel::xboxkrnl::xeNtSetEvent(overlapped->event_handle, nullptr);
  }
}

int XSocket::DoSend(const SendRequest& request, bool wait, uint32_t* bytes_sent) {
  if (request.overlapped) {
    int ready = SelectReady(true, wait);
    if (ready < 0) {
      const uint32_t error = cancel_ ? uint32_t(X_WSA_ERROR::X_WSAECANCELLED) : LastError();
      CompleteOverlapped(request.overlapped, error, 0, 0);
      return -1;
    }
    if (ready == 0) {
      request.overlapped->internal = X_STATUS_PENDING;
      return -1;
    }
  }

  // Gather the guest buffers. VDP sockets pass [cbGameData][game][voice];
  // the peer is another ReXGlue instance, so the datagram is sent verbatim.
  auto* memory = kernel_state()->memory();
  std::vector<uint8_t> data;
  for (const auto& b : request.buffers) {
    const uint8_t* src = memory->TranslateVirtual<const uint8_t*>(b.buf_ptr);
    data.insert(data.end(), src, src + uint32_t(b.len));
  }

  int ret = request.has_to ? SendTo(data.data(), uint32_t(data.size()), 0, &request.to,
                                    request.to_len)
                           : Send(data.data(), uint32_t(data.size()), 0);
  if (ret < 0) {
    const uint32_t error = LastError();
    if (request.overlapped) {
      if (error == uint32_t(X_WSA_ERROR::X_WSAEWOULDBLOCK)) {
        request.overlapped->internal = X_STATUS_PENDING;
      } else {
        CompleteOverlapped(request.overlapped, error, 0, 0);
      }
    }
    return -1;
  }
  if (bytes_sent) {
    *bytes_sent = static_cast<uint32_t>(ret);
  }
  if (request.overlapped) {
    CompleteOverlapped(request.overlapped, 0, static_cast<uint32_t>(ret), 0);
  }
  return 0;
}

int XSocket::DoRecv(const RecvRequest& request, bool wait) {
  if (request.overlapped) {
    int ready = SelectReady(false, wait);
    if (ready < 0) {
      const uint32_t error = cancel_ ? uint32_t(X_WSA_ERROR::X_WSAECANCELLED) : LastError();
      CompleteOverlapped(request.overlapped, error, 0, 0);
      return -1;
    }
    if (ready == 0) {
      request.overlapped->internal = X_STATUS_PENDING;
      return -1;
    }
  }

  auto* memory = kernel_state()->memory();
  uint8_t* dst = memory->TranslateVirtual<uint8_t*>(request.buffer.buf_ptr);
  const uint32_t flags = request.flags && !wait ? uint32_t(*request.flags) : 0;

  XSOCKADDR_IN from{};
  uint32_t from_len = sizeof(from);
  int ret = RecvFrom(dst, request.buffer.len, flags, &from, &from_len);
  if (ret < 0) {
    const uint32_t error = LastError();
    if (request.overlapped) {
      if (error == uint32_t(X_WSA_ERROR::X_WSAEWOULDBLOCK)) {
        request.overlapped->internal = X_STATUS_PENDING;
      } else {
        CompleteOverlapped(request.overlapped, error, 0, 0);
      }
    }
    return -1;
  }

  if (request.from) {
    *request.from = from;
  }
  if (request.from_len) {
    *request.from_len = from_len;
  }
  if (!wait && request.num_bytes_recv) {
    *request.num_bytes_recv = static_cast<uint32_t>(ret);
  }
  if (!wait && request.flags) {
    *request.flags = 0;
  }
  if (request.overlapped) {
    CompleteOverlapped(request.overlapped, 0, static_cast<uint32_t>(ret), 0);
  }
  return 0;
}

int XSocket::WSASendTo(const XWSABUF* buffers, uint32_t num_buffers,
                       rex::be<uint32_t>* num_bytes_sent, uint32_t /*flags*/,
                       const XSOCKADDR_IN* to,
                       uint32_t to_len, XWSAOVERLAPPED* overlapped) {
  if (!buffers || !num_buffers) {
    SetLastError(X_WSA_ERROR::X_WSA_INVALID_PARAMETER);
    return -1;
  }

  SendRequest request;
  request.buffers.assign(buffers, buffers + num_buffers);
  if (to) {
    request.has_to = true;
    request.to = *to;
    request.to_len = to_len;
  }
  request.overlapped = overlapped;

  std::lock_guard lock(send_mutex_);
  if (overlapped) {
    send_tasks_.erase(overlapped);
  }

  uint32_t sent = 0;
  if (DoSend(request, false, &sent) == 0) {
    if (num_bytes_sent) {
      *num_bytes_sent = sent;
    }
    return 0;
  }
  if (!overlapped) {
    return -1;
  }
  if (uint32_t(overlapped->internal) != X_STATUS_PENDING) {
    return -1;
  }

  if (overlapped->event_handle) {
    rex::kernel::xboxkrnl::xeNtClearEvent(overlapped->event_handle);
  }
  send_tasks_[overlapped] = std::async(std::launch::async, [this, request] {
    uint32_t bytes = 0;
    return DoSend(request, true, &bytes);
  });
  SetLastError(X_WSA_ERROR::X_WSA_IO_PENDING);
  return -1;
}

int XSocket::WSARecvFrom(const XWSABUF* buffers, uint32_t num_buffers,
                         rex::be<uint32_t>* num_bytes_recv, rex::be<uint32_t>* flags_ptr,
                         XSOCKADDR_IN* from, rex::be<uint32_t>* from_len,
                         XWSAOVERLAPPED* overlapped) {
  if (!buffers || !num_buffers || (from && !from_len)) {
    SetLastError(X_WSA_ERROR::X_WSA_INVALID_PARAMETER);
    return -1;
  }

  RecvRequest request;
  request.buffer = buffers[0];
  request.num_bytes_recv = num_bytes_recv;
  request.flags = flags_ptr;
  request.from = from;
  request.from_len = from_len;
  request.overlapped = overlapped;

  std::lock_guard lock(recv_mutex_);
  if (overlapped) {
    recv_tasks_.erase(overlapped);
  }

  if (DoRecv(request, false) == 0) {
    return 0;
  }
  if (!overlapped) {
    return -1;
  }
  if (uint32_t(overlapped->internal) != X_STATUS_PENDING) {
    return -1;
  }

  if (overlapped->event_handle) {
    rex::kernel::xboxkrnl::xeNtClearEvent(overlapped->event_handle);
  }
  recv_tasks_[overlapped] =
      std::async(std::launch::async, [this, request] { return DoRecv(request, true); });
  SetLastError(X_WSA_ERROR::X_WSA_IO_PENDING);
  return -1;
}

bool XSocket::WSAGetOverlappedResult(XWSAOVERLAPPED* overlapped,
                                     rex::be<uint32_t>* bytes_transferred, bool wait,
                                     rex::be<uint32_t>* flags_ptr) {
  if (!overlapped) {
    SetLastError(X_WSA_ERROR::X_WSA_INVALID_PARAMETER);
    return false;
  }

  auto settle = [&](std::mutex& mutex, std::map<XWSAOVERLAPPED*, std::future<int>>& tasks) {
    std::unique_lock lock(mutex);
    auto it = tasks.find(overlapped);
    if (it == tasks.end()) {
      return;
    }
    if (!wait && it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      return;
    }
    std::future<int> task = std::move(it->second);
    tasks.erase(it);
    lock.unlock();
    task.wait();
  };
  settle(send_mutex_, send_tasks_);
  settle(recv_mutex_, recv_tasks_);

  const uint32_t status = overlapped->internal;
  if (status == X_STATUS_PENDING) {
    SetLastError(X_WSA_ERROR::X_WSA_IO_INCOMPLETE);
    return false;
  }
  if (status != 0) {
    SetLastError(X_WSA_ERROR(status));
    return false;
  }
  if (bytes_transferred) {
    *bytes_transferred = overlapped->internal_high;
  }
  if (flags_ptr) {
    *flags_ptr = overlapped->offset;
  }
  return true;
}

int XSocket::WSACancelOverlappedIO() {
  cancel_ = true;
  std::vector<std::future<int>> pending;
  {
    std::lock_guard lock(send_mutex_);
    for (auto& [overlapped, task] : send_tasks_) {
      pending.push_back(std::move(task));
    }
    send_tasks_.clear();
  }
  {
    std::lock_guard lock(recv_mutex_);
    for (auto& [overlapped, task] : recv_tasks_) {
      pending.push_back(std::move(task));
    }
    recv_tasks_.clear();
  }
  for (auto& task : pending) {
    if (task.valid()) {
      task.wait();
    }
  }
  cancel_ = false;
  return 0;
}

}  // namespace rex::system
