#!/usr/bin/env python3
"""Render README performance figures from the published measurement dataset.

Run from any directory: python scripts/measurements/render_module_metrics.py
Requires Matplotlib and an installed Korean font, or --font /path/to/font.ttf.
"""
import argparse
import csv
import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import FancyBboxPatch

REPO = Path(__file__).resolve().parents[2]
DATA = REPO / 'docs/measurements'
ASSETS = REPO / 'docs/assets'
ORDER = ['g1', 'g3', 'g2']
LABELS = ['위치·화각 미통과', '영상 초중반부 차량 검출', '차량 미검출 (영상 전체 탐색)']
COLORS = ['#4F83B0', '#198576', '#B56248']
INK, SOFT, BG = '#18374A', '#526C7D', '#F5F8FA'


def base(title, subtitle):
    fig = plt.figure(figsize=(16, 9), dpi=200, facecolor=BG)
    fig.text(.055, .94, title, fontsize=24, weight='bold', color=INK)
    fig.text(.055, .895, subtitle, fontsize=12.5, color=SOFT)
    return fig


def decorate(ax):
    ax.set_facecolor(BG)
    for spine in ax.spines.values():
        spine.set_visible(False)
    ax.set_axisbelow(True)
    ax.tick_params(length=0, colors=SOFT)
    ax.grid(color='#DCE5EB', linewidth=.7)


def resource_figure(metrics):
    fig = base('2중 선별 구조로 AI 연산과 장치 부하를 줄입니다',
               '관측차의 처리 경로별 장치 사용량  |  사고 후 15초  |  완료 회차의 0.5초 평균 곡선')
    with (DATA / '20260928-e2e/resource_cycle_average.csv').open(encoding='utf-8-sig', newline='') as f:
        rows = list(csv.DictReader(f))
    data = {(r['node'], float(r['relative_center_s'])): r for r in rows}
    times = sorted({float(r['relative_center_s']) for r in rows})
    columns = [
        ('cpu_cores', 'CPU 사용량', '사용 코어 수', 1, 3),
        ('memory_mb', '메모리 사용량', 'MB', 1, 0),
        ('network_bytes_per_s', '네트워크 송수신', 'MB/s', 1e6, 3),
        ('temperature_c', 'CPU 온도', '°C', 1, 1),
    ]
    positions = [[.08, .48, .39, .275], [.575, .48, .39, .275],
                 [.08, .105, .39, .275], [.575, .105, .39, .275]]
    for x, label, color in zip([.085, .365, .675], LABELS, COLORS):
        fig.text(x, .835, '━  ' + label, fontsize=13, weight='bold', color=color)
    for bounds, (key, title, unit, divisor, decimals) in zip(positions, columns):
        ax = fig.add_axes(bounds)
        decorate(ax)
        ax.set_title(title, loc='left', fontsize=17, weight='bold', color=INK, pad=16)
        for group_id, color in zip(ORDER, COLORS):
            group = metrics['groups'][group_id]
            values = [sum(float(data[(node, t)][key + '_mean']) for node in group['nodes'])
                      / len(group['nodes']) / divisor for t in times]
            ax.plot(times, values, color=color, linewidth=2.4)
            ax.axvline(group['terminal_seconds']['mean'], color=color, linestyle='--', alpha=.5, linewidth=1)
        ax.set_xlim(0, 15)
        ax.set_xticks([0, 5, 10, 15])
        ax.set_xlabel('사고 후 경과 시간 (초)', fontsize=11, color=SOFT)
        ax.set_ylabel(unit, fontsize=11, color=SOFT)
        if key != 'temperature_c':
            ax.set_ylim(bottom=0)
    fig.text(.055, .035, '점선: 경로별 평균 완료 시각  ·  영상에서 차량을 찾으면 탐색 종료, 미검출 시 구간 전체 탐색', fontsize=11, color=SOFT)
    fig.savefig(ASSETS / 'resource-load.png', dpi=200)
    plt.close(fig)


def timing_figure(metrics):
    fig = base('관측 영상을 선별하고 서버 저장까지 연결합니다',
               '사고 이벤트 100회  |  15초 간격  |  사고 전 10초 영상  |  관측차 위치·방향 고정')
    ax = fig.add_axes([.34, .365, .59, .405])
    decorate(ax)
    means = [metrics['groups'][g]['terminal_seconds']['mean'] for g in ORDER]
    p95 = [metrics['groups'][g]['terminal_seconds']['p95'] for g in ORDER]
    ax.barh(range(3), means, color=COLORS, height=.45)
    ax.scatter(p95, range(3), color=INK, marker='D', s=36, zorder=4, label='P95')
    ax.set_yticks(range(3), [''] * 3)
    ax.invert_yaxis()
    ax.set_xlim(0, 11)
    ax.set_xticks([0, 2, 4, 6, 8, 10])
    ax.set_xlabel('이벤트 발생부터 경로별 완료까지 (초)', fontsize=12, color=SOFT)
    for i, g in enumerate(ORDER):
        group = metrics['groups'][g]
        ax.text(-.09, i, LABELS[i] + '\n' + f"완료 {group['completed_count']} / {group['expected_count']}건",
                transform=ax.get_yaxis_transform(), ha='right', va='center', fontsize=14, color=INK)
        ax.text(means[i] + .15, i - .22, f'평균 {means[i]:.3f}초', fontsize=13, weight='bold', color=COLORS[i])
        ax.text(max(means[i], p95[i]) + .15, i + .20, f'P95 {p95[i]:.3f}초', fontsize=11, color=SOFT)
    fig.text(.34, .825, '관측차   ■ 평균 완료 시간     ◆ P95', fontsize=13, color=INK)
    fig.add_artist(FancyBboxPatch((.055, .125), .89, .145, transform=fig.transFigure,
                                 boxstyle='round,pad=0.008', facecolor='#E8F3EF', edgecolor='none'))
    own = metrics['groups']['accident']['terminal_seconds']
    fig.text(.08, .217, '사고차 자체 영상 저장', fontsize=17, weight='bold', color='#157D6F')
    fig.text(.08, .162, f"100 / 100건 저장     평균 {own['mean']:.3f}초     P95 {own['p95']:.3f}초", fontsize=15, color=INK)
    fig.text(.055, .064, '종료 기준: 화각 판정 / 영상 전체 탐색 완료 / 사고 ID·차량 ID·SHA-256이 일치하는 서버 저장 ACK', fontsize=11, color=SOFT)
    fig.text(.055, .035, 'P95: 완료 기록의 95%가 포함되는 시간', fontsize=11, color=SOFT)
    fig.savefig(ASSETS / 'end-to-end.png', dpi=200)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font', type=Path)
    args = parser.parse_args()
    if args.font:
        font_manager.fontManager.addfont(str(args.font))
        name = font_manager.FontProperties(fname=str(args.font)).get_name()
    else:
        installed = {f.name for f in font_manager.fontManager.ttflist}
        name = next((n for n in ['Malgun Gothic', 'Noto Sans CJK KR', 'NanumGothic'] if n in installed), None)
        if not name:
            raise SystemExit('Supply a Korean font with --font PATH.')
    plt.rcParams.update({'font.family': name, 'font.size': 12, 'axes.unicode_minus': False})
    metrics = json.loads((DATA / 'metrics.json').read_text(encoding='utf-8'))
    resource_figure(metrics)
    timing_figure(metrics)
    print('Rendered resource-load.png and end-to-end.png (3200 x 1800).')


if __name__ == '__main__':
    main()
