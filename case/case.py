"""DNESP32S3 + ATK 4.3" RGB 屏 外壳（参数化）。

uv run python case.py  → out/front.stl, out/back.stl, out/stand.stl, out/case.3mf

坐标：X 宽（屏幕长边），Y 高（短边），Z 厚度。前壳正面贴打印平台（Z=0 是屏幕正面）。
所有带 "估" 的数字都要用实物尺寸替换。
"""
from pathlib import Path
import numpy as np
import trimesh
from manifold3d import Manifold, Mesh

# ---------- 尺寸参数（mm） ----------
P = dict(
    scr_w=105.5, scr_h=67.2, scr_t=5.5,    # 屏幕模块（含触摸玻璃）外形 估
    aa_w=95.04, aa_h=53.86,                 # 显示区
    aa_dx=0.0, aa_dy=0.0,                   # 显示区相对模块中心的偏移 估
    stack_t=22.0,                           # 屏背面到开发板最高元件的总厚度 估
    wall=2.4, face=1.8, gap=0.3,            # 壁厚 / 正面厚 / 装配间隙
    bezel=1.0,                              # 窗口比显示区每边大多少
    corner_r=4.0,
    # USB-C 开口：side 取 right/left/bottom/top；pos 为沿该边从左/下角起的中心位置；z 为距屏幕正面的中心深度
    usb=[dict(side="right", pos=33.6, z=20.0, w=10.5, h=4.5)],  # 估
    btn=[],                                 # 按键孔：dict(side=..., pos=..., z=..., d=4)
    screw_d=2.6, boss_d=6.5,                # M3 自攻
    vent_n=9, vent_w=2.0, vent_l=40.0,
    tilt=15.0,                              # 支架后仰角
)


def rrect(w, h, t, r):
    """圆角矩形块，底面在 z=0，XY 居中。"""
    r = min(r, w / 2 - 0.01, h / 2 - 0.01)
    c = Manifold.cylinder(t, r, r, 48)
    pts = [c.translate([sx * (w / 2 - r), sy * (h / 2 - r), 0]) for sx in (-1, 1) for sy in (-1, 1)]
    return Manifold.batch_hull(pts)


def box(w, h, t, x=0, y=0, z=0):
    return Manifold.cube([w, h, t], center=False).translate([x - w / 2, y - h / 2, z])


def side_cut(side, pos, z, w, h, ow, oh, depth=10):
    """在外壳侧面打一个圆角槽。pos 从左边/下边量起（屏幕正视）。"""
    if side in ("right", "left"):
        s = rrect(h, w, depth, h / 2 - 0.01).rotate([0, 90, 0])  # Y=w, Z=h, 沿 X 伸出
        x = ow / 2 if side == "right" else -ow / 2
        return s.translate([x - depth / 2, pos - oh / 2, z])
    s = rrect(w, h, depth, h / 2 - 0.01).rotate([90, 0, 0])      # X=w, Z=h, 沿 Y 伸出
    y = oh / 2 if side == "top" else -oh / 2
    return s.translate([pos - ow / 2, y + depth / 2, z])


