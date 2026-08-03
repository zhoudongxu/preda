#!/usr/bin/env python3
"""Dependency-free SVG figures for the coverage/scalability artifact."""

from __future__ import annotations

import html
import pathlib
from typing import Any, List, Mapping, Optional, Sequence, Tuple


COLORS = {"Proved": "#2f855a", "Unknown": "#d69e2e", "Unsupported": "#c53030"}


def _write(path: pathlib.Path, body: str, width: int, height: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">\n'
        '<rect width="100%%" height="100%%" fill="white"/>\n%s\n</svg>\n'
        % (width, height, width, height, body),
        encoding="utf-8",
    )


def certificate_distribution(
    summary: Mapping[str, Mapping[str, int]],
    path: pathlib.Path,
    candidate_denominators: Optional[Mapping[str, int]] = None,
) -> None:
    candidate_denominators = candidate_denominators or {}
    width, height = 980, 520
    left, top, chart_h, group_w = 90, 70, 340, 165
    maximum = max([sum(values.get(status, 0) for status in COLORS) for values in summary.values()] + [1])
    elements: List[str] = [
        '<text x="490" y="32" text-anchor="middle" font-family="sans-serif" font-size="20">Certificate distribution</text>'
    ]
    for tick in range(6):
        value = maximum * tick / 5.0
        y = top + chart_h - chart_h * tick / 5.0
        elements.append('<line x1="%d" y1="%.1f" x2="940" y2="%.1f" stroke="#e2e8f0"/>' % (left, y, y))
        elements.append('<text x="%d" y="%.1f" text-anchor="end" font-family="sans-serif" font-size="11">%.1f</text>' % (left - 8, y + 4, value))
    for index, (prop, values) in enumerate(summary.items()):
        x = left + index * group_w + 22
        cumulative = 0.0
        for status, color in COLORS.items():
            count = int(values.get(status, 0))
            bar_h = chart_h * count / float(maximum)
            y = top + chart_h - cumulative - bar_h
            elements.append('<rect x="%.1f" y="%.1f" width="72" height="%.1f" fill="%s"><title>%s: %d</title></rect>' % (x, y, bar_h, color, html.escape(status), count))
            if count:
                elements.append('<text x="%.1f" y="%.1f" text-anchor="middle" fill="white" font-family="sans-serif" font-size="11">%d</text>' % (x + 36, y + bar_h / 2 + 4, count))
            cumulative += bar_h
        elements.append('<text x="%.1f" y="435" text-anchor="middle" font-family="sans-serif" font-size="11">%s</text>' % (x + 36, html.escape(prop)))
        candidate_count = int(
            candidate_denominators.get(prop, sum(int(values.get(status, 0)) for status in COLORS))
        )
        if candidate_count == 0:
            elements.append('<text x="%.1f" y="452" text-anchor="middle" fill="#718096" font-family="sans-serif" font-size="10">N/A (0 candidates)</text>' % (x + 36))
    for index, (status, color) in enumerate(COLORS.items()):
        x = 290 + index * 145
        elements.append('<rect x="%d" y="475" width="14" height="14" fill="%s"/>' % (x, color))
        elements.append('<text x="%d" y="487" font-family="sans-serif" font-size="12">%s</text>' % (x + 20, status))
    _write(path, "\n".join(elements), width, height)


def unknown_reasons(counts: Mapping[str, int], path: pathlib.Path) -> None:
    ordered = sorted(
        ((reason, count) for reason, count in counts.items() if int(count) > 0),
        key=lambda item: (-item[1], item[0]),
    )
    width, height = 940, max(300, 95 + 42 * len(ordered))
    maximum = max([value for _, value in ordered] + [1])
    elements: List[str] = [
        '<text x="470" y="32" text-anchor="middle" font-family="sans-serif" font-size="20">Conservative fallback reasons</text>'
    ]
    for index, (reason, count) in enumerate(ordered):
        y = 66 + index * 42
        bar_w = 580.0 * count / maximum
        elements.append('<text x="250" y="%.1f" text-anchor="end" font-family="sans-serif" font-size="12">%s</text>' % (y + 16, html.escape(reason)))
        elements.append('<rect x="270" y="%.1f" width="%.1f" height="24" rx="3" fill="#4c78a8"/>' % (y, bar_w))
        elements.append('<text x="%.1f" y="%.1f" font-family="sans-serif" font-size="12">%d</text>' % (280 + bar_w, y + 16, count))
    if not ordered:
        elements.append('<text x="470" y="150" text-anchor="middle" font-family="sans-serif">No Unknown/Unsupported records</text>')
    _write(path, "\n".join(elements), width, height)


