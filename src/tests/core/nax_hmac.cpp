// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <openssl/evp.h>

#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/scope_exit.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/file_sys/xts_archive.h"
#include "core/loader/loader.h"

namespace {

using namespace Core::Crypto;

std::array<u8, 0x20> Hmac(const u8* key, std::size_t key_size, const void* data, std::size_t size) {
    std::array<u8, 0x20> out{};
    std::size_t out_size = 0;
    REQUIRE(EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key, key_size,
                      static_cast<const u8*>(data), size, out.data(), out.size(),
                      &out_size) != nullptr);
    REQUIRE(out_size == out.size());
    return out;
}

// An SD NCA archive as the console stores it under the path the nca_id gives, holding `fill`.
FileSys::VirtualFile MakeArchive(const std::array<Key256, 2>& sd_keys,
                                 const std::array<u8, 0x10>& nca_id, u8 fill) {
    SHA256Hash hash{};
    u32 hash_size = 0;
    EVP_Digest(nca_id.data(), nca_id.size(), hash.data(), &hash_size, EVP_sha256(), nullptr);
    const auto path =
        fmt::format("/registered/000000{:02X}/{}.nca", hash[0], Common::HexToString(nca_id, false));

    const auto& sd_key = sd_keys[u8(FileSys::NAXContentType::NCA)];
    const auto nax_keys = Hmac(sd_key.data(), 0x10, path.data(), path.size());

    FileSys::NAXHeader header{};
    header.magic = Common::MakeMagic('N', 'A', 'X', '0');
    for (std::size_t j = 0; j < header.key_area.size(); ++j) {
        header.key_area[j].fill(u8(fill + j));
    }
    header.file_size = 0x4000;
    header.hmac =
        Hmac(reinterpret_cast<const u8*>(&header.magic), 0x60, sd_key.data() + 0x10, 0x10);
    for (std::size_t j = 0; j < header.key_area.size(); ++j) {
        Key128 nax_key;
        std::memcpy(nax_key.data(), nax_keys.data() + j * 0x10, nax_key.size());
        AESCipher<Key128>{nax_key, Mode::ECB}.Transcode(header.key_area[j].data(), 0x10,
                                                        header.key_area[j].data(), Op::Encrypt);
    }

    std::vector<u8> data(0x8000, fill);
    std::memcpy(data.data(), &header, sizeof(header));
    return std::make_shared<FileSys::VectorVfsFile>(std::move(data));
}

struct Probe {
    Loader::ResultStatus status;
    std::vector<u8> decrypted;
    bool operator==(const Probe&) const = default;
};

Probe ProbeArchive(const FileSys::VirtualFile& file, const std::array<u8, 0x10>& nca_id) {
    const FileSys::NAX nax{file, nca_id};
    Probe probe{nax.GetStatus(), {}};
    if (const auto decrypted = nax.GetDecrypted()) {
        probe.decrypted = decrypted->ReadBytes(0x200);
    }
    return probe;
}

TEST_CASE("Parallel NAX0 parses with the SD keys already derived agree with sequential ones",
          "[.][crypto][nax]") {
    std::filesystem::path directory;
    std::random_device random;
    do {
        directory = std::filesystem::temp_directory_path() /
                    ("orchid-nax-hmac-test-" + std::to_string(random()));
    } while (!std::filesystem::create_directory(directory));
    const auto previous = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    std::filesystem::create_directories(previous);
    SCOPE_EXIT {
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, previous);
        std::filesystem::remove_all(directory);
    };
    {
        std::ofstream file{directory / "prod.keys"};
        const auto line = [&file](std::string_view name, char digit, std::size_t size) {
            file << name << " = " << std::string(size * 2, digit) << '\n';
        };
        line("sd_card_kek_source", '1', 0x10);
        line("aes_kek_generation_source", '2', 0x10);
        line("aes_key_generation_source", '3', 0x10);
        line("master_key_00", '4', 0x10);
        line("sd_seed", '5', 0x10);
        line("sd_card_save_key_source", '6', 0x20);
        line("sd_card_nca_key_source", '7', 0x20);
        file.close();
        REQUIRE(file.good());
    }
    Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, directory);
    auto& keys = KeyManager::Instance();
    keys.ReloadKeys();

    std::array<Key256, 2> sd_keys{};
    REQUIRE(DeriveSDKeys(sd_keys, keys) == Loader::ResultStatus::Success);

    const std::array<std::array<u8, 0x10>, 3> nca_ids{
        Common::HexStringToArray<0x10>("0123456789abcdef0123456789abcdef"),
        Common::HexStringToArray<0x10>("fedcba9876543210fedcba9876543210"),
        Common::HexStringToArray<0x10>("00112233445566778899aabbccddeeff"),
    };
    const std::array files{
        MakeArchive(sd_keys, nca_ids[0], 0xA0),
        MakeArchive(sd_keys, nca_ids[1], 0x50),
        MakeArchive(sd_keys, nca_ids[0], 0x30),
    };

    std::vector<Probe> expected;
    for (std::size_t i = 0; i < files.size(); ++i) {
        expected.push_back(ProbeArchive(files[i], nca_ids[i]));
    }
    CHECK(expected[0].status == Loader::ResultStatus::Success);
    CHECK(expected[1].status == Loader::ResultStatus::Success);
    CHECK(expected[0].decrypted != expected[1].decrypted);
    // Its header was keyed for the first archive's path.
    CHECK(expected[2].status == Loader::ResultStatus::ErrorNAXKeyDerivationFailed);

    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (std::size_t n = 0; n < 300; ++n) {
                const auto i = (t + n) % files.size();
                if (!(ProbeArchive(files[i], nca_ids[i]) == expected[i])) {
                    ++mismatches;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    CHECK(mismatches == 0);
}

} // namespace
