#include "bluetooth_audio.h"

#if CONFIG_BT_ENABLED

#include <cstdio>
#include <cstring>
#include <vector>

#include <esp_ae_rate_cvt.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>

#include <esp_bt.h>
#include <esp_bt_device.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>

#include <esp_a2dp_api.h>
#include <esp_a2dp_legacy_api.h>
#include <esp_avrc_api.h>

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "device_state.h"

#define TAG "BtAudio"

static constexpr int kSrcRate = 44100;
static constexpr size_t kChunkFrames = 4410;  // 100ms @44.1kHz

BluetoothAudio& BluetoothAudio::GetInstance() {
    static BluetoothAudio instance;
    return instance;
}

static void GapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            ESP_LOGI(TAG, "auth complete: %s", param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "ok" : "fail");
            break;
        case ESP_BT_GAP_CFM_REQ_EVT:
            ESP_LOGI(TAG, "SSP confirm %06" PRIu32, param->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(TAG, "SSP passkey %06" PRIu32, param->key_notif.passkey);
            break;
        case ESP_BT_GAP_PIN_REQ_EVT: {
            esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
            break;
        }
        default:
            break;
    }
}

static void A2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
    auto& self = BluetoothAudio::GetInstance();
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                self.SetConnected(true);
                ESP_LOGI(TAG, "A2DP connected");
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                self.SetConnected(false);
                ESP_LOGI(TAG, "A2DP disconnected");
            }
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            ESP_LOGI(TAG, "A2DP audio state %d", param->audio_stat.state);
            break;
        default:
            break;
    }
}

static void AvrcCtCallback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t* param) {
    (void)param;
    ESP_LOGD(TAG, "AVRCP CT event %d", event);
}

static void AvrcTgCallback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t* param) {
    if (event == ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT) {
        int volume = param->set_abs_vol.volume * 100 / 127;
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr) {
            codec->SetOutputVolume(volume);
        }
    }
}

// A2DP sink 输出的是 SBC 解码后的 44.1kHz 立体声 16bit PCM
static void A2dpDataCallback(const uint8_t* data, uint32_t len) {
    BluetoothAudio::GetInstance().OnPcm(data, len);
}

void BluetoothAudio::OnPcm(const uint8_t* data, uint32_t len) {
    if (stream_ == nullptr || data == nullptr) {
        return;
    }
    size_t frames = len / 4;  // 2ch * 16bit
    if (frames == 0) {
        return;
    }
    static int16_t mono[kChunkFrames];
    if (frames > kChunkFrames) {
        frames = kChunkFrames;
    }
    const int16_t* pcm = reinterpret_cast<const int16_t*>(data);
    for (size_t i = 0; i < frames; ++i) {
        mono[i] = static_cast<int16_t>((static_cast<int32_t>(pcm[2 * i]) + pcm[2 * i + 1]) / 2);
    }
    // 不阻塞 BT 栈；缓冲满时直接丢弃
    xStreamBufferSend(stream_, mono, frames * sizeof(int16_t), 0);
}

void BluetoothAudio::AudioTaskEntry(void* arg) {
    static_cast<BluetoothAudio*>(arg)->AudioLoop();
}

void BluetoothAudio::AudioLoop() {
    std::vector<int16_t> in(60 * kSrcRate / 1000);
    while (true) {
        size_t bytes = xStreamBufferReceive(stream_, in.data(), in.size() * sizeof(int16_t),
                                            pdMS_TO_TICKS(100));
        if (bytes == 0) {
            continue;
        }
        // 语音对话（聆听/说话/配网）期间丢弃音乐帧，避免与 TTS 抢 I2S；
        // 同时清空积压，防止对话结束后把过期音乐补播出来
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            xStreamBufferReset(stream_);
            continue;
        }
        size_t frames = bytes / sizeof(int16_t);
        uint32_t out_frames = static_cast<uint32_t>(
            static_cast<uint64_t>(frames) * dest_rate_ / kSrcRate) + 64;
        std::vector<int16_t> out(out_frames);
        uint32_t produced = out_frames;
        esp_ae_rate_cvt_process(resampler_, reinterpret_cast<esp_ae_sample_t>(in.data()),
                                static_cast<uint32_t>(frames),
                                reinterpret_cast<esp_ae_sample_t>(out.data()), &produced);
        out.resize(produced);
        if (!out.empty()) {
            Application::GetInstance().GetAudioService().PushPcmToPlayback(std::move(out));
        }
    }
}

