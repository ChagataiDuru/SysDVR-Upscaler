#include <doctest/doctest.h>
#include "render/Upscaling.h"
#include <array>
#include <cmath>
#include <limits>
using namespace ns60;

TEST_CASE("upscale mode names round trip") {
    constexpr std::array modes{UpscaleMode::Nearest, UpscaleMode::Bilinear, UpscaleMode::BicubicCatmullRom,
        UpscaleMode::Lanczos2, UpscaleMode::BilinearCas, UpscaleMode::Lanczos2Cas, UpscaleMode::Fsr1Easu, UpscaleMode::Fsr1EasuRcas};
    for (const auto mode : modes) { const auto parsed=parseUpscaleMode(toString(mode)); REQUIRE(parsed); CHECK(static_cast<int>(*parsed)==static_cast<int>(mode)); }
    CHECK_FALSE(parseUpscaleMode("FSR1-EASU")); CHECK_FALSE(parseUpscaleMode("unknown"));
}

TEST_CASE("new quality enum names round trip") {
    for (const auto filter : {FinalFilter::Nearest, FinalFilter::Bilinear}) { const auto parsed=parseFinalFilter(toString(filter)); REQUIRE(parsed); CHECK(static_cast<int>(*parsed)==static_cast<int>(filter)); }
    for (const auto mode : {ChromaUpscaleMode::Bilinear, ChromaUpscaleMode::BicubicCatmullRom, ChromaUpscaleMode::Lanczos2, ChromaUpscaleMode::EdgeAware}) { const auto parsed=parseChromaUpscaleMode(toString(mode)); REQUIRE(parsed); CHECK(static_cast<int>(*parsed)==static_cast<int>(mode)); }
    CHECK_FALSE(parseFinalFilter("linear"));
    CHECK_FALSE(parseChromaUpscaleMode("edgeaware"));
}

TEST_CASE("sharpness validation") {
    CHECK(validSharpness(0.0F)); CHECK(validSharpness(1.0F)); CHECK_FALSE(validSharpness(-0.01F)); CHECK_FALSE(validSharpness(1.01F));
    CHECK_FALSE(validSharpness(std::numeric_limits<float>::quiet_NaN())); CHECK_FALSE(validSharpness(std::numeric_limits<float>::infinity()));
}

TEST_CASE("pixel-center mapping") {
    CHECK(mapOutputPixelCenter(0,1920,1920)==doctest::Approx(0.0)); CHECK(mapOutputPixelCenter(960,1920,1920)==doctest::Approx(960.0));
    CHECK(mapOutputPixelCenter(1919,1920,1920)==doctest::Approx(1919.0)); CHECK(mapOutputPixelCenter(0,1280,1920)==doctest::Approx(-1.0/6.0));
    CHECK(mapOutputPixelCenter(1919,1280,1920)==doctest::Approx(1279.0+1.0/6.0));
}

TEST_CASE("left-sited chroma coordinate mapping") {
    CHECK(leftSitedChromaCoordinate(0, true) == doctest::Approx(0.0));
    CHECK(leftSitedChromaCoordinate(1, true) == doctest::Approx(0.5));
    CHECK(leftSitedChromaCoordinate(2, true) == doctest::Approx(1.0));
    CHECK(leftSitedChromaCoordinate(0, false) == doctest::Approx(-0.25));
    CHECK(leftSitedChromaCoordinate(1, false) == doctest::Approx(0.25));
}

TEST_CASE("filter math is finite and constant preserving") {
    for (double f : {0.0,0.125,0.5,0.875}) { double cubic{}, lanczos{}; for(int tap=-1;tap<=2;++tap){cubic+=catmullRomWeight(tap-f);lanczos+=lanczos2Weight(tap-f);} CHECK(cubic==doctest::Approx(1.0).epsilon(1e-12)); CHECK(std::isfinite(lanczos)); CHECK(lanczos!=doctest::Approx(0.0)); }
    CHECK(lanczos2Weight(0.0)==doctest::Approx(1.0)); CHECK(std::isfinite(lanczos2Weight(1.0e-15)));
}

TEST_CASE("presentation geometry detects exact 1:1 output") {
    auto g=calculatePresentationGeometry(1920,1080,1920,1080,PresentationMode::Exact);
    CHECK(g.viewportX==0); CHECK(g.viewportY==0); CHECK(g.viewportWidth==1920); CHECK(g.viewportHeight==1080);
    CHECK(g.exactOneToOne); CHECK_FALSE(g.finalResampleActive); CHECK(static_cast<int>(g.effectiveMode)==static_cast<int>(PresentationMode::Exact));
}

TEST_CASE("presentation geometry fit and fill preserve aspect ratio") {
    auto g=calculatePresentationGeometry(1920,1080,1920,1061,PresentationMode::Fit);
    CHECK(static_cast<double>(g.viewportWidth)/static_cast<double>(g.viewportHeight)==doctest::Approx(16.0/9.0).epsilon(0.002));
    CHECK(g.finalResampleActive);
    g=calculatePresentationGeometry(1920,1080,3440,1440,PresentationMode::Fit);
    CHECK(static_cast<double>(g.viewportWidth)/static_cast<double>(g.viewportHeight)==doctest::Approx(16.0/9.0).epsilon(0.002));
    CHECK(g.viewportWidth<=3440); CHECK(g.viewportHeight<=1440);
    g=calculatePresentationGeometry(1920,1080,1080,1920,PresentationMode::Fill);
    CHECK(static_cast<double>(g.viewportWidth)/static_cast<double>(g.viewportHeight)==doctest::Approx(16.0/9.0).epsilon(0.002));
    CHECK(g.viewportWidth>=1080); CHECK(g.viewportHeight>=1920);
}

TEST_CASE("exact mode centers without scaling when it fits and falls back when it cannot") {
    auto g=calculatePresentationGeometry(1920,1080,2560,1440,PresentationMode::Exact);
    CHECK(g.viewportWidth==1920); CHECK(g.viewportHeight==1080);
    CHECK(g.viewportX==320); CHECK(g.viewportY==180);
    CHECK(g.exactOneToOne); CHECK_FALSE(g.finalResampleActive);
    g=calculatePresentationGeometry(1920,1080,1280,720,PresentationMode::Exact);
    CHECK(static_cast<int>(g.effectiveMode)==static_cast<int>(PresentationMode::Fit));
    CHECK(g.viewportWidth==1280); CHECK(g.viewportHeight==720);
    CHECK(g.finalResampleActive);
}