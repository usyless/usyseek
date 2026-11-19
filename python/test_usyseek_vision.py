import io
import json
import tempfile
import unittest
import weakref
from pathlib import Path
from unittest import mock

try:
    import numpy as np
except ImportError:
    np = None

try:
    import torch
except ImportError:
    torch = None

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

    def test_zero_median_does_not_divide_by_zero(self):
        run_once, _ = timings([0.0] * 50)
        self.assertGreater(v.timed_rate(run_once, 8), 0)


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

    def test_empty_sizes_raises(self):
        measure, _ = measuring({})
        with self.assertRaises(ValueError):
            v.search_batch([], measure)


class ChannelTripletTest(unittest.TestCase):
    def test_defaults_and_scalar_expansion(self):
        self.assertEqual(v._channel_triplet(None, (0.5, 0.5, 0.5)), (0.5, 0.5, 0.5))
        self.assertEqual(v._channel_triplet(0.5, (0.0, 0.0, 0.0)), (0.5, 0.5, 0.5))
        self.assertEqual(v._channel_triplet([0.25], (0.0, 0.0, 0.0)), (0.25, 0.25, 0.25))
        self.assertEqual(v._channel_triplet((0.1, 0.2, 0.3), (0.0, 0.0, 0.0)), (0.1, 0.2, 0.3))

    def test_wrong_length_raises(self):
        with self.assertRaises(ValueError):
            v._channel_triplet((0.1, 0.2), (0.5, 0.5, 0.5))


class _FakeCuda:
    def __init__(self, *, free=8 << 30, total=8 << 30, peak_per_item=32 << 20):
        self.free = free
        self.total = total
        self.peak_per_item = peak_per_item
        self.empty_cache_calls = 0
        self.base_alloc = 1 << 30
        self.peak_alloc = self.base_alloc

    def empty_cache(self):
        self.empty_cache_calls += 1

    def memory_allocated(self):
        return self.base_alloc

    def memory_reserved(self):
        return self.base_alloc

    def reset_peak_memory_stats(self):
        self.peak_alloc = self.base_alloc

    def max_memory_allocated(self):
        return self.peak_alloc

    def max_memory_reserved(self):
        return self.peak_alloc

    def mem_get_info(self):
        return self.free, self.total


class _FakeTorch:
    class OutOfMemoryError(RuntimeError):
        pass

    def __init__(self, cuda=None):
        self.cuda = cuda or _FakeCuda()


@unittest.skipUnless(np is not None, "numpy required")
class EmbedOomTest(unittest.TestCase):
    def test_empty_batch_returns_empty_array(self):
        with mock.patch.object(v, "_dim", 16):
            out = v.embed(np.zeros((0, 4, 4, 3), dtype=np.uint8))
        self.assertEqual(out.shape, (0, 16))
        self.assertEqual(out.dtype, np.float32)

    def test_splits_on_oom_and_frees_failed_frame_before_recursing(self):
        fake_torch = _FakeTorch()
        leaked_alive_during_retry = []
        ref_holder = []

        class _DummyTensor:
            pass

        def fake_run(batch):
            if len(batch) > 2:
                pinned = _DummyTensor()
                ref_holder.append(weakref.ref(pinned))
                raise _FakeTorch.OutOfMemoryError("CUDA out of memory")
            leaked_alive_during_retry.append(ref_holder[-1]() is not None)
            return np.ones((len(batch), 4), dtype=np.float32) * len(batch)

        with (
            mock.patch.object(v, "_torch", fake_torch),
            mock.patch.object(v, "_device", "cuda"),
            mock.patch.object(v, "_hint", 0),
            mock.patch.object(v, "_run", side_effect=fake_run),
        ):
            out = v.embed(np.zeros((4, 2, 2, 3), dtype=np.uint8))
            self.assertEqual(v.batch_hint(), 2)
        self.assertEqual(out.shape, (4, 4))
        self.assertEqual(fake_torch.cuda.empty_cache_calls, 1)
        self.assertEqual(leaked_alive_during_retry, [False, False])

    def test_reraises_on_single_item_oom_or_non_oom(self):
        fake_torch = _FakeTorch()
        with (
            mock.patch.object(v, "_torch", fake_torch),
            mock.patch.object(v, "_device", "cuda"),
            mock.patch.object(v, "_run", side_effect=RuntimeError("CUDA out of memory")),
            self.assertRaises(RuntimeError),
        ):
            v.embed(np.zeros((1, 2, 2, 3), dtype=np.uint8))

        with (
            mock.patch.object(v, "_torch", fake_torch),
            mock.patch.object(v, "_device", "cuda"),
            mock.patch.object(v, "_run", side_effect=ValueError("bad input")),
            self.assertRaises(ValueError),
        ):
            v.embed(np.zeros((4, 2, 2, 3), dtype=np.uint8))


