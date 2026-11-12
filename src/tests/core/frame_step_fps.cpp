// SPDX-License-Identifier: GPL-3.0-or-later

#include <numeric>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/game_fps.h"
#include "core/frame_step_fps.h"

namespace {
constexpr s64 FrameNs = 1'000'000'000 / 60;

// Steps the game is told to simulate while every frame takes frame_ns.
std::vector<u32> Run(Core::FrameStepFps& stepper, s64 frame_ns, int frames, u32 speed = 100) {
    std::vector<u32> steps;
    for (int i = 0; i < frames; ++i) {
        steps.push_back(stepper.Next(frame_ns, speed));
    }
    return steps;
}
u32 Sum(const std::vector<u32>& steps) {
    return std::accumulate(steps.begin(), steps.end(), 0U);
}
} // namespace

TEST_CASE("Frame step follows the frame time", "[game_fps]") {
    Core::FrameStepFps stepper;
    CHECK(Run(stepper, FrameNs, 6) == std::vector<u32>{1, 1, 1, 1, 1, 1});
    // The second slow frame in a row raises the step.
    CHECK(Run(stepper, 2 * FrameNs, 4) == std::vector<u32>{1, 2, 2, 2});
    // Never more than the game's own two steps, however slow the frames are.
    CHECK(Run(stepper, 5 * FrameNs, 3) == std::vector<u32>{2, 2, 2});
    CHECK(Core::FrameStepFps::MaxStep == 2);
    Core::FrameStepFps slow;
    CHECK(Run(slow, 5 * FrameNs, 3) == std::vector<u32>{1, 2, 2});
    // Faster than 60 FPS still simulates one frame per rendered frame.
    Core::FrameStepFps fast;
    CHECK(Run(fast, FrameNs / 2, 3) == std::vector<u32>{1, 1, 1});
}

TEST_CASE("Frame step ignores a single slow frame", "[game_fps]") {
    CHECK(Core::FrameStepFps::OverBeforeUp == 2);
    Core::FrameStepFps stepper;
    for (int i = 0; i < 5; ++i) {
        // A hitch between frames that fit never adds up to two in a row.
        CHECK(stepper.Next(3 * FrameNs, 100) == 1);
        CHECK(stepper.Next(FrameNs, 100) == 1);
    }
    CHECK(Run(stepper, 3 * FrameNs, 2) == std::vector<u32>{1, 2});
}

TEST_CASE("Frame step tolerates a little slow motion before halving the frame rate", "[game_fps]") {
    // 1.2 frames (50 FPS) still fits one step, anything above takes two.
    Core::FrameStepFps fits;
    CHECK(Run(fits, FrameNs * 119 / 100, 4) == std::vector<u32>{1, 1, 1, 1});
    Core::FrameStepFps over;
    CHECK(Run(over, FrameNs * 122 / 100, 2) == std::vector<u32>{1, 2});
}

TEST_CASE("Frame step tries the faster rate again", "[game_fps]") {
    constexpr int Probe = Core::FrameStepFps::ProbeAfter;
    constexpr int Settle = Core::FrameStepFps::Settle;
    CHECK(Settle == 3);
    CHECK(Probe == 60);
    // The game waits two frames itself once the step is two: the PC could do 60 again, but only
    // frames with one step show it.
    Core::FrameStepFps stepper;
    CHECK(Run(stepper, 2 * FrameNs, 2) == std::vector<u32>{1, 2});
    CHECK(Sum(Run(stepper, 2 * FrameNs, Probe - 1)) == 2 * (Probe - 1));
    CHECK(stepper.Next(2 * FrameNs, 100) == 1); // the try starts
    // The game is still on the old step for the next frames: they do not count.
    CHECK(Run(stepper, 2 * FrameNs, Settle) == std::vector<u32>(Settle, 1));
    CHECK(Run(stepper, FrameNs, 3) == std::vector<u32>{1, 1, 1});

    // A PC that really needs two frames goes back up after the try and waits the full time again.
    Core::FrameStepFps slow;
    Run(slow, 2 * FrameNs, 2 + Probe);
    CHECK(Run(slow, 2 * FrameNs, Settle) == std::vector<u32>(Settle, 1));
    CHECK(Run(slow, 2 * FrameNs, 2) == std::vector<u32>{1, 2});
    CHECK(Sum(Run(slow, 2 * FrameNs, Probe - 1)) == 2 * (Probe - 1));
    CHECK(slow.Next(2 * FrameNs, 100) == 1);

    // Frames that run over at two steps count as well: a PC below 30 FPS still gets its tries,
    // and a hitch does not restart the count.
    Core::FrameStepFps below;
    Run(below, 4 * FrameNs, 2 + Probe - 1);
    CHECK(below.Next(4 * FrameNs, 100) == 1);
}

TEST_CASE("Frame step option is offered for Hyrule Warriors only", "[game_fps]") {
    CHECK(Settings::ShowFrameStepFps(Settings::HyruleWarriorsProgramId, false));
    CHECK_FALSE(Settings::ShowFrameStepFps(Settings::HyruleWarriorsProgramId, true));
    CHECK_FALSE(Settings::ShowFrameStepFps(Settings::PokopiaProgramId, false));
}

