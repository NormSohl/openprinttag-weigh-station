// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// Minimal stand-in for tinycbor's generated export header.
// Upstream generates this to carry symbol-visibility attributes for shared
// builds; a static native build wants them empty.
#pragma once
#define CBOR_API
#define CBOR_PRIVATE_API
#define CBOR_INLINE_API static inline
