-- AWS IoT Core Rule SQL Statements
-- Project: ESP32 Solar Charging Station Cloud Backend
-- Recommended AWS IoT SQL version: 2016-03-23

-- ============================================================
-- 1. Telemetry Rule
-- Rule name: station_telemetry_to_lambda
-- Topic filter: station/+/telemetry
-- Target Lambda: telemetry_processor
-- Purpose:
--   Receives periodic telemetry messages from the ESP32 and
--   routes them to the telemetry_processor Lambda function.
-- ============================================================

SELECT
  *,
  topic(2) AS station_id_from_topic,
  topic() AS mqtt_topic,
  timestamp() AS received_at_epoch_ms
FROM
  'station/+/telemetry';


-- ============================================================
-- 2. Status Rule
-- Rule name: station_status_to_lambda
-- Topic filter: station/+/status
-- Target Lambda: diagnostics
-- Purpose:
--   Receives station status messages and routes them to the
--   diagnostics Lambda function.
-- ============================================================

SELECT
  *,
  topic(2) AS station_id_from_topic,
  topic() AS mqtt_topic,
  timestamp() AS received_at_epoch_ms
FROM
  'station/+/status';


-- ============================================================
-- 3. Fault Events Rule
-- Rule name: station_faults_to_lambda
-- Topic filter: station/+/faults
-- Target Lambda: diagnostics
-- Purpose:
--   Receives fault, restriction, invalid data, and safety lockout
--   events from the ESP32.
-- ============================================================

SELECT
  *,
  topic(2) AS station_id_from_topic,
  topic() AS mqtt_topic,
  timestamp() AS received_at_epoch_ms
FROM
  'station/+/faults';


-- ============================================================
-- 4. Command Acknowledgement Rule
-- Rule name: station_acks_to_lambda
-- Topic filter: station/+/acks
-- Target Lambda: diagnostics
-- Purpose:
--   Receives command acknowledgement messages from the ESP32.
-- ============================================================

SELECT
  *,
  topic(2) AS station_id_from_topic,
  topic() AS mqtt_topic,
  timestamp() AS received_at_epoch_ms
FROM
  'station/+/acks';


-- ============================================================
-- 5. Optional Combined Diagnostics Rule
-- Rule name: station_diagnostics_to_lambda
-- Topic filter: station/+/+
-- Target Lambda: diagnostics
-- Purpose:
--   Development-only rule that receives status, faults, and acks
--   using a single IoT rule.
-- ============================================================

SELECT
  *,
  topic(2) AS station_id_from_topic,
  topic(3) AS message_type,
  topic() AS mqtt_topic,
  timestamp() AS received_at_epoch_ms
FROM
  'station/+/+'
WHERE
  topic(3) = 'status'
  OR topic(3) = 'faults'
  OR topic(3) = 'acks';


-- ============================================================
-- Command Direction
-- ============================================================
-- Commands are not received from the ESP32.
-- They are published by the command_dispatcher Lambda to:
--
--   station/{station_id}/commands
--
-- The ESP32 must validate every cloud command locally before
-- applying it to the physical system.
--
-- Cloud FIS = decision recommendation
-- Command Dispatcher = MQTT command request
-- ESP32 deterministic layer = final physical authorization