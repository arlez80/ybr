"""Blender アドオンの自動テスト

    blender --background --python blender/test_export.py

Blender を起動して、コードでシーンを組み立てて .ybr を書き出し、
出てきたバイト列を読み直して中身を確かめる。
CBOR の読み取りはこのファイル内に最小限のものを持っているので、
外部ライブラリは要らない。

終了コードは 0 (全部通った) / 1 (失敗あり)。
"""

import importlib.util
import os
import struct
import sys
import tempfile
import traceback

import bpy
from mathutils import Vector

# --- アドオン本体を読み込む -------------------------------------------------
#
#   単に import すると、インストール済みの同名アドオンが優先される。
#   有効化されたアドオンは既に sys.modules に載っているので、
#   sys.path をいじっても掴むのは Blender の scripts/addons 側になる。
#   テストしたいのは「このリポジトリのファイル」なので、
#   パスを指定して別名で読み込む。
_HERE = os.path.dirname(os.path.abspath(__file__))
_ADDON_PATH = os.path.join(_HERE, "io_export_yui_ybr.py")

_spec = importlib.util.spec_from_file_location("ybr_test_addon", _ADDON_PATH)
ybr = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = ybr
_spec.loader.exec_module(ybr)
print("addon under test: %s" % _ADDON_PATH)


def _disable_installed_addon():
    """インストール済みの同じアドオンが有効なら外す。

    クラス名 (YBR_ImageSettings など) が衝突して register できないため。
    """
    try:
        import addon_utils
    except ImportError:
        return
    for mod in addon_utils.modules():
        name = getattr(mod, "__name__", "")
        if not name.endswith("io_export_yui_ybr"):
            continue
        try:
            addon_utils.disable(name, default_set=False)
            print("disabled installed addon: %s" % name)
        except Exception as e:
            print("could not disable %s: %s" % (name, e))


def _unregister_conflicts():
    """同じ名前で登録済みのクラスが残っていれば外す。

    アドオン管理を通さずに登録されている場合 (前回の実行の残りなど) の保険。
    """
    for c in getattr(ybr, "classes", ()):
        existing = getattr(bpy.types, c.__name__, None)
        if existing is None or existing is c:
            continue
        try:
            bpy.utils.unregister_class(existing)
            print("unregistered leftover class: %s" % c.__name__)
        except Exception:
            pass


# アドオンは import しただけでは登録されない (__main__ のときだけ自動登録)。
# bpy.ops.export_scene.yui_ybr を使うので、ここで登録しておく。
_disable_installed_addon()
_unregister_conflicts()
try:
    ybr.register()
except Exception as e:
    print("addon register failed: %s" % e)
    raise


def push_action_to_nla(obj):
    """アクティブなアクションを NLA ストリップへ移す。

    アドオンは NLA ストリップになっているものだけを書き出すので、
    アニメーションのテストではここを通しておく。
    """
    ad = obj.animation_data
    if ad is None or ad.action is None:
        return None
    action = ad.action
    track = ad.nla_tracks.new()
    track.name = action.name
    strip = track.strips.new(action.name, 1, action)
    # 4.4 以降のスロット付きアクションでは、どのスロットを使うか指定する
    slots = getattr(action, "slots", None)
    if slots and hasattr(strip, "action_slot"):
        try:
            strip.action_slot = slots[0]
        except Exception:
            pass
    ad.action = None
    return strip


def set_opaque_blend(mat):
    """ブレンド設定を不透明にする。

    blend_method は 4.2 で surface_render_method に置き換わり、
    新しい Blender では消えているので、あるものだけ触る。
    """
    if hasattr(mat, "surface_render_method"):
        mat.surface_render_method = "DITHERED"
    elif hasattr(mat, "blend_method"):
        mat.blend_method = "OPAQUE"


def use_nodes(mat):
    """マテリアルにノードを使わせる。

    Material.use_nodes は Blender 5.x で非推奨、6.0 で無くなる予定
    (マテリアルは常にノードを持つ)。新しい Blender では触らない。
    """
    if bpy.app.version < (5, 0):
        mat.use_nodes = True
    return mat.node_tree


