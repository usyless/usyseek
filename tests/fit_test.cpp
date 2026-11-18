#include "check.hpp"

#include <usyseek/ui/fit.hpp>

using namespace usyseek::ui;

int main() {
    const FitRect tile{.left = 10.0f, .top = 20.0f, .width = 200.0f, .height = 100.0f};

    CHECK((fit_image(400, 100, tile, ThumbFit::Crop) == FitRect{-90.0f, 20.0f, 400.0f, 100.0f}));
    CHECK((fit_image(100, 400, tile, ThumbFit::Crop) == FitRect{10.0f, -330.0f, 200.0f, 800.0f}));
    CHECK((fit_image(400, 200, tile, ThumbFit::Crop) == tile));

    CHECK((fit_image(400, 100, tile, ThumbFit::Contain) == FitRect{10.0f, 45.0f, 200.0f, 50.0f}));
    CHECK((fit_image(100, 400, tile, ThumbFit::Contain) == FitRect{97.5f, 20.0f, 25.0f, 100.0f}));
    CHECK((fit_image(400, 200, tile, ThumbFit::Contain) == tile));

    for (const ThumbFit fit : {ThumbFit::Crop, ThumbFit::Contain}) {
        CHECK((fit_image(0, 100, tile, fit) == tile));
        CHECK((fit_image(100, 0, tile, fit) == tile));
        CHECK((fit_image(100, 100, FitRect{0, 0, 0, 50}, fit) == FitRect{0, 0, 0, 50}));
    }

    CHECK(parse_thumb_fit("crop") == ThumbFit::Crop);
    CHECK(parse_thumb_fit("contain") == ThumbFit::Contain);
    CHECK(parse_thumb_fit("") == ThumbFit::Crop);
    CHECK(parse_thumb_fit("stretch") == ThumbFit::Crop);

    return usyseek::test::failures != 0;
}
