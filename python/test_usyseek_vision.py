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
