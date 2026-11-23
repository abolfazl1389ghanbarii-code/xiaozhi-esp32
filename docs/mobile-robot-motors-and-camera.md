# XiaoZhi mobile robot: motors + network camera

This feature combines the existing XiaoZhi voice assistant and OLED eyes with:
- two DC geared drive motors controlled by a TB6612FNG dual H-bridge;
- a separate AI-Thinker ESP32-CAM/OV2640 node that serves JPEG frames over the local Wi-Fi network.

## Responsibilities

- Main ESP32-WROOM-32: XiaoZhi voice/audio, OLED, MCP tool dispatch, motor control, and forwarding JPEG bytes to the existing vision service.
- ESP32-CAM: camera capture only; joins the same trusted 2.4 GHz LAN and serves `/capture`.
- TB6612FNG: drives the two motors. Motor supply is separate from the ESP32 3.3 V rail; grounds are common.

The main ESP32 has no PSRAM. It downloads one bounded JPEG frame into internal 8-bit-capable heap and forwards the original JPEG bytes. It does not decode or re-encode the image.

## Motor GPIO assignment (ESP32-WROOM-32, no PSRAM)

| TB6612FNG | Main ESP32 GPIO |
|---|---:|
| AIN1 | 16 |
| AIN2 | 17 |
| PWMA | 18 |
| BIN1 | 19 |
| BIN2 | 21 |
| PWMB | 22 |
| STBY | 23 |

Existing audio and OLED pins remain unchanged: MAX9814 GPIO34; PCM5102A GPIO33/14/27; OLED GPIO4/15; BOOT GPIO0.

This assignment is specific to the no-PSRAM ESP32-WROOM-32 board. Do not reuse it blindly on an ESP32 module with PSRAM or another board revision.

## Motion API and safety limits

MCP tools:
- `self.motor.move(direction, speed, duration_ms)`
- `self.motor.stop()`

Directions: `forward`, `backward`, `left`, `right`.
Firmware rejects speed outside 1–40% and durations outside 100–1500 ms. A local one-shot timer stops both motors even if a later voice/MCP command is not received. STBY is held low at startup and after stopping.

The firmware initializes the driver outputs in standby. Until the driver and motors are physically installed and wired correctly, leave the motor connector disconnected.

Before selecting the TB6612FNG, verify each motor's stall current. Its current rating must not be exceeded, including at startup or if a wheel is blocked. If stall current is too high, choose a stronger driver instead.

## Power and wiring cautions

- TB6612FNG VCC: 3.3 V logic from the main ESP32.
- TB6612FNG VM: separate motor supply appropriate for the selected 3–6 V motors.
- Grounds: common between the motor supply, driver, and main ESP32.
- Never power motors from the ESP32 3V3 pin.
- Keep motor wiring away from the MAX9814 analog signal path; add appropriate motor suppression and supply decoupling.
- Test with wheels lifted and a current-limited supply before placing the robot on the floor.

## Camera node

See `extras/network-camera-esp32cam/README.md`. The camera endpoint has no authentication and must remain on a trusted LAN. Do not expose it through router port forwarding.

## Validation status

The code is not considered ready to flash until:
1. MCP host tests pass;
2. the `bread-compact-esp32` ESP-IDF build passes;
3. the ESP32-CAM sketch is compiled for AI Thinker ESP32-CAM;
4. hardware direction, current draw, thermal behavior, and audio-noise effects are checked with the actual motors and driver.
