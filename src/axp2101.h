// Copied from the vendor XiaoZhi example's main/boards/common/axp2101.h --
// minimal AXP2101 PMIC register wrapper, no framework deps.
#ifndef AXP2101_H
#define AXP2101_H

#include "i2c_device.h"

class Axp2101 : public I2cDevice {
public:
    Axp2101(i2c_master_bus_handle_t i2c_bus, uint8_t addr);
    bool IsCharging();
    bool IsDischarging();
    bool IsChargingDone();
    int GetBatteryLevel();
    bool IsBatteryPresent();
    bool IsUsbPowered();
    float GetTemperature();
    void PowerOff();

private:
    int GetBatteryCurrentDirection();
};

#endif  // AXP2101_H
