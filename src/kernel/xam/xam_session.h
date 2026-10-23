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

}  // namespace rex::kernel::xam::session