# ===========================================================================
# 最小限の CBOR リーダー
# ===========================================================================
class CborReader:
    def __init__(self, data):
        self.d = data
        self.i = 0

    def _u(self, n):
        v = int.from_bytes(self.d[self.i : self.i + n], "big")
        self.i += n
        return v

    def _head(self):
        """(メジャータイプ, 情報部, 引数の値) を返す。

        メジャー 7 は「情報部」で中身が決まる (20=false / 26=float32 など)
        ので、読み取った値と混同しないよう別々に返す。
        """
        b = self.d[self.i]
        self.i += 1
        major = b >> 5
        info = b & 0x1F
        if info < 24:
            return major, info, info
        if info == 24:
            return major, info, self._u(1)
        if info == 25:
            return major, info, self._u(2)
        if info == 26:
            return major, info, self._u(4)
        if info == 27:
            return major, info, self._u(8)
        if info == 31:
            return major, info, None  # 不定長
        raise ValueError("bad cbor header info %d" % info)

    def value(self):
        major, info, n = self._head()
        if major == 0:
            return n
        if major == 1:
            return -1 - n
        if major == 2:
            b = self.d[self.i : self.i + n]
            self.i += n
            return b
        if major == 3:
            b = self.d[self.i : self.i + n]
            self.i += n
            return b.decode("utf-8", "replace")
        if major == 4:
            return [self.value() for _ in range(n)]
        if major == 5:
            out = {}
            for _ in range(n):
                k = self.value()
                out[k] = self.value()
            return out
        if major == 6:
            return self.value()  # タグは中身だけ返す
        if major == 7:
            if info == 20:
                return False
            if info == 21:
                return True
            if info == 22:
                return None
            if info == 23:
                return None
            if info == 25:
                return _half_to_float(n)
            if info == 26:
                self.i -= 4
                v = struct.unpack_from(">f", self.d, self.i)[0]
                self.i += 4
                return v
            if info == 27:
                self.i -= 8
                v = struct.unpack_from(">d", self.d, self.i)[0]
                self.i += 8
                return v
            return None
        raise ValueError("bad cbor major %d" % major)


def _half_to_float(h):
    s = (h >> 15) & 1
    e = (h >> 10) & 0x1F
    m = h & 0x3FF
    if e == 0:
        v = (m / 1024.0) * (2.0**-14)
    elif e == 31:
        v = float("inf") if m == 0 else float("nan")
    else:
        v = (1.0 + m / 1024.0) * (2.0 ** (e - 15))
    return -v if s else v


def load_ybr(path):
    with open(path, "rb") as f:
        root = CborReader(f.read()).value()
    assert isinstance(root, list) and len(root) == 4, "root must be a 4 element array"
    assert root[0] == "YUI", "magic must be 'YUI'"
    return {"version": root[1], "tree": root[2], "blocks": root[3]}


