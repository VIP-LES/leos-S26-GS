# Dev Branch Changes

This document summarizes what has been implemented on the `dev` branch of
`leos-S26-ground-station` relative to `main`.

It describes the actual code changes now present in this branch, not just the
planned architecture.

## High-Level Result

The ground-station app has been refactored toward the intended split:

- Pi side:
  - durable SQLite queue
  - forwarder
  - replay telemetry source for hardware-free testing
  - native SX126x Linux receiver option over local Unix socket IPC
- Lab side:
  - ingest API
  - TimescaleDB
  - Grafana

The real hardware radio receiver has intentionally not been implemented yet.

## Major Application Changes

### 1. Explicit Pi/Lab Mode Split

`index.py` now supports:

- `GROUNDSTATION_MODE=lab`
- `GROUNDSTATION_MODE=pi`

Lab mode:

- connects to TimescaleDB
- ensures schema exists
- runs the FastAPI ingest API

Pi mode:

- initializes the SQLite spool queue
- optionally starts either:
  - a replay telemetry source
  - the native SX126x receiver process
- runs the forwarder that ships queued payloads to the lab ingest API

This replaces the previous design where one process started both the API and the
txt-file watcher together.

### 2. Ingest API Renamed and Split by Stream

`src/server.py` has been renamed to `src/ingest_api.py`.

The ingest API now provides:

- `POST /ingest/sensors-and-gps`
- `POST /ingest/efm`

### 3. Two TimescaleDB Hypertables

The database layer now creates and writes to:

- `sensors_and_gps_data`
- `efm_data`

This replaces the previous single-table `launch_data` architecture.

### 4. SQLite Queue Replaces txt Spool Files

The old txt spool file and cursor-file pipeline has been removed.

Pi-side durability is now handled by a SQLite queue table with:

- `id`
- `message_type`
- `payload`
- `created_at`
- `sent_at`

The shared queue logic lives in `src/queue_writer.py`.

### 5. Forwarder Is Now a Dedicated Structured-Payload Component

`src/forwarder.py` now:

- reads unsent structured payloads from SQLite
- chooses the correct ingest endpoint by `message_type`
- POSTs the payload to the lab ingest API
- marks rows as sent when accepted

The forwarder no longer performs text parsing.

### 6. Shared RF Decoder Added

`src/telemetry_decoder.py` now provides:

- RF envelope handling aligned to the `module-radio` `dev` branch refactoring
  instructions
- sync/version/message-type/sequence/payload-length/CRC handling
- CRC16-CCITT-FALSE verification
- conversion between RF frames and structured application payloads

Important note:

- the outer RF frame format follows `module-radio`
- the inner payload structs are still an interim ground-station-side contract
  based on the approved `sensors_and_gps` and `efm` schemas
- they may need adjustment once the final DSDL field definitions are fully
  finalized upstream

### 7. Replay Telemetry Source Added

`src/replay_radio.py` provides a hardware-free Pi-side source for testing.

It can:

- read replay input from a file
- accept either hex RF frames or JSON records
- decode them through the shared RF decoder path
- enqueue structured payloads into the same SQLite queue used in production

This branch now includes a sample replay input file:

- `testdata/fake_radio_input.jsonl`

This lets us test:

- queueing
- forwarding
- ingest

without the physical radio hardware connected.

### 7b. Native Linux Radio Receiver Added

This branch now also includes a native C receiver process under:

- `native/radio_receiver.c`

That process:

- links against the shared `leos_sx126x` radio library sources
- uses the Linux SX126x port in `leos-sdk`
- watches DIO1 GPIO edges on the Pi
- drains complete RF packets from the radios
- forwards them to Python over a local `AF_UNIX` `SOCK_SEQPACKET` socket

The Python Pi-mode process can now choose between:

- `RADIO_SOURCE=replay`
- `RADIO_SOURCE=native`

### 8. Old Watcher/Parser Pipeline Removed

These files were removed:

- `src/watcher.py`
- `src/parser.py`

That old path was specific to txt spool files and is no longer the intended
architecture.

### 9. Shared Payload Models Added

`src/models.py` now defines:

- `SensorsAndGpsPayload`
- `EfmPayload`
- message-type constants and ingest path routing

This keeps the queue, forwarder, and ingest API aligned on one payload contract.

### 10. Environment Contract Updated

`.env.example` now documents the new branch behavior, including:

- `GROUNDSTATION_MODE`
- `SPOOL_DB_PATH`
- `RADIO_SOURCE`
- `REPLAY_RADIO_INPUT_PATH`
- `REPLAY_RADIO_REPEAT`
- `REPLAY_RADIO_INTERVAL_S`
- `RADIO_SOCKET_PATH`
- `RADIO_RECEIVER_BIN`
- `RADIO_RECEIVER_AUTOSTART`

### 11. Grafana Dashboard Updated for Both Hypertables

The checked-in Grafana dashboard stored in `grafana/grafana.db` has been updated
so that:

- the existing slow-telemetry panels now query `sensors_and_gps_data`
- the stale legacy column references were corrected to the current schema
- a new `EFM Channels` panel now queries `efm_data`

## Deployment Assumptions on This Branch

This branch now expects the corresponding `dev` branch of
`leos-server-infra` to provide:

- lab-host deployment automation
- Pi forwarding deployment automation
- Tailscale install/join automation for both hosts

The main remaining non-wired application assumption is:

- the real `radio_receiver.py` still does not exist yet, so Pi mode currently
  relies on a future receiver or the existing fake/replay source

## Validation Performed

The following validation was run for this branch:

- Python compile check with `compileall`
- unit tests for:
  - RF decoder round trips and CRC
  - forwarder queue-to-HTTP behavior

Current Python test result:

- `4` tests passing

## Relationship to `leos-server-infra`

This branch assumes corresponding deployment changes in the `dev` branch of
`leos-server-infra`.

That repo now provisions and deploys this repo in two different host roles:

- lab host deployment for Docker + ingest API
- Pi deployment for forwarding mode

See the infra repo's dev-branch README for deployment-side details.

## What Is Still Not Implemented

The following is intentionally not complete in this branch:

- final field-level adjustment of RF payload contents once the upstream DSDL
  contract is locked down
