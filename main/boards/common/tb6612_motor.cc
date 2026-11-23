#include "tb6612_motor.h"

#include <driver/ledc.h>
#include <esp_err.h>
#include <esp_log.h>
#include <algorithm>
#include <cstring>

#define TAG "Tb6612Motor"
#define MOTOR_PWM_FREQUENCY_HZ 20000
#define MOTOR_PWM_RESOLUTION LEDC_TIMER_8_BIT
#define MOTOR_PWM_MAX_DUTY 255
#define MOTOR_MAX_SPEED_PERCENT 40
#define MOTOR_MAX_DURATION_MS 1500

Tb6612MotorController::Tb6612MotorController(
    gpio_num_t ain1, gpio_num_t ain2, gpio_num_t pwma,
    gpio_num_t bin1, gpio_num_t bin2, gpio_num_t pwmb,
    gpio_num_t standby)
    : ain1_(ain1), ain2_(ain2), pwma_(pwma),
      bin1_(bin1), bin2_(bin2), pwmb_(pwmb), standby_(standby) {
    mutex_ = xSemaphoreCreateMutex();
    initialized_ = Initialize();
    if (!initialized_) {
        ESP_LOGE(TAG, "Motor controller initialization failed; outputs remain disabled");
    }
}

Tb6612MotorController::~Tb6612MotorController() {
    Stop();
    if (stop_timer_ != nullptr) {
        esp_timer_delete(stop_timer_);
        stop_timer_ = nullptr;
    }
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}

bool Tb6612MotorController::Initialize() {
    if (mutex_ == nullptr) {
        return false;
    }

    gpio_config_t output_config = {};
    output_config.pin_bit_mask =
        (1ULL << ain1_) | (1ULL << ain2_) | (1ULL << bin1_) |
        (1ULL << bin2_) | (1ULL << standby_);
    output_config.mode = GPIO_MODE_OUTPUT;
    output_config.pull_up_en = GPIO_PULLUP_DISABLE;
    output_config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    output_config.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&output_config) != ESP_OK) {
        return false;
    }

    // Keep the motor driver in standby before configuring PWM.
    gpio_set_level(standby_, 0);
    gpio_set_level(ain1_, 0);
    gpio_set_level(ain2_, 0);
    gpio_set_level(bin1_, 0);
    gpio_set_level(bin2_, 0);

    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = MOTOR_PWM_RESOLUTION;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = MOTOR_PWM_FREQUENCY_HZ;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) {
        return false;
    }

    ledc_channel_config_t channel_a = {};
    channel_a.gpio_num = pwma_;
    channel_a.speed_mode = LEDC_LOW_SPEED_MODE;
    channel_a.channel = LEDC_CHANNEL_0;
    channel_a.intr_type = LEDC_INTR_DISABLE;
    channel_a.timer_sel = LEDC_TIMER_0;
    channel_a.duty = 0;
    channel_a.hpoint = 0;
    if (ledc_channel_config(&channel_a) != ESP_OK) {
        return false;
    }

    ledc_channel_config_t channel_b = channel_a;
    channel_b.gpio_num = pwmb_;
    channel_b.channel = LEDC_CHANNEL_1;
    if (ledc_channel_config(&channel_b) != ESP_OK) {
        return false;
    }

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &Tb6612MotorController::StopTimerCallback;
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "motor_stop";
    if (esp_timer_create(&timer_args, &stop_timer_) != ESP_OK) {
        return false;
    }

    return true;
}

void Tb6612MotorController::SetMotorDirection(bool a_forward, bool b_forward) {
    gpio_set_level(ain1_, a_forward ? 1 : 0);
    gpio_set_level(ain2_, a_forward ? 0 : 1);
    gpio_set_level(bin1_, b_forward ? 1 : 0);
    gpio_set_level(bin2_, b_forward ? 0 : 1);
}

void Tb6612MotorController::SetSpeed(int speed_percent) {
    const int bounded = std::clamp(speed_percent, 0, MOTOR_MAX_SPEED_PERCENT);
    const uint32_t duty = static_cast<uint32_t>(bounded * MOTOR_PWM_MAX_DUTY / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

void Tb6612MotorController::StopHardware() {
    SetSpeed(0);
    gpio_set_level(standby_, 0);
    deadline_us_ = 0;
}

void Tb6612MotorController::StopTimerCallback(void* arg) {
    auto* self = static_cast<Tb6612MotorController*>(arg);
    if (self == nullptr || self->mutex_ == nullptr) {
        return;
    }

    if (xSemaphoreTake(self->mutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }

    // A newer movement command may have replaced the timer deadline.
    const int64_t remaining = self->deadline_us_ - esp_timer_get_time();
    if (remaining <= 0) {
        self->StopHardware();
    } else if (self->stop_timer_ != nullptr) {
        esp_timer_start_once(self->stop_timer_, remaining);
    }
    xSemaphoreGive(self->mutex_);
}

bool Tb6612MotorController::Move(const std::string& direction, int speed_percent,
                                 int duration_ms) {
    if (!initialized_ || mutex_ == nullptr || stop_timer_ == nullptr) {
        return false;
    }
    if (speed_percent < 1 || speed_percent > MOTOR_MAX_SPEED_PERCENT ||
        duration_ms < 100 || duration_ms > MOTOR_MAX_DURATION_MS) {
        ESP_LOGW(TAG, "Rejected unsafe movement parameters");
        return false;
    }

    bool a_forward = true;
    bool b_forward = true;
    if (direction == "forward") {
        a_forward = true; b_forward = true;
    } else if (direction == "backward") {
        a_forward = false; b_forward = false;
    } else if (direction == "left") {
        a_forward = false; b_forward = true;
    } else if (direction == "right") {
        a_forward = true; b_forward = false;
    } else {
        ESP_LOGW(TAG, "Rejected unknown direction: %s", direction.c_str());
        return false;
    }

    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    if (esp_timer_is_active(stop_timer_)) {
        esp_timer_stop(stop_timer_);
    }
    SetMotorDirection(a_forward, b_forward);
    SetSpeed(speed_percent);
    gpio_set_level(standby_, 1);
    deadline_us_ = esp_timer_get_time() + static_cast<int64_t>(duration_ms) * 1000;
    const esp_err_t timer_result = esp_timer_start_once(
        stop_timer_, static_cast<uint64_t>(duration_ms) * 1000);
    if (timer_result != ESP_OK) {
        StopHardware();
        xSemaphoreGive(mutex_);
        ESP_LOGE(TAG, "Could not arm movement timeout: %s", esp_err_to_name(timer_result));
        return false;
    }

    xSemaphoreGive(mutex_);
    ESP_LOGI(TAG, "Movement started: %s, speed=%d%%, duration=%dms",
             direction.c_str(), speed_percent, duration_ms);
    return true;
}

void Tb6612MotorController::Stop() {
    if (!initialized_ || mutex_ == nullptr) {
        return;
    }
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (stop_timer_ != nullptr && esp_timer_is_active(stop_timer_)) {
        esp_timer_stop(stop_timer_);
    }
    StopHardware();
    xSemaphoreGive(mutex_);
}
