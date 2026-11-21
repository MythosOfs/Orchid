// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "core/crypto/key_manager.h"

namespace {

using namespace Core::Crypto;

// A throwaway RSA-2048 key made for this test (e = 65537), big-endian.
constexpr std::string_view Modulus =
    "bc8b557dfe0f8a9b0fe81b7f2e9e7c22800973ff15cc8afb7557ad8a383917e0689ddd74361962e4"
    "a5a324ce63e23678f87e8236ed42f8f0656710491b2072342cb6f646db1a8ddfaafc133373606d49"
    "c9a1d9c47ef135ef0890bfc76448e8638f7413b43cc720af3b3e19c1007ee2ac6e8d8d5024db096c"
    "063991f02d00151f71aa7d77c65432a85a33d14b5df4a602b0eea372c6eed2b46349f775bd335511"
    "c72a6c34c30da079129bcabd3ba39018ddd850df2ba23d9fc5e6fb3b668d5525bd4b1f78f2a50cf3"
    "5fe022fe5a2450cfeefb4702538ee2b571b517910d184e09d1ae299af44ad4302db7021defb5deb0"
    "474307576262af7a19d6b56a371a5fcd";

constexpr std::string_view PrivateExponent =
    "16119d0d254abe678e02160dd9f0150998c10281ce1466042ee4f75c215e956bdff2f273e2cd2fe5"
    "217a2a19ba594514fdc91906c2d8bef8ac75d6d327c8b17567f2767b2eb439fcff2f8833d2826be8"
    "62207eb3e043ed933006795c2661de49a105ee347f94b33faf88824d33f8a62196c31cd24fae0fb9"
    "443542dec0f5ac83114571f06b3439cb96728374755a420237a2a62cd2b3f5a975961c9273ad8b0e"
    "46c5f1579a42aa3c327c36d81005b23357043f0f2deaef8005aaacc1656dbdb992af9c3f7a005fc3"
    "7439a21a7b23bb780f4987f533402b51fbc1e7b92610b390be7f56ab6f6065c04f08c25c8f7c48b2"
    "202bd0d90f5c4f7530895b4398e18905";

// RSA-OAEP (SHA-256, MGF1-SHA-256, empty label) encryptions to that key: the title key
// 00112233445566778899aabbccddeeff, the title key ffeeddccbbaa99887766554433221100 in a block
// whose decryption starts with two zero bytes, and an empty message.
constexpr std::string_view TitleKeyBlock =
    "04a58b8e3addb2ba9305def8eb76cf74105d5b0fb9d0ac3f3f055310ee6d73fb9e2759a4e42efc4f"
    "653dd6b570d645868def3d28bd60ef82171937768f280f10f4e14bb65f21c39b64b9a9ba8e9d1a79"
    "e501108139d8f7e7c0a8f4381cd0b93ccd1dd871dbd2ea24e58a04c5f957e308d4680567986ff80c"
    "eb2b8cbb69dc2610cb935ab2520962d35b1a8d297a6241aeef5288e2a14a1da35fad7bc5484a9056"
    "f98c159c4f3cbc1941264db0bf4ea6a1af7b2f4e108e9d39a58ecb521cb5d94bc39d17518a5b3890"
    "71ba33fd5c2a2bc05aaa118e29b3dc240bb161a9f66e25228bfdb531e3414dc180a67704240daf09"
    "32d6a93a5714b05a8bb816cd627aea60";

constexpr std::string_view LeadingZeroBlock =
    "7bf217ac177bab8124399cd1ee21352a23bb55909bd1d917a36ec2fa37dc5871bf19ea65a22f6c45"
    "5b3a82a4b4ce22aac33e30c6f8a8d955ca226b8213143310c2d92352f0f1bfed86f3e6fbde2e36a7"
    "cfcb79d61781dc08203cafa08eeb87340cd47ef2bcebc76149effa63fca4d545324a7e94d1cd6cd6"
    "5cb5cdea6a632bfbfa3bf9551a6c5f81772e7bc170ebb0871540d3c42a82aba5f1456952fa1d0e0c"
    "2609f0f38369d232dd1da393fdae4d2994a3167a9537d92f442ddd004f8b11474b592d097a2a3ecf"
    "7153244a024acb29899f5d43a823d65a64a47cc5118db59c7e32f36f1b3f75f1d93cc0470a68d9b5"
    "f231d44035a61d2f7868fe10b176e0d0";

constexpr std::string_view EmptyMessageBlock =
    "5f51f41627b27666169fc69716b6ee8f9d444dad2e67c7ebda87ade69b0fe4385d617b1fcc9b8d47"
    "24987fd49f319b3b4fe6ee91b3218989eecaae111d053393b6088325bf2d93360f7c19ff372f6c0f"
    "30450f82dd04a36a7a8e99fd05f00878eca9adaadf648ace052fb057902f578e9f168e13c3e162ec"
    "bfd1f901292cc9afe4bcaae56d95d6aeb101cecb3e318a56fa60b1edf1c2a3f547762bd44f839420"
    "b310759510eeffcf96e899117cab9ead99799a176e243d1731f2af312da622df412cbd738a6fb415"
    "16da0584053af7bced6a6bc6b5b271e6de098b511c9f701ba1eaaff6bafb1b784608bddda1ab758f"
    "8e96dede8cac1c0794bd8085e1806558";

// The first title key in a block made for the label "orchid" instead of the empty label.
constexpr std::string_view WrongLabelBlock =
    "a05dd925575a9551caa270355cdd6727f44a897b9cd02ad4d6ef12d87160b9bb5dba80bf92529751"
    "f346ead3d1569817e8e642a15caf301026e9e25f03376d9e1d36dc6b097d350721b028958d501325"
    "fd9d11e961d42b099328a68d9a601afe27b11bc0e8ba434ff34e4385ad7dd505ba4f0185b7001ac4"
    "bf2d2320c712ecc2a0e16b523b57642e86387da9d6f9d4dad7161e39e33e7fa4de7d51c28580ee0b"
    "b797b0a0e7f74e43ddad8c0e682ea42dc5b5303412a2053491974ad277ced5407ec954cdcd59f8e0"
    "0f2c1fc8fe9b588607e33e77309bd3d36a9529591b1dcb63538a26f44abf730132d3e0452ca3cba7"
    "7bf1c2cea764751eceb44a47d475a1b9";

struct TicketKeys {
    TicketKeys() {
        std::random_device random;
        do {
            directory = std::filesystem::temp_directory_path() /
                        ("orchid-eticket-test-" + std::to_string(random()));
        } while (!std::filesystem::create_directory(directory));
        std::ofstream{directory / "prod.keys"} << "eticket_rsa_keypair = " << PrivateExponent
                                               << Modulus << "00010001" << std::string(0xC * 2, '0')
                                               << '\n';
        previous = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
        std::filesystem::create_directories(previous);
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, directory);
        keys.ReloadKeys();
    }
    ~TicketKeys() {
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, previous);
        std::filesystem::remove_all(directory);
    }

    bool Add(TitleKeyType type, std::string_view block, std::string_view rights_id_hex) {
        RSA2048Ticket raw{};
        raw.sig_type = SignatureType::RSA_2048_SHA256;
        constexpr std::string_view issuer = "Root-CA00000003-XS00000020";
        std::memcpy(raw.data.issuer.data(), issuer.data(), issuer.size());
        raw.data.type = type;
        raw.data.title_key_block = Common::HexStringToArray<0x100>(block);
        raw.data.rights_id = Common::HexStringToArray<0x10>(rights_id_hex);
        std::memcpy(rights_id.data(), raw.data.rights_id.data(), raw.data.rights_id.size());
        REQUIRE_FALSE(HasTitleKey());
        return keys.AddTicket(Ticket{raw});
    }
    bool HasTitleKey() const {
        return keys.HasKey(S128KeyType::Titlekey, rights_id[1], rights_id[0]);
    }
    Key128 TitleKey() const {
        return keys.GetKey(S128KeyType::Titlekey, rights_id[1], rights_id[0]);
    }

    KeyManager& keys = KeyManager::Instance();
    std::filesystem::path directory;
    std::filesystem::path previous;
    u128 rights_id{};
};

