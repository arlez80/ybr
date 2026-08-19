"""build_merged_armature() を bpy 無しで確かめる小さなハーネス。

io_export_yui_ybr.py 全体は bpy に依存しているので、必要な関数だけを
ソースから取り出して mathutils の代わりの最小 Matrix と一緒に動かす。
"""

import ast
import math
import sys


# --- 最小の 4x4 行列 (mathutils.Matrix の代わり) ----------------------------
class Matrix(object):
    def __init__(self, rows=None):
        if rows is None:
            rows = [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]
        self.rows = [list(r) for r in rows]

    def __getitem__(self, i):
        return self.rows[i]

    def __matmul__(self, o):
        out = [[0.0] * 4 for _ in range(4)]
        for r in range(4):
            for c in range(4):
                out[r][c] = sum(self.rows[r][k] * o.rows[k][c] for k in range(4))
        return Matrix(out)

    def inverted(self):
        # 剛体変換 (回転 + 平行移動) だけを想定した簡易逆行列
        rot = [[self.rows[c][r] for c in range(3)] for r in range(3)]
        t = [self.rows[r][3] for r in range(3)]
        inv_t = [-sum(rot[r][k] * t[k] for k in range(3)) for r in range(3)]
        out = [[0.0] * 4 for _ in range(4)]
        for r in range(3):
            for c in range(3):
                out[r][c] = rot[r][c]
            out[r][3] = inv_t[r]
        out[3][3] = 1.0
        return Matrix(out)

    def copy(self):
        return Matrix(self.rows)

    def __repr__(self):
        return "Matrix(%r)" % (self.rows,)


def translation(x, y, z):
    m = Matrix()
    m.rows[0][3] = x
    m.rows[1][3] = y
    m.rows[2][3] = z
    return m


def rot_z(a):
    m = Matrix()
    c, s = math.cos(a), math.sin(a)
    m.rows[0][0] = c
    m.rows[0][1] = -s
    m.rows[1][0] = s
    m.rows[1][1] = c
    return m


# --- ダミーの Blender データ -------------------------------------------------
class Bone(object):
    def __init__(self, name, matrix_local, parent=None, use_deform=True, length=1.0):
        self.name = name
        self.matrix_local = matrix_local
        self.parent = parent
        self.use_deform = use_deform
        self.length = length
        self.children = []
        if parent is not None:
            parent.children.append(self)


class ArmData(object):
    def __init__(self, name, bones):
        self.name = name
        self.bones = bones


class ArmObject(object):
    def __init__(self, name, data, matrix_world=None):
        self.name = name
        self.data = data
        self.matrix_world = matrix_world or Matrix()
        self.type = "ARMATURE"


# --- 対象の関数だけをソースから抜き出して実行 -------------------------------
WANTED = {
    "armature_bone_order",
    "nearest_included_parent",
    "MergedArmature",
    "build_merged_armature",
    "build_merged_armature_block",
    "retarget_armature_nodes",
}


def load_functions(path):
    src = open(path, encoding="utf-8").read()
    tree = ast.parse(src)
    picked = [
        n
        for n in tree.body
        if isinstance(n, (ast.FunctionDef, ast.ClassDef)) and n.name in WANTED
    ]
    missing = WANTED - {n.name for n in picked}
    if missing:
        raise SystemExit("not found in source: %s" % sorted(missing))

    ns = {
        "Matrix": Matrix,
        "warn": lambda msg: WARNINGS.append(msg),
        "conv_matrix": lambda m: m,  # 軸変換はここでは素通し
        "mat_to_column_major": lambda m: [m[r][c] for c in range(4) for r in range(4)],
        "_NODE_TYPE_CODE": {"ARMATURE": 3},
    }
    exec(compile(ast.Module(body=picked, type_ignores=[]), path, "exec"), ns)
    return ns


WARNINGS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    return bool(cond)