def build(p):
    iw, ih = p["scr_w"] + 2 * p["gap"], p["scr_h"] + 2 * p["gap"]
    ow, oh = iw + 2 * p["wall"], ih + 2 * p["wall"]
    depth = p["face"] + p["scr_t"] + p["stack_t"]
    lip = 2.0  # 后盖嵌入深度

    # 前壳
    front = rrect(ow, oh, depth, p["corner_r"])
    front -= rrect(iw, ih, depth, p["corner_r"] - p["wall"]).translate([0, 0, p["face"]])
    win = rrect(p["aa_w"] + 2 * p["bezel"], p["aa_h"] + 2 * p["bezel"], p["face"] + 1, 1.0)
    front -= win.translate([p["aa_dx"], p["aa_dy"], -0.5])
    # 窗口内侧倒角 45°，看起来更薄
    cham = Manifold.batch_hull([
        box(p["aa_w"] + 2 * p["bezel"], p["aa_h"] + 2 * p["bezel"], 0.01, p["aa_dx"], p["aa_dy"], p["face"] - 0.01),
        box(p["aa_w"] + 2 * p["bezel"] + 2 * (p["face"] - 0.6), p["aa_h"] + 2 * p["bezel"] + 2 * (p["face"] - 0.6), 0.01,
            p["aa_dx"], p["aa_dy"], 0.6),
    ])
    front -= cham
    # 四角螺丝柱：从屏幕背面到壳口，后盖用 M3 螺丝固定
    bz0 = p["face"] + p["scr_t"]
    bx, by = iw / 2 - p["boss_d"] / 2 + 0.5, ih / 2 - p["boss_d"] / 2 + 0.5
    for sx in (-1, 1):
        for sy in (-1, 1):
            boss = Manifold.cylinder(depth - lip - bz0, p["boss_d"] / 2, p["boss_d"] / 2, 32)
            boss -= Manifold.cylinder(depth, p["screw_d"] / 2, p["screw_d"] / 2, 24)
            front += boss.translate([sx * bx, sy * by, bz0])
    for u in p["usb"] + p["btn"]:
        w = u.get("w", u.get("d", 4)); h = u.get("h", u.get("d", 4))
        front -= side_cut(u["side"], u["pos"], u["z"] - h / 2, w, h, ow, oh)

    # 后盖：一块平板 + 嵌入前壳的唇边，带散热槽
    bt = 2.0
    back = rrect(ow, oh, bt, p["corner_r"])
    back += rrect(iw - 0.4, ih - 0.4, lip, p["corner_r"] - p["wall"]).translate([0, 0, bt])
    back -= rrect(iw - 0.4 - 2 * 1.6, ih - 0.4 - 2 * 1.6, lip + 1, 1).translate([0, 0, bt])
    for sx in (-1, 1):
        for sy in (-1, 1):
            back -= Manifold.cylinder(10, 1.7, 1.7, 24).translate([sx * bx, sy * by, -1])
            back -= Manifold.cylinder(1.6, 1.7, 3.2, 24).translate([sx * bx, sy * by, -0.01])  # 沉头
    pitch = p["vent_w"] * 2.2
    for i in range(p["vent_n"]):
        x = (i - (p["vent_n"] - 1) / 2) * pitch
        back -= rrect(p["vent_w"], p["vent_l"], 10, p["vent_w"] / 2 - 0.01).translate([x, 6, -1])
    # 角柱内侧给螺丝柱留位
    for sx in (-1, 1):
        for sy in (-1, 1):
            back -= Manifold.cylinder(lip + 1, p["boss_d"] / 2 + 0.4, p["boss_d"] / 2 + 0.4, 32).translate([sx * bx, sy * by, bt])

    # 支架：托槽，壳底边放进去后仰 tilt 度，侧面打印无需支撑
    # 侧视（Y 向后, Z 向上）：壳体截面是 th 厚的矩形，后仰 tilt 度坐在底座上，
    # 前挡挡住正面下沿，后靠背托住背面。整体当 2D 轮廓拉伸，侧放打印无支撑。
    th = depth + bt
    a = np.radians(p["tilt"])
    sw = ow * 0.6
    c, s = np.cos(a), np.sin(a)
    g = 0.4                                   # 放进去的间隙
    base_t, lip_h, rest_h, rest_t = 4.0, 9.0, 50.0, 3.5
    y0 = 3.5                                  # 前挡厚度
    from manifold3d import CrossSection
    T = th + 2 * g
    zf = base_t + T * s                       # 前下角抬高，使后下角刚好落在底座上
    def rot(y, z):                            # 壳体局部 (厚度方向, 高度方向) → 侧视坐标
        return (y0 + y * c + z * s, zf - y * s + z * c)
    case_poly = [rot(0, -5), rot(T, -5), rot(T, 200), rot(0, 200)]
    rb = rot(T, 0)
    base_len = rb[0] + rest_t + 25
    solid = CrossSection([[(0, 0), (base_len, 0), (base_len, base_t), (0, base_t)]])
    solid += CrossSection([[(0, 0), (rb[0] + 1, 0), (rb[0] + 1, zf + 1), (0, zf + 1)]])   # 楔形垫
    solid += CrossSection([[(0, 0), (y0 + 0.01, 0), (y0, zf + lip_h), (0, zf + lip_h)]])  # 前挡
    r0, r1 = rot(T, 0), rot(T, rest_h)
    solid += CrossSection([[(r0[0], 0), (r0[0] + rest_t / c + 8, 0), (r1[0] + rest_t / c, r1[1]), r1]])  # 后靠背
    prof = solid - CrossSection([case_poly])
    stand = Manifold.extrude(prof, sw).rotate([90, 0, 90]).translate([-sw / 2, 0, 0])
    # 底座中间开槽走线
    stand -= box(16, base_len - y0 - 6, 20, 0, y0 + 3 + (base_len - y0 - 6) / 2, -1)
    return front, back, stand, dict(ow=ow, oh=oh, depth=depth)


def to_tm(m):
    mesh = m.to_mesh()
    return trimesh.Trimesh(np.asarray(mesh.vert_properties)[:, :3], np.asarray(mesh.tri_verts), process=False)


if __name__ == "__main__":
    out = Path(__file__).parent / "out"; out.mkdir(exist_ok=True)
    front, back, stand, info = build(P)
    parts = {"front": front, "back": back, "stand": stand}
    for k, m in parts.items():
        t = to_tm(m)
        t.export(out / f"{k}.stl")
        print(k, "watertight" if t.is_watertight else "NOT watertight", np.round(t.extents, 1))
    print("外形", round(info["ow"], 1), "x", round(info["oh"], 1), "x", round(info["depth"] + 2, 1))
