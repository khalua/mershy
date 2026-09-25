// Pin definitions for the Waveshare ESP32-S3-Touch-AMOLED-1.75 (SKU 33691).
// Copied from the vendor XiaoZhi example's board config.h at
// main/boards/waveshare-s3-touch-amoled-1.75/config.h -- see
// ../../CLAUDE.md for the exact reference path.
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include <driver/gpio.h>

// Vendor's tested rate. An earlier attempt to drop this to 16000 (to match
// the relay's WAV format) silently broke mic capture -- CreateDuplexChannels
// in box_audio_codec.cpp hardcodes bclk_div=8 for the TDM input clock,
// tuned by the vendor specifically for 24000Hz, and reading at a mismatched
// rate produced all-zero samples with no error reported anywhere (confirmed
// via raw min/max debug logging: real hardware, real clap, ch0/ch1 stayed
// exactly 0/0). Kept at the vendor's original value instead of hand-deriving
// the correct bclk_div for 16kHz; (bark-notify's relay WAV_SAMPLE_RATE
// was changed to match 24000 instead.)
#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_INPUT_REFERENCE true  // channel 0 = mic, channel 1 = AEC reference

// GPIO16, NOT GPIO42. This board is the ESP32-S3-Touch-AMOLED-1.75**C**
// variant, which is a different product from the plain 1.75 and has its own
// vendor repo (waveshareteam/ESP32-S3-Touch-AMOLED-1.75C). The two swapped
// this pin: the plain 1.75's hardware reference says "Use GPIO42 for audio
// MCLK. GPIO16 is an expansion GPIO", while the 1.75C's pin_config.h says
// PIN_ES7210_MCLK 16.
//
// Getting this wrong costs you both audio directions and looks like dead
// silicon: with no master clock the codecs still power up, ACK on I2C and
// accept register writes, but the ADC never converts (ES7210 SDOUT stays
// floating, captured samples are exactly 0) and the DAC never plays. DOUT
// still toggles, because that is the ESP32 transmitting, not the codec.
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_16
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_45
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_10
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8

#define AUDIO_CODEC_PA_PIN        GPIO_NUM_46
#define AUDIO_CODEC_I2C_SDA_PIN   GPIO_NUM_15
#define AUDIO_CODEC_I2C_SCL_PIN   GPIO_NUM_14
#define AUDIO_CODEC_ES8311_ADDR   ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR   ES7210_CODEC_DEFAULT_ADDR

#define IO_EXPANDER_I2C_ADDRESS ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000

#define LCD_PIN_NUM_CS    GPIO_NUM_12
#define LCD_PIN_NUM_PCLK  GPIO_NUM_38
#define LCD_PIN_NUM_DATA0 GPIO_NUM_4
#define LCD_PIN_NUM_DATA1 GPIO_NUM_5
#define LCD_PIN_NUM_DATA2 GPIO_NUM_6
#define LCD_PIN_NUM_DATA3 GPIO_NUM_7
#define LCD_PIN_NUM_RST   GPIO_NUM_39

// BOOT button: pressed = low, external pull-up. Only sampled after boot
// (holding it during reset still enters the ROM download mode).
#define BOOT_BUTTON_GPIO GPIO_NUM_0

// CST9217 touch, I2C 0x5A on the shared codec bus. Values from the plain
// 1.75's vendor board file; unverified on the 1.75C.
#define TOUCH_PIN_RST  GPIO_NUM_40
#define TOUCH_MIRROR_X 1
#define TOUCH_MIRROR_Y 1

#define DISPLAY_WIDTH  466
#define DISPLAY_HEIGHT 466
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

#endif  // BOARD_CONFIG_H
