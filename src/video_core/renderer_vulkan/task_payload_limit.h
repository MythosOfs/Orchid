// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Vulkan {

constexpr bool TaskPayloadExceedsLimit(u32 payload_words, u32 max_payload_bytes) {
    return payload_words > max_payload_bytes / sizeof(u32);
}

}
