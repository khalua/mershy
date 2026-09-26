#include "board_bringup.h"
#include "board_config.h"
#include "axp2101.h"

#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_master.h>
#include <esp_io_expander_tca9554.h>
#include <esp_lcd_sh8601.h>
#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <esp_rom_sys.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <initializer_list>

static const char *TAG = "board_bringup";

// AXP2101 power-rail setup.
//
// !! DO NOT reintroduce the vendor XiaoZhi board file's rail sequencing here.
// That file (written for a different board) does:
//     WriteReg(0x80, 0x01);  // disable all DCs but DC1
//     WriteReg(0x90, 0x00);  // disable all LDOs   <-- kills ALDO1
//     WriteReg(0x91, 0x00);
//     ... then re-enables only ALDO1
// On the Touch-AMOLED-1.75 that is self-destructive. Per the board schematic
// (waveshareteam/ESP32-S3-Touch-AMOLED-1.75, Schematic/*.pdf netlist), ALDO1
// is the A3V3 rail and it is the sole supply for BOTH audio codecs -- ES8311
// (U6 pins 5/10/21) and ES7210 (U8 pins 6/7/8/21/22/23) -- plus the ES8311 CE
// pull-up (R25) and the ES7210 address straps (R35/R36). Those two chips sit
// on the same shared I2C bus as the PMIC itself. The moment ALDO1 drops, their
// unpowered SDA/SCL pads clamp GPIO15/GPIO14 to ground through their ESD
// diodes, the whole bus goes dead, and the AXP2101 can no longer be reached to
// turn ALDO1 back on. Because the PMIC keeps its register state across an ESP
// reset, the board then stays bricked for audio until it is physically
// power-cycled. Measured symptom: SDA and SCL both read 0 with the internal
// pull-up, pull-down, and floating -- and every I2C transaction times out.
//
// ALDO2/3/4, BLDO1/2, CPUSLDO, DLDO1/2 and DCDC2-5 are single-pin (unconnected)
// nets on this board, so there is nothing to gain by touching them either.
// The maintained vendor BSP configures no rails at all and relies on the
// PMIC's power-on defaults; we only ensure the codec rail is up, and never
// clear a bit we did not set.
class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110);  // PWRON > OFFLEVEL as POWEROFF source enable
        WriteReg(0x27, 0x10);   // hold 4s to power off

        // Codec rail: set the voltage, then enable ALDO1 read-modify-write so
        // no other rail is disturbed. Never a blind whole-register write.
        WriteReg(0x92, (3300 - 500) / 100);  // ALDO1 = 3.3V
        uint8_t ldo_en = ReadReg(0x90);
        WriteReg(0x90, ldo_en | 0x01);       // enable ALDO1 (both codecs)
        ESP_LOGI(TAG, "AXP2101 LDO enable 0x90: 0x%02X -> 0x%02X (ALDO1/codecs on)",
                 ldo_en, ReadReg(0x90));

        // Fuel gauge on (reg 0x18 bit 3), so 0xA4 reports battery percent.
        WriteReg(0x18, ReadReg(0x18) | 0x08);

        WriteReg(0x64, 0x02);  // CV charger voltage = 4.1V
        WriteReg(0x61, 0x02);  // precharge current = 50mA
        WriteReg(0x62, 0x08);  // charger current = 200mA
        WriteReg(0x63, 0x01);  // term charge current = 25mA
    }
};

static Pmic *s_pmic = nullptr;

// The codecs clamp the shared bus low whenever ALDO1 is off (see Pmic above),
// so a stuck-low bus at boot is a specific, recoverable condition rather than
// a mystery. Check before installing the I2C driver, where the failure mode is
// just an unhelpful "probe device timeout" on every address.
static void warn_if_i2c_bus_stuck() {
    for (gpio_num_t pin : {AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN}) {
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    }
    esp_rom_delay_us(2000);
    int sda = gpio_get_level(AUDIO_CODEC_I2C_SDA_PIN);
    int scl = gpio_get_level(AUDIO_CODEC_I2C_SCL_PIN);
    gpio_reset_pin(AUDIO_CODEC_I2C_SDA_PIN);
    gpio_reset_pin(AUDIO_CODEC_I2C_SCL_PIN);
    if (sda == 0 || scl == 0) {
        ESP_LOGE(TAG, "I2C bus is stuck LOW at boot (SDA=%d SCL=%d).", sda, scl);
        ESP_LOGE(TAG, "The board pull-ups (R23/R24 to VCC3V3) should hold both high.");
        ESP_LOGE(TAG, "This means the AXP2101 ALDO1 rail is off, so the unpowered");
        ESP_LOGE(TAG, "ES8311/ES7210 are clamping the bus. The PMIC keeps its");
        ESP_LOGE(TAG, "registers across an ESP reset, so reflashing will NOT fix it:");
        ESP_LOGE(TAG, "physically unplug USB (and the battery, if fitted) for ~30s.");
    }
}

