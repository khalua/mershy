// Persistent user settings in NVS (survive reboots and reflashing).
#ifndef SETTINGS_H
#define SETTINGS_H

// Initializes NVS (erasing it if its format is stale). Call first in app_main.
void settings_init();

int settings_get_volume();           // 0-100
void settings_set_volume(int volume);

#endif  // SETTINGS_H
