// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "core/crypto/key_manager.h"

namespace {

using namespace Core::Crypto;

TEST_CASE("CMAC matches RFC 4493", "[core][crypto]") {
    const auto key = Common::HexStringToArray<0x10>("2b7e151628aed2a6abf7158809cf4f3c");
    const auto message = Common::HexStringToVector(
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411"
        "e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
        false);
    const auto cmac = [&](std::size_t size) {
        return CalculateCMAC(std::span{message}.first(size), key);
    };
    CHECK(cmac(0) == Common::HexStringToArray<0x10>("bb1d6929e95937287fa37d129b756746"));
    CHECK(cmac(16) == Common::HexStringToArray<0x10>("070a16b46b4d4144f79bdd9dd04a287c"));
    CHECK(cmac(40) == Common::HexStringToArray<0x10>("dfa66747de9ae63030ca32611497c827"));
    CHECK(cmac(64) == Common::HexStringToArray<0x10>("51f0bebf7e3b9d92fc49741779363cfe"));
}

// encrypted_keyblob_00 for the dummy keys below: its CMAC, the CTR counter and the keyblob,
// whose package1_key_00 is 55..55.
constexpr std::string_view EncryptedKeyblob =
    "c808f121ed0d7669fe1437a1fee54b8866666666666666666666666666666666783854dda9b1b95977180ca9877249"
    "77"
    "8b3ef96de5b43cbc778550aaa878980a77184a08180e35b38282016e74d869e497b0b0733a70abe20193df28ce2616"
    "c6"
    "cfb0d747111e72784cd2ae84398467cd22735604aa89ba26d8f1c3b14b444a50826bca9c29b5152c521a4957f4c5b5"
    "02"
    "0fc5b02403cfd28ce1ec6f5704ae660b3d4425dcaa38af9ddee1b07f3cf4d895";

bool DeriveKeyblob(std::string encrypted_keyblob) {
    auto& keys = KeyManager::Instance();
    std::filesystem::path directory;
    std::random_device random;
    do {
        directory = std::filesystem::temp_directory_path() /
                    ("orchid-keyblob-mac-test-" + std::to_string(random()));
    } while (!std::filesystem::create_directory(directory));
    {
        std::ofstream file{directory / "prod.keys"};
        const auto line = [&file](std::string_view name, char digit) {
            file << name << " = " << std::string(0x20, digit) << '\n';
        };
        line("secure_boot_key", '1');
        line("tsec_key", '2');
        line("keyblob_key_source_00", '3');
        line("keyblob_mac_key_source", '4');
        file << "keyblob_00 = " << std::string(0x120, '0') << '\n';
        file << "encrypted_keyblob_00 = " << encrypted_keyblob << '\n';
    }
    const auto previous = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    std::filesystem::create_directories(previous);
    Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, directory);
    keys.ReloadKeys();
    keys.DeriveBase();
    Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, previous);
    std::filesystem::remove_all(directory);
    return keys.HasKey(S128KeyType::Package1, 0);
}

TEST_CASE("A console keyblob is decrypted only with a valid MAC", "[core][crypto]") {
    auto& keys = KeyManager::Instance();
    if (keys.HasKey(S256KeyType::Header) || keys.HasKey(S128KeyType::Package1, 0)) {
        SKIP("Keys are installed");
    }
    std::string wrong_tag{EncryptedKeyblob};
    wrong_tag[0] = wrong_tag[0] == '0' ? '1' : '0';
    CHECK_FALSE(DeriveKeyblob(wrong_tag));

    REQUIRE(DeriveKeyblob(std::string{EncryptedKeyblob}));
    CHECK(keys.GetKey(S128KeyType::Package1, 0) ==
          Common::HexStringToArray<0x10>("55555555555555555555555555555555"));
}

} // namespace