#define LCD_OPCODE_WRITE_CMD (0x02ULL)

static const sh8601_lcd_init_cmd_t kVendorInitCmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

static i2c_master_bus_handle_t init_i2c() {
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    return bus;
}

static void init_tca9554(i2c_master_bus_handle_t i2c_bus) {
    esp_io_expander_handle_t expander = nullptr;
    esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus, IO_EXPANDER_I2C_ADDRESS, &expander);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 create failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT));
}

static void init_spi() {
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = LCD_PIN_NUM_PCLK;
    buscfg.data0_io_num = LCD_PIN_NUM_DATA0;
    buscfg.data1_io_num = LCD_PIN_NUM_DATA1;
    buscfg.data2_io_num = LCD_PIN_NUM_DATA2;
    buscfg.data3_io_num = LCD_PIN_NUM_DATA3;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static esp_lcd_panel_io_handle_t s_panel_io = nullptr;

static lv_display_t *init_display() {
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;

    esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(LCD_PIN_NUM_CS, nullptr, nullptr);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));
    s_panel_io = panel_io;

    const sh8601_vendor_config_t vendor_config = {
        .init_cmds = kVendorInitCmds,
        .init_cmds_size = sizeof(kVendorInitCmds) / sizeof(sh8601_lcd_init_cmd_t),
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = LCD_PIN_NUM_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = (void *)&vendor_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel));
    esp_lcd_panel_set_gap(panel, 0x06, 0);
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, false);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    esp_lcd_panel_disp_on_off(panel, true);

    lv_init();

#if CONFIG_SPIRAM
    size_t psram_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
    } else if (psram_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
    }
#endif

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(DISPLAY_WIDTH * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(DISPLAY_WIDTH),
        .vres = static_cast<uint32_t>(DISPLAY_HEIGHT),
        .monochrome = false,
        .rotation = {
            .swap_xy = DISPLAY_SWAP_XY,
            .mirror_x = DISPLAY_MIRROR_X,
            .mirror_y = DISPLAY_MIRROR_Y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    lv_display_t *display = lvgl_port_add_disp(&display_cfg);
    if (display == nullptr) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return nullptr;
    }
    if (DISPLAY_OFFSET_X != 0 || DISPLAY_OFFSET_Y != 0) {
        lv_display_set_offset(display, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
    }
    return display;
}

// CST9217 capacitive touch on the shared I2C bus (0x5A). Ported from the
// vendor XiaoZhi board file's InitializeTouch(), which targets the plain
// 1.75 -- the 1.75C may differ. If taps don't register, try TOUCH_PIN_RST =
// GPIO_NUM_NC first, then the mirror flags in board_config.h. Non-fatal on
// failure: the buddy still boots, it just can't be tapped.
static esp_lcd_touch_handle_t s_touch = nullptr;

static bool init_touch(i2c_master_bus_handle_t i2c_bus, lv_display_t *display) {
    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max = DISPLAY_WIDTH - 1;
    tp_cfg.y_max = DISPLAY_HEIGHT - 1;
    tp_cfg.rst_gpio_num = TOUCH_PIN_RST;
    tp_cfg.int_gpio_num = GPIO_NUM_NC;
    tp_cfg.levels.reset = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy = 0;
    tp_cfg.flags.mirror_x = TOUCH_MIRROR_X;
    tp_cfg.flags.mirror_y = TOUCH_MIRROR_Y;

    esp_lcd_panel_io_handle_t tp_io = nullptr;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    tp_io_cfg.scl_speed_hz = 400 * 1000;
    // _v2 directly: IDF 5.3's C++ esp_lcd_new_panel_io_i2c() overload returns
    // void and drops the error code.
    esp_err_t ret = esp_lcd_new_panel_io_i2c_v2(i2c_bus, &tp_io_cfg, &tp_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch panel IO create failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_lcd_touch_handle_t tp = nullptr;
    ret = esp_lcd_touch_new_i2c_cst9217(tp_io, &tp_cfg, &tp);
    s_touch = tp;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CST9217 init failed: %s", esp_err_to_name(ret));
        return false;
    }

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = display,
        .handle = tp,
    };
    if (lvgl_port_add_touch(&touch_cfg) == nullptr) {
        ESP_LOGE(TAG, "lvgl_port_add_touch failed");
        return false;
    }
    ESP_LOGI(TAG, "Touch panel initialized");
    return true;
}

