# Ground Station Software Stack  
**VIP: Lightning from the Edge of Space — Spring 2026**

This repository documents the **ground station software stack** used in the VIP project *Lightning from the Edge of Space* to receive, store, and visualize telemetry from a high-altitude balloon (HAB).

The stack is intentionally simple, modular, and easy to reason about, while remaining robust to unreliable radios and evolving telemetry formats.

---

## High-Level Architecture

**Pi Receiver/Forwarder (host)**  
→ **Ingest API (host)**  
→ **TimescaleDB (Docker)**  
→ **Grafana (Docker)**  
→ **Tailscale (host)**

---

## Layer Overview

### 1. Pi Receiver / Forwarder (Host)
- Runs on the Pi host (not containerized).
- Receives or replays telemetry, writes it to a local durable queue, and
  forwards it to the lab-hosted ingest API.
- Keeps local durability during connectivity loss.
- Supports either:
  - replay RF input in Python
  - a native SX126x receiver process that feeds raw RF frames to Python over a
    local Unix domain socket

---

### 2. Ingest API (Host)
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

### 5. Tailscale (Host)
- Provides the intended secure network path between the Pi, the lab host, and
  remote viewers.
- Avoids public exposure and manual network configuration.
- Is automated in the infrastructure repo on the `dev` branch after initial host
  reachability is established.
- For a brand-new Pi, especially on Apple hotspot, a first-boot cloud-init
  Tailscale setup may still be needed before Ansible can take over.

---

## Summary

The Pi-side receiver/forwarder handles edge collection and durability, the lab
ingest API safely mediates data entry, TimescaleDB provides persistent storage,
Grafana handles visualization, and Tailscale provides the intended secure
network path between hosts.
