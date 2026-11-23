# ESP32-CAM node for XiaoZhi

Target: AI Thinker ESP32-CAM with OV2640 and PSRAM.

## Arduino IDE setup

1. Install Espressif's ESP32 Arduino core.
2. Select **AI Thinker ESP32-CAM** as the board.
3. Set PSRAM to enabled if the selected core exposes that option.
4. Open `network_camera.ino`.
5. Replace `YOUR_WIFI_NAME` and `YOUR_WIFI_PASSWORD` with the 2.4 GHz Wi-Fi credentials used by the XiaoZhi board.
6. Upload using an appropriate USB-to-TTL adapter and the board's required boot mode.

## Endpoints

- `GET /health`: reports whether the node is online.
- `GET /capture`: returns one 320x240 JPEG frame.

After boot, the Serial Monitor at 115200 baud prints the assigned IP. Open `http://CAMERA_IP/health` from a device on the same LAN to check it.

## Notes

- The endpoint is intended for a trusted local Wi-Fi network only. Do not expose it to the public internet or configure router port forwarding.
- Power the ESP32-CAM from a stable 5 V supply capable of handling its current peaks.
- This node firmware is separate from XiaoZhi's main firmware. It does not alter the main board's audio configuration.
