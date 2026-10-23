/**
 ******************************************************************************
 * ReXGlue : LAN session discovery                                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * Stands in for Xbox LIVE matchmaking between ReXGlue instances. Every
 * machine broadcasts a small beacon per session it hosts (with the host's
 * contexts and properties) on the lobby port, and XSessionSearch* answer from
 * the beacons heard from other machines.
 */

#pragma once

#include <cstdint>

#include <rex/system/xtypes.h>

namespace rex::kernel::xam::lobby {

// Records the local user's XGIUserSetContextEx / XGIUserSetPropertyEx values
// so they can be advertised with hosted sessions. `data` is guest memory.
void SetContext(uint32_t context_id, uint32_t value);
void SetProperty(uint32_t property_id, const uint8_t* data, uint32_t size);

// XGI search messages. `buffer` points at the guest message struct.
X_HRESULT Search(uint8_t* buffer, uint32_t buffer_length);         // 0x000B0016 / 0x000B001C
X_HRESULT SearchById(uint8_t* buffer, uint32_t buffer_length);     // 0x000B001B
X_HRESULT SearchByIds(uint8_t* buffer, uint32_t buffer_length);    // 0x000B0060

// Starts the beacon thread (no-op when networking is off or already running).
void Start();

}  // namespace rex::kernel::xam::lobby
