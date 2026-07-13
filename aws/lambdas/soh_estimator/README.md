# soh_estimator

## Status

Pending design and implementation.

## Purpose

Estimate battery State of Health using Victron SmartShunt measurements
and historical telemetry stored in AWS.

## Initial available inputs

Frequent telemetry:

- Battery voltage
- Battery current
- Battery power
- Battery SOC

Low-frequency SmartShunt diagnostics:

- Consumed amp-hours (`CE` / `consumed_ah`)

## Planned cloud-derived indicators

The following indicators should be calculated in AWS whenever possible:

- Charged energy
- Discharged energy
- Accumulated energy throughput
- Equivalent full cycles
- Time spent in different SOC ranges
- Voltage response under comparable loads
- Preliminary usable-capacity estimate

## Current limitations

- No definitive SOH estimation formula has been selected.
- The minimum historical observation window has not been defined.
- The real VE.Direct data path has not yet been integrated into the AWS firmware.
- `BatterySOHHistory` has not yet been created in AWS.
- Any early SOH result must be explicitly marked as preliminary.
- Any early SOH result must include a confidence indicator.

## Payload constraint

Additional SmartShunt fields must not be inserted indiscriminately into
the frequent telemetry payload.

Low-frequency diagnostics should use the MQTT topic:

```text
station/{station_id}/battery_diagnostics
```

The initial diagnostic payload should contain only the minimum fields
required for future SOH estimation.

## Current implementation status

- Folder created.
- Design documented.
- Lambda code not yet implemented.
- IAM role not yet created.
- DynamoDB table not yet created.
- IoT rule not yet created.
