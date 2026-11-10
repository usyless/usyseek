import unittest

import usyseek_vision as v


def measuring(rates):
    calls = []

    def measure(size):
        calls.append(size)
        return rates[size]

    return measure, calls


class CandidateSizesTest(unittest.TestCase):
    def test_doubles_up_to_the_cap_and_includes_it(self):
        self.assertEqual(v.candidate_sizes(64), [8, 16, 32, 64])
        self.assertEqual(v.candidate_sizes(48), [8, 16, 32, 48])

    def test_cap_below_first_size_is_the_only_candidate(self):
        self.assertEqual(v.candidate_sizes(8), [8])
        self.assertEqual(v.candidate_sizes(4), [4])


def timings(values):
    remaining = iter(values)
    calls = []

    def run_once():
        calls.append(1)
        return next(remaining)

    return run_once, calls


class WarmUpTest(unittest.TestCase):
    def test_stops_once_consecutive_runs_agree(self):
        run_once, calls = timings([1.0, 0.5, 0.31, 0.30, 0.30, 0.30])
        v.warm_up(run_once)
        self.assertEqual(len(calls), 4)

    def test_runs_the_minimum_even_when_already_steady(self):
        run_once, calls = timings([0.3] * 10)
        v.warm_up(run_once)
        self.assertEqual(len(calls), v._BENCH_WARMUP_MIN_RUNS)

    def test_stops_at_the_maximum_when_times_never_settle(self):
        run_once, calls = timings([1.0 / (2 ** n) for n in range(20)])
        v.warm_up(run_once)
        self.assertEqual(len(calls), v._BENCH_WARMUP_MAX_RUNS)


class TimedRateTest(unittest.TestCase):
    def test_runs_until_the_minimum_duration(self):
        run_once, calls = timings([0.1] * 20)
        v.timed_rate(run_once, 8)
        self.assertEqual(len(calls), 3)
        run_once, calls = timings([0.05] * 20)
        v.timed_rate(run_once, 8)
        self.assertEqual(len(calls), 6)

    def test_runs_the_minimum_count_for_slow_runs(self):
        run_once, calls = timings([2.0] * 20)
        v.timed_rate(run_once, 8)
        self.assertEqual(len(calls), v._BENCH_TIMED_MIN_RUNS)

    def test_stops_at_the_maximum_run_count(self):
        run_once, calls = timings([0.0001] * 100)
        v.timed_rate(run_once, 8)
        self.assertEqual(len(calls), v._BENCH_TIMED_MAX_RUNS)

    def test_rate_is_images_per_second_at_the_median_run(self):
        run_once, _ = timings([0.1, 0.1, 5.0])
        self.assertEqual(v.timed_rate(run_once, 32), 320)

    def test_even_run_count_averages_the_middle_pair(self):
        run_once, _ = timings([0.05, 0.05, 0.1, 0.15])
        self.assertAlmostEqual(v.timed_rate(run_once, 3), 40)


class SearchBatchTest(unittest.TestCase):
    def test_picks_fastest(self):
        measure, _ = measuring({8: 100, 16: 150, 32: 200, 64: 190})
        self.assertEqual(v.search_batch([8, 16, 32, 64], measure), 32)

    def test_stops_early_on_sharp_drop(self):
        measure, calls = measuring({8: 100, 16: 200, 32: 100, 64: 900})
        self.assertEqual(v.search_batch([8, 16, 32, 64], measure), 16)
        self.assertEqual(calls, [8, 16, 32])

    def test_mild_dip_keeps_searching(self):
        measure, calls = measuring({8: 100, 16: 200, 32: 180, 64: 260})
        self.assertEqual(v.search_batch([8, 16, 32, 64], measure), 64)
        self.assertEqual(calls, [8, 16, 32, 64])

    def test_never_exceeds_the_given_sizes(self):
        measure, calls = measuring({8: 100, 16: 200, 24: 300})
        self.assertEqual(v.search_batch([8, 16, 24], measure), 24)
        self.assertEqual(max(calls), 24)

    def test_single_candidate(self):
        measure, calls = measuring({8: 100})
        self.assertEqual(v.search_batch([8], measure), 8)
        self.assertEqual(calls, [8])

    def test_out_of_memory_ends_the_search(self):
        measure, calls = measuring({8: 100, 16: 200, 32: None, 64: 900})
        self.assertEqual(v.search_batch([8, 16, 32, 64], measure), 16)
        self.assertEqual(calls, [8, 16, 32])

    def test_out_of_memory_at_first_size_falls_back_to_it(self):
        measure, _ = measuring({8: None, 16: 200})
        self.assertEqual(v.search_batch([8, 16], measure), 8)

    def test_negligible_gain_prefers_smaller_size(self):
        measure, _ = measuring({8: 100, 16: 101})
        self.assertEqual(v.search_batch([8, 16], measure), 8)


if __name__ == "__main__":
    unittest.main()
