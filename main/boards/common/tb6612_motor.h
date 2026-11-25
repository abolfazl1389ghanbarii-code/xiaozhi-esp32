#ifndef TB6612_MOTOR_H
#define TB6612_MOTOR_H

#include "motor_controller.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class Tb6612MotorController final : public MotorController {
public:
    Tb6612MotorController(gpio_num_t ain1, gpio_num_t ain2, gpio_num_t pwma,
                          gpio_num_t bin1, gpio_num_t bin2, gpio_num_t pwmb,
                          gpio_num_t standby);
    ~Tb6612MotorController() override;

    bool Move(const std::string& direction, int speed_percent, int duration_ms) override;
    void Stop() override;

private:
    gpio_num_t ain1_, ain2_, pwma_;
    gpio_num_t bin1_, bin2_, pwmb_;
    gpio_num_t standby_;
    esp_timer_handle_t stop_timer_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    int64_t deadline_us_ = 0;
    bool initialized_ = false;

    bool Initialize();
    void SetMotorDirection(bool a_forward, bool b_forward);
    void SetSpeed(int speed_percent);
    void StopHardware();
    static void StopTimerCallback(void* arg);
};

#endif  // TB6612_MOTOR_H
