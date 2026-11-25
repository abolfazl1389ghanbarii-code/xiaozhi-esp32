# 编译配置命令

**配置编译目标为 ESP32：**

```bash
idf.py set-target esp32
```

**打开 menuconfig：**

```bash
idf.py menuconfig
```

**选择板子：**

```
Xiaozhi Assistant -> Board Type -> 面包板 ESP32 DevKit
```

**编译：**

```bash
idf.py build
```

## Integrated mobile robot firmware

The `bread-compact-esp32` target in the mobile-robot integration branch keeps the MAX9814/PCM5102A audio path and SSD1306 OLED, and adds the network-camera adapter plus TB6612 motor controller.

For the classic ESP32-WROOM-32 without PSRAM, the integration build selects the experimental ESP-SR WakeNet10s model `wn10s_hilexin` (phrase: **"Hi Lexin"**). This is a technical wake-word test, not the requested Persian phrase "سلام XiaoZhi". Wake-word recognition must be validated on the actual microphone and board.

The separate AI-Thinker ESP32-CAM firmware is built from `extras/network-camera-esp32cam/network_camera.ino`; it is not flashed to the main ESP32.