BoardHandles board_bringup_init() {
    BoardHandles handles = {};

    warn_if_i2c_bus_stuck();
    handles.i2c_bus = init_i2c();

    ESP_LOGI(TAG, "Init AXP2101");
    static Pmic pmic(handles.i2c_bus, 0x34);  // powers the mic (ALDO1); left running
    s_pmic = &pmic;

    // Deliberately after the PMIC, not before (as the vendor reference
    // orders it): if the TCA9554's own supply rail comes from the PMIC,
    // talking to it before Pmic's construction would explain the I2C NACK
    // seen during earlier bring-up ("TCA9554 create failed").
    init_tca9554(handles.i2c_bus);

    init_spi();
    handles.display = init_display();
    if (handles.display != nullptr) {
        handles.touch_ok = init_touch(handles.i2c_bus, handles.display);
    }

    return handles;
}

void board_set_brightness(uint8_t percent) {
    if (s_panel_io == nullptr) return;
    if (percent > 100) percent = 100;
    uint8_t level = (uint8_t)(255 * percent / 100);
    int cmd = (int)((LCD_OPCODE_WRITE_CMD << 24) | (0x51 << 8));
    lvgl_port_lock(0);
    esp_lcd_panel_io_tx_param(s_panel_io, cmd, &level, 1);
    lvgl_port_unlock();
}

static void panel_cmd(uint8_t cmd) {
    int lcd_cmd = (int)((LCD_OPCODE_WRITE_CMD << 24) | ((uint32_t)cmd << 8));
    esp_lcd_panel_io_tx_param(s_panel_io, lcd_cmd, nullptr, 0);
}

void board_display_power(bool on) {
    if (s_panel_io == nullptr) return;
    lvgl_port_lock(0);
    if (on) {
        panel_cmd(0x11);  // sleep out
        vTaskDelay(pdMS_TO_TICKS(120));  // SH8601 needs 120ms after sleep-out
        panel_cmd(0x29);  // display on
    } else {
        panel_cmd(0x28);  // display off
        panel_cmd(0x10);  // sleep in
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    lvgl_port_unlock();
}

bool board_touch_is_pressed() {
    if (s_touch == nullptr) return false;
    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) return false;
    esp_lcd_touch_point_data_t point;
    uint8_t count = 0;
    esp_lcd_touch_get_data(s_touch, &point, &count, 1);
    return count > 0;
}

void board_prepare_light_sleep() {
    // By default light sleep switches every GPIO to its isolated sleep
    // config. These must keep driving: the panel/touch reset lines (a float
    // would reset them), the panel chip-select (keeps QSPI idle), the
    // speaker amp enable, and the shared I2C bus.
    for (gpio_num_t pin : {LCD_PIN_NUM_RST, LCD_PIN_NUM_CS, TOUCH_PIN_RST, AUDIO_CODEC_PA_PIN,
                           AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN}) {
        gpio_sleep_sel_dis(pin);
    }
    gpio_wakeup_enable(BOOT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}

BatteryStatus board_battery() {
    BatteryStatus b = {};
    if (s_pmic == nullptr) return b;
    b.present = s_pmic->IsBatteryPresent();
    b.usb = s_pmic->IsUsbPowered();
    b.charging = b.present && s_pmic->IsCharging();
    if (b.present) {
        int level = s_pmic->GetBatteryLevel();
        b.percent = level < 0 ? 0 : (level > 100 ? 100 : level);
    }
    return b;
}
