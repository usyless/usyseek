#include "check.hpp"

#include <usyseek/ui/flow.hpp>

using namespace usyseek::ui;

int main() {
    {
        const FlowResult r = flow_layout({}, 8.0f, 6.0f, 300.0f);
        CHECK(r.placements.empty());
        CHECK(r.height == 0.0f);
    }

    {
        const FlowResult r = flow_layout({{100, 30}, {80, 30}, {60, 30}}, 10.0f, 6.0f, 300.0f);
        CHECK(r.placements.size() == 3);
        CHECK(r.placements[0].x == 0.0f);
        CHECK(r.placements[1].x == 110.0f);
        CHECK(r.placements[2].x == 200.0f);
        for (const auto& p : r.placements) {
            CHECK(p.y == 0.0f);
            CHECK(p.line == 0);
            CHECK(!p.oversize);
        }
        CHECK(r.height == 30.0f);
    }

    {
        const FlowResult r = flow_layout({{100, 30}, {100, 30}, {100, 30}}, 10.0f, 6.0f, 250.0f);
        CHECK(r.placements[0].line == 0);
        CHECK(r.placements[1].line == 0);
        CHECK(r.placements[2].line == 1);
        CHECK(r.placements[1].x == 110.0f);
        CHECK(r.placements[2].x == 0.0f);
        CHECK(r.placements[2].y == 36.0f);
        CHECK(r.height == 66.0f);
    }

    {
        const FlowResult r = flow_layout({{100, 30}, {100, 30}}, 10.0f, 6.0f, 210.0f);
        CHECK(r.placements[1].line == 0);
        CHECK(r.placements[1].x == 110.0f);

        const FlowResult narrower = flow_layout({{100, 30}, {100, 30}}, 10.0f, 6.0f, 209.0f);
        CHECK(narrower.placements[1].line == 1);
        CHECK(narrower.placements[1].x == 0.0f);
    }

    {
        const FlowResult r = flow_layout({{50, 30}, {400, 30}, {50, 30}}, 10.0f, 6.0f, 200.0f);
        CHECK(r.placements[0].line == 0);
        CHECK(r.placements[1].line == 1);
        CHECK(r.placements[1].x == 0.0f);
        CHECK(r.placements[1].width == 200.0f);
        CHECK(r.placements[1].oversize);
        CHECK(r.placements[2].line == 2);
        CHECK(!r.placements[2].oversize);
        CHECK(r.height == 30.0f * 3 + 6.0f * 2);
    }

    {
        const FlowResult r = flow_layout({{400, 20}}, 10.0f, 6.0f, 200.0f);
        CHECK(r.placements.size() == 1);
        CHECK(r.placements[0].oversize);
        CHECK(r.height == 20.0f);
    }

    {
        const FlowResult r = flow_layout({{100, 20}, {100, 40}, {100, 10}}, 10.0f, 6.0f, 220.0f);
        CHECK(r.placements[2].y == 46.0f);
        CHECK(r.height == 46.0f + 10.0f);
    }

    {
        const FlowResult r = flow_layout({{60, 30}, {60, 30, true}, {60, 30, true}}, 10.0f, 6.0f, 300.0f);
        CHECK(r.placements[1].width == 60.0f + 50.0f);
        CHECK(r.placements[2].width == 60.0f + 50.0f);
        CHECK(r.placements[2].x + r.placements[2].width == 300.0f);
    }

    {
        const FlowResult r = flow_layout({{200, 30}, {100, 30, true}}, 10.0f, 6.0f, 250.0f);
        CHECK(r.placements[1].line == 1);
        CHECK(r.placements[1].width == 250.0f);
    }

    return usyseek::test::failures != 0;
}
