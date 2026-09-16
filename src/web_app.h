// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#pragma once

// Built-in web app (redesign Phase 4+). Replaces Spoolman's web UI with a
// self-hosted inventory dashboard + onboarding form served straight off the
// device. Async (ESPAsyncWebServer) so it never stalls the NFC/scale/display
// tasks. Reads from the local store + config catalog; onboarding writes the
// tag's Main section (via the shared gTagMain/gWriteMainPending path) and
// appends a reconcile event to the log.
//
// Started from syncTask once the network (station or SoftAP) is up.
void webAppBegin();

// Pushed by controller_task.cpp's setState() on every device-state change, so
// pages watching what's on the scale (currently just /onboard) can react the
// instant it actually changes instead of polling on a timer. Cheap to call
// unconditionally -- a no-op when nothing has an /events connection open.
void webAppNotifyStateChanged();
