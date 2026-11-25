# Network camera for bread-compact-esp32 (no-PSRAM design)

## Goal

Add image capture to the existing bread-compact-esp32 XiaoZhi board by using a separate ESP32-CAM (AI Thinker + OV2640) as a JPEG-over-Wi-Fi camera node. Preserve the current MAX9814 microphone, PCM5102A DAC, OLED, and all audio pins and processing.

## Architecture

1. ESP32-CAM joins the same 2.4 GHz Wi-Fi network as XiaoZhi.
2. ESP32-CAM exposes a small HTTP endpoint, initially `GET /capture`, returning one JPEG image.
3. XiaoZhi's network camera adapter downloads that JPEG and passes it to the existing vision-explain endpoint as multipart form data (question + file).
4. The image is not decoded, resized, or JPEG-encoded on the no-PSRAM XiaoZhi board.

## Memory and reliability limits

- Request JPEG directly from ESP32-CAM; do not request RGB565/YUV.
- Enforce a strict maximum JPEG payload (initial target: 60 KiB) before allocating or reading.
- Allocate from ordinary 8-bit internal heap only; never use `MALLOC_CAP_SPIRAM`.
- Validate JPEG SOI/EOI markers and HTTP status before accepting a frame.
- Upload in bounded chunks through the existing HTTP abstraction; do not make a second full-size copy.
- Fail clearly if the camera is unreachable, returns an oversized/invalid image, or the vision service is not configured.

## XiaoZhi integration points

- Implement a `Camera` subclass for the HTTP camera node.
- Add a persisted camera URL setting, with a user-facing MCP configuration tool.
- Override `CompactWifiBoard::GetCamera()` to expose the adapter.
- Ensure `self.camera.take_photo` is registered even for this board's non-LVGL OLED build; keep display-only MCP tools under their existing LVGL guards.
- Add the new source file to `main/CMakeLists.txt`.

## Explicitly out of scope

- No edits to audio codec implementation, audio pin definitions, sample rates, microphone gain, I2S configuration, or wake/chat flow.
- No use of the main ESP32's camera peripheral or camera GPIO pins.
- No PSRAM requirement on the main XiaoZhi board.

## Validation plan

1. Compile the ESP32-CAM node firmware for AI Thinker ESP32-CAM.
2. Compile XiaoZhi for `bread-compact-esp32`.
3. Inspect the build diff to confirm no audio files or board audio configuration changed.
4. Runtime-test camera reachability, JPEG size/marker validation, photo capture, and vision response after the user has the ESP32-CAM module.
