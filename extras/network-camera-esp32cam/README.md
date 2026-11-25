# ESP32-CAM node for XiaoZhi

Target: **AI Thinker ESP32-CAM with OV2640 camera and working PSRAM**.

This is a separate firmware for the camera board. It does not replace or modify the main XiaoZhi ESP32 firmware or its audio path.

## Wi-Fi setup (no source-code editing required)

1. Flash the separate camera firmware to the AI Thinker ESP32-CAM.
2. On first boot, connect a phone to the camera's setup Wi-Fi:
   - Network: `XiaoZhiCam-Setup`
   - Password: `camsetup123`
3. Open `http://192.168.4.1` in the phone browser.
4. Enter the **2.4 GHz Wi-Fi** name and password used by the main XiaoZhi board.
5. Save. The camera restarts and joins that network.

If saved Wi-Fi credentials stop working, the camera returns to setup mode. Credentials are stored locally in ESP32 NVS Preferences; they are not embedded in the public source or firmware.

## Endpoints

Once connected to the same local Wi-Fi network as XiaoZhi:

- `GET /health`: reports camera-node status.
- `GET /capture`: returns one 320x240 JPEG frame.

The camera IP is printed at 115200 baud on serial during startup. The main XiaoZhi camera URL should be set to:

`http://CAMERA_IP/capture`

Replace `CAMERA_IP` with the IP printed by the camera.

## Build

The dedicated camera workflow is also installed on the integration branch so pull-request checks compile this separate target.

A dedicated GitHub Actions workflow builds this sketch for **AI Thinker ESP32-CAM, 4 MB flash, PSRAM enabled, DIO, 40 MHz**. It produces an independent camera application binary. The build artifact is separate from the main XiaoZhi firmware.

## Hardware and network notes

- Use an AI Thinker ESP32-CAM board with OV2640 and working PSRAM.
- Power the camera board from a stable 5 V supply suitable for its current peaks.
- The camera endpoint is intended for a trusted local network only. Do not expose it to the public internet or configure router port forwarding.
- This firmware provides image capture and HTTP delivery only; vision interpretation is performed by the main XiaoZhi/vision service.
