#include "check.hpp"

#include <usyseek/progress.hpp>

using usyseek::displayed_progress;

namespace {

void shows_at_rest_counts_before_indexer_has_counted_files() {
    const auto p = displayed_progress(true, 0, 0, 900, 1000);
    CHECK(p.embedded == 900);
    CHECK(p.total == 1000);
}

void shows_live_counts_once_indexer_has_counted_files() {
    const auto p = displayed_progress(true, 950, 1200, 900, 1000);
    CHECK(p.embedded == 950);
    CHECK(p.total == 1200);
}

void shows_live_counts_when_nothing_is_embedded_yet() {
    const auto p = displayed_progress(true, 0, 1200, 900, 1000);
    CHECK(p.embedded == 0);
    CHECK(p.total == 1200);
}

void shows_at_rest_counts_when_indexer_is_not_running() {
    const auto p = displayed_progress(false, 950, 1200, 900, 1000);
    CHECK(p.embedded == 900);
    CHECK(p.total == 1000);
}

void shows_zero_when_there_is_no_previous_index() {
    const auto p = displayed_progress(true, 0, 0, 0, 0);
    CHECK(p.embedded == 0);
    CHECK(p.total == 0);
}

} // namespace

int main() {
    shows_at_rest_counts_before_indexer_has_counted_files();
    shows_live_counts_once_indexer_has_counted_files();
    shows_live_counts_when_nothing_is_embedded_yet();
    shows_at_rest_counts_when_indexer_is_not_running();
    shows_zero_when_there_is_no_previous_index();
    return usyseek::test::failures != 0;
}
