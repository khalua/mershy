// Copy this file to secrets.h and fill in real values. secrets.h is
// gitignored so none of this ever gets committed.
#ifndef SECRETS_H
#define SECRETS_H

// Known WiFi networks as {ssid, password}. The device joins the strongest
// one in range and switches automatically when the link drops.
//
// iPhone hotspot: turn on Settings > Personal Hotspot > "Maximize
// Compatibility" (the ESP32 only does 2.4GHz), and keep that screen open
// while the device first connects -- iOS stops advertising the hotspot when
// nobody is connected.
#define WIFI_NETWORKS {                          \
    {"your-home-wifi", "your-wifi-password"},    \
    {"Your iPhone", "your-hotspot-password"},    \
}

#endif  // SECRETS_H
