# soh_estimator

## Status

Design defined. Implementation pending.

## Purpose

Monitor battery health using measurements obtainable from the
Victron SmartShunt and calculate battery State of Health when a
valid maintenance capacity test is available.

The module uses two complementary battery-health mechanisms:

1. Apparent internal resistance trend during normal operation.
2. Capacity-based SOH assessment during maintenance.

These two results must not be interpreted as equivalent.

## 1. Battery Health Indicator

The Battery Health Indicator is based on the apparent internal
resistance of the battery bank estimated from comparable load-step
events.

Required SmartShunt-equivalent measurements:

- Battery voltage before the load step
- Battery voltage after the load step
- Battery current before the load step
- Battery current after the load step
- Battery SOC

The cloud calculates:

R_app = abs((V_after - V_before) / (I_after - I_before))

The ESP32 must not use the internal resistance configured in the
battery emulator as an estimator input.

The resistance value is used as a degradation trend indicator.
It must not be directly interpreted as battery SOH percentage.

Only valid load-step events with a sufficiently large current
change should be accepted. Samples should be compared under
similar operating conditions whenever possible.

The minimum accepted current step is initially a configurable
validation parameter and must not be presented as a
manufacturer-defined threshold.

## 2. Capacity-Based SOH

A stronger SOH assessment is performed through a maintenance
capacity test.

The capacity test uses SmartShunt amp-hour measurements during a
controlled battery discharge.

The cloud calculates:

SOH_percent = (measured_capacity_ah / nominal_capacity_ah) * 100

For the current physical battery bank:

Nominal bank capacity = 300 Ah

The detailed discharge conditions must follow the battery
manufacturer reference conditions selected for the maintenance
test.

The capacity-based SOH test is intended for scheduled maintenance,
approximately every 6 to 12 months, and may also be performed
after relevant maintenance or when battery degradation is
suspected.

## MQTT diagnostics topic

Low-frequency battery diagnostic events use:

station/{station_id}/battery_diagnostics

Battery diagnostic data must not be added indiscriminately to the
15-second telemetry payload.

Two diagnostic event types are planned:

- resistance_step
- capacity_test

## Planned resistance-step event

Minimum information:

- station_id
- timestamp
- event_type
- voltage_before_v
- voltage_after_v
- current_before_a
- current_after_a
- soc_percent

The apparent resistance is calculated in AWS, not on the ESP32.

## Planned capacity-test event

Minimum information:

- station_id
- timestamp
- event_type
- measured_capacity_ah
- nominal_capacity_ah

The SOH percentage is calculated in AWS.

## Current implementation status

- Battery degradation emulation implemented.
- Local load-step detector implemented.
- Local nominal resistance test validated.
- Local increased-resistance test validated.
- Cloud SOH estimator Lambda not yet implemented.
- Battery diagnostics MQTT publishing not yet implemented.
- IAM role not yet created.
- BatterySOHHistory DynamoDB table not yet created.
- Battery diagnostics IoT rule not yet created.
