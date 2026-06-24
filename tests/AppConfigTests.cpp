#include <doctest/doctest.h>

#include "app/AppConfig.h"

TEST_CASE("CLI requires input") {
    const auto result = ns60::parseCommandLine({"NexusStream60.exe"}, true);
    CHECK_FALSE(result.config);
    CHECK(result.error.find("Missing") != std::string::npos);
}

TEST_CASE("CLI parses the required baseline command") {
    const auto result = ns60::parseCommandLine({"NexusStream60.exe", "--input", "sample with spaces.mp4",
        "--width", "1920", "--height", "1080", "--upscale", "bilinear", "--loop"}, false);
    REQUIRE(result.config);
    CHECK(result.config->input.filename() == "sample with spaces.mp4");
    CHECK(result.config->outputWidth == 1920);
    CHECK(result.config->outputHeight == 1080);
    CHECK(result.config->loop);
}

TEST_CASE("CLI rejects invalid dimensions, mode, and boolean") {
    CHECK_FALSE(ns60::parseCommandLine({"app", "--input", "x", "--width", "0"}, false).config);
    CHECK_FALSE(ns60::parseCommandLine({"app", "--input", "x", "--upscale", "fsr"}, false).config);
    CHECK_FALSE(ns60::parseCommandLine({"app", "--input", "x", "--vsync", "yes"}, false).config);
}

TEST_CASE("CLI handles help without an input") {
    const auto result = ns60::parseCommandLine({"app", "--help"}, false);
    CHECK(result.action == ns60::ParseAction::Help);
    CHECK(result.error.empty());
}

TEST_CASE("CLI parses quality modes, comparison, and sharpening") {
    const auto result = ns60::parseCommandLine({"app", "--input", "x.mp4", "--upscale", "fsr1-easu-rcas",
        "--cas-sharpness", "0.35", "--rcas-sharpness", "0.25", "--anti-ringing", "off",
        "--compare", "bilinear,fsr1-easu-rcas", "--presentation", "fill"}, false);
    REQUIRE(result.config);
    CHECK(static_cast<int>(result.config->upscale) == static_cast<int>(ns60::UpscaleMode::Fsr1EasuRcas));
    CHECK(result.config->sharpen.casSharpness == doctest::Approx(0.35F));
    CHECK(result.config->sharpen.rcasSharpness == doctest::Approx(0.25F));
    CHECK_FALSE(result.config->antiRinging);
    REQUIRE(result.config->comparisonA);
    REQUIRE(result.config->comparisonB);
    CHECK(static_cast<int>(*result.config->comparisonA) == static_cast<int>(ns60::UpscaleMode::Bilinear));
    CHECK(static_cast<int>(*result.config->comparisonB) == static_cast<int>(ns60::UpscaleMode::Fsr1EasuRcas));
    CHECK(static_cast<int>(result.config->presentation) == static_cast<int>(ns60::PresentationMode::Fill));
}

TEST_CASE("CLI rejects invalid sharpening and comparison") {
    CHECK_FALSE(ns60::parseCommandLine({"app", "--input", "x", "--cas-sharpness", "nan"}, false).config);
    CHECK_FALSE(ns60::parseCommandLine({"app", "--input", "x", "--rcas-sharpness", "1.1"}, false).config);
    CHECK_FALSE(ns60::parseCommandLine({"app", "--input", "x", "--compare", "bilinear,nope"}, false).config);
}

TEST_CASE("CLI parses monitor, final filter, and chroma options") {
    const auto result = ns60::parseCommandLine({"app", "--input", "x.mp4", "--monitor", "0",
        "--final-filter", "nearest", "--chroma-upscale", "edge-aware"}, false);
    REQUIRE(result.config);
    REQUIRE(result.config->monitorIndex);
    CHECK(*result.config->monitorIndex == 0);
    CHECK(static_cast<int>(result.config->finalFilter) == static_cast<int>(ns60::FinalFilter::Nearest));
    CHECK(static_cast<int>(result.config->chromaUpscale) == static_cast<int>(ns60::ChromaUpscaleMode::EdgeAware));
}

TEST_CASE("CLI rejects invalid phase 1.5 options") {
    CHECK_FALSE(ns60::parseCommandLine({"app", "--input", "x", "--monitor", "-1"}, false).config);
    CHECK_FALSE(ns60::parseCommandLine({"app", "--input", "x", "--final-filter", "cubic"}, false).config);
    CHECK_FALSE(ns60::parseCommandLine({"app", "--input", "x", "--chroma-upscale", "edgy"}, false).config);
}
TEST_CASE("CLI parses SysDVR pipe source") {
    const auto result = ns60::parseCommandLine({"app", "--source", "sysdvr-pipe", "--pipe-name", "SysDVR-Upscaler.Video",
        "--presentation", "exact"}, false);
    REQUIRE(result.config);
    CHECK(static_cast<int>(result.config->source) == static_cast<int>(ns60::SourceKind::SysDvrPipe));
    CHECK(result.config->pipeName == "SysDVR-Upscaler.Video");
    CHECK(static_cast<int>(result.config->presentation) == static_cast<int>(ns60::PresentationMode::Exact));
}

TEST_CASE("CLI parses unified SysDVR launch") {
    const auto result = ns60::parseCommandLine({"app", "--source", "sysdvr", "--sysdvr-bridge", ".\\artifacts\\sysdvr-upscaler-bridge\\win-x64\\SysDVR-Client.exe",
        "--quality-preset", "balanced", "--fullscreen", "--borderless"}, false);
    REQUIRE(result.config);
    CHECK(static_cast<int>(result.config->source) == static_cast<int>(ns60::SourceKind::SysDvr));
    CHECK(result.config->sysdvrBridge.filename() == "SysDVR-Client.exe");
    CHECK(static_cast<int>(result.config->upscale) == static_cast<int>(ns60::UpscaleMode::Fsr1EasuRcas));
    CHECK(result.config->fullscreen);
    CHECK(result.config->borderless);
}

TEST_CASE("CLI rejects live-only option conflicts") {
    CHECK_FALSE(ns60::parseCommandLine({"app", "--source", "sysdvr-pipe", "--input", "x.mp4"}, false).config);
    CHECK_FALSE(ns60::parseCommandLine({"app", "--source", "sysdvr-pipe", "--loop"}, false).config);
    CHECK_FALSE(ns60::parseCommandLine({"app", "--source", "sysdvr"}, false).config);
}
