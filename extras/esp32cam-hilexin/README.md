# ESP32-CAM Hi Lexin wake node

Status: architecture and dependency investigation. This is not a flashable firmware yet.

## Hardware target

- Board: AI Thinker ESP32-CAM (classic ESP32, OV2640, PSRAM)
- Microphone candidate: MAX4466 analog microphone module (not approved for purchase/wiring until ADC audio validation)
- Wake phrase: "Hi Lexin" / Espressif model family `hilexin`
- microSD: intentionally unused
- Camera node communicates with the existing XiaoZhi WROOM board over 3.3 V GPIO and common ground.

## Model selection

The Espressif ESP-SR model catalogue lists `wn10_hilexin` and `wn10s_hilexin`. For the classic ESP32 without PIE/SIMD acceleration, the reduced WakeNet10s family is the candidate to test first. The actual model binary must be present in the ESP-SR model partition/package; compiling the interface alone is not enough.

## Critical peripheral constraint

On classic ESP32, ADC continuous mode uses I2S0 as its DMA FIFO. The OV2640 camera driver also uses I2S0 in camera mode. These cannot run concurrently.

The node therefore has mutually exclusive operating states:

1. **LISTEN**: camera deinitialized; Wi-Fi off; ADC continuous sampling on GPIO13 (ADC2_CH4); WakeNet consumes 16 kHz, mono, signed 16-bit PCM.
2. **WAKE_EVENT**: stop and deinitialize ADC continuous mode before initializing the camera.
3. **CAMERA_ONLINE**: initialize OV2640, connect Wi-Fi, serve the existing /health and /capture endpoints; no ADC sampling.
4. **RETURN_TO_LISTEN**: only after the main WROOM board reports that the conversation has ended, stop server/Wi-Fi, deinitialize camera, then restart ADC sampling and WakeNet.

Do not merely turn off Wi-Fi to solve the I2S0 conflict: the camera itself must be deinitialized before ADC continuous mode starts.

## Candidate pin allocation (not yet approved for wiring)

| ESP32-CAM AI Thinker pin | Candidate role | Notes |
|---|---|---|
| GPIO13 | MAX4466 OUT -> ADC2_CH4 | microSD signal pin; SD remains unused. Validate ADC2 continuous mode on the exact IDF version. |
| GPIO14 | CAM -> WROOM wake request | microSD clock pin; SD remains unused. |
| GPIO26 | WROOM input from CAM | Existing WROOM board config has this pin unallocated; firmware input handler still needs to be added. |
| GND | common ground | Required between both boards. |

Do not connect the GPIO signal wire until both firmware endpoints and safe startup levels are implemented. GPIO output must be 3.3 V only.

## Main-board compatibility

The current bread-compact ESP32 board source handles only the BOOT button on GPIO0 and calls `Application::GetInstance().ToggleChatState()`. It does not currently handle an external wake input on GPIO26. A minimal GPIO26 event handler is therefore required in a separate main-board firmware rebuild. Do not add WakeNet to the WROOM board and do not alter its MAX9814/PCM5102A audio path.

The reverse notification (conversation ended) must be based on the actual Application/chat state transition, not an arbitrary timeout. Implement a second signal or a defined bidirectional handshake only after locating a reliable state callback in the current source.

## Build approach

Use ESP-IDF with Espressif's `esp-sr` component and the ESP32 camera component rather than extending the current Arduino-only camera sketch with an unverified speech-recognition dependency. The existing camera setup portal and HTTP routes will be ported after the audio/WakeNet target builds.

## Validation gates

- [ ] Confirm the exact ESP32-CAM module and PSRAM at runtime.
- [ ] Build ESP-SR for target `esp32` with the `wn10s_hilexin` model assets included.
- [ ] Validate ADC2_CH4 continuous capture at 16 kHz while Wi-Fi and camera are off.
- [ ] Validate PCM centering/scaling and WakeNet detection from MAX4466.
- [ ] Validate camera init/deinit and Wi-Fi only after ADC teardown.
- [ ] Add GPIO wake event to the WROOM firmware without changing its audio codec path.
- [ ] Add a real chat-ended notification from WROOM back to CAM.
- [ ] Produce merged flash image and verify artifact before asking the user to flash.

## Safety / flashing

No wiring or flashing is requested by this document. Keep the current working camera and WROOM firmware backups unchanged until a tested replacement artifact is available.
