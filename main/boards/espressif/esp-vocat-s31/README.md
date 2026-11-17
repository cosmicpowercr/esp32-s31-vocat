# ESP-VoCat S31（喵伴）板型

ESP32-S31 版「喵伴」核心板（`ESP-VoCat-S31-CoreBoard-V1_0`），在 xiaozhi-esp32 上新增的移植板型。

## 硬件

| 模块 | 说明 |
| --- | --- |
| 主控 | ESP32-S31-WROOM-1（RISC-V 双核 320MHz，Wi-Fi 6 + BT 5.4） |
| 屏幕 | 1.85" 圆形 QSPI ST77916 360×360 |
| 触摸 | CST816S（I2C 0x2A，INT GPIO42） |
| 音频 | ES8389 codec（I2C 0x20，MCLK-less）+ NS4150B 功放 |
| 摄像头 | SC101IOT DVP（SCCB 0x68，与 codec/触摸共用 I2C） |
| 交互 | BOOT 键 GPIO61、触摸屏 |

## 引脚

见 [config.h](config.h)。要点：

- I2C：SDA=GPIO1，SCL=GPIO0（codec / 触摸 / 摄像头 SCCB 共用一条总线）
- I2S：BCLK=16，WS=18，DOUT=35（→codec），DIN=19（←codec），PA=2，无 MCLK
- 电源使能（低有效）：CODEC=GPIO17、LCD=GPIO9、CAM=GPIO10
- QSPI 屏：SCLK=39，D0=38，D1=44，D2=37，D3=45，DC=40，CS=43，RST=60，BL=46
- 摄像头 DVP：D0..D7=57/49/50/48/56/55/54/47，PCLK=51，HREF=53，VSYNC=8，XCLK=52（24MHz）

## 功能

- 小智语音对话、表情（EmoteDisplay，360×360 资源同 ESP-VoCat）
- **MCP 拍照识图**：`self.camera.take_photo`（通用工具，板级 `GetCamera()` 返回 EspVideo 后自动注册）
- **蓝牙音乐**：A2DP Sink，手机配对后当蓝牙音箱放歌；语音对话时自动让出 I2S，回到空闲自动恢复
- 蓝牙相关 MCP 工具：`self.bluetooth.get_status` / `self.bluetooth.set_discoverable` / `self.bluetooth.media_control`

## 编译

需要 ESP-IDF `release/v6.1` 或更新（`esp32s31` target，本仓库在 IDF master 6.2.0 上验证）。

```bash
source ~/esp/esp-idf-master/export.sh
python3 scripts/build.py espressif/esp-vocat-s31
```

或手工：

```bash
idf.py set-target esp32s31
idf.py menuconfig    # Xiaozhi Assistant -> Board Type -> Espressif ESP-VoCat S31 (喵伴)
idf.py build
```

## 烧录

S31 的 flash 偏移与 S3 不同，`idf.py flash` 生成的是：

```
0x2000   bootloader/bootloader.bin
0x8000   partition_table/partition-table.bin
0xd000   ota_data_initial.bin
0x20000  xiaozhi.bin
0x800000 configured_assets.bin
```

USB 串口烧录建议 921600 波特率（115200 在 ~6MB 处容易掉线）。

## 已知限制 / 待硬件验证

- I2S 位宽：xiaozhi 音频链路是单声道 16bit，而官方 S31 固件用 32bit slot；若实测无声或杂音，需要给 ES8389 增加 32bit slot 配置。
- QSPI 屏初始化序列沿用 ESP-VoCat（S3）的 ST77916 厂商表，若花屏/黑屏需要按 S31 面板实测调整。
- 蓝牙音乐与 Wi-Fi 共存、A2DP 音质/延迟需实测。
- 未实现：SD 卡、BMI270、电池电量、在线音乐搜索。
