#include "check.hpp"

#include <usyseek/ui/flow.hpp>

#include <array>
#include <cstdio>

using namespace usyseek::ui;

namespace {

void run_flow_tests() {
    {
        const FlowResult r = flow_layout({}, 8.0f, 6.0f, 300.0f);
        CHECK(r.placements.empty());
        CHECK(r.height == 0.0f);
    }

    {
        const FlowResult r = flow_layout(std::array<FlowItem, 3>{{
                                             {.width = 100, .height = 30},
                                             {.width = 80, .height = 30},
                                             {.width = 60, .height = 30},
                                         },},
                                         10.0f, 6.0f, 300.0f);
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
        const FlowResult r = flow_layout(std::array<FlowItem, 3>{{
                                             {.width = 100, .height = 30},
                                             {.width = 100, .height = 30},
                                             {.width = 100, .height = 30},
                                         },},
                                         10.0f, 6.0f, 250.0f);
        CHECK(r.placements[0].line == 0);
        CHECK(r.placements[1].line == 0);
        CHECK(r.placements[2].line == 1);
        CHECK(r.placements[1].x == 110.0f);
        CHECK(r.placements[2].x == 0.0f);
        CHECK(r.placements[2].y == 36.0f);
        CHECK(r.height == 66.0f);
    }

    {
        const FlowResult r = flow_layout(std::array<FlowItem, 2>{{
                                             {.width = 100, .height = 30},
                                             {.width = 100, .height = 30},
                                         },},
                                         10.0f, 6.0f, 210.0f);
        CHECK(r.placements[1].line == 0);
        CHECK(r.placements[1].x == 110.0f);

        const FlowResult narrower = flow_layout(std::array<FlowItem, 2>{{
                                                    {.width = 100, .height = 30},
                                                    {.width = 100, .height = 30},
                                                },},
                                                10.0f, 6.0f, 209.0f);
        CHECK(narrower.placements[1].line == 1);
        CHECK(narrower.placements[1].x == 0.0f);
    }

    {
        const FlowResult r = flow_layout(std::array<FlowItem, 3>{{
                                             {.width = 50, .height = 30},
                                             {.width = 400, .height = 30},
                                             {.width = 50, .height = 30},
                                         },},
                                         10.0f, 6.0f, 200.0f);
        CHECK(r.placements[0].line == 0);
        CHECK(r.placements[1].line == 1);
        CHECK(r.placements[1].x == 0.0f);
        CHECK(r.placements[1].width == 200.0f);
        CHECK(r.placements[1].oversize);
        CHECK(r.placements[2].line == 2);
        CHECK(!r.placements[2].oversize);
        CHECK(r.height == (30.0f * 3) + (6.0f * 2));
    }

    {
        const FlowResult r = flow_layout(std::array<FlowItem, 1>{{{.width = 400, .height = 20}}}, 10.0f, 6.0f, 200.0f);
        CHECK(r.placements.size() == 1);
        CHECK(r.placements[0].oversize);
        CHECK(r.height == 20.0f);
    }

    {
        const FlowResult r = flow_layout(std::array<FlowItem, 3>{{
                                             {.width = 100, .height = 20},
                                             {.width = 100, .height = 40},
                                             {.width = 100, .height = 10},
                                         },},
                                         10.0f, 6.0f, 220.0f);
        CHECK(r.placements[2].y == 46.0f);
        CHECK(r.height == 46.0f + 10.0f);
    }

    {
        const FlowResult r = flow_layout(std::array<FlowItem, 3>{{
                                             {.width = 60, .height = 30},
                                             {.width = 60, .height = 30, .grow = true},
                                             {.width = 60, .height = 30, .grow = true},
                                         },},
                                         10.0f, 6.0f, 300.0f);
        CHECK(r.placements[1].width == 60.0f + 50.0f);
        CHECK(r.placements[2].width == 60.0f + 50.0f);
        CHECK(r.placements[2].x + r.placements[2].width == 300.0f);
    }

    {
        const FlowResult r = flow_layout(std::array<FlowItem, 2>{{
                                             {.width = 200, .height = 30},
                                             {.width = 100, .height = 30, .grow = true},
                                         },},
                                         10.0f, 6.0f, 250.0f);
        CHECK(r.placements[1].line == 1);
        CHECK(r.placements[1].width == 250.0f);
    }
}

} // namespace

int main() {
    try {
        run_flow_tests();
    } catch (...) {
        (void)std::fputs("flow_test: unexpected exception\n", stderr);
        return 1;
    }
    return usyseek::test::failures != 0;
}
