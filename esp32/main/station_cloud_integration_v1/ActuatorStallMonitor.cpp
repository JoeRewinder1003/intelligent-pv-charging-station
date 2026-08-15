#include "ActuatorStallMonitor.h"

ActuatorStallMonitor::ActuatorStallMonitor(
    const ActuatorStallConfig &config) : config_(config)
{
    if (!isfinite(config_.evaluationWindowSeconds) ||
        config_.evaluationWindowSeconds <= 0.0f)
    {
        config_.evaluationWindowSeconds = 2.0f;
    }

    if (!isfinite(config_.minimumPositionChangeMm) ||
        config_.minimumPositionChangeMm <= 0.0f)
    {
        config_.minimumPositionChangeMm = 1.0f;
    }

    reset();
}

void ActuatorStallMonitor::reset()
{
    state_.masterStallDetected = false;
    state_.slaveStallDetected = false;

    state_.masterObservedChangeMm = 0.0f;
    state_.slaveObservedChangeMm = 0.0f;

    masterReferencePositionMm_ = 0.0f;
    slaveReferencePositionMm_ = 0.0f;

    masterEvaluationTimeSeconds_ = 0.0f;
    slaveEvaluationTimeSeconds_ = 0.0f;

    masterMonitoringActive_ = false;
    slaveMonitoringActive_ = false;

    masterPreviousCommand_ = ActuatorCommand::STOP;
    slavePreviousCommand_ = ActuatorCommand::STOP;
}

void ActuatorStallMonitor::update(
    float deltaTimeSeconds,
    ActuatorCommand masterCommand,
    float masterPositionMm,
    bool masterAtMinimumLimit,
    bool masterAtMaximumLimit,
    ActuatorCommand slaveCommand,
    float slavePositionMm,
    bool slaveAtMinimumLimit,
    bool slaveAtMaximumLimit)
{
    if (!isfinite(deltaTimeSeconds) ||
        deltaTimeSeconds <= 0.0f)
    {
        return;
    }

    const bool masterAtExpectedLimit =
        (masterCommand == ActuatorCommand::EXTEND &&
         masterAtMaximumLimit) ||
        (masterCommand == ActuatorCommand::RETRACT &&
         masterAtMinimumLimit);

    if (masterCommand == ActuatorCommand::STOP ||
        masterAtExpectedLimit ||
        !isfinite(masterPositionMm))
    {
        masterMonitoringActive_ = false;
        masterEvaluationTimeSeconds_ = 0.0f;
        masterReferencePositionMm_ = masterPositionMm;
    }
    else
    {
        if (!masterMonitoringActive_ ||
            masterCommand != masterPreviousCommand_)
        {
            masterMonitoringActive_ = true;
            masterEvaluationTimeSeconds_ = 0.0f;
            masterReferencePositionMm_ = masterPositionMm;
        }
        else
        {
            masterEvaluationTimeSeconds_ += deltaTimeSeconds;

            if (masterEvaluationTimeSeconds_ >=
                config_.evaluationWindowSeconds)
            {
                state_.masterObservedChangeMm =
                    fabsf(masterPositionMm - masterReferencePositionMm_);

                if (state_.masterObservedChangeMm <
                    config_.minimumPositionChangeMm)
                {
                    state_.masterStallDetected = true;
                }

                masterReferencePositionMm_ = masterPositionMm;
                masterEvaluationTimeSeconds_ = 0.0f;
            }
        }
    }

    masterPreviousCommand_ = masterCommand;

    const bool slaveAtExpectedLimit =
        (slaveCommand == ActuatorCommand::EXTEND &&
         slaveAtMaximumLimit) ||
        (slaveCommand == ActuatorCommand::RETRACT &&
         slaveAtMinimumLimit);

    if (slaveCommand == ActuatorCommand::STOP ||
        slaveAtExpectedLimit ||
        !isfinite(slavePositionMm))
    {
        slaveMonitoringActive_ = false;
        slaveEvaluationTimeSeconds_ = 0.0f;
        slaveReferencePositionMm_ = slavePositionMm;
    }
    else
    {
        if (!slaveMonitoringActive_ ||
            slaveCommand != slavePreviousCommand_)
        {
            slaveMonitoringActive_ = true;
            slaveEvaluationTimeSeconds_ = 0.0f;
            slaveReferencePositionMm_ = slavePositionMm;
        }
        else
        {
            slaveEvaluationTimeSeconds_ += deltaTimeSeconds;

            if (slaveEvaluationTimeSeconds_ >=
                config_.evaluationWindowSeconds)
            {
                state_.slaveObservedChangeMm =
                    fabsf(slavePositionMm - slaveReferencePositionMm_);

                if (state_.slaveObservedChangeMm <
                    config_.minimumPositionChangeMm)
                {
                    state_.slaveStallDetected = true;
                }

                slaveReferencePositionMm_ = slavePositionMm;
                slaveEvaluationTimeSeconds_ = 0.0f;
            }
        }
    }

    slavePreviousCommand_ = slaveCommand;
}

const ActuatorStallState &
ActuatorStallMonitor::state() const
{
    return state_;
}