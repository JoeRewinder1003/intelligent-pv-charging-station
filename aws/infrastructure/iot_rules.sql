-- =============================================================
-- IoT Rules SQL Statements
-- station_cloud_v1
-- =============================================================
-- These SQL statements are entered in the AWS IoT Core console
-- when creating each IoT Rule.
-- Each rule listens on a topic and triggers a Lambda function.
-- =============================================================


-- -------------------------------------------------------------
-- Rule 1: TelemetryRule
-- -------------------------------------------------------------
-- Triggered by: ESP32 telemetry published every 15-30 seconds.
-- Topic (Basic Ingest): $aws/rules/TelemetryRule/station/+/telemetry/raw
-- Target: StationTelemetryProcessorLambda
-- -------------------------------------------------------------

SELECT
    station_id,
    timestamp,
    battery_soc,
    battery_voltage,
    battery_current,
    p_net,
    local_irradiance,
    shortwave_radiation,
    cloud_cover,
    precipitation_probability,
    demand_index,
    active_outputs,
    tracking_angle,
    fault_state,
    topic() AS mqtt_topic
FROM
    '$aws/rules/TelemetryRule/station/+/telemetry/raw'


-- -------------------------------------------------------------
-- Rule 2: FaultRule
-- -------------------------------------------------------------
-- Triggered by: ESP32 fault or alert events (event-driven).
-- Topic (Basic Ingest): $aws/rules/FaultRule/station/+/events/fault
-- Target: StationDiagnosticsLambda
-- -------------------------------------------------------------

SELECT
    station_id,
    timestamp,
    fault_type,
    severity,
    description,
    battery_soc,
    topic() AS mqtt_topic
FROM
    '$aws/rules/FaultRule/station/+/events/fault'


-- -------------------------------------------------------------
-- Rule 3: ChargingRule
-- -------------------------------------------------------------
-- Triggered by: ESP32 charging session start or end events.
-- Topic (Basic Ingest): $aws/rules/ChargingRule/station/+/events/charging
-- Target: StationDiagnosticsLambda
-- -------------------------------------------------------------

SELECT
    station_id,
    timestamp,
    event_type,
    output_id,
    duration_minutes,
    energy_wh,
    topic() AS mqtt_topic
FROM
    '$aws/rules/ChargingRule/station/+/events/charging'


-- -------------------------------------------------------------
-- Rule 4: HeartbeatRule
-- -------------------------------------------------------------
-- Triggered by: ESP32 heartbeat published every 60 seconds.
-- Topic (Basic Ingest): $aws/rules/HeartbeatRule/station/+/status/heartbeat
-- Target: StationDiagnosticsLambda
-- Updates connection_status = online in StationState table.
-- -------------------------------------------------------------

SELECT
    station_id,
    timestamp,
    firmware_version,
    uptime_seconds,
    topic() AS mqtt_topic
FROM
    '$aws/rules/HeartbeatRule/station/+/status/heartbeat'


-- -------------------------------------------------------------
-- Rule 5: AckRule
-- -------------------------------------------------------------
-- Triggered by: ESP32 command acknowledgment (event-driven).
-- Topic (Basic Ingest): $aws/rules/AckRule/station/+/ack/command
-- Target: StationDiagnosticsLambda
-- Updates ack_status in StationCommands table.
-- -------------------------------------------------------------

SELECT
    station_id,
    command_id,
    status,
    timestamp,
    topic() AS mqtt_topic
FROM
    '$aws/rules/AckRule/station/+/ack/command'


-- =============================================================
-- Notes
-- =============================================================
-- 1. Basic Ingest topics ($aws/rules/...) do not count as
--    standard MQTT messages, which reduces IoT Core cost.
--
-- 2. The topic() function extracts the full topic string so
--    Lambda can parse the station_id from it if needed.
--
-- 3. All rules use a single Lambda target per rule to keep
--    the architecture simple and cost-effective.
--
-- 4. Error actions should be configured in each rule to write
--    failed messages to a CloudWatch Logs group for debugging.
-- =============================================================
