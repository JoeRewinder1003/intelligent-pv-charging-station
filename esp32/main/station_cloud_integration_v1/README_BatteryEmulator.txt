BATTERY EMULATOR INTEGRATION
============================

Contents
--------
- station_cloud_integration_v1_battery_emulator.ino
- BatteryEmulator.h
- BatteryEmulator.cpp

The package does not include secrets.h or certificates.

Steps
-----
1. Copy the station_cloud_integration_v1_battery_emulator folder into the local project.
2. Copy the working secrets.h file from the previous sketch into this new folder.
3. Open station_cloud_integration_v1_battery_emulator.ino in Arduino IDE.
4. Select the same board and port used in the previous test.
5. Compile the sketch before uploading it to the ESP32.
6. If compilation succeeds, upload the firmware and open the Serial Monitor at 115200 baud.

Serial Monitor tests
--------------------
BATTERY STATUS
  Displays SOC, OCV, terminal voltage, current, power, and protection flags.

BATTERY POWER 300
  Forces a +300 W charging condition.

BATTERY POWER 0
  Forces an idle condition.

BATTERY POWER -250
  Forces a -250 W discharging condition.

BATTERY SCALE 60
  Accelerates battery time so that one simulated minute passes per real second.

BATTERY SOC 49
  Places the battery bank in the RESTRICTED state.

BATTERY SOC 19
  Places the battery bank in the CRITICAL state.

BATTERY SOC 56
  Verifies recovery to the NORMAL state.

BATTERY AUTO
  Returns to the provisional automatic station power balance.

STATUS
  Displays the combined safety state and battery state.

Initial behavior
----------------
- Default bank: three 12 V, 100 Ah batteries connected in parallel.
- Nominal energy: 3600 Wh.
- Initial SOC: 95%.
- Internal update interval: 1 s.
- MQTT publication interval: 15 s.
- Provisional automatic power balance:
  245.5 W PV - 5 W base load - two outputs at 71 W / 0.88.
- No physical device is activated.

Parameter classification
------------------------
Confirmed reference data:
- Felicity Solar G12V100AH, 12 V, 100 Ah.
- Approximate internal resistance: 5 mOhm per battery.

Configurable emulation assumptions:
- Generic VRLA/GEL OCV-SOC curve.
- Charging efficiency: 0.90.
- Discharging efficiency: 0.95.
- Wiring resistance: initially 0 Ohm.

Default protections
-------------------
- Enter RESTRICTED at 50% SOC while discharging.
- Return to NORMAL at 55% SOC while charging.
- Enter CRITICAL at 20% SOC.
- Leave CRITICAL at 25% SOC.
- Discharge overcurrent threshold: 35 A.
- Charge overcurrent threshold: 50 A.
- Absolute undervoltage threshold: 10.5 V.

Note
----
The additional fields in the battery JSON block are temporary validation fields.
Their impact on MQTT payload size will be evaluated later before deciding which
fields should remain in the final telemetry format.
