// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// Host shim: the store's mutex is uncontended in a single-threaded test.
#pragma once
#include <cstdint>
typedef void* SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFFu
