#include "application.h"
#include "backlight.h"
#include "button.h"
#include "codecs/es8389_audio_codec.h"
#include "config.h"
#include "display/emote_display.h"
#include "display/lcd_display.h"
#include "esp_video.h"
#include "st77916_init.h"
#include "wifi_board.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#define TAG "EspVocatS31"

class EspVocatS31 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    Display* display_ = nullptr;
    PwmBacklight* backlight_ = nullptr;
    EspVideo* camera_ = nullptr;
    i2c_master_dev_handle_t touch_dev_ = nullptr;
    TaskHandle_t touch_task_ = nullptr;

    // Emote UI 由 esp_emote_gfx 自绘，没有 esp_lvgl_port 的默认 display，
    // 因此触摸不挂 LVGL indev，而是当作应用层按键：按下再抬起 = 切换对话/配网。
    static void TouchTaskEntry(void* arg) {
        auto* self = static_cast<EspVocatS31*>(arg);
        bool pressed_stable = false;
        int press_samples = 0;
        while (true) {
            bool pressed = false;
            if (!self->ReadTouchPoint(pressed)) {
                pressed = false;
            }
            if (pressed) {
                if (press_samples < 100) {
                    press_samples++;
                }
            } else if (press_samples > 0) {
                press_samples--;
            }

            const bool stable = press_samples >= 3;
            if (stable != pressed_stable) {
                pressed_stable = stable;
                ESP_LOGI(TAG, "touch %s", stable ? "pressed" : "released");
                if (!stable) {
                    auto& app = Application::GetInstance();
                    if (app.GetDeviceState() == kDeviceStateStarting) {
                        self->EnterWifiConfigMode();
                    } else {
                        app.ToggleChatState();
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    // 板上三路电源使能均为低有效：LCD_3V3(GPIO9)、CAM_3V3(GPIO10)、CODEC_3V3(GPIO17)
    void InitializePower() {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = (1ULL << LCD_POWER_GPIO) | (1ULL << CAMERA_POWER_GPIO) |
                           (1ULL << CODEC_POWER_GPIO) | (1ULL << AUDIO_CODEC_PA_PIN);
        cfg.mode = GPIO_MODE_OUTPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&cfg));

        gpio_set_level(LCD_POWER_GPIO, 0);        // LCD_3V3 PMOS: 低电平开启
        gpio_set_level(CAMERA_POWER_GPIO, 0);     // CAM_3V3 PMOS: 低电平开启
        gpio_set_level(CODEC_POWER_GPIO, 1);      // CODEC_3V3 LDO: 高电平开启（官方固件 level 1 = ON）
        gpio_set_level(AUDIO_CODEC_PA_PIN, 0);

        // 等三路 LDO 与摄像头 AVDD 稳定后再访问 I2C 设备
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus_));
    }

    void InitializeSpi() {
        const spi_bus_config_t bus_config = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(
            QSPI_PIN_NUM_LCD_PCLK, QSPI_PIN_NUM_LCD_DATA0, QSPI_PIN_NUM_LCD_DATA1,
            QSPI_PIN_NUM_LCD_DATA2, QSPI_PIN_NUM_LCD_DATA3,
            QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = QSPI_PIN_NUM_LCD_CS;
        io_config.dc_gpio_num = QSPI_PIN_NUM_LCD_DC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST,
                                                 &io_config, &panel_io));

        st77916_vendor_config_t vendor_config = {
            .init_cmds = vendor_specific_init_yysj,
            .init_cmds_size = sizeof(vendor_specific_init_yysj) / sizeof(st77916_lcd_init_cmd_t),
            .flags =
                {
                    .use_qspi_interface = 1,
                },
        };
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = QSPI_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL;
        panel_config.flags.reset_active_high = false;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
        backlight_ = new PwmBacklight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        backlight_->RestoreBrightness();
    }

    // CST816S：直接读寄存器 0x02 的 6 字节（num, xh, xl, yh, yl, ...）。
    // 不用 esp_lcd_touch 驱动：在这块板上它会把噪声当成触摸点，导致状态机反复切换。
    bool ReadTouchPoint(bool& pressed) {
        if (touch_dev_ == nullptr) {
            return false;
        }
        uint8_t reg = 0x02;
        uint8_t buf[6] = {0};
        esp_err_t err = i2c_master_transmit_receive(touch_dev_, &reg, 1, buf, sizeof(buf), 50);
        if (err != ESP_OK) {
            return false;
        }
        pressed = (buf[0] & 0x0F) > 0;
        return true;
    }

    void InitializeTouch() {
        for (int addr = 0x08; addr < 0x78; ++addr) {
            if (i2c_master_probe(i2c_bus_, addr, 20) == ESP_OK) {
                ESP_LOGI(TAG, "i2c device found at 0x%02x", addr);
            }
        }

        // CST816S 常见地址是 0x15；原理图笔记写的是 0x2A，两个都试
        const uint8_t candidates[] = {0x15, 0x2A};
        for (uint8_t addr : candidates) {
            if (i2c_master_probe(i2c_bus_, addr, 20) != ESP_OK) {
                continue;
            }
            i2c_device_config_t dev_cfg = {};
            dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            dev_cfg.device_address = addr;
            dev_cfg.scl_speed_hz = 100 * 1000;
            i2c_master_dev_handle_t dev = nullptr;
            if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev) != ESP_OK) {
                continue;
            }
            uint8_t reg = 0x02;
            uint8_t buf[6] = {0};
            if (i2c_master_transmit_receive(dev, &reg, 1, buf, sizeof(buf), 50) == ESP_OK) {
                touch_dev_ = dev;
                ESP_LOGI(TAG, "CST816S selected at 0x%02x", addr);
                break;
            }
            i2c_master_bus_rm_device(dev);
        }

        if (touch_dev_ == nullptr) {
            ESP_LOGW(TAG, "CST816S not found, touch disabled");
            return;
        }
        xTaskCreatePinnedToCore(TouchTaskEntry, "touch_task", 3 * 1024, this, 4,
                                &touch_task_, 1);
    }

    void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        // SCCB 复用 codec/触摸的 I2C 总线（GPIO0/1），SC101IOT 地址 0x68
        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus_,
            .freq = CAMERA_SCCB_FREQ_HZ,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = CAMERA_XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);
        ESP_LOGI(TAG, "camera object created (Capture() will report sensor state)");
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                ESP_LOGI(TAG, "BOOT pressed, enter WiFi configuration mode");
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    EspVocatS31() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePower();
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeCamera();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8389AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8389_ADDR,
            AUDIO_CODEC_USE_MCLK, AUDIO_INPUT_CHANNELS, AUDIO_OUTPUT_CHANNELS, 32);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override { return backlight_; }

    virtual Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(EspVocatS31);
