#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Wi-Fi driver ownership, and the station role on top of it.
 *
 * 05_dictation brought the driver up to join and tore it down again, because
 * internal RAM was the ceiling there. Here the radio is a *sensor*: wifiscan.c
 * sweeps every channel on a cycle and needs the driver resident for the whole
 * run. So the lifecycle is owned here and the two users share it.
 *
 *   wifi_radio_up()   driver in STA mode, started, NOT connected. Idempotent.
 *   wifi_sta_join()   additionally associates with the configured network.
 *
 * Scanning does not require a join and never associates. Joining happens only in
 * maintenance mode and for the boot NTP sync.
 */

/* Netif, event loop, esp_wifi_init, STA mode, esp_wifi_start. Safe to call repeatedly. */
esp_err_t wifi_radio_up(void);

/* True while the driver is started. */
bool wifi_radio_is_up(void);

/* Stop and deinit the driver entirely. Ends scanning too. */
void wifi_radio_down(void);

/* Join the home network from Kconfig credentials. Brings the radio up if needed. Blocks up to timeout_s. */
bool wifi_sta_join(int timeout_s);

/* True while joined. `ip` (16+ bytes) receives the dotted address. */
bool wifi_sta_connected(char *ip, int len);

/* Signal while joined: RSSI in dBm and the channel. False when not joined. */
bool wifi_sta_signal(int *rssi, int *channel);

/* How long the last join took, ms (success or failure). */
int wifi_sta_join_ms(void);

/* Disassociate but leave the driver up, so scanning continues. */
void wifi_sta_leave(void);
