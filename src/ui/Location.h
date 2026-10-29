#pragma once
#include <Messenger.h>
#include <SupportDefs.h>

namespace summit {
// Finds where this computer is, for pages the user allowed to know it. On
// air/OS from the system's location service (location_server: a position the
// user entered, else a Wi-Fi or network-address lookup, and a system-wide
// switch). Without it (stock Haiku) the position comes from BeaconDB
// (https://beacondb.net), the open successor of Mozilla Location Service: the
// Wi-Fi networks in range (their hardware addresses) when there are at least
// two, and the network address the request comes from otherwise. Runs on a
// thread of its own and sends `what` to reply with "latitude", "longitude",
// "accuracy" (metres), "timestamp" (seconds since the epoch) and from the
// service "source" ("fixed", "wifi" or "ip"), or with "error".
void LookUpLocation(BMessenger reply, uint32 what);
// Whether the system has a location service (air/OS's location_server), for
// what Summit tells the user about where the position comes from.
bool HasSystemLocationService();
}