def scalability_curve(rows: Sequence[Mapping[str, Any]], path: pathlib.Path) -> None:
    """Render the three paper-facing scalability projections in one SVG.

    The experiment itself remains an OFAT design.  The third panel uses the
    measured constraint count (not the generator knob) as its x coordinate.
    """

    panels = (
        ("relay_sites", "actual_relay_sites", "Relay sites"),
        ("statements", "actual_statements", "Source statements"),
        (
            "arguments_per_relay",
            "actual_constraint_count",
            "FormulaIR constraints",
        ),
    )
    width, height = 1320, 520
    panel_width, gap = 380, 45
    chart_top, chart_bottom = 72, 410
    elements: List[str] = [
        '<text x="660" y="30" text-anchor="middle" font-family="sans-serif" font-size="20">R-PREDA scalability</text>',
    ]
    for panel_index, (axis, x_field, x_label) in enumerate(panels):
        points = [
            (
                float(row.get(x_field, 0)),
                float(row.get("analysis_time_median_ms", 0)),
                float(row.get("peak_rss_median_kib", 0)) / 1024.0,
                str(row.get("case_id", "")),
            )
            for row in rows
            if row.get("status") == "Completed" and row.get("sweep_axis") == axis
        ]
        points.sort(key=lambda value: value[0])
        left = 62 + panel_index * (panel_width + gap)
        right = left + panel_width
        max_x = max([value[0] for value in points] + [1.0])
        min_x = min([value[0] for value in points]) if points else 0.0
        x_span = max(max_x - min_x, 1.0)
        max_time = max([value[1] for value in points] + [1.0])
        max_rss = max([value[2] for value in points] + [1.0])
        elements.extend([
            '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#2d3748"/>' % (left, chart_bottom, right, chart_bottom),
            '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#2d3748"/>' % (left, chart_top, left, chart_bottom),
            '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#718096"/>' % (right, chart_top, right, chart_bottom),
            '<text x="%.1f" y="447" text-anchor="middle" font-family="sans-serif" font-size="13">%s</text>' % ((left + right) / 2.0, html.escape(x_label)),
            '<text x="%d" y="62" text-anchor="start" fill="#2563eb" font-family="sans-serif" font-size="11">time max %.2f ms</text>' % (left, max_time),
            '<text x="%d" y="62" text-anchor="end" fill="#dd6b20" font-family="sans-serif" font-size="11">RSS max %.2f MiB</text>' % (right, max_rss),
            '<text x="%d" y="%.1f" text-anchor="end" fill="#2563eb" font-family="sans-serif" font-size="9">%.2f ms</text>' % (left - 4, chart_top + 3, max_time),
            '<text x="%d" y="%.1f" text-anchor="start" fill="#dd6b20" font-family="sans-serif" font-size="9">%.2f MiB</text>' % (right + 4, chart_top + 3, max_rss),
            '<text x="%d" y="%d" text-anchor="end" fill="#2563eb" font-family="sans-serif" font-size="9">0</text>' % (left - 4, chart_bottom),
            '<text x="%d" y="%d" text-anchor="start" fill="#dd6b20" font-family="sans-serif" font-size="9">0</text>' % (right + 4, chart_bottom),
            '<text x="%d" y="428" text-anchor="middle" font-family="sans-serif" font-size="10">%g</text>' % (left, min_x),
            '<text x="%d" y="428" text-anchor="middle" font-family="sans-serif" font-size="10">%g</text>' % (right, max_x),
        ])
        time_coords: List[Tuple[float, float]] = []
        rss_coords: List[Tuple[float, float]] = []
        for x, elapsed, rss, case_id in points:
            px = left + panel_width * (x - min_x) / x_span
            py_time = chart_bottom - (chart_bottom - chart_top) * elapsed / max_time
            py_rss = chart_bottom - (chart_bottom - chart_top) * rss / max_rss
            time_coords.append((px, py_time))
            rss_coords.append((px, py_rss))
            elements.append('<circle cx="%.1f" cy="%.1f" r="4" fill="#2563eb"><title>%s: x %.3f, time %.3f ms</title></circle>' % (px, py_time, html.escape(case_id), x, elapsed))
            elements.append('<circle cx="%.1f" cy="%.1f" r="4" fill="#dd6b20"><title>%s: x %.3f, RSS %.3f MiB</title></circle>' % (px, py_rss, html.escape(case_id), x, rss))
        if time_coords:
            elements.append('<polyline fill="none" stroke="#2563eb" stroke-width="2" points="%s"/>' % " ".join("%.1f,%.1f" % point for point in time_coords))
            elements.append('<polyline fill="none" stroke="#dd6b20" stroke-width="2" points="%s"/>' % " ".join("%.1f,%.1f" % point for point in rss_coords))
    elements.extend([
        '<line x1="450" y1="486" x2="480" y2="486" stroke="#2563eb" stroke-width="3"/><text x="488" y="490" font-family="sans-serif" font-size="12">analysis time</text>',
        '<line x1="690" y1="486" x2="720" y2="486" stroke="#dd6b20" stroke-width="3"/><text x="728" y="490" font-family="sans-serif" font-size="12">peak RSS</text>',
    ])
    _write(path, "\n".join(elements), width, height)
