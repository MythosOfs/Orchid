// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "core/crypto/key_manager.h"

using namespace Core::Crypto;

TEST_CASE("A keys file line with a short value does not replace the key read before it",
          "[.][crypto][keys_file]") {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path directory = fs::temp_directory_path() / ("orchid_keys_test_" + std::to_string(stamp));
    fs::create_directories(directory);
    const auto previous = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, directory);
    SCOPE_EXIT {
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, previous);
        std::error_code ec;
        fs::remove_all(directory, ec);
    };

    const std::string rights_id = "0100000000000000" + std::string(16, '0');
    {
        std::ofstream prod{directory / "prod.keys"};
        prod << "header_key = " << std::string(64, '1') << "\nheader_key = 00\n";
        prod << "master_key_00 = " << std::string(32, '3') << "\nmaster_key_00 = 0033\n";
        prod << "master_key_00 = " << std::string(30, '6') << '\n';
        prod << "master_key_00 = 00 # " << std::string(40, 'x') << '\n';
        prod << "key_area_key_ocean_00 = " << std::string(32, '5') << " # comment\n";
        prod << "key_area_key_application_00 = " << std::string(32, '4')
             << "\nkey_area_key_application_00 = 44\n";
        prod << "key_area_key_application_00 = " << std::string(30, '6') << '\n';
        prod << "sd_card_kek_source = " << std::string(32, '7') << '\n';
        prod << "sd_card_kek_source = " << std::string(30, '8') << '\n';
        prod << "keyblob_ff = " << std::string(0x90 * 2, 'a') << '\n';
        prod << "encrypted_keyblob_ff = " << std::string(0xB0 * 2, 'b') << '\n';
        prod << "keyblob_20 = " << std::string(0x90 * 2, 'c') << '\n';
        prod << "encrypted_keyblob_20 = " << std::string(0xB0 * 2, 'd') << '\n';
        std::ofstream title{directory / "title.keys"};
        title << rights_id << " = " << std::string(32, '2') << '\n';
        title << rights_id << " = 00\n";
        title << "0100000000000000 = " << std::string(32, '9') << '\n';
        title << std::string(30, '1') << " = " << std::string(32, '9') << '\n';
    }

    KeyManager& keys = KeyManager::Instance();
    keys.ReloadKeys();

    Key256 header;
    header.fill(0x11);
    CHECK(keys.GetKey(S256KeyType::Header) == header);
    Key128 master;
    master.fill(0x33);
    CHECK(keys.GetKey(S128KeyType::Master, 0) == master);
    Key128 key_area;
    key_area.fill(0x44);
    CHECK(keys.GetKey(S128KeyType::KeyArea, 0, u64(KeyAreaKeyType::Application)) == key_area);
    Key128 sd_kek_source;
    sd_kek_source.fill(0x77);
    CHECK(keys.GetKey(S128KeyType::Source, u64(SourceKeyType::SDKek)) == sd_kek_source);
    Key128 ocean;
    ocean.fill(0x55);
    CHECK(keys.GetKey(S128KeyType::KeyArea, 0, u64(KeyAreaKeyType::Ocean)) == ocean);
    Key128 title_key;
    title_key.fill(0x22);
    CHECK(keys.GetKey(S128KeyType::Titlekey, 0, 0x0000000000000001ULL) == title_key);
    CHECK_FALSE(keys.HasKey(S128KeyType::Titlekey, 0, 0));
}
