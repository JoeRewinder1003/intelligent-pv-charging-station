# Create DynamoDB Tables

This document describes the initial manual procedure for creating the DynamoDB tables required by the AWS cloud backend of the ESP32-based solar charging station.

The purpose of this step is to create the database layer before deploying the Lambda functions.

## Minimum Required Tables

The first prototype requires the following tables:

```text
TelemetryHistory
StationStatus
FaultEvents
CommandLog
DemandProfile
FISDecisionHistory
```

Optional tables for later thesis stages:

```text
BatterySOHHistory
ActuatorLifeHistory
```

## General Configuration

For the first prototype, use a cost-aware configuration:

```text
Billing mode: On-demand or low provisioned capacity
Secondary indexes: None initially
Streams: Disabled
Point-in-time recovery: Disabled initially
Backups: Manual only if needed
```

For early testing, on-demand mode is simpler because it avoids manual capacity tuning. For long-term controlled operation, provisioned capacity can be considered later.

---

## 1. TelemetryHistory

### Purpose

Stores periodic telemetry samples sent by the ESP32.

### Table Name

```text
TelemetryHistory
```

### Keys

| Key          | Type          |
| ------------ | ------------- |
| `station_id` | Partition key |
| `timestamp`  | Sort key      |

### Key Types

```text
station_id: String
timestamp: String
```

---

## 2. StationStatus

### Purpose

Stores the latest known state of each charging station.

### Table Name

```text
StationStatus
```

### Keys

| Key          | Type          |
| ------------ | ------------- |
| `station_id` | Partition key |

### Key Types

```text
station_id: String
```

---

## 3. FaultEvents

### Purpose

Stores fault events, restrictions, invalid data reports, and safety lockouts.

### Table Name

```text
FaultEvents
```

### Keys

| Key          | Type          |
| ------------ | ------------- |
| `station_id` | Partition key |
| `timestamp`  | Sort key      |

### Key Types

```text
station_id: String
timestamp: String
```

---

## 4. CommandLog

### Purpose

Stores cloud-generated commands and ESP32 acknowledgements.

### Table Name

```text
CommandLog
```

### Keys

| Key          | Type          |
| ------------ | ------------- |
| `station_id` | Partition key |
| `command_id` | Sort key      |

### Key Types

```text
station_id: String
command_id: String
```

---

## 5. DemandProfile

### Purpose

Stores the default and adaptive demand table used to estimate the Demand Index.

### Table Name

```text
DemandProfile
```

### Keys

| Key          | Type          |
| ------------ | ------------- |
| `station_id` | Partition key |
| `slot_id`    | Sort key      |

### Key Types

```text
station_id: String
slot_id: String
```

Example slot format:

```text
day_0_slot_18
```

Where:

```text
day_0 = Monday
slot_18 = 09:00–09:30
```

---

## 6. FISDecisionHistory

### Purpose

Stores cloud-side fuzzy decision results for analysis and thesis documentation.

### Table Name

```text
FISDecisionHistory
```

### Keys

| Key          | Type          |
| ------------ | ------------- |
| `station_id` | Partition key |
| `timestamp`  | Sort key      |

### Key Types

```text
station_id: String
timestamp: String
```

---

## Optional Tables

## 7. BatterySOHHistory

### Purpose

Stores estimated battery state-of-health records.

### Table Name

```text
BatterySOHHistory
```

### Keys

| Key          | Type          |
| ------------ | ------------- |
| `station_id` | Partition key |
| `timestamp`  | Sort key      |

### Key Types

```text
station_id: String
timestamp: String
```

---

## 8. ActuatorLifeHistory

### Purpose

Stores actuator movement, duty-cycle, and lifetime indicators.

### Table Name

```text
ActuatorLifeHistory
```

### Keys

| Key          | Type          |
| ------------ | ------------- |
| `station_id` | Partition key |
| `timestamp`  | Sort key      |

### Key Types

```text
station_id: String
timestamp: String
```

---

## Recommended Manual Creation Order

Create the tables in this order:

```text
1. TelemetryHistory
2. StationStatus
3. FaultEvents
4. CommandLog
5. DemandProfile
6. FISDecisionHistory
```

Then, if needed:

```text
7. BatterySOHHistory
8. ActuatorLifeHistory
```

## Validation Checklist

After creating the tables, confirm that:

```text
All table names match the Lambda environment variables
All partition keys are correctly named
All sort keys are correctly named where required
All key types are String
No unnecessary secondary indexes were created
Streams are disabled unless explicitly needed
```

## Notes

The table names must match the names used by the Lambda functions:

```text
TELEMETRY_TABLE_NAME=TelemetryHistory
STATUS_TABLE_NAME=StationStatus
FAULT_EVENTS_TABLE_NAME=FaultEvents
COMMAND_LOG_TABLE_NAME=CommandLog
```

If a table name is changed in AWS, the corresponding Lambda environment variable must also be updated.
