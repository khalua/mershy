// WiFi station over a list of known networks (WIFI_NETWORKS in secrets.h).
#ifndef WIFI_STA_H
#define WIFI_STA_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the network stack and Wi-Fi in station mode, joins the strongest
// known network, and blocks until connected or the timeout expires. Returns
// true once we have an IP. Either way, a background task keeps reconnecting
// (and switching networks) whenever the link is down. Needs NVS initialized.
bool wifi_sta_connect(int timeout_ms);

bool wifi_sta_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif  // WIFI_STA_H
