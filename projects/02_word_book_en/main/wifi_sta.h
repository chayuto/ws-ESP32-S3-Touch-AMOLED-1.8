#pragma once

#include <stdbool.h>

/* Join the home network from Kconfig credentials. Blocks up to timeout_s. */
bool wifi_sta_join(int timeout_s);

/* True while joined. `ip` (16+ bytes) receives the dotted address. */
bool wifi_sta_connected(char *ip, int len);

/* Signal while joined: RSSI in dBm and the channel. False when not joined. */
bool wifi_sta_signal(int *rssi, int *channel);

/* How long the last join took, ms (success or failure). */
int wifi_sta_join_ms(void);

/* Radio off and the driver torn down; most of its RAM comes back. */
void wifi_sta_leave(void);
