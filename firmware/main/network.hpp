// Wired Ethernet (the board's IP101 PHY) and the SCMP server on UDP, as Setup's
// Network settings say: DHCP or a fixed address, and the SCMP port. Changes to them take
// effect at the next start.
#pragma once

#include "leota/settings.hpp"

namespace fw {

void network_start(const leota::Network& net);

}  // namespace fw
