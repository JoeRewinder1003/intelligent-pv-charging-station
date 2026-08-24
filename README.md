# Intelligent Photovoltaic Charging Station



Firmware and cloud implementation of an intelligent photovoltaic charging station for low-power electric vehicles.



This repository contains the embedded firmware, cloud functions, fuzzy decision-making modules, simulation tools, and validation scripts developed as part of a master's thesis at UPIITA-IPN.



\## Project Overview



The system combines a photovoltaic charging station with local embedded control and a cloud-based supervisory architecture.



The implemented architecture includes:



\* ESP32-based embedded control and telemetry

\* Photovoltaic and battery functional emulation

\* Solar tracking control

\* Local safety validation and interlocks

\* MQTT communication through AWS IoT Core

\* AWS Lambda processing

\* DynamoDB storage

\* Cloud-based fuzzy inference

\* Demand-profile integration

\* External weather-data integration

\* Battery diagnostic functions

\* Actuator usage and maintenance monitoring

\* Cloud command dispatch and acknowledgement handling



The cloud-supervised system follows the general flow:



```text

ESP32

\\\&#x20; -> MQTT

\\\&#x20; -> AWS IoT Core

\\\&#x20; -> AWS Lambda

\\\&#x20; -> DynamoDB

\\\&#x20; -> Fuzzy inference and deterministic validation

\\\&#x20; -> Command dispatcher

\\\&#x20; -> MQTT commands

\\\&#x20; -> ESP32

\\\&#x20; -> Local validation and execution

\\\&#x20; -> ACK and telemetry

```



\## Repository Structure



```text

aws/

\\\&#x20;   deployment/

\\\&#x20;   fis/

\\\&#x20;   infrastructure/

\\\&#x20;   lambdas/



esp32/

\\\&#x20;   main/

\\\&#x20;       aws\\\\\\\_iot\\\\\\\_connectivity\\\\\\\_test/

\\\&#x20;       station\\\\\\\_cloud\\\\\\\_integration\\\\\\\_v1/



tools/



tests/



test\\\\\\\_\\\\\\\*.py

```



\### `esp32/main/station\\\\\\\_cloud\\\\\\\_integration\\\\\\\_v1`



Main ESP32 implementation used for the cloud-integrated functional experiments.



It includes modules for:



\* Battery emulation

\* Photovoltaic generation emulation

\* Scenario management

\* Solar tracking

\* Actuator emulation

\* Actuator synchronization

\* Tracking safety

\* Battery health indicators

\* Battery capacity testing

\* Maintenance functions

\* MQTT telemetry

\* Cloud command reception

\* Local command validation



\### `aws/lambdas`



Contains the implemented AWS Lambda functions associated with the supervisory system, including:



\* Telemetry processing

\* Fuzzy inference processing

\* Command dispatch

\* Demand estimation

\* Weather acquisition

\* Battery diagnostics

\* Actuator lifetime estimation



\### `aws/fis`



Contains reference and archived implementations used to verify alignment between the original fuzzy inference system and the cloud implementation.



\### `tools`



Utility scripts used for configuration, comparison, and validation tasks.



\### `tests`



Automated and functional tests used during development and system validation.



\## Fuzzy Decision-Making System



The decision-making architecture uses two fuzzy inference systems.



The Weather FIS uses:



\* Shortwave radiation

\* Cloud cover

\* Precipitation probability



and produces a normalized Weather Index.



The main FIS uses:



\* Battery state of charge

\* Net battery power

\* Local irradiance

\* Weather Index

\* Demand Index



and selects an operating mode from `M0` to `M5`.



Fuzzy decisions are complemented by deterministic validation and safety rules before commands are applied to the station.



\## Functional Simulation



The ESP32 implementation used in the thesis includes functional models of the photovoltaic system, battery bank, charging loads, solar tracking system, and actuator behavior.



These models were intended to validate control logic, cloud communication, decision-making, and system integration. They are not intended to represent high-fidelity electrical or electrochemical models of the physical station.



\## Validation Tags



The repository includes Git tags corresponding to validated development milestones.



Examples include:



```text

cloud-fis-v1-validated

cloud-closed-loop-v1-e2e-validated

cloud-command-application-v1-validated

cloud-demand-profile-v1-e2e-validated

cloud-stale-data-e2e-v1-validated

battery-health-e2e-v1-validated

battery-capacity-soh-v1-e2e-validated

actuator-life-estimator-v1-e2e-validated

solar-mems-tracking-v1-local-validated

```



These tags preserve specific implementation states associated with functional and end-to-end validation activities.



\## Credentials and AWS IoT Certificates



Credentials, AWS IoT private keys, device certificates, and local secrets are intentionally excluded from version control.



Template files such as:



```text

secrets.example.h

```



are provided only to indicate the expected configuration structure.



Users must provide their own AWS IoT credentials and certificates before using the connectivity functions.



\## Status



The repository represents the research implementation used during thesis development. Some components correspond to functional simulation or experimental validation and should not be interpreted as production-ready charging infrastructure.