TEST_CASE("A personalized ticket's title key is decrypted with the eticket RSA key",
          "[core][crypto]") {
    TicketKeys keys;
    REQUIRE(
        keys.Add(TitleKeyType::Personalized, TitleKeyBlock, "01004d5f17e4c0000000000000000041"));
    CHECK(keys.TitleKey() == Common::HexStringToArray<0x10>("00112233445566778899aabbccddeeff"));
}

TEST_CASE("A personalized ticket whose decryption starts with two zero bytes gives its title key",
          "[core][crypto]") {
    TicketKeys keys;
    REQUIRE(
        keys.Add(TitleKeyType::Personalized, LeadingZeroBlock, "01004d5f17e4c0000000000000000043"));
    CHECK(keys.TitleKey() == Common::HexStringToArray<0x10>("ffeeddccbbaa99887766554433221100"));
}

TEST_CASE("A personalized ticket whose block holds no title key gives none", "[core][crypto]") {
    TicketKeys keys;
    CHECK_FALSE(keys.Add(TitleKeyType::Personalized, EmptyMessageBlock,
                         "01004d5f17e4c0000000000000000042"));
    CHECK_FALSE(keys.HasTitleKey());
}

TEST_CASE("A personalized ticket with a corrupted block gives no title key", "[core][crypto]") {
    TicketKeys keys;
    std::string block{TitleKeyBlock};
    block[100] = block[100] == '0' ? '1' : '0';
    CHECK_FALSE(keys.Add(TitleKeyType::Personalized, block, "01004d5f17e4c0000000000000000044"));
    CHECK_FALSE(keys.HasTitleKey());
}

TEST_CASE("A personalized ticket with the wrong OAEP label gives no title key", "[core][crypto]") {
    TicketKeys keys;
    CHECK_FALSE(
        keys.Add(TitleKeyType::Personalized, WrongLabelBlock, "01004d5f17e4c0000000000000000046"));
    CHECK_FALSE(keys.HasTitleKey());
}

TEST_CASE("A common ticket's title key is taken as stored", "[core][crypto]") {
    TicketKeys keys;
    std::string block{"0123456789abcdeffedcba9876543210"};
    block.resize(0x200, '0');
    REQUIRE(keys.Add(TitleKeyType::Common, block, "01004d5f17e4c0000000000000000045"));
    CHECK(keys.TitleKey() == Common::HexStringToArray<0x10>("0123456789abcdeffedcba9876543210"));
}

} // namespace
