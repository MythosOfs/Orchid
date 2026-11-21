#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"

namespace {

using VideoCommon::ImageInfo;
using VideoCommon::ImageType;
using VideoCore::Surface::PixelFormat;

ImageInfo Target(PixelFormat format, u32 width, u32 height) {
    ImageInfo info;
    info.format = format;
    info.type = ImageType::e2D;
    info.size = {width, height, 1};
    info.block = {0, 4, 0};
    return info;
}

}

TEST_CASE("A target padded to whole GOBs is as wide as its clip", "[video_core][texture_cache]") {
    REQUIRE(VideoCommon::IsPaddedWidth(Target(PixelFormat::R16_FLOAT, 2112, 1179), 2095));
    REQUIRE(VideoCommon::IsPaddedWidth(Target(PixelFormat::D32_FLOAT, 2096, 1179), 2095));
    REQUIRE(VideoCommon::IsPaddedWidth(Target(PixelFormat::B10G11R11_FLOAT, 368, 204), 360));
    REQUIRE(VideoCommon::IsPaddedWidth(Target(PixelFormat::R16_FLOAT, 2112, 1179), 2081));
}

TEST_CASE("A clip that ends before the last GOB column is no padding",
          "[video_core][texture_cache]") {
    REQUIRE_FALSE(VideoCommon::IsPaddedWidth(Target(PixelFormat::R16_FLOAT, 2112, 1179), 2080));
    REQUIRE_FALSE(VideoCommon::IsPaddedWidth(Target(PixelFormat::D32_FLOAT, 2096, 1179), 1440));
    REQUIRE_FALSE(VideoCommon::IsPaddedWidth(Target(PixelFormat::D32_FLOAT, 2096, 1179), 2096));
    REQUIRE_FALSE(VideoCommon::IsPaddedWidth(Target(PixelFormat::D32_FLOAT, 2096, 1179), 2100));
    REQUIRE_FALSE(VideoCommon::IsPaddedWidth(Target(PixelFormat::D32_FLOAT, 2096, 1179), 0));

    ImageInfo multisampled = Target(PixelFormat::D32_FLOAT, 2096, 1179);
    multisampled.num_samples = 4;
    REQUIRE_FALSE(VideoCommon::IsPaddedWidth(multisampled, 2095));
}

TEST_CASE("A padded target follows clip changes the RT registers do not make",
          "[video_core][texture_cache]") {
    const ImageInfo target = Target(PixelFormat::R16_FLOAT, 2112, 1179);
    VideoCommon::RenderTargetClip clip;
    REQUIRE(clip.Update(2112));
    REQUIRE(VideoCommon::PaddedTargetWidth(target, clip.right) == 2112);
    REQUIRE_FALSE(clip.Update(2112));

    REQUIRE(clip.Update(2095));
    REQUIRE(VideoCommon::PaddedTargetWidth(target, clip.right) == 2095);
    REQUIRE_FALSE(clip.Update(2095));

    REQUIRE(clip.Update(2112));
    REQUIRE(VideoCommon::PaddedTargetWidth(target, clip.right) == 2112);

    REQUIRE(clip.Update(1920));
    REQUIRE(VideoCommon::PaddedTargetWidth(target, clip.right) == 2112);
}

TEST_CASE("A blit surface is narrowed only for a forward span into the padding",
          "[video_core][texture_cache]") {
    const ImageInfo surface = Target(PixelFormat::A8B8G8R8_UNORM, 16, 8);
    REQUIRE(VideoCommon::PaddedBlitWidth(surface, 0, 8) == 8);
    REQUIRE(VideoCommon::PaddedBlitWidth(surface, 4, 8) == 8);
    REQUIRE(VideoCommon::PaddedBlitWidth(surface, 8, 0) == 16);
    REQUIRE(VideoCommon::PaddedBlitWidth(surface, 16, 8) == 16);
    REQUIRE(VideoCommon::PaddedBlitWidth(surface, 8, 8) == 16);
    REQUIRE(VideoCommon::PaddedBlitWidth(surface, -4, 8) == 16);
    REQUIRE(VideoCommon::PaddedBlitWidth(surface, 0, 16) == 16);
    REQUIRE(VideoCommon::PaddedBlitWidth(surface, 0, 20) == 16);
}
