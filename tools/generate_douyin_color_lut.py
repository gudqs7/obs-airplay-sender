#!/usr/bin/env python3
"""Generate a receiver calibration LUT from measured Douyin color samples."""

from __future__ import annotations

import argparse
import math
from pathlib import Path


# Input is the color displayed by Douyin. Output is the color shown by OBS.
ANCHORS = [
    ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)),
    ((1.0, 5 / 255, 4 / 255), (1.0, 0.0, 0.0)),
    ((0.0, 237 / 255, 0.0), (0.0, 1.0, 0.0)),
    ((8 / 255, 6 / 255, 1.0), (0.0, 0.0, 1.0)),
    ((251 / 255, 251 / 255, 251 / 255), (1.0, 1.0, 1.0)),
    ((85 / 255, 85 / 255, 253 / 255), (85 / 255, 85 / 255, 1.0)),
]

BOOSTED_ANCHORS = [
    ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)),
    ((1.0, 5 / 255, 4 / 255), (1.03, -0.02, -0.02)),
    ((0.0, 237 / 255, 0.0), (-0.03, 1.0, -0.03)),
    ((8 / 255, 6 / 255, 1.0), (-0.02, -0.02, 1.10)),
    ((251 / 255, 251 / 255, 251 / 255), (1.01, 1.01, 1.01)),
    ((85 / 255, 85 / 255, 253 / 255), (85 / 255, 85 / 255, 1.05)),
]


def distance_squared(left: tuple[float, ...], right: tuple[float, ...]) -> float:
    return sum((a - b) ** 2 for a, b in zip(left, right))


def kernel(left: tuple[float, ...], right: tuple[float, ...], sigma: float) -> float:
    return math.exp(-distance_squared(left, right) / (2.0 * sigma * sigma))


def solve(matrix: list[list[float]], values: list[float]) -> list[float]:
    size = len(values)
    augmented = [row[:] + [value] for row, value in zip(matrix, values)]

    for column in range(size):
        pivot = max(range(column, size), key=lambda row: abs(augmented[row][column]))
        if abs(augmented[pivot][column]) < 1e-12:
            raise ValueError("singular interpolation matrix")
        augmented[column], augmented[pivot] = augmented[pivot], augmented[column]

        divisor = augmented[column][column]
        for index in range(column, size + 1):
            augmented[column][index] /= divisor

        for row in range(size):
            if row == column:
                continue
            factor = augmented[row][column]
            for index in range(column, size + 1):
                augmented[row][index] -= factor * augmented[column][index]

    return [augmented[row][size] for row in range(size)]


def build_weights(
    sigma: float, anchors: list[tuple[tuple[float, ...], tuple[float, ...]]]
) -> list[tuple[float, float, float]]:
    inputs = [source for source, _ in anchors]
    matrix = [[kernel(left, right, sigma) for right in inputs] for left in inputs]
    for index in range(len(matrix)):
        matrix[index][index] += 1e-9

    columns = []
    for channel in range(3):
        deltas = [target[channel] - source[channel] for source, target in anchors]
        columns.append(solve(matrix, deltas))

    return [
        (columns[0][index], columns[1][index], columns[2][index])
        for index in range(len(anchors))
    ]


def apply_correction(
    rgb: tuple[float, float, float],
    weights: list[tuple[float, float, float]],
    anchors: list[tuple[tuple[float, ...], tuple[float, ...]]],
    sigma: float,
    clamp_min: float,
    clamp_max: float,
) -> tuple[float, float, float]:
    correction = [0.0, 0.0, 0.0]
    for (source, _), weight in zip(anchors, weights):
        influence = kernel(rgb, source, sigma)
        for channel in range(3):
            correction[channel] += influence * weight[channel]

    return tuple(
        min(clamp_max, max(clamp_min, rgb[channel] + correction[channel]))
        for channel in range(3)
    )


def write_lut(
    path: Path,
    size: int,
    sigma: float,
    fast_channel: str,
    anchors: list[tuple[tuple[float, ...], tuple[float, ...]]],
    clamp_min: float,
    clamp_max: float,
) -> None:
    weights = build_weights(sigma, anchors)
    path.parent.mkdir(parents=True, exist_ok=True)

    with path.open("w", encoding="ascii", newline="\n") as output:
        output.write("TITLE \"Douyin AirPlay calibration\"\n")
        output.write(f"LUT_3D_SIZE {size}\n")
        output.write("DOMAIN_MIN 0.0 0.0 0.0\n")
        output.write("DOMAIN_MAX 1.0 1.0 1.0\n\n")

        denominator = size - 1
        if fast_channel == "blue":
            indices = (
                (red_index, green_index, blue_index)
                for red_index in range(size)
                for green_index in range(size)
                for blue_index in range(size)
            )
        else:
            # The Douyin cube reader advances the red axis fastest.
            indices = (
                (red_index, green_index, blue_index)
                for blue_index in range(size)
                for green_index in range(size)
                for red_index in range(size)
            )

        for red_index, green_index, blue_index in indices:
            rgb = (
                red_index / denominator,
                green_index / denominator,
                blue_index / denominator,
            )
            corrected = apply_correction(
                rgb, weights, anchors, sigma, clamp_min, clamp_max
            )
            output.write(
                f"{corrected[0]:.6f} {corrected[1]:.6f} {corrected[2]:.6f}\n"
            )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).parent
        / "luts"
        / "douyin-airplay-color-correct.cube",
    )
    parser.add_argument("--size", type=int, default=33)
    parser.add_argument("--sigma", type=float, default=0.18)
    parser.add_argument(
        "--fast-channel",
        choices=("blue", "red"),
        default="blue",
        help="Axis advanced first in the .cube data rows",
    )
    parser.add_argument(
        "--profile",
        choices=("measured", "boosted"),
        default="measured",
        help="Use measured targets or high-end compensated targets",
    )
    parser.add_argument(
        "--strength",
        type=float,
        default=1.0,
        help="Scale the correction from identity (0.0) to full strength (1.0)",
    )
    args = parser.parse_args()

    if args.size < 2:
        parser.error("--size must be at least 2")
    if args.sigma <= 0:
        parser.error("--sigma must be positive")
    if not 0.0 <= args.strength <= 1.0:
        parser.error("--strength must be between 0 and 1")

    if args.profile == "boosted":
        anchors = BOOSTED_ANCHORS
        clamp_min, clamp_max = -0.25, 1.25
    else:
        anchors = ANCHORS
        clamp_min, clamp_max = 0.0, 1.0

    anchors = [
        (
            source,
            tuple(
                source[channel]
                + args.strength * (target[channel] - source[channel])
                for channel in range(3)
            ),
        )
        for source, target in anchors
    ]

    write_lut(
        args.output,
        args.size,
        args.sigma,
        args.fast_channel,
        anchors,
        clamp_min,
        clamp_max,
    )
    print(args.output)


if __name__ == "__main__":
    main()
