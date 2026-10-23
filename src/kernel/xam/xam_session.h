/**
 ******************************************************************************
 * ReXGlue : XSession bookkeeping for System Link                             *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * Host-side state behind the XGI session messages (XSessionCreate, Join,
 * Leave, Start, End, Delete, Modify, GetDetails, MigrateHost). System Link
 * titles find each other with their own broadcast packets; XAM only has to
 * hand out a session key and host XNADDR when hosting, accept the session
 * info the title received when joining, and report consistent details.
 *
 * Layouts and semantics follow the Xenia Canary netplay XSession
 * (AdrianCassar/xenia-canary, BSD).
 */

#pragma once

#include <cstdint>
#include <vector>

#include <rex/kernel/xam/systemlink.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xam::session {

// XamSessionCreateHandle / XamSessionRefObjByHandle backing.
uint32_t CreateHandle();
uint32_t ObjectForHandle(uint32_t handle);

// XGI message handlers. `buffer` points at the guest message struct.
X_HRESULT Create(uint8_t* buffer);
X_HRESULT Delete(uint8_t* buffer);
X_HRESULT Join(uint8_t* buffer);
X_HRESULT Leave(uint8_t* buffer);
X_HRESULT Start(uint8_t* buffer);
X_HRESULT End(uint8_t* buffer);
X_HRESULT Modify(uint8_t* buffer);
X_HRESULT GetDetails(uint8_t* buffer);
X_HRESULT MigrateHost(uint8_t* buffer);

// Sessions this machine hosts, for LAN advertisement.
struct HostedSession {
  XSESSION_INFO info;
  uint32_t flags;
  uint32_t max_public;
  uint32_t max_private;
  uint32_t used_public;
  uint32_t used_private;
  uint64_t host_xuid;
};
std::vector<HostedSession> HostedSessions();

// Address (host byte order) of the host of a peer session this machine joined
// as a client, or 0 if it is not in someone else's session.
uint32_t JoinedHostIp();

}  // namespace rex::kernel::xam::session
