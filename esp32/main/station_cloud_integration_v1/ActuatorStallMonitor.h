#pragma once

#include <Arduino.h>
#include "ActuatorEmulator.h"

struct ActuatorStallConfig
{
    // Provisional functional-validation parameters.
    // Final thresholds must be validated for the physical mechanism.
    float evaluationWindowSeconds = 2.0f;
    float minimumPositionChangeMm = 1.0f;
};

struct ActuatorStallState
{
    bool masterStallDetected = false;
    bool slaveStallDetected = false;
    float masterObservedChangeMm = 0.0f;
    float slaveObservedChangeMm = 0.0f;
};

class ActuatorStallMonitor
{
public:
    explicit ActuatorStallMonitor(
        const ActuatorStallConfig &config = ActuatorStallConfig());

    void reset();

    void update(
        float deltaTimeSeconds,
        ActuatorCommand masterCommand,
        float masterPositionMm,
        bool masterAtMinimumLimit,
        bool masterAtMaximumLimit,
        ActuatorCommand slaveCommand,
        float slavePositionMm,
        bool slaveAtMinimumLimit,
        bool slaveAtMaximumLimit);

    const ActuatorStallState &state() const;

private:
    ActuatorStallConfig config_;
    ActuatorStallState state_;

    float masterReferencePositionMm_ = 0.0f;
    float slaveReferencePositionMm_ = 0.0f;
    float masterEvaluationTimeSeconds_ = 0.0f;
    float slaveEvaluationTimeSeconds_ = 0.0f;

    bool masterMonitoringActive_ = false;
    bool slaveMonitoringActive_ = false;

    ActuatorCommand masterPreviousCommand_ = ActuatorCommand::STOP;
    ActuatorCommand slavePreviousCommand_ = ActuatorCommand::STOP;
};

