"""Analytical checks for the published comparison's statistics."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import numpy as np
from benchmark_math import bd_rate
from analyze_release_benchmark import bootstrap_counts, curve, interval
from analyze_release_timing import timing_bootstrap


class StatisticsTests(unittest.TestCase):
    def test_constant_rate_saving_and_reciprocity(self):
        reference = [(x, 2 ** x) for x in (1., 2., 3., 4., 5.)]
        candidate = [(x, rate * .8) for x, rate in reference]
        self.assertAlmostEqual(bd_rate(candidate, reference)[0], -20., places=10)
        self.assertAlmostEqual(bd_rate(reference, candidate)[0], 25., places=10)
        self.assertAlmostEqual(bd_rate(reference, reference)[0], 0., places=10)

    def test_no_extrapolation(self):
        a = [(1., 1.), (2., 2.), (3., 4.)]
        b = [(4., 1.), (5., 2.), (6., 4.)]
        self.assertIsNone(bd_rate(a, b))

    def test_whole_image_strata_and_pooled_mse(self):
        counts = bootstrap_counts(["a", "a", "b", "b", "b"], 100, 148)
        np.testing.assert_array_equal(counts[:, :2].sum(axis=1), 2)
        np.testing.assert_array_equal(counts[:, 2:].sum(axis=1), 3)
        np.testing.assert_array_equal(counts, bootstrap_counts(["a", "a", "b", "b", "b"], 100, 148))
        pixels = np.array([1., 3.])
        sizes = np.array([[2., 4., 8.], [2., 4., 8.]])
        mses = np.array([[1., .5, .25], [9., 4.5, 2.25]])
        result = curve(np.ones(2), pixels, sizes, mses, mse=True)
        self.assertAlmostEqual(result[0][0], 10 * np.log10(65025 / 7.))

    def test_paired_constant_effect_interval(self):
        weights = bootstrap_counts(["a"] * 6, 80, 123)
        pixels = np.arange(1., 7.)
        size = np.arange(1., 7.)[:, None] * np.array([1., 2., 4., 8., 16.])
        quality = np.tile(np.arange(1., 6.), (6, 1))
        values = [bd_rate(curve(w, pixels, size * .8, quality), curve(w, pixels, size, quality))[0] for w in weights]
        np.testing.assert_allclose(interval(values), [-20., -20.], atol=1e-10)

    def test_nested_paired_timing_preserves_known_ratio(self):
        reference = np.arange(1., 85.).reshape(6, 2, 7)
        values = timing_bootstrap(reference * .75, reference, ["a"] * 3 + ["b"] * 3, 256, 9)
        np.testing.assert_allclose(values, -25., atol=1e-10)


if __name__ == "__main__": unittest.main()
