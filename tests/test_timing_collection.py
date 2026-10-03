"""A noisy codec invalidates its whole paired operating-point comparison."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from time_release_benchmark import collect_group


class TimingCollectionTests(unittest.TestCase):
    def test_before_or_after_noise_repeats_every_codec(self):
        for noisy_side in ("before", "after"):
            with self.subTest(noisy_side=noisy_side):
                calls, audits = [], []

                def run(codec):
                    calls.append(codec)
                    row = {"codec": codec, "before": {"interference": []}, "after": {"interference": []}}
                    if len(calls) == 4:
                        row[noisy_side]["interference"] = [{"external_cpu": 1.0}]
                    return row

                accepted = collect_group(list(range(8)), run, lambda: {"interference": []},
                                         lambda: self.fail("Unexpected initial wait"), audits.append)
                self.assertEqual(calls, list(range(4)) + list(range(8)))
                self.assertEqual([a["status"] for a in audits], ["interference", "accepted"])
                self.assertEqual([r["codec"] for r in accepted["rows"]], list(range(8)))

    def test_waits_before_start_and_rejects_noise_at_group_end(self):
        observations, calls, pauses, audits = 0, [], [], []

        def observe():
            nonlocal observations
            observations += 1
            # Initial wait, then noise at the final observation of attempt 1.
            return {"interference": [{"external_cpu": 1.0}] if observations in (1, 11) else []}

        def run(codec):
            calls.append(codec)
            return {"codec": codec, "before": {"interference": []}, "after": {"interference": []}}

        result = collect_group(list(range(8)), run, observe, lambda: pauses.append(True), audits.append)
        self.assertEqual(len(pauses), 1)
        self.assertEqual(calls, list(range(8)) * 2)
        self.assertEqual([a["status"] for a in audits], ["interference", "accepted"])
        self.assertEqual(result["attempt"], 2)


if __name__ == "__main__":
    unittest.main()
