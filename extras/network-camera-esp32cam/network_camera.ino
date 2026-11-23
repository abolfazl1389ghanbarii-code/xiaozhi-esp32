/*
 * XiaoZhi network camera node
 * Target: AI Thinker ESP32-CAM + OV2640
 *
 * Joins the same trusted 2.4 GHz Wi-Fi network as the XiaoZhi ESP32.
 * GET /health  -> JSON status
 * GET /capture -> one JPEG frame (QVGA)
 *
 * Keep this endpoint on a trusted local network; do not port-forward it.
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>

#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

static constexpr char kTag[] = "XiaoZhiCam";
WebServer server(80);

// AI Thinker ESP32-CAM pin map
static camera_config_t camera_config = {
    .pin_pwdn = 32,
    .pin_reset = -1,
    .pin_xclk = 0,
    .pin_sccb_sda = 26,
    .pin_sccb_scl = 27,
    .pin_d7 = 35,
    .pin_d6 = 34,
    .pin_d5 = 39,
    .pin_d4 = 36,
    .pin_d3 = 21,
    .pin_d2 = 19,
    .pin_d1 = 18,
    .pin_d0 = 5,
    .pin_vsync = 25,
    .pin_href = 23,
    .pin_pclk = 22,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 16,
    .fb_count = 1,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST,
};

static void handleHealth() {
    String json = "{\"ok\":true,\"camera\":\"ov2640\",\"format\":\"jpeg\",\"resolution\":\"320x240\"}";
    server.send(200, "application/json", json);
}

static void handleCapture() {
    camera_fb_t* frame = esp_camera_fb_get();
    if (frame == nullptr) {
        server.send(503, "text/plain", "Camera capture failed");
        return;
    }

    if (frame->format != PIXFORMAT_JPEG || frame->len < 4 ||
        frame->buf[0] != 0xFF || frame->buf[1] != 0xD8 ||
        frame->buf[frame->len - 2] != 0xFF || frame->buf[frame->len - 1] != 0xD9) {
        esp_camera_fb_return(frame);
        server.send(500, "text/plain", "Invalid JPEG frame");
        return;
    }

    // Send headers first, then stream the frame directly from the camera buffer.
    server.setContentLength(frame->len);
    server.send(200, "image/jpeg", "");
    WiFiClient client = server.client();
    size_t sent = 0;
    while (client.connected() && sent < frame->len) {
        size_t n = client.write(frame->buf + sent, frame->len - sent);
        if (n == 0) break;
        sent += n;
        delay(0);
    }

    Serial.printf("[%s] JPEG %u bytes, sent %u bytes\n",
                  kTag, static_cast<unsigned>(frame->len), static_cast<unsigned>(sent));
    esp_camera_fb_return(frame);
}

void setup() {
    Serial.begin(115200);
    delay(300);

    if (!psramFound()) {
        Serial.println("[XiaoZhiCam] ERROR: PSRAM not detected. Use an ESP32-CAM board with PSRAM.");
        while (true) delay(1000);
    }

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        Serial.printf("[XiaoZhiCam] Camera init failed: 0x%x\n", err);
        while (true) delay(1000);
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[XiaoZhiCam] Connecting to Wi-Fi");
    const unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) {
        delay(400);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[XiaoZhiCam] ERROR: Wi-Fi connection timed out; check credentials.");
        while (true) delay(1000);
    }

    server.on("/health", HTTP_GET, handleHealth);
    server.on("/capture", HTTP_GET, handleCapture);
    server.begin();

    Serial.printf("[XiaoZhiCam] Ready. Camera IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[XiaoZhiCam] Test: http://%s/health\n", WiFi.localIP().toString().c_str());
    Serial.printf("[XiaoZhiCam] JPEG: http://%s/capture\n", WiFi.localIP().toString().c_str());
}

void loop() {
    server.handleClient();
    delay(2);
}
