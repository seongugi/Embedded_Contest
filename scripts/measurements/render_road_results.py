#!/usr/bin/env python3
"""Render the README figure from the saved simulation and cost summaries."""
import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, help="Korean TTF/OTF font; defaults to an installed Korean font")
    args = parser.parse_args()
    if args.font:
        font_manager.fontManager.addfont(str(args.font))
        font_name = font_manager.FontProperties(fname=str(args.font)).get_name()
    else:
        available = {font.name for font in font_manager.fontManager.ttflist}
        font_name = next((name for name in ("Malgun Gothic", "Noto Sans CJK KR", "NanumGothic") if name in available), None)
        if not font_name:
            raise SystemExit("Use --font PATH to supply a Korean font.")
    plt.rcParams.update({"font.family": font_name, "font.size": 12, "axes.unicode_minus": False})
    source = repo / "docs/measurements/road-simulation"
    sim = json.loads((source / "summary.json").read_text(encoding="utf-8"))
    cost = json.loads((source / "cost_summary.json").read_text(encoding="utf-8"))
    totals, avg = sim["overall"], cost["mean_per_scenario"]
    blue, amber, teal = "#5385B7", "#D7A041", "#30977F"
    ink, soft = "#18374A", "#617788"
    fig, axes = plt.subplots(2, 2, figsize=(15, 9.4))
    fig.patch.set_facecolor("#F4F7FA")
    fig.subplots_adjust(left=.085, right=.965, top=.81, bottom=.13, wspace=.30, hspace=.58)
    fig.text(.055, .943, "도로 배치 100회 · 필요한 영상부터 선별", fontsize=25, weight="bold", color=ink)
    fig.text(.055, .9, "일반도로·교차로·지하차도·고가도로  |  유형별 25회  |  관측차 20~100대 무작위 배치", fontsize=13, color=soft)

    def style(ax, title, subtitle):
        ax.set_facecolor("#F4F7FA")
        for spine in ax.spines.values():
            spine.set_visible(False)
        ax.set_axisbelow(True)
        ax.grid(axis="x", color="#DFE7ED", linewidth=.7)
        ax.tick_params(axis="both", length=0, labelcolor=soft)
        ax.set_title(title, loc="left", fontsize=16, fontweight="bold", color=ink, pad=28)
        ax.text(0, 1.035, subtitle, transform=ax.transAxes, fontsize=10.5, color=soft)

    def bars(ax, labels, values, colors, formatter, unit):
        ax.barh(labels, values, color=colors, height=.55)
        ax.invert_yaxis()
        xmax = max(values)
        ax.set_xlim(0, xmax * 1.32)
        for y, value in enumerate(values):
            ax.text(value + xmax * .025, y, formatter(value), va="center", fontsize=13, weight="bold", color=ink)
        ax.set_xlabel(unit, loc="right", fontsize=10, color=soft)

    ax = axes[0, 0]
    style(ax, f"① 관측차 후보 {100 * totals['total_reduction']:.1f}% 감소", "100개 배치 합계 · 화각과 차량·구조물의 가림 반영")
    bars(ax, ["전체", "화각 통과", "가림까지 반영"],
         [totals[k] for k in ("N", "N1", "N2")], [blue, amber, teal], lambda x: f"{x:,.0f}대", "관측차 수")
    ax = axes[0, 1]
    style(ax, "② 도로별 최종 후보", "전체 관측차 중 남은 비율 · 각 도로 25회")
    road_keys = ["normal", "urban", "underpass", "elevated"]
    shares = [100 * sim["by_road"][r]["N2"] / sim["by_road"][r]["N"] for r in road_keys]
    bars(ax, [sim["by_road"][r]["label"] for r in road_keys], shares, teal, lambda x: f"{x:.1f}%", "최종 후보 비율 (%)")

    ax = axes[1, 0]
    style(ax, f"③ MP4 전송량 {100 * cost['totals']['network_total_reduction_fraction']:.1f}% 감소 추정", f"사고 1건 평균 · 실측 MP4 {cost['unit_costs']['mean_clip_MB']:.2f}MB/편 적용")
    bars(ax, ["전체 업로드", "화각만 적용", "두 단계 선별"],
         [avg[k] for k in ("network_all_upload_MB", "network_fov_only_MB", "network_two_stage_MB")],
         [blue, amber, teal], lambda x: f"{x:,.2f}", "MB / 사고")
    ax = axes[1, 1]
    style(ax, f"④ CPU 작업량 {100 * cost['totals']['cpu_reduction_fraction']:.1f}% 감소 추정", "사고 1건 평균 · 처리 단계별 실측값 적용")
    bars(ax, ["전체 AI 분석", "두 단계 선별"],
         [avg[k] for k in ("cpu_all_ai_core_seconds", "cpu_two_stage_core_seconds")],
         [blue, teal], lambda x: f"{x:,.2f}", "코어·초 / 사고")
    fig.text(.055, .067,
             "화각으로 분석 대상을 줄이고, 가림을 한 번 더 검사해 전송할 영상을 고릅니다.",
             fontsize=11, color=ink)
    fig.text(.055, .034,
             "전송량·CPU는 실측 영상 크기와 처리 단계별 사용량을 적용한 예상값입니다.",
             fontsize=10, color=soft)
    target = repo / "docs/assets/road-simulation-results.png"
    fig.savefig(target, dpi=160, facecolor=fig.get_facecolor())
    plt.close(fig)
    print(target)


if __name__ == "__main__":
    main()
