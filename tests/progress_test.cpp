#include "check.hpp"

#include <usyseek/progress.hpp>

using usyseek::displayed_progress;

namespace {

void shows_at_rest_counts_before_indexer_has_counted_files() {
    const auto p = displayed_progress(true, 0, 0, 900, 1000, 900);
    CHECK(p.embedded == 900);
    CHECK(p.total == 1000);
}

void shows_live_counts_once_indexer_has_counted_files() {
    const auto p = displayed_progress(true, 950, 1200, 900, 1000, 900);
    CHECK(p.embedded == 950);
    CHECK(p.total == 1200);
}

void shows_live_counts_when_nothing_is_embedded_yet() {
    const auto p = displayed_progress(true, 0, 1200, 900, 1000, 900);
    CHECK(p.embedded == 0);
    CHECK(p.total == 1200);
}

void shows_at_rest_counts_when_indexer_is_not_running() {
    const auto p = displayed_progress(false, 950, 1200, 900, 1000, 900);
    CHECK(p.embedded == 900);
    CHECK(p.total == 1000);
}

void shows_zero_when_there_is_no_previous_index() {
    const auto p = displayed_progress(true, 0, 0, std::nullopt, 0, std::nullopt);
    CHECK(p.embedded == 0);
    CHECK(p.total == 0);
}

void at_rest_counts_exclude_failed_files_before_the_database_count_arrives() {
    const auto p = displayed_progress(false, 0, 0, 990, 1000, std::nullopt);
    CHECK(p.embedded == 990);
    CHECK(p.total == 1000);
}

void at_rest_counts_ignore_a_stale_database_count_after_a_run() {
    const auto p = displayed_progress(false, 0, 0, 990, 1000, 700);
    CHECK(p.embedded == 990);
    CHECK(p.total == 1000);
}

void at_rest_counts_fall_back_to_the_database_count_without_saved_state() {
    const auto p = displayed_progress(false, 0, 0, std::nullopt, 1000, 990);
    CHECK(p.embedded == 990);
    CHECK(p.total == 1000);
}

void at_rest_total_covers_a_database_count_above_the_last_found_count() {
    const auto p = displayed_progress(false, 0, 0, std::nullopt, 0, 50);
    CHECK(p.embedded == 50);
    CHECK(p.total == 50);
}

} // namespace

int main() {
    shows_at_rest_counts_before_indexer_has_counted_files();
    shows_live_counts_once_indexer_has_counted_files();
    shows_live_counts_when_nothing_is_embedded_yet();
    shows_at_rest_counts_when_indexer_is_not_running();
    shows_zero_when_there_is_no_previous_index();
    at_rest_counts_exclude_failed_files_before_the_database_count_arrives();
    at_rest_counts_ignore_a_stale_database_count_after_a_run();
    at_rest_counts_fall_back_to_the_database_count_without_saved_state();
    at_rest_total_covers_a_database_count_above_the_last_found_count();
    return usyseek::test::failures != 0;
}
