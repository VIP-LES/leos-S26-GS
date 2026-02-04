# Ground Station Software Stack  
**VIP: Lightning from the Edge of Space — Spring 2026**

This repository documents the **ground station software stack** used in the VIP project *Lightning from the Edge of Space* to receive, store, and visualize telemetry from a high-altitude balloon (HAB).

The stack is intentionally simple, modular, and easy to reason about, while remaining robust to unreliable radios and evolving telemetry formats.

---

## High-Level Architecture

**Radio Packet Decoder (host)**  
→ **Ingest API (Docker)**  
→ **TimescaleDB (Docker)**  
→ **Grafana (Docker)**  
→ **Tailscale (host)**

---

## Layer Overview

### 1. Radio Packet Decoder (Host)
- Runs on the host (not containerized).
- Receives raw LoRa packets and converts them into structured telemetry.
- Handles framing, integrity checks, decoding, and timestamping.
- Forwards decoded telemetry to the ingest API.

---

### 2. Ingest API (Docker)
- A narrow entry point for all telemetry writes.
- Validates and normalizes incoming telemetry.
- Writes data into the database.
- Exists to isolate the database from radio code and centralize schema enforcement.

---

### 3. Time-Series Database: TimescaleDB (Docker)
- Durable storage for all telemetry.
- Optimized for time-indexed data.
- Written to only by the ingest API.
- Queried by Grafana for visualization.

---

### 4. GUI: Grafana (Docker)
- Browser-based visualization frontend.
- Displays plots, tables, maps, and dashboards.
- Reads telemetry directly from TimescaleDB.
- Supports multi-user viewing with no client installs.

---

### 5. Multi-Laptop Access: Tailscale (Host)
- Provides secure access to Grafana from multiple laptops in different locations.
- Avoids public exposure and manual network configuration.
- Users access Grafana via the host’s Tailscale IP.

---

## Summary

The radio decoder handles packet-level complexity, the ingest API safely mediates data entry, TimescaleDB provides persistent storage, Grafana handles visualization, and Tailscale enables distributed access—together forming a clean and maintainable ground station stack for the Spring 2026 *Lightning from the Edge of Space* VIP project.
