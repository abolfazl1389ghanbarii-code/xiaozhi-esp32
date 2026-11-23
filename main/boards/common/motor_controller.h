#ifndef MOTOR_CONTROLLER_H
#define MOTOR_CONTROLLER_H

#include <string>

class MotorController {
public:
    virtual ~MotorController() = default;
    virtual bool Move(const std::string& direction, int speed_percent, int duration_ms) = 0;
    virtual void Stop() = 0;
};

#endif  // MOTOR_CONTROLLER_H