bool BluetoothAudio::Start() {
    if (started_.load()) {
        return true;
    }

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        char name[32];
        snprintf(name, sizeof(name), "Xiaozhi-Vocat-%02X%02X", mac[4], mac[5]);
        device_name_ = name;
    } else {
        device_name_ = "Xiaozhi-Vocat";
    }

    // 触发水位 = 60ms 单声道 PCM：攒够一帧再送播放队列，避免 11.6ms 一个的小任务刷屏。
    // 缓冲放在 PSRAM：内部 RAM 要留给 Wi-Fi/BT 协议栈、AFE 和 mbedTLS。
    constexpr size_t kTriggerBytes = 60 * kSrcRate / 1000 * sizeof(int16_t);
    constexpr size_t kStreamBytes = 16 * 1024;
    auto* storage = static_cast<uint8_t*>(
        heap_caps_malloc(kStreamBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* sb = static_cast<StaticStreamBuffer_t*>(malloc(sizeof(StaticStreamBuffer_t)));
    if (storage == nullptr || sb == nullptr) {
        ESP_LOGE(TAG, "Failed to create stream buffer");
        free(storage);
        free(sb);
        return false;
    }
    stream_ = xStreamBufferCreateStatic(kStreamBytes, kTriggerBytes, storage, sb);
    if (stream_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create static stream buffer");
        return false;
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    dest_rate_ = codec != nullptr ? codec->output_sample_rate() : 16000;

    esp_ae_rate_cvt_cfg_t cfg = {};
    cfg.src_rate = kSrcRate;
    cfg.dest_rate = static_cast<uint32_t>(dest_rate_);
    cfg.channel = ESP_AUDIO_MONO;
    cfg.bits_per_sample = ESP_AUDIO_BIT16;
    cfg.complexity = 2;
    cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
    esp_err_t err = esp_ae_rate_cvt_open(&cfg, &resampler_);
    if (err != ESP_OK || resampler_ == nullptr) {
        ESP_LOGE(TAG, "Failed to open resampler: %s", esp_err_to_name(err));
        vStreamBufferDelete(stream_);
        stream_ = nullptr;
        return false;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_gap_register_callback(GapCallback);
    esp_bt_gap_set_device_name(device_name_.c_str());

    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code = {'1', '2', '3', '4'};
    esp_bt_gap_set_pin(pin_type, 4, pin_code);

    // AVRCP 必须在 A2DP 之前初始化，否则手机会拒绝 AVCTP 连接
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    esp_avrc_ct_register_callback(AvrcCtCallback);
    ESP_ERROR_CHECK(esp_avrc_tg_init());
    esp_avrc_tg_register_callback(AvrcTgCallback);

    ESP_ERROR_CHECK(esp_a2d_register_callback(A2dpCallback));
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(A2dpDataCallback));

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    xTaskCreate(AudioTaskEntry, "bt_a2dp", 4096, this, 5, &task_);
    started_ = true;
    ESP_LOGI(TAG, "A2DP sink started, name=%s, out_rate=%d", device_name_.c_str(), dest_rate_);
    return true;
}

void BluetoothAudio::SetConnected(bool connected) {
    connected_ = connected;
}

void BluetoothAudio::SetDiscoverable(bool enabled) {
    if (!started_.load()) {
        return;
    }
    discoverable_ = enabled;
    if (enabled) {
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    } else {
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    }
}

void BluetoothAudio::SendMediaCommand(int command) {
    if (!started_.load()) {
        return;
    }
    uint8_t cmd = ESP_AVRC_PT_CMD_PLAY;
    switch (command) {
        case 0: cmd = ESP_AVRC_PT_CMD_PLAY; break;
        case 1: cmd = ESP_AVRC_PT_CMD_PAUSE; break;
        case 2: cmd = ESP_AVRC_PT_CMD_FORWARD; break;
        case 3: cmd = ESP_AVRC_PT_CMD_BACKWARD; break;
        default: return;
    }
    esp_avrc_ct_send_passthrough_cmd(0, cmd, ESP_AVRC_PT_CMD_STATE_PRESSED);
    esp_avrc_ct_send_passthrough_cmd(0, cmd, ESP_AVRC_PT_CMD_STATE_RELEASED);
}

std::string BluetoothAudio::GetStatusJson() {
    char buffer[256];
    snprintf(buffer, sizeof(buffer),
             "{\"started\":%s,\"connected\":%s,\"discoverable\":%s,\"device_name\":\"%s\"}",
             started_.load() ? "true" : "false", connected_.load() ? "true" : "false",
             discoverable_.load() ? "true" : "false", device_name_.c_str());
    return std::string(buffer);
}

#endif  // CONFIG_BT_ENABLED