def main():
    path = sys.argv[1] if 1 < len(sys.argv) else "blender/io_export_yui_ybr.py"
    ns = load_functions(path)
    build = ns["build_merged_armature"]
    block_of = ns["build_merged_armature_block"]
    retarget = ns["retarget_armature_nodes"]

    failures = 0

    def C(cond, what):
        nonlocal failures
        if not check(cond, what):
            failures += 1

    # --- アーマチュア A : root -> child ---
    a_root = Bone("Root", translation(0, 0, 0))
    a_child = Bone("Child", translation(0, 1, 0), parent=a_root)
    arm_a = ArmObject("ArmA", ArmData("ArmAData", [a_root, a_child]))

    # --- アーマチュア B : 別の場所に置いてある / ボーン名が A と衝突 ---
    b_root = Bone("Root", translation(0, 0, 0))
    b_tip = Bone("Tip", translation(0, 2, 0), parent=b_root)
    arm_b = ArmObject(
        "ArmB", ArmData("ArmBData", [b_root, b_tip]), matrix_world=translation(5, 0, 0)
    )

    print("\n[1] 単一アーマチュア")
    del WARNINGS[:]
    m = build([arm_a], True)
    C(m is not None, "合成結果が返る")
    C(m.data_id == "ArmAData", "id は元のアーマチュアデータ名")
    C(m.merged() is False, "合成扱いにならない")
    C(len(m.bones) == 2, "ボーンが 2 本")
    C([b["name"] for b in m.bones] == ["Root", "Child"], "名前はそのまま")
    C(m.bones[0]["parent"] == -1 and m.bones[1]["parent"] == 0, "親子関係が入る")
    C(not WARNINGS, "警告は出ない")

    print("\n[2] アーマチュア 0 個")
    C(build([], True) is None, "None が返る")

    print("\n[3] 2 つを合成")
    del WARNINGS[:]
    m = build([arm_b, arm_a], True)  # 渡す順に依らず名前順で基準を選ぶ
    C(m.primary is arm_a, "名前順で先頭が基準")
    C(m.data_id == "ArmAData", "id は基準のもの")
    C(m.merged() is True, "合成扱いになる")
    C(len(m.bones) == 4, "ボーンは 4 本")

    names = [b["name"] for b in m.bones]
    C(names[:2] == ["Root", "Child"], "基準のボーンが先に来て名前も変わらない")
    C("ArmB/Root" in names, "衝突した名前だけ改名される")
    C("Tip" in names, "衝突していない名前はそのまま")

    parents = [b["parent"] for b in m.bones]
    C(parents.count(-1) == 2, "ルートが 2 本 (合成先はマルチルート)")
    tip_i = names.index("Tip")
    C(parents[tip_i] == names.index("ArmB/Root"), "B 側の親子関係が保たれる")

    # B のルートは基準空間へ +5 ずれて入る
    root_b = m.bones[names.index("ArmB/Root")]
    # column-major の 13 番目 (index 12) が平行移動 x
    C(abs(root_b["rest"][12] - 5.0) < 1e-6, "B のルートが基準空間へ移されている")
    C(abs(root_b["rest_parent"][12] - 5.0) < 1e-6, "rest_parent にも反映される")

    # B の子ボーンは親相対なので rel は掛からない
    tip = m.bones[tip_i]
    C(abs(tip["rest_parent"][12]) < 1e-6, "子ボーンの親相対には rel が掛からない")
    C(abs(tip["rest"][12] - 5.0) < 1e-6, "子ボーンの rest は基準空間")

    C(any("merged into one" in w for w in WARNINGS), "合成した旨の警告が出る")
    C(any("renamed" in w for w in WARNINGS), "改名した旨の警告が出る")

    print("\n[4] 索引まわり")
    C(m.bone_index(arm_a, "Child") == 1, "基準側のボーン index を引ける")
    C(m.bone_out_name(arm_b, "Root") == "ArmB/Root", "改名後の名前を引ける")
    C(m.bone_index(arm_b, "Root") == names.index("ArmB/Root"), "B 側の index を引ける")
    C(m.bone_index(arm_a, "Nope") == -1, "無いボーンは -1")
    C(m.included(arm_b) == {"Root", "Tip"}, "アーマチュアごとの対象ボーン集合")
    C(abs(m.relative(arm_b)[0][3] - 5.0) < 1e-6, "rel が引ける")
    C(abs(m.relative(arm_a)[0][3]) < 1e-6, "基準の rel は単位行列")

    print("\n[5] ARMATURE ブロック")
    blk = block_of(m)
    C(blk["kind"] == "ARMATURE", "kind が ARMATURE")
    C(blk["id"] == "ArmAData", "id が基準のもの")
    C(len(blk["bones"]) == 4, "ボーンが全部入る")

    print("\n[6] シーンツリーの張り替え")
    tree = [
        {
            "name": "Collection",
            "type": 0,
            "data": None,
            "children": [
                {"name": "ArmA", "type": 3, "data": "ArmAData", "children": []},
                {"name": "ArmB", "type": 3, "data": "ArmBData", "children": []},
                {"name": "Cube", "type": 1, "data": "CubeMesh", "children": []},
            ],
        }
    ]
    retarget(tree, m)
    kids = tree[0]["children"]
    C(kids[0]["data"] == "ArmAData", "基準ノードは合成後の id を指す")
    C(kids[1]["data"] is None, "合成された側は data を外される")
    C(kids[2]["data"] == "CubeMesh", "メッシュノードは触られない")

    print("\n[7] deform_only")
    ctrl = Bone("Ctrl", translation(0, 0, 0), use_deform=False)
    defo = Bone("Def", translation(0, 1, 0), parent=ctrl, use_deform=True)
    arm_c = ArmObject("ArmC", ArmData("ArmCData", [ctrl, defo]))
    m2 = build([arm_c], True)
    C([b["name"] for b in m2.bones] == ["Def"], "非 deform ボーンは落ちる")
    C(m2.bones[0]["parent"] == -1, "除外された親は飛ばされてルートになる")
    m3 = build([arm_c], False)
    C(len(m3.bones) == 2, "deform_only を切れば全部入る")

    print("\n%s" % ("すべて通りました" if failures == 0 else "%d 件 失敗" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
