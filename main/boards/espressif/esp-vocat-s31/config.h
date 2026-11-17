#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>

// ESP32-S31 VoCat（喵伴）核心板 V1.0
// 引脚来源：docs/ESP32-S31-VoCat-引脚速查.md（EasyEDA 原理图网表核对）

#define AUDIO_INPUT_SAMPLE_RATE 16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// ES8389：MCLK-less 模式（codec 从 BCLK 恢复时钟）
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_NC
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_16
#define AUDIO_I2S_GPIO_WS GPIO_NUM_18
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_35
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_19

#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_1
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_0
#define AUDIO_CODEC_PA_PIN GPIO_NUM_2
#define AUDIO_CODEC_ES8389_ADDR ES8389_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_USE_MCLK false
#define AUDIO_INPUT_CHANNELS 1
#define AUDIO_OUTPUT_CHANNELS 2

// 电源使能：LCD/CAM 为 PMOS 低有效，CODEC_3V3 是 LDO、高有效（官方固件 level 1 = ON）
#define CODEC_POWER_GPIO GPIO_NUM_17
#define LCD_POWER_GPIO GPIO_NUM_9
#define CAMERA_POWER_GPIO GPIO_NUM_10

#define BUILTIN_LED_GPIO GPIO_NUM_NC
#define BOOT_BUTTON_GPIO GPIO_NUM_61

// 1.85" 圆形 QSPI 屏 ST77916 360x360
#define DISPLAY_WIDTH 360
#define DISPLAY_HEIGHT 360
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

#define QSPI_LCD_H_RES 360
#define QSPI_LCD_V_RES 360
#define QSPI_LCD_BIT_PER_PIXEL 16

#define QSPI_LCD_HOST SPI2_HOST
#define QSPI_PIN_NUM_LCD_PCLK GPIO_NUM_39
#define QSPI_PIN_NUM_LCD_CS GPIO_NUM_43
#define QSPI_PIN_NUM_LCD_DATA0 GPIO_NUM_38
#define QSPI_PIN_NUM_LCD_DATA1 GPIO_NUM_44
#define QSPI_PIN_NUM_LCD_DATA2 GPIO_NUM_37
#define QSPI_PIN_NUM_LCD_DATA3 GPIO_NUM_45
#define QSPI_PIN_NUM_LCD_RST GPIO_NUM_60
#define QSPI_PIN_NUM_LCD_DC GPIO_NUM_40
#define QSPI_PIN_NUM_LCD_BL GPIO_NUM_46

#define DISPLAY_BACKLIGHT_PIN QSPI_PIN_NUM_LCD_BL
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz) \
    {                                                                             \
        .data0_io_num = d0,                                                       \
        .data1_io_num = d1,                                                       \
        .sclk_io_num = sclk,                                                      \
        .data2_io_num = d2,                                                       \
        .data3_io_num = d3,                                                       \
        .max_transfer_sz = max_trans_sz,                                          \
    }

// CST816S 触摸（与 codec / 摄像头 SCCB 共用 I2C：GPIO0/1）
#define TP_PIN_NUM_INT GPIO_NUM_42
#define TP_PIN_NUM_RST GPIO_NUM_NC

// SC101IOT DVP 摄像头
#define CAMERA_PIN_XCLK GPIO_NUM_52
#define CAMERA_PIN_PCLK GPIO_NUM_51
#define CAMERA_PIN_VSYNC GPIO_NUM_8
#define CAMERA_PIN_HREF GPIO_NUM_53
#define CAMERA_PIN_D0 GPIO_NUM_57
#define CAMERA_PIN_D1 GPIO_NUM_49
#define CAMERA_PIN_D2 GPIO_NUM_50
#define CAMERA_PIN_D3 GPIO_NUM_48
#define CAMERA_PIN_D4 GPIO_NUM_56
#define CAMERA_PIN_D5 GPIO_NUM_55
#define CAMERA_PIN_D6 GPIO_NUM_54
#define CAMERA_PIN_D7 GPIO_NUM_47
#define CAMERA_PIN_RESET GPIO_NUM_NC
#define CAMERA_PIN_PWDN GPIO_NUM_NC
#define CAMERA_XCLK_FREQ_HZ (20 * 1000 * 1000)
#define CAMERA_SCCB_FREQ_HZ (100 * 1000)

#endif  // _BOARD_CONFIG_H_
