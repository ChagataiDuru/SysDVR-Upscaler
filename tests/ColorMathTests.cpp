#include <doctest/doctest.h>

#include "utility/ColorMath.h"

namespace {
constexpr ns60::ColorDescription limited709{ns60::ColorRange::Limited, ns60::ColorMatrix::Bt709};
}

TEST_CASE("BT.709 limited black and white map to endpoints") {
    const auto black = ns60::yuvToRgb(16, 128, 128, limited709);
    const auto white = ns60::yuvToRgb(235, 128, 128, limited709);
    for (const auto channel : black) CHECK(channel == doctest::Approx(0.0F).epsilon(0.0001));
    for (const auto channel : white) CHECK(channel == doctest::Approx(1.0F).epsilon(0.0001));
}

TEST_CASE("BT.709 neutral gray remains neutral") {
    const auto gray = ns60::yuvToRgb(126, 128, 128, limited709);
    CHECK(gray[0] == doctest::Approx(gray[1]).epsilon(0.0001));
    CHECK(gray[1] == doctest::Approx(gray[2]).epsilon(0.0001));
    CHECK(gray[0] == doctest::Approx(110.0 / 219.0).epsilon(0.001));
}

TEST_CASE("BT.709 strong chroma vectors favor the expected channels") {
    const auto redLike = ns60::yuvToRgb(81, 90, 240, limited709);
    CHECK(redLike[0] > 0.95F);
    CHECK(redLike[1] < 0.1F);
    CHECK(redLike[2] < 0.1F);
    const auto blueLike = ns60::yuvToRgb(41, 240, 110, limited709);
    CHECK(blueLike[2] > 0.95F);
    CHECK(blueLike[0] < 0.1F);
}

TEST_CASE("Full range path does not use studio offsets") {
    const ns60::ColorDescription full709{ns60::ColorRange::Full, ns60::ColorMatrix::Bt709};
    const auto fullBlack = ns60::yuvToRgb(0, 128, 128, full709);
    const auto limitedCodeBlack = ns60::yuvToRgb(16, 128, 128, full709);
    CHECK(fullBlack[0] == doctest::Approx(0.0F).epsilon(0.0001));
    CHECK(limitedCodeBlack[0] == doctest::Approx(16.0 / 255.0).epsilon(0.001));
}

