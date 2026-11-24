// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "common/hex_util.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"

namespace {

using namespace Core::Crypto;

TEST_CASE("AES-XTS decrypts with matching key halves with debug asserts on", "[core][crypto]") {
    const auto previous_debug_asserts = Settings::values.use_debug_asserts.GetValue();
    Settings::values.use_debug_asserts.SetValue(true);
    SCOPE_EXIT {
        Settings::values.use_debug_asserts.SetValue(previous_debug_asserts);
    };

    const Key256 key{};
    const auto ciphertext = Common::HexStringToArray<32>(
        "917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e");
    std::array<u8, 32> plaintext{};
    plaintext.fill(0xA5);

    AESCipher<Key256> cipher(key, Mode::XTS);
    cipher.XTSTranscode(ciphertext.data(), ciphertext.size(), plaintext.data(), 0,
                        ciphertext.size(), Op::Decrypt);
    CHECK(plaintext == std::array<u8, 32>{});
}

}