@unittest.skipUnless(np is not None, "numpy required")
class SuggestBatchTest(unittest.TestCase):
    def test_cpu_respects_small_limit(self):
        with mock.patch.object(v, "_device", "cpu"):
            self.assertEqual(v.suggest_batch(4), 4)
            self.assertEqual(v.suggest_batch(0), 1)
            self.assertEqual(v.suggest_batch(64), 16)

    def test_cuda_respects_limit_below_eight(self):
        cuda = _FakeCuda(free=12 << 30, total=12 << 30)
        fake_torch = _FakeTorch(cuda)
        seen = []

        def fake_run(batch):
            seen.append(len(batch))
            cuda.peak_alloc = cuda.base_alloc + len(batch) * (32 << 20)
            return np.zeros((len(batch), 4), dtype=np.float32)

        with (
            mock.patch.object(v, "_torch", fake_torch),
            mock.patch.object(v, "_device", "cuda"),
            mock.patch.object(v, "_size", 224),
            mock.patch.object(v, "_run", side_effect=fake_run),
        ):
            self.assertEqual(v.suggest_batch(4), 4)
        self.assertEqual(seen, [4])

    def test_cuda_probe_oom_halves_probe_and_caps_batch(self):
        cuda = _FakeCuda(free=12 << 30, total=12 << 30)
        fake_torch = _FakeTorch(cuda)
        seen = []

        def fake_run(batch):
            seen.append(len(batch))
            if len(batch) > 4:
                raise _FakeTorch.OutOfMemoryError("HIP out of memory")
            cuda.peak_alloc = cuda.base_alloc + len(batch) * (32 << 20)
            return np.zeros((len(batch), 4), dtype=np.float32)

        with (
            mock.patch.object(v, "_torch", fake_torch),
            mock.patch.object(v, "_device", "cuda"),
            mock.patch.object(v, "_size", 224),
            mock.patch.object(v, "_run", side_effect=fake_run),
        ):
            self.assertEqual(v.suggest_batch(128), 4)
        self.assertEqual(seen, [8, 4])


class ExportTextEdgeCasesTest(unittest.TestCase):
    def test_missing_weights_raises_file_not_found(self):
        with tempfile.TemporaryDirectory() as td, self.assertRaises(FileNotFoundError):
            v.export_text(td, "no-such-model")

    def test_existing_onnx_returns_immediately(self):
        with tempfile.TemporaryDirectory() as td:
            onnx_file = Path(td) / "demo-text.onnx"
            onnx_file.write_bytes(b"existing")
            self.assertEqual(v.export_text(td, "demo"), str(onnx_file))

    @unittest.skipUnless(torch is not None, "torch required")
    def test_classic_clip_causal_attn_mask_fallback(self):
        buf = torch.empty((4, 4), dtype=torch.float32, device="meta")
        mask = v._build_text_meta_buffer(torch, object(), "attn_mask", buf)
        self.assertEqual(mask.device.type, "cpu")
        self.assertEqual(mask.shape, (4, 4))
        expected = torch.full((4, 4), float("-inf"), dtype=torch.float32).triu_(1)
        self.assertTrue(torch.equal(mask, expected))

        class _WithBuilder:
            def build_causal_mask(self):
                return torch.ones((4, 4), dtype=torch.float32)

        built = v._build_text_meta_buffer(torch, _WithBuilder(), "attn_mask", buf)
        self.assertTrue(torch.equal(built, torch.ones((4, 4), dtype=torch.float32)))


class _FakePipe:
    def __init__(self, messages, max_write_chunk=None):
        payload = "".join(json.dumps(m) + "\n" for m in messages).encode("utf-8")
        self._reader = io.BytesIO(payload)
        self._written = bytearray()
        self._max_write_chunk = max_write_chunk

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self, n=-1):
        return self._reader.read(n)

    def write(self, data):
        raw = bytes(data)
        if self._max_write_chunk is not None:
            raw = raw[: self._max_write_chunk]
        self._written.extend(raw)
        return len(raw)

    def responses(self):
        lines = [ln for ln in self._written.decode("utf-8").splitlines() if ln]
        return [json.loads(ln) for ln in lines]


class _FakeMmap:
    def __init__(self, size):
        self._buf = bytearray(size)
        self.closed = False

    def __buffer__(self, flags):
        if self.closed:
            raise ValueError("mmap closed or invalid")
        return memoryview(self._buf)

    def close(self):
        self.closed = True


