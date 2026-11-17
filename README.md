# ESP32-S31 VoCat（喵伴）· xiaozhi-esp32 移植

基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的 **ESP32-S31 喵伴** 固件移植，目标硬件是乐鑫官方开源的 **ESP-VoCat-S31-CoreBoard-V1.0**（`ESP32-S31-WROOM-1`，16MB Flash + 16MB PSRAM）。

当前分支：`s31-vocat`（历史完整保留，可随时和上游对比）。

## 已经跑通的功能

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| 离线唤醒 | ✅ | `wn9_nihaoxiaozhi_tts`（“你好小智”），1 麦 AFE |
| 语音对话 | ✅ | MQTT 协议，唤醒 → 连接 → 聆听 → 说话全链路 |
| 喇叭播放 | ✅ | ES8389 + NS4150B，32bit I2S 槽宽 |
| 麦克风采集 | ✅ | ES8389 ADC，单端 MIC1P + MICBIAS，双声道 I2S 下混单声道 |
| 圆形屏显示 | ✅ | ST77916 QSPI 360×360，emote 表情引擎 |
| 触摸 | ✅ | CST816S（I2C 0x15），点击切换对话/配网 |
| 拍照 + 视觉理解 | ✅ | `self.camera.take_photo`，SC101IOT DVP，JPEG 上传 |
| 拍照预览 | ✅ | 拍照后屏幕显示中心 360×360 画面，5 秒后回表情页 |
| Wi-Fi 配网 | ✅ | SoftAP 热点 + 网页配网（沿用上游方案） |
| OTA 检查 | ✅ | 兼容官方固件写进 NVS 的带引号 UUID |

## 硬件与关键引脚

| 模块 | 型号 / 引脚 |
| --- | --- |
| 主控 | ESP32-S31-WROOM-1，Wi-Fi 6 + BT 5.4，16MB Flash / 16MB PSRAM |
| 屏幕 | ST77916 QSPI 360×360：SCLK 39、D0-D3 38/44/37/45、DC 40、CS 43、RST 60、BL 46、TE 12 |
| 触摸 | CST816S：I2C 地址 0x15，INT 42 |
| 音频 | ES8389：I2C 0x20，BCLK 16、WS 18、DOUT 35、DIN 19、PA 2；无 MCLK |
| 电源 | LCD_3V3 GPIO9（低有效）、CAM_3V3 GPIO10（低有效）、CODEC_3V3 GPIO17（**高有效**） |
| 摄像头 | SC101IOT DVP：D0-D7 57/49/50/48/56/55/54/47、PCLK 51、HREF 53、VSYNC 8、XCLK 52（20MHz） |
| 按键 | BOOT GPIO61 |

完整定义见 [`main/boards/espressif/esp-vocat-s31/config.h`](main/boards/espressif/esp-vocat-s31/config.h)。

## 编译

需要 ESP-IDF **v6.0.1 以上**（推荐 `release/v6.1`；本仓库在 IDF master 6.2.0 上验证），`esp32s31` target。

```bash
source ~/esp/esp-idf-master/export.sh
cd ~/Gitlab/xiaozhi-esp32
python3 scripts/build.py espressif/esp-vocat-s31 --language zh-CN
```

或者手工：

```bash
idf.py set-target esp32s31
idf.py menuconfig     # Xiaozhi Assistant -> Target Board -> Espressif ESP-VoCat S31 (喵伴)
idf.py build
```

> ES8389 驱动使用了 esp32-s31-vocat 的补丁（MICBIAS、单端 MIC1P、完整 ADC 初始化）。
> 组件被重新解压后需要重新应用一次：`scripts/apply_es8389_vocat_patch.sh`。

## 烧录

S31 的 flash 布局和 S3 不同，构建完成后使用：

```bash
idfx flash COM21
# 或
python -m esptool --chip esp32s31 -b 460800 write-flash \
  0x2000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xd000 build/ota_data_initial.bin \
  0x20000 build/xiaozhi.bin \
  0x800000 build/configured_assets.bin
```

USB 串口建议 921600 波特率。

## TODO（还没做完的部分）

- [ ] **蓝牙音乐**：A2DP Sink 代码已集成（`main/bt/bluetooth_audio.cc`），但还没有在硬件上完整验证配对、播放、与语音对话的抢占
- [ ] **内部 RAM 紧张**：开启蓝牙后内部堆只剩几 KB，MQTT/TLS 会 `Failed to allocate memory`。计划开启 `CONFIG_SPIRAM_XIP_FROM_PSRAM`（官方固件就是这么做的，BT 起来后仍有 ~98KB 内部空闲）
- [ ] **摄像头预览收尾**：刚修掉“裁剪尺寸 0×0”和 RGB565 字节序问题，颜色/构图仍待最终确认
- [ ] **电池与充电状态**：板上有充电电路，但尚未接入 `GetBatteryLevel()`
- [ ] **IMU（BMI270）**：未接入，晃动表情、姿态交互都还没有
- [ ] **SD 卡 / 本地音乐 / 在线音乐搜索**：未实现
- [ ] **Wi-Fi 二维码配网**：官方是扫码配网，当前仍是热点 + 网页配网
- [ ] **触摸手势**：目前只有“点击”，滑动翻页/长按等未做
- [ ] **双麦阵列 / AEC**：当前按单麦运行，声源定位和回声消除未启用
- [ ] **表情资源缺图标**：`icon_speaker`、`cloud_off`、`gear` 等图标在当前资源包里不存在，配网/报错时表情显示为默认
- [ ] **唤醒词**：默认使用 `wn9_nihaoxiaozhi_tts`，官方固件用的是 `wn9_hiesp`

## 说明

- 本仓库保留 xiaozhi-esp32 的完整上游历史，方便后续 rebase / 对比上游改动。
- 板级适配、ES8389 补丁、摄像头 MCP 与预览、OTA UUID 修复都已拆成独立提交。
- 硬件调试过程中的临时日志（mic peak / raw32 / 寄存器 dump）已清理，需要时可用 `idf.py menuconfig` 的日志级别或自行加回。

---

## 商品说明（闲鱼）

**新一代乐鑫官方开源 ESP-VoCat-S31喵伴成品开发板｜支持摄像头拓展**

新一代乐鑫官方开源ESP-VoCat-S31喵伴成品开发板（一组三块），全网首发，此商品为乐鑫开源版本，非第三方魔改，使用乐鑫原装ESP32-S31-WROOM-1-N16R16V芯片。

✅【新一代新增功能】预留摄像头拓展接口，可以拓展视觉，实现语音+视觉多模态 AI，用来做多模态机器人、智能中控、语音交互原型。

✅【保留旧版全部功能】双麦阵列、离线唤醒、声源定位、圆屏接口，支持小智、ESP-Brookesia、火山引擎实时音视频固件。

✅专业贴片焊接，做工扎实，发货前都会烧写固件并测试拍照，品质有保障，上手即用。

包邮！喜欢直接拍，细节私聊，随时答疑～

ESP-VoCat（EchoEar）喵伴是乐鑫携手火山引擎扣子大模型团队打造的智能 AI 开发套件，适用于玩具、智能音箱、智能中控等需要大模型赋能的语音交互类产品。结合火山引擎提供的大模型能力，喵伴可实现全双工语音交互、多模态识别与智能体控制，为开发者打造完整的端侧 AI 应用体验提供坚实基础。

商品链接：https://www.goofish.com/item?id=1098829341080
