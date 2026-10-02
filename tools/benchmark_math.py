"""Rate-distortion integration over shared measured quality ranges.
Copyright (c) 2026 Micilini Roll. MIT License.
"""
import math
from scipy.interpolate import PchipInterpolator

def pareto_points(points: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """Return increasing-quality, increasing-rate non-dominated points."""
    finite = sorted(
        (quality, rate) for quality, rate in points
        if math.isfinite(quality) and math.isfinite(rate) and rate > 0
    )
    unique: list[tuple[float, float]] = []
    for quality, rate in finite:
        if unique and abs(quality - unique[-1][0]) <= 1e-12:
            if rate < unique[-1][1]:
                unique[-1] = (quality, rate)
        else:
            unique.append((quality, rate))
    # A point is dominated if equal/better quality exists at a lower rate.
    kept_reversed: list[tuple[float, float]] = []
    best_rate = math.inf
    for quality, rate in reversed(unique):
        if rate < best_rate:
            kept_reversed.append((quality, rate))
            best_rate = rate
    kept = list(reversed(kept_reversed))
    # PCHIP also expects rate to grow with quality for a physical RD curve.
    monotone: list[tuple[float, float]] = []
    largest = -math.inf
    for point in kept:
        if point[1] > largest:
            monotone.append(point)
            largest = point[1]
    return monotone

def bd_rate(
    candidate: list[tuple[float, float]], anchor: list[tuple[float, float]]
) -> tuple[float, float, float] | None:
    """PCHIP Bjontegaard delta-rate; negative means candidate uses fewer bits."""
    candidate = pareto_points(candidate)
    anchor = pareto_points(anchor)
    if len(candidate) < 3 or len(anchor) < 3:
        return None
    low = max(candidate[0][0], anchor[0][0])
    high = min(candidate[-1][0], anchor[-1][0])
    if not math.isfinite(low + high) or high - low <= 1e-9:
        return None
    candidate_curve = PchipInterpolator(
        [point[0] for point in candidate],
        [math.log(point[1]) for point in candidate], extrapolate=False,
    )
    anchor_curve = PchipInterpolator(
        [point[0] for point in anchor],
        [math.log(point[1]) for point in anchor], extrapolate=False,
    )
    difference = (
        candidate_curve.integrate(low, high) - anchor_curve.integrate(low, high)
    ) / (high - low)
    return (math.exp(float(difference)) - 1.0) * 100.0, low, high
