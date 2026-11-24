// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/hle/service/am/service/application_functions.h"
#include "core/hle/service/ns/language.h"
#include "core/hle/service/ns/read_only_application_control_data_interface.h"
#include "core/hle/service/set/settings_types.h"
#include "core/loader/loader.h"

namespace {

using Service::AM::ResolveDesiredLanguage;
using Service::NS::ApplicationLanguage;
using Service::NS::GetSupportedLanguageFlag;
using Service::NS::IReadOnlyApplicationControlDataInterface;
using Service::Set::LanguageCode;

class FakeControlAppLoader final : public Loader::AppLoader {
public:
    FakeControlAppLoader(Loader::ResultStatus program_id_status_, u64 program_id_,
                         Loader::ResultStatus control_status_, u32 supported_languages_)
        : AppLoader{nullptr}, program_id_status{program_id_status_}, program_id{program_id_},
          control_status{control_status_}, supported_languages{supported_languages_} {}

    Loader::FileType GetFileType() const override {
        return Loader::FileType::NSP;
    }

    LoadResult Load(Kernel::KProcess&, Core::System&) override {
        return {Loader::ResultStatus::ErrorNotImplemented, {}};
    }

    Loader::ResultStatus ReadProgramId(u64& out_program_id) override {
        if (program_id_status != Loader::ResultStatus::Success) {
            return program_id_status;
        }
        out_program_id = program_id;
        return Loader::ResultStatus::Success;
    }

