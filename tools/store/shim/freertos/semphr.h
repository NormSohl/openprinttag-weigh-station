// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#pragma once
#include "FreeRTOS.h"
inline SemaphoreHandle_t xSemaphoreCreateMutex()             { return (SemaphoreHandle_t)1; }
inline int  xSemaphoreTake(SemaphoreHandle_t, uint32_t)      { return 1; }
inline int  xSemaphoreGive(SemaphoreHandle_t)                { return 1; }
