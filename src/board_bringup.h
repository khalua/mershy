// Board bring-up for the Waveshare ESP32-S3-Touch-AMOLED-1.75C: I2C bus, PMIC
// power rails (including the mic supply), QSPI bus, SH8601 AMOLED panel,
// LVGL attachment, and CST9217 touch as an LVGL input device. Adapted from
// the vendor XiaoZhi example's board file (see CLAUDE.md). Buttons/MCP tools
// from that example are intentionally not ported.
#ifndef BOARD_BRINGUP_H
#define BOARD_BRINGUP_H

#include <driver/i2c_master.h>
#include "lvgl.h"

struct BoardHandles {
    i2c_master_bus_handle_t i2c_bus;
    lv_display_t *display;
    bool touch_ok;
};

// Runs the full bring-up sequence (I2C -> PMIC -> IO expander -> SPI ->
// SH8601 panel -> LVGL -> touch). Aborts via ESP_ERROR_CHECK on display
// failure, matching the vendor example. Touch failure is non-fatal and
// reported via touch_ok.
BoardHandles board_bringup_init();

// Panel sleep: display off + SH8601 sleep-in (lowest power; contents are
// kept), and back. Takes the LVGL port lock.
void board_display_power(bool on);

// True while the touch panel reports a finger. Reads the controller
// directly, so only call it while LVGL is stopped (lvgl_port_stop), i.e.
// during low-power sleep.
bool board_touch_is_pressed();

// Holds the display/touch/codec control pins in their current state
// through light sleep, and arms BOOT (GPIO0, low) as a light-sleep wakeup.
// Call once after bring-up.
void board_prepare_light_sleep();

struct BatteryStatus {
    bool present;   // a battery is connected
    bool usb;       // USB power is present
    bool charging;
    int percent;    // 0-100 from the AXP2101 fuel gauge (valid if present)
};

// Reads the AXP2101 PMIC (I2C, a few ms).
BatteryStatus board_battery();

// AMOLED brightness, 0-100 (SH8601 command 0x51). Takes the LVGL port lock,
// since the command shares the QSPI bus with LVGL's flushes.
void board_set_brightness(uint8_t percent);

#endif  // BOARD_BRINGUP_H
