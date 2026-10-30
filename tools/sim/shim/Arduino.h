// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// Host shim: the store harness's Arduino.h, plus what the real Arduino.h
// pulls in implicitly on the ESP32 -- the FreeRTOS types web_app.cpp uses
// without including them itself.
#pragma once
#include "../../store/shim/Arduino.h"
#include "../../store/shim/freertos/FreeRTOS.h"
#include "../../store/shim/freertos/semphr.h"
