// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Common {

class SessionCounter {
public:
    u64 Current() const {
        return current;
    }

    bool IsCurrent(u64 session) const {
        return session == current;
    }

    void End() {
        ++current;
    }

private:
    u64 current = 0;
};

} // namespace Common