    Loader::ResultStatus ReadControlData(FileSys::NACP& control) override {
        if (control_status != Loader::ResultStatus::Success) {
            return control_status;
        }
        control.raw.supported_languages = static_cast<FileSys::SupportedLanguage>(supported_languages);
        return Loader::ResultStatus::Success;
    }

private:
    Loader::ResultStatus program_id_status;
    u64 program_id;
    Loader::ResultStatus control_status;
    u32 supported_languages;
};

FileSys::NACP MakeControl(u32 supported_languages) {
    FileSys::NACP control{};
    control.raw.supported_languages = static_cast<FileSys::SupportedLanguage>(supported_languages);
    return control;
}

TEST_CASE("ResolveDesiredLanguage reads AppLoader control data when installed metadata is absent",
          "[core][am][language]") {
    const auto previous_language = Settings::values.language_index.GetValue();
    Settings::values.language_index.SetValue(Settings::Language::German);
    SCOPE_EXIT {
        Settings::values.language_index.SetValue(previous_language);
    };

    Core::System system;
    IReadOnlyApplicationControlDataInterface control_data{system};
    constexpr u64 base_id = 0x0400000000002000ULL;
    constexpr u64 program1_id = 0x0400000000002001ULL;

    SECTION("installed control data takes precedence over AppLoader") {
        const auto installed = MakeControl(GetSupportedLanguageFlag(ApplicationLanguage::German));
        FakeControlAppLoader loader{Loader::ResultStatus::Success, base_id,
                                    Loader::ResultStatus::Success,
                                    GetSupportedLanguageFlag(ApplicationLanguage::Japanese)};
        u64 language_code{};
        REQUIRE(ResolveDesiredLanguage(control_data, base_id, &installed, &loader, &language_code)
                    .IsSuccess());
        CHECK(language_code == static_cast<u64>(LanguageCode::DE));
    }

    SECTION("matching base title ID reads supported languages from AppLoader") {
        FakeControlAppLoader loader{Loader::ResultStatus::Success, base_id,
                                    Loader::ResultStatus::Success,
                                    GetSupportedLanguageFlag(ApplicationLanguage::German) |
                                        GetSupportedLanguageFlag(ApplicationLanguage::Japanese)};
        u64 language_code{};
        REQUIRE(ResolveDesiredLanguage(control_data, program1_id, nullptr, &loader, &language_code)
                    .IsSuccess());
        CHECK(language_code == static_cast<u64>(LanguageCode::DE));
    }

    SECTION("AppLoader mask restricts system language to a supported language") {
        FakeControlAppLoader loader{Loader::ResultStatus::Success, base_id,
                                    Loader::ResultStatus::Success,
                                    GetSupportedLanguageFlag(ApplicationLanguage::Japanese)};
        u64 language_code{};
        REQUIRE(ResolveDesiredLanguage(control_data, base_id, nullptr, &loader, &language_code)
                    .IsSuccess());
        CHECK(language_code == static_cast<u64>(LanguageCode::JA));
    }
}

TEST_CASE("ResolveDesiredLanguage falls back to AmericanEnglish without control data or zero mask",
          "[core][am][language]") {
    const auto previous_language = Settings::values.language_index.GetValue();
    Settings::values.language_index.SetValue(Settings::Language::German);
    SCOPE_EXIT {
        Settings::values.language_index.SetValue(previous_language);
    };

    Core::System system;
    IReadOnlyApplicationControlDataInterface control_data{system};
    constexpr u64 base_id = 0x0400000000002000ULL;
    constexpr u64 other_id = 0x010000000000100DULL;
    const u32 german_mask = GetSupportedLanguageFlag(ApplicationLanguage::German);

    SECTION("no installed control and null AppLoader") {
        u64 language_code{};
        REQUIRE(ResolveDesiredLanguage(control_data, base_id, nullptr, nullptr, &language_code)
                    .IsSuccess());
        CHECK(language_code == static_cast<u64>(LanguageCode::EN_US));
    }

    SECTION("AppLoader belongs to a different base title ID") {
        FakeControlAppLoader loader{Loader::ResultStatus::Success, base_id,
                                    Loader::ResultStatus::Success, german_mask};
        u64 language_code{};
        REQUIRE(ResolveDesiredLanguage(control_data, other_id, nullptr, &loader, &language_code)
                    .IsSuccess());
        CHECK(language_code == static_cast<u64>(LanguageCode::EN_US));
    }

    SECTION("AppLoader ReadProgramId fails") {
        FakeControlAppLoader loader{Loader::ResultStatus::ErrorNotImplemented, base_id,
                                    Loader::ResultStatus::Success, german_mask};
        u64 language_code{};
        REQUIRE(ResolveDesiredLanguage(control_data, base_id, nullptr, &loader, &language_code)
                    .IsSuccess());
        CHECK(language_code == static_cast<u64>(LanguageCode::EN_US));
    }

    SECTION("AppLoader ReadControlData fails") {
        FakeControlAppLoader loader{Loader::ResultStatus::Success, base_id,
                                    Loader::ResultStatus::ErrorNoControl, german_mask};
        u64 language_code{};
        REQUIRE(ResolveDesiredLanguage(control_data, base_id, nullptr, &loader, &language_code)
                    .IsSuccess());
        CHECK(language_code == static_cast<u64>(LanguageCode::EN_US));
    }

    SECTION("AppLoader control has a zero language mask") {
        FakeControlAppLoader loader{Loader::ResultStatus::Success, base_id,
                                    Loader::ResultStatus::Success, 0};
        u64 language_code{};
        REQUIRE(ResolveDesiredLanguage(control_data, base_id, nullptr, &loader, &language_code)
                    .IsSuccess());
        CHECK(language_code == static_cast<u64>(LanguageCode::EN_US));
    }

    SECTION("installed control has a zero language mask") {
        const auto installed = MakeControl(0);
        FakeControlAppLoader loader{Loader::ResultStatus::Success, base_id,
                                    Loader::ResultStatus::Success, german_mask};
        u64 language_code{};
        REQUIRE(ResolveDesiredLanguage(control_data, base_id, &installed, &loader, &language_code)
                    .IsSuccess());
        CHECK(language_code == static_cast<u64>(LanguageCode::EN_US));
    }
}

}
