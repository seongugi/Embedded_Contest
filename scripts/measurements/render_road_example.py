#!/usr/bin/env python3
"""Plot the representative scene exported by export_example.cjs, without a browser."""
import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.collections import PolyCollection
from matplotlib.colors import to_rgb
from matplotlib.patches import FancyBboxPatch, Rectangle
import numpy as np


COLORS = ["#5385B7", "#D99A36", "#169B84"]
INK, SOFT, BG = "#193348", "#708494", "#F1F5F7"
RED, REJECT = "#E5695C", "#ADBBC4"


def project(points):
    points = np.asarray(points, dtype=float) - [500, 340, 15]
    yaw, pitch = -.4, .28
    right = np.cos(yaw) * points[:, 0] - np.sin(yaw) * points[:, 1]
    front = np.sin(yaw) * points[:, 0] + np.cos(yaw) * points[:, 1]
    up = points[:, 2] * np.cos(pitch) - front * np.sin(pitch)
    depth = front * np.cos(pitch) + points[:, 2] * np.sin(pitch)
    perspective = 2200 / (2200 - depth)
    return np.column_stack([right * perspective, up * perspective, depth])


def shade(color, factor):
    return np.clip(np.asarray(to_rgb(color)) * factor, 0, 1)


def scene_faces(data, stage):
    faces = []

    def face(points, color):
        q = project(points)
        faces.append((float(np.mean(q[:, 2])), q[:, :2], color))

    def box(center, axes, size, color):
        center, axes, size = np.asarray(center), np.asarray(axes), np.asarray(size)
        points = [center + (np.array([x, y, z]) * size / 2) @ axes
                  for z in (-1, 1) for x, y in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        face(points[4:], color)
        for i, factor in enumerate((.69, .86, .77, .94)):
            face([points[i], points[(i+1) % 4], points[(i+1) % 4+4], points[i+4]], shade(color, factor))

    def simple_box(x, y, z, length, width, height, color, heading=0, pitch=0):
        u = np.array([np.cos(heading)*np.cos(pitch), np.sin(heading)*np.cos(pitch), np.sin(pitch)])
        v = np.array([-np.sin(heading), np.cos(heading), 0])
        n = np.cross(u, v)
        box(np.array([x, y, z]) + n * height/2, [u, v, n], [length, width, height], color)

    for road in data["roads"]:
        # Split long slabs so the painter ordering preserves the bridge crossing.
        for segment in road["segments"]:
            a, b = np.asarray(segment["a"]), np.asarray(segment["b"])
            u, v, n = map(np.asarray, segment["axes"])
            length, width, thick = segment["size"]
            count = max(1, int(np.ceil(length/20)))
            for i in range(count):
                center = a + (b-a)*(i+.5)/count - n*thick/2
                box(center, [u, v, n], [length/count+.1, width, thick], "#596C7D")
            for side in (-1, 1):
                for i in range(count):
                    center = a + (b-a)*(i+.5)/count + v*side*(width/2-3) + n*.3
                    box(center, [u, v, n], [length/count+.1, .9, .12], "#D5DFE3")
            for lane in range(1, road["lanes"]):
                offset = (lane-road["lanes"]/2)*19
                centerline = lane == road["lanes"]/2 and road["kind"] != "frontage"
                dash_length = 8
                for distance in np.arange(5, segment["len"]-5, 18):
                    center = a + u*distance + v*offset + n*.5
                    box(center, [u, v, n], [dash_length, .65, .1], "#E3C77E" if centerline else "#9CABBA")
        if road["raised"]:
            for segment in road["segments"]:
                if max(segment["a"][2], segment["b"][2]) < 15:
                    continue
                a, b = np.asarray(segment["a"]), np.asarray(segment["b"])
                u, v, n = map(np.asarray, segment["axes"])
                count = max(1, int(np.ceil(segment["len"]/20)))
                for side in (-1, 1):
                    for i in range(count):
                        center = a+(b-a)*(i+.5)/count+v*side*(road["width"]/2+1)+n*3
                        box(center, [u, v, n], [segment["len"]/count+.1, 3, 6], "#BAC8CE")

    for structure in data["structures"]:
        if "axes" in structure:  # Decks are already plotted from the roads.
            continue
        simple_box(structure["x"], structure["y"], structure["zMin"], structure["w"], structure["h"],
                   structure["zMax"]-structure["zMin"], "#BAC8C9")
    for building in data["buildings"]:
        simple_box(building["x"], building["y"], building["zMin"], building["w"], building["h"],
                   building["zMax"]-building["zMin"], "#C0CEC2")

    def car(vehicle, accident=False):
        active = accident or stage == 0 or (vehicle["first"] if stage == 1 else vehicle["final"])
        color = RED if accident else COLORS[stage] if active else REJECT
        length = 30 if accident else 34 if vehicle.get("truck") else 24
        height = 28 if vehicle.get("truck") else 16
        x, y, z, heading, pitch = [vehicle.get(key, 0) for key in ("x", "y", "z", "h", "pitch")]

        def position(dx, dy, dz):
            return [x+dx*np.cos(heading)-dy*np.sin(heading),
                    y+dx*np.sin(heading)+dy*np.cos(heading), z+dz+dx*np.sin(pitch)]

        simple_box(x, y, z+2, length, 16 if accident else 14, height*.54, color, heading, pitch)
        simple_box(*position(-2, 0, height*.53+2), length*.56, 11, height*.43, shade(color, 1.04), heading, pitch)
        simple_box(*position(length*.17, 0, height*.92+2), 3, 9, .6, "#C5E3EB", heading, pitch)
        if accident:
            simple_box(x, y, z+26, 2, 2, 16, RED)
            simple_box(x+6, y, z+38, 12, 2, 7, RED)

    for vehicle in data["vehicles"]:
        car(vehicle)
    incident = dict(data["accident"], h=np.deg2rad(data["accident"]["heading_degrees"]))
    car(incident, True)
    return sorted(faces, key=lambda item: item[0])


def draw_scene(ax, data, stage):
    ax.set_facecolor("#EFF4F6")
    ground = project([[-15, -15, -3], [1015, -15, -3], [1015, 695, -3], [-15, 695, -3]])[:, :2]
    ax.add_collection(PolyCollection([ground], facecolors=["#D5E1D5"], edgecolors="none"))
    faces = scene_faces(data, stage)
    ax.add_collection(PolyCollection([item[1] for item in faces], facecolors=[item[2] for item in faces], edgecolors="none"))
    ax.set_xlim(-600, 660)
    ax.set_ylim(-220, 250)
    ax.set_aspect("equal")
    ax.set_axis_off()


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=repo/"docs/measurements/road-simulation/example.json")
    parser.add_argument("--out", type=Path, default=repo/"docs/assets/road-simulation-example.png")
    parser.add_argument("--font", type=Path)
    args = parser.parse_args()
    if args.font:
        font_manager.fontManager.addfont(str(args.font))
        font_name = font_manager.FontProperties(fname=str(args.font)).get_name()
    else:
        available = {font.name for font in font_manager.fontManager.ttflist}
        font_name = next((name for name in ("Malgun Gothic", "Noto Sans CJK KR", "NanumGothic") if name in available), None)
        if not font_name:
            raise SystemExit("Use --font PATH to supply a Korean font.")
    plt.rcParams.update({"font.family": font_name, "axes.unicode_minus": False})
    data = json.loads(args.data.read_text(encoding="utf-8"))
    counts, design = data["counts"], data["design"]
    actual_counts = [len(data["vehicles"]), sum(v["first"] for v in data["vehicles"]), sum(v["final"] for v in data["vehicles"])]
    if actual_counts != [counts[k] for k in ("N", "N1", "N2")]:
        raise ValueError("Vehicle states do not match the summary counts")
    fig = plt.figure(figsize=(19, 1336/150), dpi=150, facecolor=BG)

    def card(x, y, w, h, color="white", edge="#E2E9ED"):
        patch = FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.006,rounding_size=0.01",
                               transform=fig.transFigure, facecolor=color, edgecolor=edge, linewidth=.7, zorder=0)
        fig.add_artist(patch)

    fig.text(.03, .956, "ACE", color="white", fontsize=16, weight="bold",
             bbox={"boxstyle": "round,pad=.6", "facecolor": "#183B51", "edgecolor": "none"})
    fig.text(.076, .967, "WITNESS LAB / 관측차 선별 · 대표 배치", color=SOFT, fontsize=11, weight="bold")
    fig.text(.076, .931, "메시지는 모두에게, 증거는 필요한 차량에서.", color=INK, fontsize=22, weight="bold")
    fig.text(.968, .95, "위치·화각 → 기하 가시성", color=SOFT, fontsize=12, ha="right")

    stages = [("01 / RECEIVE", "전체 수신", counts["N"], "사고 이벤트를 수신한 전체 관측차", "이벤트 전달"),
              ("02 / GEOMETRY", "1차 · 위치 + 화각", counts["N1"], f"{counts['N']-counts['N1']}대 제외 · 영상 확인 대상 {counts['N1']}대", f"영상 확인 대상 -{100*counts['first_reduction']:.1f}%"),
              ("03 / VISIBILITY", "2차 · 가림 반영", counts["N2"], f"{counts['N1']-counts['N2']}대 추가 제외 · 최종 {counts['N2']}대 선정", f"전체 대비 -{100*counts['total_reduction']:.1f}%")]
    for i, (step, label, number, caption, badge) in enumerate(stages):
        x, width = .025+i*.325, .308
        card(x, .765, width, .131)
        fig.text(x+.012, .869, step, color=COLORS[i], fontsize=10, weight="bold")
        fig.text(x+.102, .869, label, color=INK, fontsize=11, weight="bold")
        fig.text(x+width-.008, .805, badge, color=COLORS[i], fontsize=10, ha="right")
        fig.text(x+.012, .81, str(number), color=INK, fontsize=31, weight="bold")
        fig.text(x+.058, .815, "대", color=SOFT, fontsize=11)
        fig.text(x+.012, .782, caption, color=SOFT, fontsize=10)
        fig.add_artist(Rectangle((x+.012, .773), (width-.024)*number/counts["N"], .003,
                                 transform=fig.transFigure, facecolor=COLORS[i], edgecolor="none"))

    card(.025, .23, .79, .504)
    fig.text(.036, .704, "사고차 영상이 남는 관측차", color=INK, fontsize=14, weight="bold")
    fig.text(.036, .679, "고가 본선과 지상 측도 · 같은 배치에서 최종 후보만 강조", color=SOFT, fontsize=11)
    ax = fig.add_axes([.032, .263, .775, .401])
    draw_scene(ax, data, 2)
    legend = [("사고차", RED), ("전체 수신", COLORS[0]), ("1차 후보", COLORS[1]), ("최종 선정", COLORS[2]), ("제외", REJECT)]
    for i, (label, color) in enumerate(legend):
        x = .037+i*.102
        fig.add_artist(Rectangle((x, .244), .005, .008, transform=fig.transFigure, facecolor=color, edgecolor="none"))
        fig.text(x+.01, .243, label, color=SOFT, fontsize=10)

    card(.838, .23, .137, .504)
    fig.text(.851, .699, "배치 조건", color=INK, fontsize=12, weight="bold")
    conditions = [("도로 유형", "고가도로"), ("도로별 왕복 차선", f"{design['lanes']}차로"),
                  ("전체 관측 차량", f"{design['count']}대"), ("카메라 수평 화각", f"{design['camera_horizontal_fov_degrees']}° · ±{design['camera_horizontal_fov_degrees']/2:g}°"),
                  ("배치 시드", str(design["seed"]))]
    for i, (label, value) in enumerate(conditions):
        y = .651-i*.070
        fig.text(.851, y, label, color=SOFT, fontsize=10)
        fig.text(.851, y-.027, value, color=INK, fontsize=14, weight="bold")
    fig.text(.851, .259, "기하계산으로 생성한\n대표 배치", color=SOFT, fontsize=10, linespacing=1.7)

    for i, (_, label, number, _, _) in enumerate(stages):
        x, width = .025+i*.325, .308
        card(x, .045, width, .155, edge=COLORS[i] if i == 2 else "#E2E9ED")
        fig.text(x+.008, .177, label, color=SOFT, fontsize=10)
        fig.text(x+width-.008, .177, f"{number}대", color=COLORS[i], fontsize=11, weight="bold", ha="right")
        preview = fig.add_axes([x+.003, .05, width-.006, .115])
        draw_scene(preview, data, i)
    fig.text(.027, .018, f"고가도로 · {design['lanes']}차로 · 배치 시드 {design['seed']}", color=SOFT, fontsize=9)
    fig.text(.973, .018, "단위: 시뮬레이션 scene units", color=SOFT, fontsize=9, ha="right")
    fig.savefig(args.out, dpi=150, facecolor=BG)
    plt.close(fig)
    print(args.out)


if __name__ == "__main__":
    main()
