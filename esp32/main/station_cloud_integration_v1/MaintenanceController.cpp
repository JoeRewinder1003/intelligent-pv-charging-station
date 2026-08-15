#include "MaintenanceController.h"

MaintenanceController::MaintenanceController()
{
    state_.controlMode = StationControlMode::AUTOMATIC;
    state_.actuatorMovementLocked = false;
    state_.output1Locked = false;
    state_.output2Locked = false;
    state_.output3Locked = false;
}

void MaintenanceController::enterMaintenance()
{
    state_.controlMode = StationControlMode::MAINTENANCE;
}

void MaintenanceController::exitMaintenance()
{
    state_.controlMode = StationControlMode::AUTOMATIC;
}

void MaintenanceController::lockActuatorMovement()
{
    state_.actuatorMovementLocked = true;
}

void MaintenanceController::unlockActuatorMovement()
{
    state_.actuatorMovementLocked = false;
}

void MaintenanceController::lockOutput(uint8_t outputNumber)
{
    if (outputNumber == 1)
    {
        state_.output1Locked = true;
    }
    else if (outputNumber == 2)
    {
        state_.output2Locked = true;
    }
    else if (outputNumber == 3)
    {
        state_.output3Locked = true;
    }
}

void MaintenanceController::unlockOutput(uint8_t outputNumber)
{
    if (outputNumber == 1)
    {
        state_.output1Locked = false;
    }
    else if (outputNumber == 2)
    {
        state_.output2Locked = false;
    }
    else if (outputNumber == 3)
    {
        state_.output3Locked = false;
    }
}

bool MaintenanceController::outputLocked(
    uint8_t outputNumber) const
{
    if (outputNumber == 1)
    {
        return state_.output1Locked;
    }

    if (outputNumber == 2)
    {
        return state_.output2Locked;
    }

    if (outputNumber == 3)
    {
        return state_.output3Locked;
    }

    return true;
}

bool MaintenanceController::maintenanceModeActive() const
{
    return state_.controlMode == StationControlMode::MAINTENANCE;
}

bool MaintenanceController::manualActuatorControlAllowed() const
{
    return maintenanceModeActive() &&
           !state_.actuatorMovementLocked;
}

bool MaintenanceController::automaticTrackingAllowed() const
{
    return !maintenanceModeActive() &&
           !state_.actuatorMovementLocked;
}

const MaintenanceState &
MaintenanceController::state() const
{
    return state_;
}
