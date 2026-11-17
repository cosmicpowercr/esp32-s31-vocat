#ifndef BLUETOOTH_AUDIO_H
#define BLUETOOTH_AUDIO_H

#include <atomic>
#include <cstdint>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

/**
 * A2DP Sink：手机通过经典蓝牙连接设备，把音乐推给设备播放。
 *
 * 数据路径：
 *   BT 栈 SBC 解码 -> 44.1kHz 立体声 PCM -> 下混单声道 -> 重采样到 codec 输出采样率
 *   -> AudioService 播放队列 -> ES8389 -> 喇叭
 *
 * 语音助手说话/聆听时丢弃音乐帧（ducking），回到空闲后自动恢复，避免和 TTS 抢 I2S。
 */
class BluetoothAudio {
public:
    static BluetoothAudio& GetInstance();

    bool Start();
    bool started() const { return started_.load(); }
    bool connected() const { return connected_.load(); }

    void SetConnected(bool connected);

    // 供 MCP 工具使用
    std::string GetStatusJson();
    void SetDiscoverable(bool enabled);
    // 0=play 1=pause 2=next 3=prev（AVRCP CT 发给手机）
    void SendMediaCommand(int command);
    void OnPcm(const uint8_t* data, uint32_t len);

    const char* GetDeviceName() const { return device_name_.c_str(); }

private:
    BluetoothAudio() = default;
    BluetoothAudio(const BluetoothAudio&) = delete;
    BluetoothAudio& operator=(const BluetoothAudio&) = delete;

    static void AudioTaskEntry(void* arg);
    void AudioLoop();
    void* resampler_ = nullptr;
    StreamBufferHandle_t stream_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> discoverable_{true};
    std::string device_name_;
    int dest_rate_ = 16000;
};

#endif  // BLUETOOTH_AUDIO_H
