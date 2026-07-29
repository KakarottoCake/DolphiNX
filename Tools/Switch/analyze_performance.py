#!/usr/bin/env python3
"""Summarize Dolphin Switch frame-time and host-metrics logs."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from pathlib import Path


def percentile(values: list[float], percent: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    position = (len(ordered) - 1) * percent / 100.0
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def read_intervals(path: Path, warmup_seconds: float) -> list[float]:
    values: list[float] = []
    elapsed_ms = 0.0
    warmup_ms = warmup_seconds * 1000.0
    with path.open("r", encoding="utf-8") as source:
        for line in source:
            try:
                interval_ms = float(line.strip())
            except ValueError:
                continue
            if not math.isfinite(interval_ms) or interval_ms <= 0:
                continue
            elapsed_ms += interval_ms
            if elapsed_ms >= warmup_ms:
                values.append(interval_ms)
    return values


def report_intervals(label: str, path: Path, target_fps: float, warmup: float) -> None:
    values = read_intervals(path, warmup)
    if not values:
        print(f"{label}: no samples after the {warmup:.0f}s warm-up")
        return
    target_ms = 1000.0 / target_fps
    elapsed_seconds = sum(values) / 1000.0
    measured_fps = len(values) / elapsed_seconds
    late_15 = sum(value > target_ms * 1.5 for value in values)
    late_20 = sum(value > target_ms * 2.0 for value in values)
    print(f"{label}: {len(values):,} samples over {elapsed_seconds:.1f}s")
    print(
        f"  rate {measured_fps:.3f} fps | mean {statistics.fmean(values):.3f} ms | "
        f"p95 {percentile(values, 95):.3f} ms | p99 {percentile(values, 99):.3f} ms | "
        f"worst {max(values):.3f} ms"
    )
    print(
        f"  >1.5x budget: {late_15} ({late_15 / len(values) * 100:.3f}%) | "
        f">2x budget: {late_20} ({late_20 / len(values) * 100:.3f}%)"
    )


def read_metric_rows(path: Path, warmup_seconds: float) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as source:
        rows = list(csv.DictReader(source))
    warmup_ms = warmup_seconds * 1000.0
    return [
        row
        for row in rows
        if float(row.get("elapsed_ms", "0") or 0) >= warmup_ms
    ]


def numeric(rows: list[dict[str, str]], key: str) -> list[float]:
    values: list[float] = []
    for row in rows:
        try:
            value = float(row[key])
        except (KeyError, TypeError, ValueError):
            continue
        if math.isfinite(value):
            values.append(value)
    return values


def report_metrics(
    path: Path,
    warmup: float,
    minimum_mean_speed: float,
    minimum_low_speed: float,
) -> bool:
    rows = read_metric_rows(path, warmup)
    if not rows:
        print(f"Host metrics: no samples after the {warmup:.0f}s warm-up")
        return False
    speeds = numeric(rows, "speed_pct")
    max_speeds = numeric(rows, "max_speed_pct")
    fps = numeric(rows, "fps")
    temperatures = numeric(rows, "soc_temp_c")
    power = numeric(rows, "power_mw")
    thermal_events = sum(row.get("thermal_guard") == "1" for row in rows)
    profile = rows[-1].get("profile", "Unknown")
    mode = rows[-1].get("mode", "Unknown")
    hardware = rows[-1].get("hardware", "Unknown")
    print(f"Host metrics: {len(rows):,} samples | {profile} | {mode} | {hardware}")
    passed = bool(speeds)
    if speeds:
        below_full_speed = sum(value < 99.0 for value in speeds)
        mean_speed = statistics.fmean(speeds)
        low_speed = percentile(speeds, 1)
        print(
            f"  emulation speed mean {mean_speed:.3f}% | "
            f"1% low {low_speed:.3f}% | minimum {min(speeds):.3f}% | "
            f"<99% {below_full_speed / len(speeds) * 100:.2f}%"
        )
        passed = mean_speed >= minimum_mean_speed and low_speed >= minimum_low_speed
    if max_speeds:
        mean_max_speed = statistics.fmean(max_speeds)
        low_max_speed = percentile(max_speeds, 1)
        print(
            f"  unthrottled headroom mean {mean_max_speed:.3f}% | "
            f"1% low {low_max_speed:.3f}%"
        )
        if mean_max_speed < 100.0:
            print("  diagnosis: host compute limit; higher clocks or code optimization may help")
        elif speeds and statistics.fmean(speeds) < minimum_mean_speed:
            print("  diagnosis: headroom exists; inspect pacing, synchronization, or I/O stalls")
    if fps:
        print(
            f"  sampled FPS mean {statistics.fmean(fps):.3f} | "
            f"1% low {percentile(fps, 1):.3f}"
        )
    if temperatures:
        print(f"  maximum SOC temperature {max(temperatures):.1f} C")
    if power:
        print(f"  mean power {statistics.fmean(power) / 1000.0:.2f} W")
    print(f"  thermal-guard samples {thermal_events}")
    passed = passed and thermal_events == 0
    print(
        "  baseline verdict "
        + ("PASS" if passed else "FAIL")
        + f" (mean >= {minimum_mean_speed:.2f}%, 1% low >= {minimum_low_speed:.2f}%, "
        "no thermal guard)"
    )
    return passed


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze Dolphin render/vblank intervals and Switch performance CSV files."
    )
    parser.add_argument("--render", type=Path, help="Path to render_times.txt")
    parser.add_argument("--vblank", type=Path, help="Path to vblank_times.txt")
    parser.add_argument("--metrics", type=Path, help="Path to a performance CSV")
    parser.add_argument("--render-target", type=float, default=30.0)
    parser.add_argument("--vblank-target", type=float, default=60.0)
    parser.add_argument("--warmup", type=float, default=30.0, help="Warm-up seconds to discard")
    parser.add_argument(
        "--minimum-mean-speed",
        type=float,
        default=99.5,
        help="Minimum mean emulation speed for the baseline verdict (default: 99.5)",
    )
    parser.add_argument(
        "--minimum-low-speed",
        type=float,
        default=99.0,
        help="Minimum 1%% low emulation speed for the baseline verdict (default: 99.0)",
    )
    args = parser.parse_args()

    selected = [
        value for value in (args.render, args.vblank, args.metrics) if value is not None
    ]
    if not selected:
        parser.error("provide at least one of --render, --vblank or --metrics")
    for path in selected:
        if not path.is_file():
            parser.error(f"file not found: {path}")

    if args.render:
        report_intervals("Rendered frames", args.render, args.render_target, args.warmup)
    if args.vblank:
        report_intervals("Emulated vblanks", args.vblank, args.vblank_target, args.warmup)
    metrics_passed = True
    if args.metrics:
        metrics_passed = report_metrics(
            args.metrics,
            args.warmup,
            args.minimum_mean_speed,
            args.minimum_low_speed,
        )
    return 0 if metrics_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
