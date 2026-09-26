#include "axp2101.h"

Axp2101::Axp2101(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
}

int Axp2101::GetBatteryCurrentDirection() {
    return (ReadReg(0x01) & 0b01100000) >> 5;
}

bool Axp2101::IsCharging() {
    return GetBatteryCurrentDirection() == 1;
}

bool Axp2101::IsDischarging() {
    return GetBatteryCurrentDirection() == 2;
}

bool Axp2101::IsChargingDone() {
    uint8_t value = ReadReg(0x01);
    return (value & 0b00000111) == 0b00000100;
}

int Axp2101::GetBatteryLevel() {
    return ReadReg(0xA4);
}

// Status register 0x00: bit 5 = VBUS good, bit 3 = battery present.
bool Axp2101::IsBatteryPresent() {
    return (ReadReg(0x00) & 0b00001000) != 0;
}

bool Axp2101::IsUsbPowered() {
    return (ReadReg(0x00) & 0b00100000) != 0;
}

float Axp2101::GetTemperature() {
    return ReadReg(0xA5);
}

void Axp2101::PowerOff() {
    uint8_t value = ReadReg(0x10);
    value = value | 0x01;
    WriteReg(0x10, value);
}