@unittest.skipUnless(np is not None, "numpy required")
class WorkerProtocolTest(unittest.TestCase):
    def _run_with_script(self, script_text, messages, *, max_write_chunk=None, mmap_factory=None):
        with tempfile.TemporaryDirectory() as td:
            script_path = ""
            if script_text is not None:
                p = Path(td) / "custom_vision.py"
                p.write_text(script_text, encoding="utf-8")
                script_path = str(p)
            pipe = _FakePipe(messages, max_write_chunk=max_write_chunk)
            orig_open = open

            def fake_open(path, mode="r", *args, **kwargs):
                if path == "fake://pipe":
                    return pipe
                return orig_open(path, mode, *args, **kwargs)

            patches = [mock.patch("builtins.open", side_effect=fake_open)]
            if mmap_factory is not None:
                patches.append(mock.patch("mmap.mmap", side_effect=mmap_factory))
            with patches[0]:
                if len(patches) == 2:
                    with patches[1]:
                        v._run_worker("fake://pipe", script_path)
                else:
                    v._run_worker("fake://pipe", script_path)
            return pipe.responses()

    def test_benchmark_batch_custom_script_fallbacks(self):
        only_bench = "def benchmark_batch(limit=128):\n    return 24\n"
        res = self._run_with_script(only_bench, [{"cmd": "benchmark_batch", "limit": 64}, {"cmd": "quit"}])
        self.assertEqual(res, [{"ok": True, "batch": 24}])

        only_suggest = "def suggest_batch(limit=128):\n    return 12\n"
        res = self._run_with_script(only_suggest, [{"cmd": "benchmark_batch", "limit": 64}, {"cmd": "quit"}])
        self.assertEqual(res, [{"ok": True, "batch": 12}])

        neither = "def load(cfg):\n    return {'size': 4, 'dim': 4}\n"
        res = self._run_with_script(neither, [{"cmd": "benchmark_batch", "limit": 64}, {"cmd": "quit"}])
        self.assertEqual(res, [{"ok": True, "batch": 16}])

    def test_custom_script_error_reported_over_pipe(self):
        broken = "raise RuntimeError('custom boom')\n"
        res = self._run_with_script(broken, [{"cmd": "load", "cfg": {}}, {"cmd": "quit"}])
        self.assertEqual(len(res), 1)
        self.assertFalse(res[0]["ok"])
        self.assertIn("custom boom", res[0]["error"])

    def test_embed_validation_and_partial_pipe_writes(self):
        script = (
            "import numpy as np\n"
            "def load(cfg):\n"
            "    return {'size': 2, 'dim': 4}\n"
            "def embed(arr):\n"
            "    return np.ones((len(arr), 3), dtype=np.float32)\n"
        )
        mmaps = []

        def make_mmap(_fd, length, **_kwargs):
            m = _FakeMmap(length)
            mmaps.append(m)
            return m

        res = self._run_with_script(
            script,
            [
                {"cmd": "embed", "shm": "s1", "shm_bytes": 64, "frames": 1},
                {"cmd": "load", "cfg": {}},
                {"cmd": "embed", "shm": "s1", "shm_bytes": 64, "frames": 0},
                {"cmd": "embed", "shm": "s1", "shm_bytes": 8, "frames": 1},
                {"cmd": "embed", "shm": "s1", "shm_bytes": 64, "frames": 1},
                {"cmd": "quit"},
            ],
            max_write_chunk=7,
            mmap_factory=make_mmap,
        )
        self.assertFalse(res[0]["ok"])
        self.assertIn("before load", res[0]["error"])
        self.assertTrue(res[1]["ok"])
        self.assertFalse(res[2]["ok"])
        self.assertIn("invalid frame count", res[2]["error"])
        self.assertFalse(res[3]["ok"])
        self.assertIn("shared memory too small", res[3]["error"])
        self.assertFalse(res[4]["ok"])
        self.assertIn("expected 4", res[4]["error"])
        self.assertTrue(all(m.closed for m in mmaps))

    def test_shm_reopen_failure_resets_closed_handle(self):
        script = (
            "import numpy as np\n"
            "def load(cfg):\n"
            "    return {'size': 2, 'dim': 4}\n"
            "def embed(arr):\n"
            "    return np.ones((len(arr), 4), dtype=np.float32)\n"
        )
        calls = {"n": 0}

        def make_mmap(_fd, length, **_kwargs):
            calls["n"] += 1
            if calls["n"] == 2:
                raise OSError("open failed")
            return _FakeMmap(length)

        res = self._run_with_script(
            script,
            [
                {"cmd": "load", "cfg": {}},
                {"cmd": "embed", "shm": "s1", "shm_bytes": 64, "frames": 1},
                {"cmd": "embed", "shm": "s2", "shm_bytes": 64, "frames": 1},
                {"cmd": "embed", "shm": "s1", "shm_bytes": 64, "frames": 1},
                {"cmd": "quit"},
            ],
            mmap_factory=make_mmap,
        )
        self.assertTrue(res[0]["ok"])
        self.assertTrue(res[1]["ok"])
        self.assertFalse(res[2]["ok"])
        self.assertIn("open failed", res[2]["error"])
        self.assertTrue(res[3]["ok"])


if __name__ == "__main__":
    unittest.main()
