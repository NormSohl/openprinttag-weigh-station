# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Firmware for a 3D-printing filament scale intended for Seattle Makers (a makerspace). The device:
- Reads spool weight via a physical scale
- Identifies spools via RFID using [OpenPrintTag](https://github.com/normsohl/OpenPrintTag) tags
- Integrates with [Spoolman](https://github.com/Donkie/Spoolman) (open-source filament management server) to record spool weights and assist with spool registration
- Exposes metrics to Prometheus for historical weight tracking

**Current state:** Early development — only a README exists. No build system, dependencies, or source files have been committed yet.

## Intended Architecture

Based on the project description, the system has three external integration points:

1. **Spoolman REST API** — CRUD operations for spools and filament data; the scale pushes weight readings and can trigger spool creation workflows
2. **OpenPrintTag / RFID reader** — identifies which spool is on the scale by reading an RFID tag attached to the spool
3. **Prometheus** — the scale exposes a `/metrics` endpoint (or pushes via Pushgateway) so weight history is queryable over time

## Development Notes

- The project is firmware, likely targeting an embedded Linux board (e.g., Raspberry Pi) or a microcontroller with network access given the Spoolman/Prometheus integrations.
- When the first source files land, update this file with: language/runtime, build commands, how to run/flash, and any hardware-specific setup.
- Spoolman's API documentation lives at `<spoolman-host>/api/v1/docs` on a running instance.
