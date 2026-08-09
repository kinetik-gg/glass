#!/usr/bin/env python3
"""Compare Hyprland superellipse corners with a reverse-engineered Apple curve.

The Apple reference points are normalized from Liam Rosenfeld's reconstruction
of UIBezierPath's continuous rounded rectangle. The tool uses no third-party
packages and writes a browser-viewable SVG overlay plus JSON metrics.
"""

from __future__ import annotations

import argparse
from functools import cache
import json
import math
from pathlib import Path


EXTENT = 1.528665
APPLE_SEGMENTS = (
    ((1.528665, 0.0), (1.08849296, 0.0), (0.86840694, 0.0), (0.63149379, 0.07491139)),
    ((0.63149379, 0.07491139), (0.37282383, 0.16905956), (0.16905956, 0.37282383), (0.07491139, 0.63149379)),
    ((0.07491139, 0.63149379), (0.0, 0.86840694), (0.0, 1.08849296), (0.0, 1.52866498)),
)
APPLE_SEGMENTS = tuple(
    tuple((x / EXTENT, y / EXTENT) for x, y in segment)
    for segment in APPLE_SEGMENTS
)


def cubic(points: tuple[tuple[float, float], ...], t: float) -> tuple[float, float]:
    mt = 1.0 - t
    weights = (mt**3, 3.0 * mt * mt * t, 3.0 * mt * t * t, t**3)
    return tuple(sum(weight * point[axis] for weight, point in zip(weights, points)) for axis in (0, 1))


@cache
def apple_y(x: float) -> float:
    """Return the Apple boundary y for normalized corner coordinate x."""
    x = min(max(x, 0.0), 1.0)
    segment = next((item for item in APPLE_SEGMENTS if item[-1][0] <= x <= item[0][0]), APPLE_SEGMENTS[-1])
    lo, hi = 0.0, 1.0
    for _ in range(30):
        mid = (lo + hi) * 0.5
        if cubic(segment, mid)[0] > x:
            lo = mid
        else:
            hi = mid
    return cubic(segment, (lo + hi) * 0.5)[1]


def superellipse_y(x: float, exponent: float) -> float:
    """Hyprland's rounded-corner boundary, normalized to the same extent."""
    return 1.0 - max(0.0, 1.0 - (1.0 - x) ** exponent) ** (1.0 / exponent)


def metrics(exponent: float, samples: int) -> dict[str, float]:
    differences = [abs(apple_y(i / samples) - superellipse_y(i / samples, exponent)) for i in range(samples + 1)]
    signed_area = sum(differences) / samples
    return {
        "exponent": exponent,
        "mean_boundary_error_percent_of_corner_extent": 100.0 * sum(differences) / len(differences),
        "rms_boundary_error_percent_of_corner_extent": 100.0 * math.sqrt(sum(value * value for value in differences) / len(differences)),
        "max_boundary_error_percent_of_corner_extent": 100.0 * max(differences),
        "normalized_corner_area_mismatch_percent": 100.0 * signed_area,
    }


def path_for(function, samples: int, scale: float, offset: tuple[float, float]) -> str:
    points = [(offset[0] + scale * i / samples, offset[1] + scale * function(i / samples)) for i in range(samples + 1)]
    return "M " + " L ".join(f"{x:.3f} {y:.3f}" for x, y in points)


def write_svg(path: Path, best_exponent: float, samples: int) -> None:
    scale, offset = 420.0, (46.0, 46.0)
    apple = path_for(apple_y, samples, scale, offset)
    current = path_for(lambda x: superellipse_y(x, 4.0), samples, scale, offset)
    best = path_for(lambda x: superellipse_y(x, best_exponent), samples, scale, offset)
    path.write_text(
        f"""<svg xmlns="http://www.w3.org/2000/svg" width="760" height="540" viewBox="0 0 760 540">
<rect width="760" height="540" fill="#050505"/>
<path d="{current}" fill="none" stroke="#ffffff" stroke-width="5" opacity="0.7"/>
<path d="{best}" fill="none" stroke="#56d4dd" stroke-width="3"/>
<path d="{apple}" fill="none" stroke="#ff6384" stroke-width="2"/>
<g fill="#d8d8d8" font-family="system-ui, sans-serif" font-size="18">
  <text x="510" y="80" fill="#ff6384">Apple Bézier reference</text>
  <text x="510" y="116" fill="#56d4dd">best native n={best_exponent:.2f}</text>
  <text x="510" y="152" fill="#ffffff">current native n=4</text>
  <text x="46" y="510">Normalized top-left corner; curves are intentionally magnified.</text>
</g>
</svg>
"""
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--samples", type=int, default=4096)
    parser.add_argument("--json", type=Path, default=Path("artifacts/corner-metrics.json"))
    parser.add_argument("--svg", type=Path, default=Path("artifacts/corner-comparison.svg"))
    args = parser.parse_args()

    candidates = [2.0 + index * 0.01 for index in range(801)]
    results = [metrics(exponent, args.samples) for exponent in candidates]
    best = min(results, key=lambda item: item["rms_boundary_error_percent_of_corner_extent"])
    report = {
        "reference": "Reverse-engineered UIBezierPath continuous rounded rectangle",
        "samples": args.samples,
        "current": metrics(4.0, args.samples),
        "published_icon_candidate": metrics(5.2, args.samples),
        "best_native_superellipse_fit": best,
        "note": "Errors are normalized to corner extent; zero requires the piecewise cubic reference rather than one superellipse exponent.",
    }

    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.svg.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n")
    write_svg(args.svg, best["exponent"], min(args.samples, 512))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
