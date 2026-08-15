#pragma once

#include <Arduino.h>

enum class StationControlMode
{
    AUTOMATIC,
    MAINTENANCE
};

struct MaintenanceState
{
    StationControlMode controlMode = StationControlMode::AUTOMATIC;

    // Technician-controlled locks.
    bool actuatorMovementLocked = false;

    bool output1Locked = false;
    bool output2Locked = false;
    bool output3Locked = false;
};

class MaintenanceController
{
public:
    MaintenanceController();

    void enterMaintenance();
    void exitMaintenance();

    void lockActuatorMovement();
    void unlockActuatorMovement();

    void lockOutput(uint8_t outputNumber);
    void unlockOutput(uint8_t outputNumber);

    bool maintenanceModeActive() const;
    bool manualActuatorControlAllowed() const;
    bool automaticTrackingAllowed() const;
    bool outputLocked(uint8_t outputNumber) const;

    const MaintenanceState &state() const;

private:
    MaintenanceState state_;
};