TEST_CASE("Frame step does not replay stalls", "[game_fps]") {
    Core::FrameStepFps stepper;
    Run(stepper, 2 * FrameNs, 3);
    // A loading stall or pause: one step, and every count starts again.
    CHECK(stepper.Next(250'000'001, 100) == 1);
    CHECK(Run(stepper, FrameNs, 3) == std::vector<u32>{1, 1, 1});
    // Exactly at the limit still counts as a slow frame.
    CHECK(Run(stepper, 250'000'000, 2) == std::vector<u32>{1, 2});
    CHECK(stepper.Next(0, 100) == 1);
    CHECK(stepper.Next(-5, 100) == 1);
    CHECK(Run(stepper, FrameNs, 2) == std::vector<u32>{1, 1});

    // The count of slow frames in a row.
    Core::FrameStepFps over;
    CHECK(over.Next(2 * FrameNs, 100) == 1);
    CHECK(over.Next(300'000'000, 100) == 1);
    CHECK(Run(over, 2 * FrameNs, 2) == std::vector<u32>{1, 2});
    // The count towards the next try.
    Core::FrameStepFps counted;
    Run(counted, 2 * FrameNs, 2 + Core::FrameStepFps::ProbeAfter - 1);
    CHECK(counted.Next(300'000'000, 100) == 1);
    CHECK(Run(counted, 2 * FrameNs, 2) == std::vector<u32>{1, 2});
    CHECK(Sum(Run(counted, 2 * FrameNs, Core::FrameStepFps::ProbeAfter - 1)) ==
          2 * (Core::FrameStepFps::ProbeAfter - 1));
    // A stall during a try ends it: the next frames count again.
    Core::FrameStepFps trying;
    Run(trying, 2 * FrameNs, 2 + Core::FrameStepFps::ProbeAfter);
    CHECK(trying.Next(300'000'000, 100) == 1);
    CHECK(Run(trying, 2 * FrameNs, 2) == std::vector<u32>{1, 2});
}

TEST_CASE("Frame step follows the emulation speed limit", "[game_fps]") {
    Core::FrameStepFps stepper;
    // At 200 % a 60 Hz frame lasts half as long: 60 real FPS are two game frames each.
    CHECK(Run(stepper, FrameNs, 3, 200) == std::vector<u32>{1, 2, 2});
    // At 50 % a frame may take twice as long before a second step is due.
    Core::FrameStepFps half;
    CHECK(Run(half, 2 * FrameNs, 3, 50) == std::vector<u32>{1, 1, 1});
    // Unlimited speed has no real-time target: the game's own single step.
    CHECK(Run(stepper, 5 * FrameNs, 3, 0) == std::vector<u32>{1, 1, 1});
    CHECK(Run(stepper, FrameNs, 2) == std::vector<u32>{1, 1});
}

#include <cstring>
#include "core/file_sys/hyrule_frame_step.h"

namespace {
// A main module image with only the words FindHyruleFrameStep checks.
std::vector<u8> MakeHyruleImage() {
    std::vector<u8> image(FileSys::HyruleFrameStepVariable + 4);
    const auto put32 = [&](u64 offset, u32 value) { std::memcpy(image.data() + offset, &value, 4); };
    put32(0x1af5af8, 0xb0006ac9);
    put32(0x1af5b00, 0xf9432929);
    put32(0x1af5b10, 0xb0006d08);
    put32(0x1af5b14, 0x911c8108);
    put32(0x1af5b18, 0xb9400133);
    put32(0x1af5b1c, 0xb9000113);
    const u64 variable = FileSys::HyruleFrameStepVariable;
    std::memcpy(image.data() + 0x284e650, &variable, 8);
    return image;
}
} // namespace

TEST_CASE("Hyrule Warriors frame step is found only in the checked build", "[game_fps]") {
    const auto image = MakeHyruleImage();
    std::array<u8, 0x20> build_id{};
    std::copy(FileSys::HyruleFrameStepBuildId.begin(), FileSys::HyruleFrameStepBuildId.end(),
              build_id.begin());
    REQUIRE(FileSys::FindHyruleFrameStep(image, build_id) == 0x28966a8);

    SECTION("another build") {
        auto other = build_id;
        other[19] ^= 1;
        CHECK_FALSE(FileSys::FindHyruleFrameStep(image, other));
        CHECK_FALSE(FileSys::FindHyruleFrameStep(image, std::span{build_id}.first(19)));
    }
    SECTION("any changed code word") {
        for (const u64 offset : {0x1af5af8, 0x1af5b00, 0x1af5b10, 0x1af5b14, 0x1af5b18, 0x1af5b1c}) {
            auto changed = image;
            changed[offset] ^= 1;
            CHECK_FALSE(FileSys::FindHyruleFrameStep(changed, build_id));
        }
    }
    SECTION("the GOT slot points elsewhere") {
        auto changed = image;
        changed[0x284e650] ^= 8;
        CHECK_FALSE(FileSys::FindHyruleFrameStep(changed, build_id));
    }
    SECTION("truncated images") {
        // One byte short of the variable, of the GOT slot and of the last code word.
        CHECK_FALSE(FileSys::FindHyruleFrameStep(std::span{image}.first(image.size() - 1), build_id));
        CHECK_FALSE(FileSys::FindHyruleFrameStep(std::span{image}.first(0x284e650 + 7), build_id));
        CHECK_FALSE(FileSys::FindHyruleFrameStep(std::span{image}.first(0x1af5b1c + 3), build_id));
        CHECK_FALSE(FileSys::FindHyruleFrameStep(std::span{image}.first(0x1af5af8 + 3), build_id));
        CHECK(FileSys::FindHyruleFrameStep(std::span{image}.first(image.size()), build_id));
    }
}
