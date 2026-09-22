#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Send a Wake-on-LAN Magic Packet via UDP port 9 to broadcast
bool wol_send_magic_packet(const char *mac_address_str);

#ifdef __cplusplus
}
#endif
