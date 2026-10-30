// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// Shared between sim_server.cpp (the ESPAsyncWebServer shim) and sim_main.cpp
// (boot, simulated hardware, control panel).
#pragma once
#include <mutex>

namespace httplib { class Server; }

// Everything that touches firmware state -- web handlers, the simulated
// hardware, the control endpoints -- runs under this one lock. On the device
// web handlers run one at a time on the async_tcp task; this keeps that.
extern std::recursive_mutex gSimLock;

// Routes every request that is not under /_sim/ to the firmware's handlers.
// Call after webAppBegin() has registered them.
void simServeFirmware(httplib::Server& svr);