# 数値配列は「型を名前で決めるバイト列」として入っている (タグは使わない)
def f32(b):
    return list(struct.unpack("<%df" % (len(b) // 4), b)) if b else []


def u32(b):
    return list(struct.unpack("<%dI" % (len(b) // 4), b)) if b else []


def u16(b):
    return list(struct.unpack("<%dH" % (len(b) // 2), b)) if b else []


def u8(b):
    return list(b) if b else []


def blocks_of(scene, kind):
    return [b for b in scene["blocks"] if b.get("kind") == kind]


def iter_nodes(nodes):
    """ツリーを深さ優先でたどる (子まで含めて全ノードを返す)"""
    for n in nodes:
        yield n
        for c in iter_nodes(n.get("children") or []):
            yield c


def node_names(sc):
    """ツリーに出てくるノード名を全部集める"""
    return [n["name"] for n in iter_nodes(sc["tree"])]


def find_node(sc, name):
    for n in iter_nodes(sc["tree"]):
        if n["name"] == name:
            return n
    return None


def find_block(scene, kind, ident):
    for b in blocks_of(scene, kind):
        if b.get("id") == ident:
            return b
    return None


# ===========================================================================
# テストの土台
# ===========================================================================
_passed = 0
_failed = 0
_group = ""


def group(name):
    global _group
    _group = name
    print("\n--- %s ---" % name)


def check(cond, what):
    global _passed, _failed
    if cond:
        _passed += 1
    else:
        _failed += 1
        print("  FAIL [%s] %s" % (_group, what))


def near(a, b, eps=1e-4):
    return abs(float(a) - float(b)) <= eps


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def EXPORT_OT_yui_ybr_props():
    """オペレーターが受け取れるプロパティ名の一覧

    クラスのアノテーション (name: BoolProperty(...)) を見る。
    引数名を間違えると Blender の例外は分かりにくいので、
    export() の中で先に突き合わせるために使う。
    """
    names = set(getattr(ybr.EXPORT_OT_yui_ybr, "__annotations__", {}).keys())
    names.discard("filter_glob")
    return names


def export(path, **kw):
    """書き出して読み戻す"""
    opts = dict(
        filepath=path,
        material_mode="PRO",
        embed_textures=True,
        texture_compression="NONE",
        apply_modifiers=True,
        export_animations=True,
    )
    opts.update(kw)

    # 引数名の打ち間違いは Blender の例外だと分かりにくいので、
    # ここでオペレーターの持ち物と突き合わせておく。
    known = set(EXPORT_OT_yui_ybr_props())
    unknown = [k for k in opts if k not in known and k != "filepath"]
    assert not unknown, "unknown export option(s): %s / available: %s" % (
        ", ".join(sorted(unknown)),
        ", ".join(sorted(known)),
    )

    ybr._warnings.clear()
    res = bpy.ops.export_scene.yui_ybr(**opts)
    assert "FINISHED" in res, "export failed: %s" % (res,)
    return load_ybr(path)


# ===========================================================================
# テスト
# ===========================================================================
def test_basic_mesh(tmp):
    group("basic mesh")
    reset_scene()
    bpy.ops.mesh.primitive_cube_add(size=2.0, location=(1.0, 2.0, 3.0))
    cube = bpy.context.object
    cube.name = "MyCube"

    sc = export(os.path.join(tmp, "cube.ybr"))

    check(sc["version"] == ybr.FORMAT_VERSION, "version")
    meshes = blocks_of(sc, "MESH")
    check(len(meshes) == 1, "MESH ブロックが 1 つ")
    if not meshes:
        return
    m = meshes[0]
    check(m["triangle_count"] == 12, "三角形 12 枚")
    pos = f32(m["positions"])
    idx = u32(m["indices"])

    # 法線 / UV が違うコーナーは頂点分割されるので、立方体の頂点は
    # 8 個 (完全共有) から 24 個 (面ごとに分割) の間になる。
    # フラットシェードの既定の立方体は 24 個。
    n = m["vertex_count"]
    check(8 <= n <= 24, "頂点数は 8〜24 (分割されるため / 実際は %d)" % n)
    check(len(pos) == n * 3, "positions の長さが vertex_count と合う")
    check(len(idx) == 12 * 3, "indices の長さ")
    check(max(idx) < n, "index が範囲内")
    check(m.get("normals") is not None, "法線がある")

    # 分割されていても、位置の種類は立方体の 8 隅ぶんしか無いはず
    corners = {
        (round(pos[i * 3], 4), round(pos[i * 3 + 1], 4), round(pos[i * 3 + 2], 4))
        for i in range(n)
    }
    check(len(corners) == 8, "位置の種類は 8 隅 (実際は %d)" % len(corners))

    # --- Z-up -> Y-up の変換 ---
    # Blender の (1, 2, 3) は raylib の (1, 3, -2)
    # ツリーの根はシーンコレクションで、オブジェクトはその子になる
    tree = sc["tree"]
    check(len(tree) == 1, "根が 1 つ (シーンコレクション)")
    node = find_node(sc, "MyCube")
    check(node is not None, "ノードが見つかる")
    if node is None:
        return
    mat = f32(node["matrix"]) if isinstance(node["matrix"], bytes) else node["matrix"]
    check(len(mat) == 16, "行列は 16 要素")
    # 列基底なので平行移動は m12..m14
    check(
        near(mat[12], 1.0) and near(mat[13], 3.0) and near(mat[14], -2.0),
        "Z-up -> Y-up の座標変換 (1,2,3) -> (1,3,-2)",
    )


def test_transparency(tmp):
    group("transparency flag")
    reset_scene()
    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.object

    opaque = bpy.data.materials.new("Opaque")
    use_nodes(opaque)
    set_opaque_blend(opaque)  # 既定のブレンド設定に依存しない
    obj.data.materials.append(opaque)

    clear = bpy.data.materials.new("Clear")
    use_nodes(clear)
    bsdf = clear.node_tree.nodes.get("Principled BSDF")
    if bsdf and "Alpha" in bsdf.inputs:
        bsdf.inputs["Alpha"].default_value = 0.35
    obj.data.materials.append(clear)

    sc = export(os.path.join(tmp, "alpha.ybr"))
    a = find_block(sc, "MATERIAL", "Opaque")
    b = find_block(sc, "MATERIAL", "Clear")
    check(a is not None and b is not None, "両方のマテリアルが出る")
    if a and b:
        check(a.get("transparent") is False, "不透明マテリアルは transparent=False")
        check(b.get("transparent") is True, "Alpha < 1 は transparent=True")


def test_colorspace(tmp):
    group("color space")
    reset_scene()
    bpy.ops.mesh.primitive_plane_add()
    obj = bpy.context.object

    mat = bpy.data.materials.new("ColorMat")
    use_nodes(mat)
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = (0.8, 0.8, 0.8, 1.0)
    obj.data.materials.append(mat)

    sc = export(os.path.join(tmp, "cs.ybr"), material_mode="SIMPLE")
    m = find_block(sc, "MATERIAL", "ColorMat")
    check(m is not None, "マテリアルが出る")
    if m:
        # リニア 0.8 は sRGB では 0.9063
        check(
            near(m["base_color"][0], 0.9063, 1e-3),
            "base_color がリニアから sRGB に変換される (0.8 -> 0.906)",
        )
        check(near(m["base_color"][3], 1.0), "アルファは変換しない")


def test_textures(tmp):
    group("textures")
    reset_scene()
    bpy.ops.mesh.primitive_plane_add()
    obj = bpy.context.object

    # 3 つのマテリアルにそれぞれ別の画像を割り当てる
    names = []
    for i in range(3):
        img = bpy.data.images.new("Tex%d" % i, width=8, height=8)
        img.generated_color = (1.0, 0.0, 0.0, 1.0)
        mat = bpy.data.materials.new("TexMat%d" % i)
        use_nodes(mat)
        node = mat.node_tree.nodes.new("ShaderNodeTexImage")
        node.image = img
        bsdf = mat.node_tree.nodes.get("Principled BSDF")
        mat.node_tree.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
        obj.data.materials.append(mat)
        names.append(img.name)

    sc = export(os.path.join(tmp, "tex.ybr"))
    tex = blocks_of(sc, "TEXTURE")
    check(len(tex) == 3, "3 枚とも TEXTURE ブロックになる (1 枚だけにならない)")
    got = sorted(b["id"] for b in tex)
    check(got == sorted(names), "画像名が一致する")
    for b in tex:
        check(b.get("embedded") is True, "%s が埋め込まれている" % b["id"])
        check(b.get("width") == 8 and b.get("height") == 8, "%s の大きさ" % b["id"])
        if b.get("compression") == "NONE":
            check(
                len(b.get("pixels") or b"") == 8 * 8 * 4,
                "%s の画素数 (RGBA8)" % b["id"],
            )


def test_node_groups(tmp):
    group("node groups")
    reset_scene()
    bpy.ops.mesh.primitive_plane_add()
    obj = bpy.context.object

    grp = bpy.data.node_groups.new("MyGroup", "ShaderNodeTree")
    gin = grp.nodes.new("NodeGroupInput")
    gout = grp.nodes.new("NodeGroupOutput")
    if hasattr(grp, "interface"):
        grp.interface.new_socket("Color", in_out="INPUT", socket_type="NodeSocketColor")
        grp.interface.new_socket(
            "Color", in_out="OUTPUT", socket_type="NodeSocketColor"
        )
    else:  # Blender 3.x
        grp.inputs.new("NodeSocketColor", "Color")
        grp.outputs.new("NodeSocketColor", "Color")
    grp.links.new(gin.outputs[0], gout.inputs[0])

    mat = bpy.data.materials.new("GroupMat")
    use_nodes(mat)
    inst = mat.node_tree.nodes.new("ShaderNodeGroup")
    inst.node_tree = grp
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    mat.node_tree.links.new(inst.outputs[0], bsdf.inputs["Base Color"])
    obj.data.materials.append(mat)

    sc = export(os.path.join(tmp, "group.ybr"))
    groups = blocks_of(sc, "NODEGROUP")
    check(len(groups) == 1, "NODEGROUP ブロックが 1 つ")
    if groups:
        g = groups[0]
        check(g["id"] == "MyGroup", "グループ名")
        check(0 < len(g["nodes"]), "中身のノードが入っている")
        check(len(g["inputs"]) == 1, "インターフェースの入力")
        check(len(g["outputs"]) == 1, "インターフェースの出力")

    m = find_block(sc, "MATERIAL", "GroupMat")
    check(m is not None, "マテリアルが出る")
    if m:
        gnodes = [n for n in m["nodes"] if n["props"].get("node_tree") == "MyGroup"]
        check(len(gnodes) == 1, "GROUP ノードがグループ名を指している")


def test_armature_animation(tmp):
    group("armature / animation")
    reset_scene()

    bpy.ops.object.armature_add(enter_editmode=True)
    arm = bpy.context.object
    arm.name = "Arm"
    eb = arm.data.edit_bones
    root = eb[0]
    root.name = "root"
    child = eb.new("child")
    child.head = root.tail
    child.tail = root.tail + Vector((0.0, 0.0, 1.0))
    child.parent = root
    bpy.ops.object.mode_set(mode="OBJECT")

    bpy.ops.mesh.primitive_cube_add()
    cube = bpy.context.object
    mod = cube.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    for b in ("root", "child"):
        vg = cube.vertex_groups.new(name=b)
        vg.add(list(range(len(cube.data.vertices))), 0.5, "REPLACE")

    # 簡単なアニメーション
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="POSE")
    pb = arm.pose.bones["child"]
    bpy.context.scene.frame_set(1)
    pb.location = (0.0, 0.0, 0.0)
    pb.keyframe_insert("location", frame=1)
    pb.location = (0.0, 0.0, 1.0)
    pb.keyframe_insert("location", frame=10)
    bpy.ops.object.mode_set(mode="OBJECT")
    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_end = 10

    # アドオンは NLA ストリップになっているアクションだけを書き出す
    # (アクティブなアクションのままだと警告を出して飛ばす)
    push_action_to_nla(arm)

    sc = export(os.path.join(tmp, "anim.ybr"))

    arms = blocks_of(sc, "ARMATURE")
    check(len(arms) == 1, "ARMATURE ブロックが 1 つ")
    if arms:
        bones = arms[0]["bones"]
        check(len(bones) == 2, "ボーンが 2 本")
        names = [b["name"] for b in bones]
        check("root" in names and "child" in names, "ボーン名")
        # 親は自分より前に来る (前から順に計算できる)
        for i, b in enumerate(bones):
            check(b["parent"] < i, "%s の親が自分より前にある" % b["name"])

    meshes = blocks_of(sc, "MESH")
    if meshes:
        skin = meshes[0].get("skin")
        check(skin is not None, "スキンが出る")
        if skin:
            n = meshes[0]["vertex_count"]
            joints = u16(skin["joints"])
            weights = f32(skin["weights"])
            check(len(joints) == n * 4, "joints の長さ")
            check(len(weights) == n * 4, "weights の長さ")
            check(near(sum(weights[0:4]), 1.0, 1e-3), "ウェイトの合計が 1")

    anims = blocks_of(sc, "ANIMATION")
    check(len(anims) == 1, "ANIMATION ブロックが 1 つ")
    if anims:
        a = anims[0]
        check(a["frame_count"] == 10, "フレーム数 (1..10)")
        check(0 < len(a["tracks"]), "トラックがある")
        tr = a["tracks"][0]
        check(len(f32(tr["transforms"])) == tr["frame_count"] * 16, "行列の長さ")
        check(len(u32(tr["frames"])) == tr["frame_count"], "フレーム番号の長さ")


def _add_armature(name, bone_names, location):
    """指定の場所に、直列につないだボーンを持つアーマチュアを作る"""
    bpy.ops.object.armature_add(enter_editmode=True, location=location)
    arm = bpy.context.object
    arm.name = name
    eb = arm.data.edit_bones
    prev = eb[0]
    prev.name = bone_names[0]
    for nm in bone_names[1:]:
        b = eb.new(nm)
        b.head = prev.tail
        b.tail = prev.tail + Vector((0.0, 0.0, 1.0))
        b.parent = prev
        prev = b
    bpy.ops.object.mode_set(mode="OBJECT")
    return arm


def test_multi_armature_merge(tmp):
    group("multi armature / merge")
    reset_scene()

    # ボーン名がわざと衝突する 2 つのアーマチュアを別々の場所に置く
    arm_a = _add_armature("ArmA", ["root", "child"], (0.0, 0.0, 0.0))
    arm_b = _add_armature("ArmB", ["root", "tip"], (5.0, 0.0, 0.0))

    bpy.ops.mesh.primitive_cube_add()
    cube = bpy.context.object
    for arm in (arm_a, arm_b):
        mod = cube.modifiers.new("Armature_%s" % arm.name, "ARMATURE")
        mod.object = arm
    for b in ("root", "child", "tip"):
        vg = cube.vertex_groups.new(name=b)
        vg.add(list(range(len(cube.data.vertices))), 0.5, "REPLACE")

    sc = export(os.path.join(tmp, "merge.ybr"))

    arms = blocks_of(sc, "ARMATURE")
    check(len(arms) == 1, "アーマチュアが 2 つでも ARMATURE ブロックは 1 つ")
    if not arms:
        return

    bones = arms[0]["bones"]
    names = [b["name"] for b in bones]
    check(len(bones) == 4, "両方のボーンが入る")
    check(arms[0]["id"] == arm_a.data.name, "id は名前順で先頭のアーマチュア")

    # 衝突した "root" だけが改名される
    check(names.count("root") == 1, "衝突した名前は 1 つだけ残る")
    check("ArmB/root" in names, "衝突した側が改名される")
    check("child" in names and "tip" in names, "衝突していない名前はそのまま")

    # 親は必ず自分より前 (前から順に計算できる)
    for i, b in enumerate(bones):
        check(b["parent"] < i, "%s の親が自分より前にある" % b["name"])

    # 合成された側はルートになり、基準アーマチュアの空間へ移されている
    rb = bones[names.index("ArmB/root")]
    check(rb["parent"] == -1, "合成された側のルートはルートのまま")
    rest = rb["rest"]
    check(near(rest[12], 5.0, 1e-4), "基準アーマチュアの空間へ移されている")

    # スキンは合成後の index を指す
    meshes = blocks_of(sc, "MESH")
    if meshes:
        skin = meshes[0].get("skin")
        check(skin is not None, "両方のアーマチュアに従うメッシュのスキンが出る")
        check(
            meshes[0]["armature_data"] == arms[0]["id"],
            "メッシュは合成後のアーマチュアを指す",
        )
        if skin:
            joints = u16(skin["joints"])
            check(max(joints) < len(bones), "joint index が合成後の範囲に収まる")

    # シーンツリー側 : ARMATURE ノードは基準だけが data を持つ
    found = {}

    def walk(node):
        if node.get("type") == 3:  # ARMATURE
            found[node["name"]] = node.get("data")
        for c in node.get("children", ()):
            walk(c)

    for root in sc["tree"]:
        walk(root)
    check(found.get("ArmA") == arms[0]["id"], "基準ノードは合成後の id を指す")
    check(found.get("ArmB") is None, "合成された側のノードは data を持たない")


def test_armature_by_modifier_only(tmp):
    group("armature: modifier decides")
    reset_scene()

    # モディファイアを持たず、アーマチュアにペアレントしただけのメッシュ。
    # Blender でも変形しないので、スキンの対象にはしない。
    arm = _add_armature("Arm", ["root"], (0.0, 0.0, 0.0))

    bpy.ops.mesh.primitive_cube_add()
    cube = bpy.context.object
    cube.parent = arm
    vg = cube.vertex_groups.new(name="root")
    vg.add(list(range(len(cube.data.vertices))), 1.0, "REPLACE")

    sc = export(os.path.join(tmp, "parented.ybr"))

    check(
        len(blocks_of(sc, "ARMATURE")) == 0,
        "モディファイアが無ければ ARMATURE ブロックは出ない",
    )
    meshes = blocks_of(sc, "MESH")
    if meshes:
        check(meshes[0].get("skin") is None, "スキンも出ない")
        check(meshes[0].get("armature_data") is None, "アーマチュア参照も無い")


def test_curve(tmp):
    group("curve")
    reset_scene()
    bpy.ops.curve.primitive_bezier_circle_add()
    sc = export(os.path.join(tmp, "curve.ybr"))
    curves = blocks_of(sc, "CURVE")
    check(len(curves) == 1, "CURVE ブロックが 1 つ")
    if curves:
        sp = curves[0]["splines"]
        check(len(sp) == 1, "スプラインが 1 本")
        check(sp[0]["type"] == 1, "BEZIER (コード 1)")
        check(sp[0]["cyclic"] is True, "閉じている")
        check(len(f32(sp[0]["points"])) == sp[0]["point_count"] * 3, "点の数")
        check(sp[0].get("handles_left") is not None, "ハンドルが出る")


def test_visibility_filter(tmp):
    group("visibility filter")
    reset_scene()
    bpy.ops.mesh.primitive_cube_add()
    shown = bpy.context.object
    shown.name = "Shown"
    bpy.ops.mesh.primitive_cube_add(location=(3, 0, 0))
    hidden = bpy.context.object
    hidden.name = "HiddenInRender"
    hidden.hide_render = True

    # 既定は「レンダリング無効を除外 / ビューポート非表示は除外しない」
    sc = export(os.path.join(tmp, "vis.ybr"))
    names = node_names(sc)
    check("Shown" in names, "表示されているものは出る")
    check("HiddenInRender" not in names, "レンダリング無効は既定で除外される")

    sc = export(os.path.join(tmp, "vis2.ybr"), skip_hidden_render=False)
    names = node_names(sc)
    check("HiddenInRender" in names, "skip_hidden_render=False なら出る")


def test_roundtrip_stability(tmp):
    group("stability")
    reset_scene()
    bpy.ops.mesh.primitive_uv_sphere_add()
    p1 = os.path.join(tmp, "a.ybr")
    p2 = os.path.join(tmp, "b.ybr")
    export(p1)
    export(p2)
    with open(p1, "rb") as f:
        a = f.read()
    with open(p2, "rb") as f:
        b = f.read()
    check(a == b, "同じシーンを 2 回書き出すと同じバイト列になる")


def test_material_none(tmp):
    group("material mode: None")
    reset_scene()
    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.object
    for i in range(2):
        mat = bpy.data.materials.new("Mat%d" % i)
        use_nodes(mat)
        obj.data.materials.append(mat)

    sc = export(os.path.join(tmp, "nomat.ybr"), material_mode="NONE")
    mats = blocks_of(sc, "MATERIAL")
    check(len(mats) == 1, "MATERIAL ブロックはダミー 1 つだけ")
    if mats:
        check(mats[0]["id"] == ybr.YBR_DUMMY_MATERIAL, "ダミーの名前")
        check(mats[0]["mode"] == "SIMPLE", "SIMPLE として出る")
        check(mats[0]["transparent"] is False, "不透明")
    check(len(blocks_of(sc, "TEXTURE")) == 0, "テクスチャは出ない")
    check(len(blocks_of(sc, "NODEGROUP")) == 0, "ノードグループも出ない")

    meshes = blocks_of(sc, "MESH")
    check(len(meshes) == 1, "メッシュは出る")
    if meshes:
        m = meshes[0]
        check(m["materials"] == [ybr.YBR_DUMMY_MATERIAL], "ダミーだけを参照する")
        mi = u32(m.get("material_indices") or b"")
        check(len(mi) == m["triangle_count"], "material_indices の長さ")
        check(all(v == 0 for v in mi), "全三角形がマテリアル 0 を向く")


# ===========================================================================
def test_osl_rejected(tmp):
    group("OSL script node is rejected")
    reset_scene()
    bpy.ops.mesh.primitive_cube_add()
    mat = bpy.data.materials.new("WithOSL")
    use_nodes(mat)
    bpy.context.object.data.materials.append(mat)

    try:
        mat.node_tree.nodes.new("ShaderNodeScript")
    except RuntimeError:
        # OSL が無効なビルドでは作れないのでスキップ
        print("  SKIP (ShaderNodeScript is not available in this build)")
        return

    path = os.path.join(tmp, "osl.ybr")
    try:
        export(path)
        check(False, "OSL があると書き出しに失敗する")
    except (AssertionError, RuntimeError, ybr.YbrExportError) as e:
        # ERROR を report すると bpy.ops は RuntimeError を投げる。
        # CANCELLED だけ返った場合は export() 内の assert に引っかかる。
        check(
            "OSL" in str(e) or "CANCELLED" in str(e), "OSL があると書き出しに失敗する"
        )

    check(not os.path.exists(path), "失敗したらファイルを残さない")


def test_alpha_one_is_opaque(tmp):
    group("alpha = 1.0 is not transparent")
    reset_scene()

    def make(name, build):
        mat = bpy.data.materials.new(name)
        use_nodes(mat)
        build(mat.node_tree)
        return mat

    def principled(nt):
        for n in nt.nodes:
            if n.type == "BSDF_PRINCIPLED":
                return n
        return None

    # 1. Alpha を触らない -> 不透明
    plain = make("Plain", lambda nt: None)

    # 2. Alpha に 1.0 の Value をつなぐ -> 不透明であってほしい
    def link_one(nt):
        v = nt.nodes.new("ShaderNodeValue")
        v.outputs[0].default_value = 1.0
        nt.links.new(v.outputs[0], principled(nt).inputs["Alpha"])

    linked_one = make("LinkedOne", link_one)

    # 3. Alpha に 0.5 の Value -> 半透明
    def link_half(nt):
        v = nt.nodes.new("ShaderNodeValue")
        v.outputs[0].default_value = 0.5
        nt.links.new(v.outputs[0], principled(nt).inputs["Alpha"])

    linked_half = make("LinkedHalf", link_half)

    # 4. Alpha の既定値が 0.5 -> 半透明
    def set_half(nt):
        principled(nt).inputs["Alpha"].default_value = 0.5

    half = make("Half", set_half)

    # 5. たどれないノード (Noise) -> 半透明に倒す
    def link_noise(nt):
        nz = nt.nodes.new("ShaderNodeTexNoise")
        nt.links.new(nz.outputs["Fac"], principled(nt).inputs["Alpha"])

    noise = make("Noise", link_noise)

    for m in (plain, linked_one, linked_half, half, noise):
        set_opaque_blend(m)

    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.object
    for m in (plain, linked_one, linked_half, half, noise):
        obj.data.materials.append(m)

    sc = export(os.path.join(tmp, "alpha.ybr"))
    mats = {b["id"]: b for b in blocks_of(sc, "MATERIAL")}

    check(mats["Plain"]["transparent"] is False, "Alpha を触らなければ不透明")
    check(
        mats["LinkedOne"]["transparent"] is False,
        "1.0 をつないだだけなら不透明 (ここが今回の変更点)",
    )
    check(mats["LinkedHalf"]["transparent"] is True, "0.5 をつなげば半透明")
    check(mats["Half"]["transparent"] is True, "既定値が 0.5 なら半透明")
    check(mats["Noise"]["transparent"] is True, "たどれないものは半透明に倒す")


def main():
    tests = [
        test_basic_mesh,
        test_transparency,
        test_material_none,
        test_colorspace,
        test_textures,
        test_node_groups,
        test_armature_animation,
        test_multi_armature_merge,
        test_armature_by_modifier_only,
        test_curve,
        test_visibility_filter,
        test_roundtrip_stability,
        test_osl_rejected,
        test_alpha_one_is_opaque,
    ]

    with tempfile.TemporaryDirectory(prefix="ybr_test_") as tmp:
        for t in tests:
            try:
                t(tmp)
            except Exception:
                global _failed
                _failed += 1
                print("  FAIL [%s] exception:" % t.__name__)
                traceback.print_exc()

    print("\n==================================")
    print("passed : %d" % _passed)
    print("failed : %d" % _failed)
    print("==================================")
    return 1 if _failed else 0


if __name__ == "__main__":
    try:
        code = main()
    finally:
        try:
            ybr.unregister()
        except Exception:
            pass
    # --background でも終了コードを返せるようにする
    sys.exit(code)
