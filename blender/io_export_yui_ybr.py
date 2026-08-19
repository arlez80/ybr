# SPDX-License-Identifier: MIT
"""
Yui Blender to Raylib Scene Exporter  (.ybr)

Blender のシーンを raylib (OpenGL / Y-up) 向けの CBOR ファイルとして書き出す。
"""

import array
import math
import os
import struct
import tempfile

try:
    import numpy as np
except ImportError:
    np = None

import bpy
from bpy.props import (
    BoolProperty,
    EnumProperty,
    IntProperty,
    PointerProperty,
    StringProperty,
)
from bpy_extras.io_utils import ExportHelper
from mathutils import Matrix

bl_info = {
    "name": "Yui Blender to Raylib Scene Exporter",
    "author": "yui",
    "version": (0, 2, 0),
    "blender": (4, 5, 0),
    "location": "File > Export > Yui Blender to Raylib (.ybr)",
    "description": "Export scene to CBOR based .ybr for raylib",
    "category": "Import-Export",
}

FORMAT_NAME = "Yui Blender to Raylib"
MAGIC = "YUI"
FORMAT_VERSION = 1


# ---------------------------------------------------------------------------
# 警告収集
# ---------------------------------------------------------------------------
_warnings = []


def warn(msg):
    _warnings.append(msg)
    print("[YBR] WARNING: %s" % msg)


# ---------------------------------------------------------------------------
# 最小 CBOR エンコーダ (RFC 8949 / definite-length only)
# ---------------------------------------------------------------------------
class CBORWriter:
    __slots__ = ("out",)

    def __init__(self):
        self.out = bytearray()

    def _head(self, major, n):
        out = self.out
        mt = major << 5
        if n < 24:
            out.append(mt | n)
        elif n <= 0xFF:
            out.append(mt | 24)
            out.append(n)
        elif n <= 0xFFFF:
            out.append(mt | 25)
            out.extend(struct.pack(">H", n))
        elif n <= 0xFFFFFFFF:
            out.append(mt | 26)
            out.extend(struct.pack(">I", n))
        else:
            out.append(mt | 27)
            out.extend(struct.pack(">Q", n))

    def write(self, v):
        out = self.out
        if v is None:
            out.append(0xF6)
        elif v is True:
            out.append(0xF5)
        elif v is False:
            out.append(0xF4)
        elif isinstance(v, int):
            if 0 <= v:
                self._head(0, v)
            else:
                self._head(1, -1 - v)
        elif isinstance(v, float):
            out.append(0xFB)
            out.extend(struct.pack(">d", v))
        elif isinstance(v, (bytes, bytearray, memoryview)):
            b = bytes(v)
            self._head(2, len(b))
            out.extend(b)
        elif isinstance(v, str):
            b = v.encode("utf-8")
            self._head(3, len(b))
            out.extend(b)
        elif isinstance(v, (list, tuple)):
            self._head(4, len(v))
            for x in v:
                self.write(x)
        elif isinstance(v, dict):
            self._head(5, len(v))
            for k, x in v.items():
                self.write(k)
                self.write(x)
        else:
            raise TypeError("CBOR: unsupported type %r" % type(v))

    def getvalue(self):
        return bytes(self.out)


# ---------------------------------------------------------------------------
# 軸変換 : Blender(Z-up, RH) -> OpenGL/raylib(Y-up, RH)
#   (x, y, z)_blender -> (x, z, -y)_gl
# ---------------------------------------------------------------------------
AXIS_CONV = Matrix(
    (
        (1.0, 0.0, 0.0, 0.0),
        (0.0, 0.0, 1.0, 0.0),
        (0.0, -1.0, 0.0, 0.0),
        (0.0, 0.0, 0.0, 1.0),
    )
)
AXIS_CONV_INV = AXIS_CONV.inverted()


def conv_matrix(m):
    """行列を Blender 空間から GL 空間へ (基底変換)"""
    return AXIS_CONV @ m @ AXIS_CONV_INV


def mat_to_column_major(m):
    """mathutils.Matrix -> float 16個 (column-major / OpenGL 準拠)"""
    return [float(m[r][c]) for c in range(4) for r in range(4)]


def conv_vec3(x, y, z):
    return (x, z, -y)


def _f32(values):
    return struct.pack("<%df" % len(values), *values)


def _u32(values):
    return struct.pack("<%dI" % len(values), *values)


def _u16(values):
    return struct.pack("<%dH" % len(values), *values)


def _u8(values):
    return bytes(values)


def _conv_flat3(arr):
    out = []
    for i in range(0, len(arr), 3):
        x, y, z = conv_vec3(arr[i], arr[i + 1], arr[i + 2])
        out.extend((x, y, z))
    return out


# ---------------------------------------------------------------------------
# 定数コード (文字列は出力せず、すべて整数で書き出す)
# ---------------------------------------------------------------------------
NODE_TYPE_UNKNOWN = 0
NODE_TYPE_OBJECT = 7  # 個別のコードを持たないオブジェクト
NODE_TYPE_COLLECTION = 8  # コレクション
_NODE_TYPE_CODE = {
    "EMPTY": 1,
    "MESH": 2,
    "ARMATURE": 3,
    "CURVE": 4,
    "SURFACE": 4,
    "FONT": 4,
    "LIGHT": 5,
    "CAMERA": 6,
}
_SPLINE_TYPE_CODE = {"POLY": 0, "BEZIER": 1, "NURBS": 2}
_LIGHT_TYPE_CODE = {"POINT": 0, "SUN": 1, "SPOT": 2, "AREA": 3}

_TEX_EXTENSION_CODE = {"REPEAT": 0, "EXTEND": 1, "CLIP": 2, "MIRROR": 3}
_CAMERA_TYPE_CODE = {"PERSP": 0, "ORTHO": 1, "PANO": 2}
_SENSOR_FIT_CODE = {"AUTO": 0, "HORIZONTAL": 1, "VERTICAL": 2}

SHADER_NODE_UNKNOWN = 0
SHADER_SOCKET_UNKNOWN = 0

_SHADER_NODE_TYPE_CODE = {
    "OUTPUT_MATERIAL": 1,
    "OUTPUT_WORLD": 2,
    "OUTPUT_LIGHT": 3,
    "OUTPUT_AOV": 4,
    "OUTPUT_LINESTYLE": 5,
    "BSDF_PRINCIPLED": 10,
    "BSDF_DIFFUSE": 11,
    "BSDF_GLOSSY": 12,
    "BSDF_GLASS": 13,
    "BSDF_REFRACTION": 14,
    "BSDF_TRANSLUCENT": 15,
    "BSDF_TRANSPARENT": 16,
    "BSDF_VELVET": 17,
    "BSDF_SHEEN": 17,
    "BSDF_TOON": 18,
    "BSDF_HAIR": 19,
    "BSDF_HAIR_PRINCIPLED": 20,
    "BSDF_ANISOTROPIC": 21,
    "SUBSURFACE_SCATTERING": 22,
    "EMISSION": 23,
    "BACKGROUND": 24,
    "HOLDOUT": 25,
    "ADD_SHADER": 26,
    "MIX_SHADER": 27,
    "VOLUME_ABSORPTION": 28,
    "VOLUME_SCATTER": 29,
    "PRINCIPLED_VOLUME": 30,
    "SHADERTORGB": 31,
    "LIGHT_FALLOFF": 32,
    "TEX_IMAGE": 40,
    "TEX_ENVIRONMENT": 41,
    "TEX_NOISE": 42,
    "TEX_CHECKER": 43,
    "TEX_BRICK": 44,
    "TEX_GRADIENT": 45,
    "TEX_MAGIC": 46,
    "TEX_VORONOI": 47,
    "TEX_WAVE": 48,
    "TEX_WHITE_NOISE": 49,
    "TEX_SKY": 50,
    "TEX_IES": 51,
    "TEX_POINTDENSITY": 52,
    "TEX_MUSGRAVE": 53,
    "TEX_COORD": 54,
    "UVMAP": 60,
    "ATTRIBUTE": 61,
    "NEW_GEOMETRY": 62,
    "OBJECT_INFO": 63,
    "PARTICLE_INFO": 64,
    "HAIR_INFO": 65,
    "POINT_INFO": 66,
    "VOLUME_INFO": 67,
    "CAMERA": 68,
    "LIGHT_PATH": 69,
    "FRESNEL": 70,
    "LAYER_WEIGHT": 71,
    "WIREFRAME": 72,
    "BEVEL": 73,
    "AMBIENT_OCCLUSION": 74,
    "TANGENT": 75,
    "RGB": 80,
    "VALUE": 81,
    "MIX_RGB": 82,
    "MIX": 83,
    "VALTORGB": 84,
    "RGBTOBW": 85,
    "MATH": 86,
    "VECT_MATH": 87,
    "VECT_TRANSFORM": 88,
    "SEPRGB": 89,
    "SEPARATE_COLOR": 89,
    "COMBRGB": 90,
    "COMBINE_COLOR": 90,
    "SEPHSV": 91,
    "COMBHSV": 92,
    "SEPXYZ": 93,
    "COMBXYZ": 94,
    "HUE_SAT": 95,
    "BRIGHTCONTRAST": 96,
    "GAMMA": 97,
    "INVERT": 98,
    "CURVE_RGB": 99,
    "CURVE_VEC": 100,
    "CURVE_FLOAT": 101,
    "CLAMP": 102,
    "MAP_RANGE": 103,
    "FLOAT_CURVE": 104,
    "BLACKBODY": 105,
    "WAVELENGTH": 106,
    "NORMAL": 110,
    "NORMAL_MAP": 111,
    "BUMP": 112,
    "DISPLACEMENT": 113,
    "VECTOR_DISPLACEMENT": 114,
    "MAPPING": 115,
    "VECTOR_ROTATE": 116,
    "GROUP": 120,
    "GROUP_INPUT": 121,
    "GROUP_OUTPUT": 122,
    "REROUTE": 123,
}

_SHADER_SOCKET_TYPE_CODE = {
    "VALUE": 1,
    "INT": 2,
    "BOOLEAN": 3,
    "VECTOR": 4,
    "ROTATION": 5,
    "MATRIX": 6,
    "STRING": 7,
    "RGBA": 8,
    "SHADER": 9,
    "OBJECT": 10,
    "IMAGE": 11,
    "GEOMETRY": 12,
    "COLLECTION": 13,
    "TEXTURE": 14,
    "MATERIAL": 15,
    "MENU": 16,
    "CUSTOM": 17,
}


def _enum_code(table, key, what, owner):
    """未対応の値は UNKNOWN (0) を返しつつ警告する"""
    code = table.get(key)
    if code is None:
        warn("%s: unsupported %s '%s', exported as UNKNOWN" % (owner, what, key))
        return 0
    return code


# カスタムプロパティの型コード
CUSTOM_PREFIX = "ybr_"
CUSTOM_BOOL = 0
CUSTOM_INT = 1
CUSTOM_FLOAT = 2
CUSTOM_STRING = 3
CUSTOM_ARRAY = 4
_TEX_INTERPOLATION_CODE = {"Linear": 0, "Closest": 1, "Cubic": 2, "Smart": 3}

COLORSPACE_SRGB = 0
COLORSPACE_LINEAR = 1
COLORSPACE_NON_COLOR = 2

# --- カラースペースを raylib の流儀に合わせる ---------------------------------
#
# raylib はテクスチャを sRGB テクスチャとして扱わない。GL_RGBA8 でそのまま
# アップロードし、シェーダーもそのままサンプリングして、リニア -> sRGB の
# 変換をせずにフレームバッファへ書く。つまり
#
#     「テクスチャに入っているバイト列が、そのまま画面に出る値」
#
# なので、書き出す側で表示用の値 (sRGB エンコード済み) にしておく必要がある。
#
# 一方 Blender の Image.pixels は、その画像のカラースペースから
# **シーンリニアへ変換した float** を返す。したがって:
#
#   ・色として使う画像 (sRGB / Filmic sRGB / AgX / Linear / ACEScg など)
#       -> リニアで返ってくるので sRGB エンコードしてから 8bit にする。
#          格納後は sRGB なので colorspace には 0 (sRGB) を書く。
#   ・データとして使う画像 (Non-Color / Raw / Data)
#       -> 変換されずに返ってくるので、そのまま 8bit にする。
#          colorspace には 2 (Non-Color) を書く。
#
# この結果、Blender が何色空間で扱っていても .ybr に入るのは
# 「sRGB」か「Non-Color」の 2 種類だけになり、raylib 側は
# 何も考えずに LoadTextureFromImage() すればよい。
# (コード 1 = Linear は他のツールが書き出す可能性があるので enum には残す)

_DATA_COLORSPACE_KEYWORDS = ("non-color", "non color", "raw", "data")


def _resolve_colorspace(name, owner):
    """Blender のカラースペース名を (書き出すコード, sRGB エンコードするか) にする"""
    n = (name or "").strip().lower()
    for kw in _DATA_COLORSPACE_KEYWORDS:
        if kw in n:
            return COLORSPACE_NON_COLOR, False
    if n and n not in (
        "srgb",
        "linear",
        "linear rec.709",
        "filmic srgb",
        "agx base srgb",
        "rec.1886",
        "display p3",
        "linear acescg",
        "acescg",
        "aces2065-1",
        "xyz",
        "linear fbx srgb",
    ):
        # 知らない名前でも「色として使う」とみなして sRGB に寄せる。
        # raylib には表示用の値しか渡せないので、これが一番マシな推測になる。
        warn(
            "%s: unknown color space '%s', treated as a color texture "
            "(encoded to sRGB)" % (owner, name)
        )
    return COLORSPACE_SRGB, True


class YbrExportError(Exception):
    """書き出しを続けられないときに投げる (オペレーターがエラー表示する)"""


def _code(table, key, what, owner):
    """未知の値は None を返しつつ警告する"""
    if key in table:
        return table[key]
    warn("%s: unsupported %s '%s', exported as null" % (owner, what, key))
    return None


# ---------------------------------------------------------------------------
# 色変換 (linear -> sRGB, 8bit)
# ---------------------------------------------------------------------------
_SRGB_CACHE = {}


def linear_to_srgb(c):
    """シーンリニア -> sRGB (Blender の linearrgb_to_srgb() と同じ式)"""
    if c <= 0.0:
        return 0.0
    if c <= 0.0031308:
        return 12.92 * c
    if 1.0 <= c:
        return 1.0
    return 1.055 * (c ** (1.0 / 2.4)) - 0.055


def srgb_rgba(values):
    """色 (RGB(A)) をシーンリニアから sRGB へ。アルファはそのまま。

    Blender のマテリアルの色はすべてシーンリニアだが、raylib は
    「バイト列がそのまま画面に出る」前提なので、テクスチャと同じく
    表示用の値へ寄せておかないと、色が暗く濁って見える。
    """
    out = [float(x) for x in values]
    for i in range(min(3, len(out))):
        out[i] = linear_to_srgb(out[i])
    return out


def linear_to_srgb_byte(c):
    key = int(c * 4096.0)
    v = _SRGB_CACHE.get(key)
    if v is not None:
        return v
    if c <= 0.0:
        f = 0.0
    elif c <= 0.0031308:
        f = 12.92 * c
    elif 1.0 <= c:
        f = 1.0
    else:
        f = 1.055 * (c ** (1.0 / 2.4)) - 0.055
    v = max(0, min(255, int(f * 255.0 + 0.5)))
    _SRGB_CACHE[key] = v
    return v


# ---------------------------------------------------------------------------
# メッシュ
# ---------------------------------------------------------------------------
def get_corner_normals(me):
    arr = [0.0] * (len(me.loops) * 3)
    me.corner_normals.foreach_get("vector", arr)  # Blender 4.1+
    return arr


def get_color_layer(me):
    """(domain, flat float list) を返す。domain は 'POINT' / 'CORNER'。無ければ None"""
    if len(me.color_attributes) == 0:
        return None
    layer = me.color_attributes.active_color or me.color_attributes[0]
    domain = layer.domain
    count = len(me.vertices) if domain == "POINT" else len(me.loops)
    arr = [0.0] * (count * 4)
    layer.data.foreach_get("color", arr)
    return domain, arr


def get_uv_layer(me):
    if len(me.uv_layers) == 0:
        return None
    layer = me.uv_layers.active or me.uv_layers[0]
    arr = [0.0] * (len(me.loops) * 2)
    layer.data.foreach_get("uv", arr)
    return arr


MAX_INFLUENCES = 4


def build_skin_data(obj, me, out_orig, keep_raw, merged, arm_objects):
    """頂点グループを「出力頂点インデックス」基準に展開する。

    戻り値 (skin, raw_groups):
      skin       : joint は合成後 ARMATURE ブロックの bones 配列の index。
                   ウェイト上位 4 本に絞って合計 1.0 に正規化したもの。
      raw_groups : keep_raw が True のときのみ元の全ウェイトを残す

    arm_objects はこのメッシュが使っているアーマチュア (モディファイア順)。
    頂点グループ名はこの順で照合し、最初に見つかったボーンに割り当てる。
    """
    if len(obj.vertex_groups) == 0:
        return None, []

    names = [vg.name for vg in obj.vertex_groups]
    buckets = {} if keep_raw else None

    def build_raw():
        raw = []
        if not keep_raw:
            return raw
        for gi, nm in enumerate(names):
            idx, wts = buckets.get(gi, ([], []))
            raw.append(
                {
                    "name": nm,
                    "index": gi,
                    "count": len(idx),
                    "indices": _u32(idx),
                    "weights": _f32(wts),
                }
            )
        return raw

    # 頂点グループ index -> 合成後ボーン index の対応表を作る
    bone_of_group = None
    if merged is not None and arm_objects:
        bone_of_group = []
        for nm in names:
            found = -1
            for ao in arm_objects:
                bi = merged.bone_index(ao, nm)
                if 0 <= bi:
                    found = bi
                    break
            bone_of_group.append(found)

        unmatched = [nm for nm, bi in zip(names, bone_of_group) if bi < 0]
        if unmatched:
            warn(
                "mesh '%s': %d vertex group(s) have no matching bone in the "
                "armature(s) %s and were dropped from the skin data (%s)"
                % (
                    obj.name,
                    len(unmatched),
                    ", ".join("'%s'" % ao.name for ao in arm_objects),
                    ", ".join(sorted(unmatched)[:5]),
                )
            )

    joints, weights = [], []
    clipped = 0

    for out_i, orig_vi in enumerate(out_orig):
        entries = []
        for g in me.vertices[orig_vi].groups:
            w = g.weight
            if keep_raw:
                b = buckets.get(g.group)
                if b is None:
                    b = buckets[g.group] = ([], [])
                b[0].append(out_i)
                b[1].append(w)
            if bone_of_group is None or w <= 0.0 or len(names) <= g.group:
                continue
            bone_index = bone_of_group[g.group]
            if 0 <= bone_index:
                entries.append((w, bone_index))

        if bone_of_group is None:
            continue

        if MAX_INFLUENCES < len(entries):
            clipped += 1
            entries.sort(reverse=True)
            del entries[MAX_INFLUENCES:]

        total = 0.0
        for w, _ in entries:
            total += w

        for i in range(MAX_INFLUENCES):
            if i < len(entries) and 0.0 < total:
                joints.append(entries[i][1])
                weights.append(entries[i][0] / total)
            else:
                joints.append(0)
                weights.append(0.0)

    if bone_of_group is None:
        # アーマチュアが無いので skin としては解決できない
        return None, build_raw()

    if clipped:
        warn(
            "mesh '%s': %d vertices had more than %d bone influences, "
            "extra weights were dropped and the rest renormalized"
            % (obj.name, clipped, MAX_INFLUENCES)
        )

    skin = {
        "influences": MAX_INFLUENCES,
        "joints": _u16(joints),  # ARMATURE の bones 配列の index
        "weights": _f32(weights),  # 4 * vertex_count / 合計 1.0
    }
    return skin, build_raw()


def mesh_armature_objects(obj):
    """メッシュのアーマチュアモディファイアが指しているアーマチュアを順に返す。

    「使っているアーマチュア」の判定はモディファイアだけを見る。
    ペアレントしただけ (モディファイアが無い) のメッシュは Blender でも
    変形しないので、スキンの対象にはしない。

    1 つのメッシュに複数のアーマチュアモディファイアが載っていることもある。
    その場合はモディファイアの並び順のまま全部返す。
    """
    out = []
    for m in getattr(obj, "modifiers", []):
        if m.type != "ARMATURE":
            continue
        ao = m.object
        if ao is None or ao.type != "ARMATURE" or ao.data is None:
            continue
        if ao not in out:
            out.append(ao)
    return out


def get_tangents(me):
    """ループごとの接線と従法線の向きを返す。UV が無ければ (None, None)。

    法線マップを正しく解くには接線が要る。Blender が計算してくれるので
    calc_tangents() を呼ぶだけだが、三角形化されている必要がある。
    """
    if not me.uv_layers:
        return None, None
    try:
        me.calc_tangents()
    except Exception as e:
        warn("mesh: could not compute tangents (%s)" % e)
        return None, None

    n = len(me.loops)
    tan = [0.0] * (n * 3)
    sign = [0.0] * n
    me.loops.foreach_get("tangent", tan)
    me.loops.foreach_get("bitangent_sign", sign)
    return tan, sign


def build_mesh_block(data_id, obj, me, materials, keep_raw_groups, deform_only, merged):
    me.calc_loop_triangles()

    # このメッシュが使っているアーマチュア (モディファイア順)。
    # 出力先は合成後の唯一のアーマチュアなので、参照は常にその 1 つ。
    arm_objects = mesh_armature_objects(obj) if merged is not None else []
    arm_objects = [ao for ao in arm_objects if ao in merged.objects] if merged else []
    use_arm = bool(arm_objects)
    arm_obj = merged.primary.name if use_arm else None
    arm_data = merged.data_id if use_arm else None
    tri_count = len(me.loop_triangles)

    block = {
        "kind": "MESH",
        "id": data_id,
        "vertex_count": 0,
        "triangle_count": tri_count,
        "positions": b"",
        "indices": b"",
        "materials": materials,
        "armature": arm_obj,
        "armature_data": arm_data,
        "skin": None,
        "vertex_groups": [],
    }
    if tri_count == 0:
        return block

    # --- bulk 取得 ---
    vco = [0.0] * (len(me.vertices) * 3)
    me.vertices.foreach_get("co", vco)

    loop_vi = [0] * len(me.loops)
    me.loops.foreach_get("vertex_index", loop_vi)

    tri_loops = [0] * (tri_count * 3)
    me.loop_triangles.foreach_get("loops", tri_loops)

    tri_mat = [0] * tri_count
    me.loop_triangles.foreach_get("material_index", tri_mat)

    normals = get_corner_normals(me)
    uvs = get_uv_layer(me)
    tangents, bitangent_signs = get_tangents(me)
    col = get_color_layer(me)
    col_domain, col_arr = col if col else (None, None)

    # --- 頂点分割 & 索引化 ---
    key_map = {}
    out_pos, out_nrm, out_uv, out_col, out_idx, out_orig = [], [], [], [], [], []
    out_tan = []

    for li in tri_loops:
        vi = loop_vi[li]

        nx, ny, nz = normals[li * 3], normals[li * 3 + 1], normals[li * 3 + 2]
        if uvs is not None:
            u, v = uvs[li * 2], 1.0 - uvs[li * 2 + 1]  # V 反転 (raylib は上原点)
        else:
            u = v = 0.0
        if col_arr is not None:
            ci = (vi if col_domain == "POINT" else li) * 4
            r = linear_to_srgb_byte(col_arr[ci + 0])
            g = linear_to_srgb_byte(col_arr[ci + 1])
            b = linear_to_srgb_byte(col_arr[ci + 2])
            a = max(0, min(255, int(col_arr[ci + 3] * 255.0 + 0.5)))
        else:
            r = g = b = a = 255

        if tangents is not None:
            tx, ty, tz = (tangents[li * 3], tangents[li * 3 + 1], tangents[li * 3 + 2])
            tw = bitangent_signs[li]
        else:
            tx = ty = tz = tw = 0.0

        key = (
            vi,
            int(nx * 32767.0),
            int(ny * 32767.0),
            int(nz * 32767.0),
            int(u * 65535.0),
            int(v * 65535.0),
            r,
            g,
            b,
            a,
            int(tx * 32767.0),
            int(ty * 32767.0),
            int(tz * 32767.0),
            int(tw),
        )
        idx = key_map.get(key)
        if idx is None:
            idx = len(out_pos) // 3
            key_map[key] = idx
            px, py, pz = conv_vec3(vco[vi * 3], vco[vi * 3 + 1], vco[vi * 3 + 2])
            out_pos.extend((px, py, pz))
            gx, gy, gz = conv_vec3(nx, ny, nz)
            out_nrm.extend((gx, gy, gz))
            if tangents is not None:
                # 接線もベクトルなので Z-up -> Y-up の入れ替えが要る。
                # w (従法線の向き) は座標系を入れ替えると裏返る。
                ax, ay, az = conv_vec3(tx, ty, tz)
                out_tan.extend((ax, ay, az, -tw))
            out_uv.extend((u, v))
            out_col.extend((r, g, b, a))
            out_orig.append(vi)
        out_idx.append(idx)

    block["vertex_count"] = len(out_pos) // 3
    block["positions"] = _f32(out_pos)
    block["normals"] = _f32(out_nrm)
    block["indices"] = _u32(out_idx)
    block["material_indices"] = _u32(tri_mat)
    block["vertex_map"] = _u32(out_orig)  # 出力頂点 -> 元頂点
    skin, raw_groups = build_skin_data(
        obj,
        me,
        out_orig,
        keep_raw_groups,
        merged if use_arm else None,
        arm_objects,
    )
    block["skin"] = skin
    block["vertex_groups"] = raw_groups
    if uvs is not None:
        block["uvs"] = _f32(out_uv)
    if tangents is not None:
        block["tangents"] = _f32(out_tan)
    if col_arr is not None:
        block["colors"] = _u8(out_col)
    return block


# ---------------------------------------------------------------------------
# アーマチュア
# ---------------------------------------------------------------------------
def armature_bone_order(arm, deform_only):
    """(順序付きボーン, 名前 -> index) を返す。親は必ず子より前に来る。

    deform_only が True のとき use_deform が False のボーンは含めない。
    除外されたボーンの子は、最も近い「含まれる祖先」に付け替えられる。
    """
    ordered = []

    def included(b):
        return (not deform_only) or b.use_deform

    def rec(b):
        if included(b):
            ordered.append(b)
        for c in sorted(b.children, key=lambda x: x.name):
            rec(c)

    for b in sorted(arm.bones, key=lambda x: x.name):
        if b.parent is None:
            rec(b)

    index_of = {b.name: i for i, b in enumerate(ordered)}
    return ordered, index_of


def nearest_included_parent(bone, index_of):
    p = bone.parent
    while p is not None and p.name not in index_of:
        p = p.parent
    return p


# ---------------------------------------------------------------------------
# 合成アーマチュア
#
# .ybr に入るアーマチュアは 0 個か 1 個だけ。書き出し対象のメッシュが
# 2 つ以上のアーマチュアを使っている場合は、ここで 1 つに合成してから書く。
#
# 合成のしかた:
#   - 名前順で先頭のものを「基準」にする
#   - 基準以外のボーンは rel = 基準^-1 @ その他 を掛けて基準の空間へ移し、
#     合成後アーマチュアのルートとしてぶら下げる
#   - ボーン名が衝突したときだけ "<アーマチュア名>/<ボーン名>" に改名する
# ---------------------------------------------------------------------------
class MergedArmature(object):
    """書き出す唯一のアーマチュア"""

    def __init__(self, data_id, objects):
        self.data_id = data_id
        self.objects = objects  # 合成元 (先頭が基準)
        self.primary = objects[0]
        self.out_name = {}  # (arm名, ボーン名) -> 出力ボーン名
        self.index_of = {}  # (arm名, ボーン名) -> 出力 index
        self.local_bones = {}  # arm名 -> そのアーマチュアの出力対象ボーン名
        self.rel = {}  # arm名 -> 基準空間へ持っていく行列
        self.bones = []  # ARMATURE ブロックに入れる辞書の列

    def merged(self):
        return 1 < len(self.objects)

    def bone_out_name(self, arm_obj, bone_name):
        return self.out_name.get((arm_obj.name, bone_name))

    def bone_index(self, arm_obj, bone_name):
        return self.index_of.get((arm_obj.name, bone_name), -1)

    def included(self, arm_obj):
        return self.local_bones.get(arm_obj.name, set())

    def relative(self, arm_obj):
        return self.rel.get(arm_obj.name, Matrix())


def build_merged_armature(arm_objects, deform_only):
    """アーマチュアオブジェクトの列から MergedArmature を作る。空なら None"""
    objs = sorted(arm_objects, key=lambda o: o.name)
    if not objs:
        return None

    primary = objs[0]
    merged = MergedArmature(primary.data.name, objs)

    if 1 < len(objs):
        warn(
            "%d armatures are used by the exported meshes; they are merged into "
            "one armature '%s' (bones of the others are moved into its space): %s"
            % (len(objs), merged.data_id, ", ".join(o.name for o in objs))
        )

    try:
        base_inv = primary.matrix_world.inverted()
    except Exception:
        base_inv = Matrix()

    used_names = set()
    renamed = 0

    for ao in objs:
        arm = ao.data
        ordered, local_index = armature_bone_order(arm, deform_only)
        merged.local_bones[ao.name] = set(local_index.keys())

        rel = Matrix() if ao is primary else (base_inv @ ao.matrix_world)
        merged.rel[ao.name] = rel

        skipped = len(arm.bones) - len(ordered)
        if 0 < skipped:
            warn(
                "armature '%s': %d non-deform bone(s) were skipped"
                % (arm.name, skipped)
            )

        # 先に名前だけ決める (親を引くときに要る)
        for b in ordered:
            name = b.name
            if name in used_names:
                name = "%s/%s" % (ao.name, b.name)
                n = 1
                while name in used_names:
                    n += 1
                    name = "%s/%s.%03d" % (ao.name, b.name, n)
                renamed += 1
            used_names.add(name)
            merged.out_name[(ao.name, b.name)] = name

        for b in ordered:
            out_name = merged.out_name[(ao.name, b.name)]
            merged.index_of[(ao.name, b.name)] = len(merged.bones)

            parent = nearest_included_parent(b, local_index)
            if parent is not None:
                # 親も同じアーマチュアの中にいるので rel は打ち消し合う
                local = parent.matrix_local.inverted() @ b.matrix_local
                parent_idx = merged.index_of[(ao.name, parent.name)]
            else:
                # 合成後アーマチュアのルート。基準の空間へ移す
                local = rel @ b.matrix_local
                parent_idx = -1

            merged.bones.append(
                {
                    "name": out_name,
                    "parent": parent_idx,
                    "rest": mat_to_column_major(conv_matrix(rel @ b.matrix_local)),
                    "rest_parent": mat_to_column_major(conv_matrix(local)),
                    "length": float(b.length),
                }
            )

    if renamed:
        warn(
            "%d bone name(s) collided while merging armatures and were renamed to "
            "'<armature>/<bone>'" % renamed
        )

    return merged


def build_merged_armature_block(merged):
    return {"kind": "ARMATURE", "id": merged.data_id, "bones": list(merged.bones)}


# ---------------------------------------------------------------------------
# カーブ (スプラインのみ)
# ---------------------------------------------------------------------------
_HANDLE_CODE = {"FREE": 0, "AUTO": 1, "VECTOR": 2, "ALIGNED": 3, "AUTO_CLAMPED": 1}


def build_curve_block(data_id, cu):
    splines = []
    for sp in cu.splines:
        entry = {
            "type": _code(
                _SPLINE_TYPE_CODE, sp.type, "spline type", "curve '%s'" % cu.name
            ),
            "cyclic": bool(sp.use_cyclic_u),
            "order": int(sp.order_u),
        }

        if sp.type == "BEZIER":
            n = len(sp.bezier_points)
            co = [0.0] * (n * 3)
            hl = [0.0] * (n * 3)
            hr = [0.0] * (n * 3)
            tilt = [0.0] * n
            rad = [0.0] * n
            sp.bezier_points.foreach_get("co", co)
            sp.bezier_points.foreach_get("handle_left", hl)
            sp.bezier_points.foreach_get("handle_right", hr)
            sp.bezier_points.foreach_get("tilt", tilt)
            sp.bezier_points.foreach_get("radius", rad)
            entry["point_count"] = n
            entry["points"] = _f32(_conv_flat3(co))
            entry["handles_left"] = _f32(_conv_flat3(hl))
            entry["handles_right"] = _f32(_conv_flat3(hr))
            entry["handle_types_left"] = _u8(
                [_HANDLE_CODE.get(p.handle_left_type, 0) for p in sp.bezier_points]
            )
            entry["handle_types_right"] = _u8(
                [_HANDLE_CODE.get(p.handle_right_type, 0) for p in sp.bezier_points]
            )
            entry["tilts"] = _f32(tilt)
            entry["radii"] = _f32(rad)
        else:
            n = len(sp.points)
            co = [0.0] * (n * 4)  # x, y, z, w
            tilt = [0.0] * n
            rad = [0.0] * n
            sp.points.foreach_get("co", co)
            sp.points.foreach_get("tilt", tilt)
            sp.points.foreach_get("radius", rad)
            pts, wgt = [], []
            for i in range(n):
                x, y, z = conv_vec3(co[i * 4], co[i * 4 + 1], co[i * 4 + 2])
                pts.extend((x, y, z))
                wgt.append(co[i * 4 + 3])
            entry["point_count"] = n
            entry["points"] = _f32(pts)
            entry["weights"] = _f32(wgt)
            entry["tilts"] = _f32(tilt)
            entry["radii"] = _f32(rad)

        splines.append(entry)

    return {
        "kind": "CURVE",
        "id": data_id,
        "is_3d": (getattr(cu, "dimensions", "3D") == "3D"),
        "splines": splines,
    }


# ---------------------------------------------------------------------------
# ライト
# ---------------------------------------------------------------------------
def build_light_block(data_id, li):
    block = {
        "kind": "LIGHT",
        "id": data_id,
        "type": _code(_LIGHT_TYPE_CODE, li.type, "light type", "light '%s'" % li.name),
        "color": [float(c) for c in li.color],
        "energy": float(li.energy),
        "use_shadow": bool(getattr(li, "use_shadow", True)),
        "radius": float(getattr(li, "shadow_soft_size", 0.0)),
    }
    if li.type == "SUN":
        block["angle"] = float(getattr(li, "angle", 0.0))
    elif li.type == "SPOT":
        block["spot_size"] = float(li.spot_size)  # ラジアン (全角)
        block["spot_blend"] = float(li.spot_blend)
    elif li.type == "AREA":
        block["shape"] = li.shape  # SQUARE / RECTANGLE / DISK / ELLIPSE
        block["size"] = float(li.size)
        block["size_y"] = float(getattr(li, "size_y", li.size))

    if getattr(li, "use_custom_distance", False):
        block["cutoff_distance"] = float(li.cutoff_distance)
    return block


# ---------------------------------------------------------------------------
# カメラ
# ---------------------------------------------------------------------------
def build_camera_block(data_id, cam):
    owner = "camera '%s'" % cam.name
    block = {
        "kind": "CAMERA",
        "id": data_id,
        "type": _code(_CAMERA_TYPE_CODE, cam.type, "camera type", owner),
        "lens": float(cam.lens),  # mm
        "sensor_width": float(cam.sensor_width),  # mm
        "sensor_height": float(cam.sensor_height),  # mm
        "sensor_fit": _code(_SENSOR_FIT_CODE, cam.sensor_fit, "sensor fit", owner),
        "fov_x": float(cam.angle_x),  # ラジアン
        "fov_y": float(cam.angle_y),  # ラジアン
        "clip_start": float(cam.clip_start),
        "clip_end": float(cam.clip_end),
        "ortho_scale": float(cam.ortho_scale),
        "shift_x": float(cam.shift_x),
        "shift_y": float(cam.shift_y),
    }
    return block


# ---------------------------------------------------------------------------
# カスタムプロパティ ("ybr_" で始まるものだけ / プレフィックスは外して格納)
# ---------------------------------------------------------------------------
def build_custom_properties(obj):
    props = []
    try:
        keys = list(obj.keys())
    except Exception:
        return props

    for key in sorted(keys):
        if not key.startswith(CUSTOM_PREFIX):
            continue
        name = key[len(CUSTOM_PREFIX) :]
        if not name:
            warn(
                "object '%s': custom property '%s' has an empty name after "
                "removing the prefix, skipped" % (obj.name, key)
            )
            continue
        try:
            value = obj[key]
        except Exception:
            continue

        if isinstance(value, bool):
            props.append({"key": name, "type": CUSTOM_BOOL, "value": bool(value)})
        elif isinstance(value, int):
            props.append({"key": name, "type": CUSTOM_INT, "value": int(value)})
        elif isinstance(value, float):
            props.append({"key": name, "type": CUSTOM_FLOAT, "value": float(value)})
        elif isinstance(value, str):
            props.append({"key": name, "type": CUSTOM_STRING, "value": value})
        elif hasattr(value, "__len__"):
            try:
                arr = [float(x) for x in value]
            except (TypeError, ValueError):
                warn(
                    "object '%s': custom property '%s' has an unsupported "
                    "element type, skipped" % (obj.name, key)
                )
                continue
            props.append(
                {
                    "key": name,
                    "type": CUSTOM_ARRAY,
                    "count": len(arr),
                    "value": _f32(arr),
                }
            )
        else:
            warn(
                "object '%s': custom property '%s' has an unsupported type "
                "(%s), skipped" % (obj.name, key, type(value).__name__)
            )
    return props


# ---------------------------------------------------------------------------
# マテリアル
# ---------------------------------------------------------------------------
def _resolve_link(socket):
    """Reroute を辿って実際のソースノードとソケットを返す"""
    if not socket.is_linked:
        return None, None
    link = socket.links[0]
    node, out = link.from_node, link.from_socket
    guard = 0
    while node.type == "REROUTE" and guard < 64:
        guard += 1
        if not node.inputs[0].is_linked:
            return None, None
        link = node.inputs[0].links[0]
        node, out = link.from_node, link.from_socket
    return node, out


def _socket_default(sock):
    try:
        dv = sock.default_value
    except AttributeError:
        return None
    if hasattr(dv, "__len__"):
        # 色ソケットはシーンリニアなので、表示用 (sRGB) に寄せる。
        # ベクトルや行列は色ではないので触らない。
        if getattr(sock, "type", "") == "RGBA":
            return srgb_rgba(dv)
        return [float(x) for x in dv]
    if isinstance(dv, bool):
        return bool(dv)
    if isinstance(dv, (int, float)):
        return float(dv)
    return None


def _texture_info(node):
    img = getattr(node, "image", None)
    if img is None:
        return None
    try:
        path = bpy.path.abspath(img.filepath) if img.filepath else ""
    except Exception:
        path = ""
    owner = "texture '%s'" % img.name
    return {
        "image": img.name,
        "filepath": path,
        # 格納されるバイト列に合わせたコードを書く (raylib 基準)
        "colorspace": _resolve_colorspace(
            getattr(img.colorspace_settings, "name", ""), owner
        )[0],
        "extension": _code(
            _TEX_EXTENSION_CODE,
            getattr(node, "extension", "REPEAT"),
            "extension",
            owner,
        ),
        "interpolation": _code(
            _TEX_INTERPOLATION_CODE,
            getattr(node, "interpolation", "Linear"),
            "interpolation",
            owner,
        ),
    }


def _read_channel(mat_name, node, names):
    """(value, texture) を返す。読めない場合は警告を出す"""
    sock = None
    for nm in names:
        s = node.inputs.get(nm)
        if s is not None:
            sock = s
            break
    if sock is None:
        warn(
            "material '%s': input %s not found on node '%s'"
            % (mat_name, "/".join(names), node.name)
        )
        return None, None

    value = _socket_default(sock)
    if not sock.is_linked:
        return value, None

    src, out = _resolve_link(sock)
    if src is None:
        warn(
            "material '%s': input '%s' link could not be resolved"
            % (mat_name, sock.name)
        )
        return value, None

    if src.type == "TEX_IMAGE":
        tex = _texture_info(src)
        if tex is None:
            warn(
                "material '%s': image texture node '%s' has no image"
                % (mat_name, src.name)
            )
        return value, tex

    if src.type == "RGB":
        return srgb_rgba(src.outputs[0].default_value), None
    if src.type == "VALUE":
        return float(src.outputs[0].default_value), None

    warn(
        "material '%s': input '%s' is driven by node type '%s' which cannot be "
        "evaluated in SIMPLE mode (use PRO mode)" % (mat_name, sock.name, src.type)
    )
    return value, None


def _find_surface_node(mat):
    nt = material_nodes(mat)
    if nt is None:
        return None
    out = None
    for n in nt.nodes:
        if n.type == "OUTPUT_MATERIAL" and n.is_active_output:
            out = n
            break
    if out is None:
        for n in nt.nodes:
            if n.type == "OUTPUT_MATERIAL":
                out = n
                break
    if out is not None:
        src, _ = _resolve_link(out.inputs["Surface"])
        if src is not None:
            return src
        warn("material '%s': Material Output has no Surface link" % mat.name)

    for n in nt.nodes:  # フォールバック : 最初の *BSDF ノード
        if n.bl_label.endswith("BSDF") or n.type.startswith("BSDF"):
            return n
    return None


_SIMPLE_CHANNELS = (
    ("base_color", ("Base Color", "Color")),
    ("specular", ("Specular IOR Level", "Specular", "Specular Tint")),
    ("metallic", ("Metallic",)),
    ("roughness", ("Roughness",)),
    ("alpha", ("Alpha",)),
)

_SIMPLE_DEFAULTS = {
    "base_color": [0.8, 0.8, 0.8, 1.0],
    "specular": 0.5,
    "metallic": 0.0,
    "roughness": 0.5,
    "alpha": 1.0,
}


# 半透明とみなすシェーダーノード
# ---------------------------------------------------------------------------
# マテリアルのノードツリー
#
#   Material.use_nodes は Blender 5.x で非推奨、6.0 で無くなる予定
#   (マテリアルは常にノードを持つようになる)。
#   5.0 より前でだけ use_nodes を見て、それ以降は node_tree の有無だけで
#   判断する。触るだけで DeprecationWarning が出るので、
#   新しい Blender では参照しないこと。
# ---------------------------------------------------------------------------
_HAS_USE_NODES = bpy.app.version < (5, 0)


def material_nodes(mat):
    """このマテリアルが使っているノードツリー。使っていなければ None。"""
    if mat is None:
        return None
    nt = getattr(mat, "node_tree", None)
    if nt is None:
        return None
    if _HAS_USE_NODES and not getattr(mat, "use_nodes", True):
        return None
    return nt


_TRANSPARENT_NODES = {
    "BSDF_TRANSPARENT",
    "BSDF_TRANSLUCENT",
    "BSDF_GLASS",
    "BSDF_REFRACTION",
    "MIX_SHADER",
    "ADD_SHADER",
    "VOLUME_ABSORPTION",
    "VOLUME_SCATTER",
    "PRINCIPLED_VOLUME",
}


# ---------------------------------------------------------------------------
# Alpha が 1.0 だと言い切れるか
#
#   Alpha にリンクがあるだけで半透明扱いにすると、
#   「1.0 を配線しているだけ」のマテリアルまで半透明になり、
#   描画順の並べ替えに乗って余計に重くなる (深度書き込みも止まる)。
#   そこで、たどれる範囲で定数畳み込みして 1.0 と分かるものは除く。
#
#   たどれないものは None を返して、これまでどおり半透明扱いにする。
# ---------------------------------------------------------------------------
_CONST_MAX_DEPTH = 16

# アルファチャンネルを持たない画像のビット深度 (Blender の image.depth)
_NO_ALPHA_DEPTHS = (8, 24, 48, 96)


def _socket_scalar(sock):
    """ソケットの既定値を float にする。

    Blender は色を float につなぐと輝度に変換するので、それに合わせる。
    """
    try:
        v = sock.default_value
    except AttributeError:
        return None
    if isinstance(v, (int, float)):
        return float(v)
    try:
        seq = list(v)
    except TypeError:
        return None
    if len(seq) >= 3:
        # linearrgb_to_grayscale と同じ係数
        return 0.2126 * seq[0] + 0.7152 * seq[1] + 0.0722 * seq[2]
    if len(seq) == 1:
        return float(seq[0])
    return None


def _math_constant(node, group_inputs, depth):
    """Math ノードを定数畳み込みする。分かる演算だけ扱う。"""
    a = _socket_constant(node.inputs[0], group_inputs, depth + 1)
    b = None
    if 1 < len(node.inputs):
        b = _socket_constant(node.inputs[1], group_inputs, depth + 1)

    op = getattr(node, "operation", "ADD")
    if a is None:
        return None
    if op in ("ADD", "SUBTRACT", "MULTIPLY", "DIVIDE", "MINIMUM", "MAXIMUM", "POWER"):
        if b is None:
            return None
        try:
            if op == "ADD":
                r = a + b
            elif op == "SUBTRACT":
                r = a - b
            elif op == "MULTIPLY":
                r = a * b
            elif op == "DIVIDE":
                r = a / b if b != 0.0 else 0.0
            elif op == "MINIMUM":
                r = min(a, b)
            elif op == "MAXIMUM":
                r = max(a, b)
            else:
                r = a**b
        except (ValueError, OverflowError, ZeroDivisionError):
            return None
    elif op == "ABSOLUTE":
        r = abs(a)
    else:
        return None

    if getattr(node, "use_clamp", False):
        r = min(1.0, max(0.0, r))
    return float(r)


def _image_alpha_is_one(node):
    """Image Texture の Alpha 出力が必ず 1.0 か。

    アルファチャンネルを持たない画像や、Alpha を無視する設定なら 1.0。
    """
    img = getattr(node, "image", None)
    if img is None:
        return False
    if str(getattr(img, "alpha_mode", "")) == "NONE":
        return True
    depth = int(getattr(img, "depth", 0) or 0)
    return depth in _NO_ALPHA_DEPTHS


def _socket_constant(sock, group_inputs=None, depth=0):
    """このソケットに入ってくる値。定数と言い切れなければ None。

    group_inputs はノードグループの中を見るときに、
    GROUP_INPUT から外側のソケットへ戻るための対応表。
    """
    if sock is None or _CONST_MAX_DEPTH < depth:
        return None
    if not sock.is_linked:
        return _socket_scalar(sock)

    links = list(getattr(sock, "links", ()))
    if len(links) != 1:
        return None
    node = links[0].from_node
    out = links[0].from_socket
    t = node.type

    if t == "REROUTE":
        return _socket_constant(node.inputs[0], group_inputs, depth + 1)
    if t in ("VALUE", "RGB"):
        return _socket_scalar(out)
    if t == "MATH":
        return _math_constant(node, group_inputs, depth)
    if t == "TEX_IMAGE":
        return 1.0 if (out.name == "Alpha" and _image_alpha_is_one(node)) else None

    if t == "GROUP_INPUT":
        # グループの中から外側の入力へ戻る
        if not group_inputs:
            return None
        outer = group_inputs.get(out.name)
        # 外側のさらに外は追えないので、そこで打ち切る
        return _socket_constant(outer, None, depth + 1)

    if t == "GROUP":
        nt = getattr(node, "node_tree", None)
        if nt is None:
            return None
        outer = {s.name: s for s in node.inputs}
        for gn in nt.nodes:
            if gn.type != "GROUP_OUTPUT":
                continue
            inner = gn.inputs.get(out.name)
            if inner is not None:
                return _socket_constant(inner, outer, depth + 1)
        return None

    return None


def _alpha_socket_is_opaque(sock):
    """Alpha ソケットが 1.0 と言い切れるか"""
    v = _socket_constant(sock)
    return v is not None and 1.0 - 1e-6 <= v


def _material_is_transparent(mat):
    """このマテリアルが半透明を含むか。

    再生側は「不透明を先に、半透明はカメラから遠い順にあとで」描く必要が
    あるので、書き出し側で判定して持たせておく。
    判定は次のいずれかに当てはまるとき。

      1. Blender のブレンド設定が不透明でない
         (4.2 以降の surface_render_method、それ以前の blend_method)
      2. Alpha が 1.0 だと言い切れない
      3. 半透明を作るノード (Transparent / Glass / Mix Shader など) がある

    2 は、リンクがあっても遡って 1.0 だと分かるもの
    (Value ノードに 1.0、アルファを持たない画像の Alpha 出力、
     定数どうしの Math など) は不透明として扱う。
    たどれないものは半透明に倒す。

    誤って True にしても「描く順が変わるだけ」で見た目は正しいので、
    迷ったら True に倒す。
    """
    # --- 1. ブレンド設定 ---
    # 4.2 で blend_method は surface_render_method に置き換わった。
    # 新しいほうがあるときに古いほうも見てはいけない
    # (互換のために残っていて、実際の設定と食い違うことがある)。
    if hasattr(mat, "surface_render_method"):
        method = str(mat.surface_render_method or "")
        if method and method.upper() != "DITHERED":
            return True
    else:
        legacy = str(getattr(mat, "blend_method", "") or "")
        if legacy and legacy.upper() not in ("OPAQUE", "CLIP"):
            return True

    nt = material_nodes(mat)
    if nt is None:
        return float(getattr(mat, "diffuse_color", (1, 1, 1, 1))[3]) < 1.0

    # --- 2, 3. ノードツリーを見る (グループの中も) ---
    seen = set()

    def walk(nt):
        if nt is None or nt.as_pointer() in seen:
            return False
        seen.add(nt.as_pointer())
        for n in nt.nodes:
            if n.type in _TRANSPARENT_NODES:
                return True
            if n.type == "GROUP" and walk(getattr(n, "node_tree", None)):
                return True
            sock = n.inputs.get("Alpha")
            if sock is not None and not _alpha_socket_is_opaque(sock):
                return True
        return False

    return walk(nt)


YBR_DUMMY_MATERIAL = "YbrDefault"


def build_material_dummy():
    """Material Mode = None のときに使う、白いだけのマテリアル 1 枚"""
    return {
        "kind": "MATERIAL",
        "id": YBR_DUMMY_MATERIAL,
        "mode": "SIMPLE",
        "render_method": "",
        "backface_culling": False,
        "transparent": False,
        "base_color": [1.0, 1.0, 1.0, 1.0],
        "specular": 0.5,
        "metallic": 0.0,
        "roughness": 0.5,
        "alpha": 1.0,
        "normal_strength": 1.0,
        "base_color_map": None,
        "specular_map": None,
        "metallic_map": None,
        "roughness_map": None,
        "alpha_map": None,
        "normal_map": None,
    }


def build_material_simple(mat):
    block = {
        "kind": "MATERIAL",
        "id": mat.name,
        "mode": "SIMPLE",
        "render_method": str(getattr(mat, "surface_render_method", "")),
        "backface_culling": bool(getattr(mat, "use_backface_culling", False)),
        "transparent": _material_is_transparent(mat),
    }

    nt = material_nodes(mat)
    if nt is None:
        warn(
            "material '%s': node tree not used, falling back to viewport values"
            % mat.name
        )
        block["base_color"] = srgb_rgba(mat.diffuse_color)
        block["specular"] = float(mat.specular_intensity)
        block["metallic"] = float(mat.metallic)
        block["roughness"] = float(mat.roughness)
        block["alpha"] = float(mat.diffuse_color[3])
        for key, _ in _SIMPLE_CHANNELS:
            block[key + "_map"] = None
        return block

    node = _find_surface_node(mat)
    if node is None:
        warn("material '%s': no BSDF node found" % mat.name)
        for key, _ in _SIMPLE_CHANNELS:
            block[key] = _SIMPLE_DEFAULTS[key]
            block[key + "_map"] = None
        return block

    for key, names in _SIMPLE_CHANNELS:
        value, tex = _read_channel(mat.name, node, names)
        if value is None:
            value = _SIMPLE_DEFAULTS[key]
        if key == "base_color" and isinstance(value, list) and len(value) == 3:
            value = value + [1.0]
        if tex is not None:
            # ソケットにリンクがあるとき、Blender は既定値を使わない。
            # 再生側は「値 x テクスチャ」で掛けるので、中立値にしておかないと
            # テクスチャが余計に暗く (濁って) なる。
            value = [1.0, 1.0, 1.0, 1.0] if key == "base_color" else 1.0
        block[key] = value
        block[key + "_map"] = tex

    nrm = node.inputs.get("Normal")
    if nrm is not None and nrm.is_linked:
        src, _ = _resolve_link(nrm)
        if src is not None and src.type == "NORMAL_MAP":
            tex_src, _ = _resolve_link(src.inputs["Color"])
            if tex_src is not None and tex_src.type == "TEX_IMAGE":
                block["normal_map"] = _texture_info(tex_src)
                block["normal_strength"] = float(src.inputs["Strength"].default_value)
        elif src is not None and src.type == "TEX_IMAGE":
            block["normal_map"] = _texture_info(src)

    # データとして使うマップが色として扱われていると、sRGB エンコードされて
    # 値が変わってしまう。よくある設定ミスなので知らせておく。
    for key in ("normal", "roughness", "metallic", "specular", "alpha"):
        tex = block.get(key + "_map")
        if tex and tex.get("colorspace") == COLORSPACE_SRGB:
            warn(
                "material '%s': %s map '%s' is a color texture. "
                "Set its color space to 'Non-Color' in Blender"
                % (mat.name, key, tex.get("image", "?"))
            )

    return block


_SKIP_PROPS = {
    "rna_type",
    "type",
    "location",
    "location_absolute",
    "width",
    "height",
    "dimensions",
    "name",
    "label",
    "inputs",
    "outputs",
    "internal_links",
    "parent",
    "select",
    "show_options",
    "show_preview",
    "show_texture",
    "hide",
    "mute",
    "color",
    "use_custom_color",
    "bl_idname",
    "bl_label",
    "bl_description",
    "bl_icon",
    "bl_static_type",
    "bl_width_default",
    "bl_width_min",
    "bl_width_max",
    "bl_height_default",
    "bl_height_min",
    "bl_height_max",
    "warning_propagation",
    "is_active_output",
    "width_hidden",
}


def _node_props(node):
    props = {}
    for p in node.bl_rna.properties:
        pid = p.identifier
        if pid in _SKIP_PROPS:
            continue
        try:
            v = getattr(node, pid)
        except Exception:
            continue

        if p.type in {"BOOLEAN", "INT", "FLOAT"}:
            if getattr(p, "is_array", False) and 0 < getattr(p, "array_length", 0):
                try:
                    props[pid] = [float(x) for x in v]
                except Exception:
                    continue
            elif p.type == "BOOLEAN":
                props[pid] = bool(v)
            elif p.type == "INT":
                props[pid] = int(v)
            else:
                props[pid] = float(v)
        elif p.type in {"STRING", "ENUM"}:
            props[pid] = str(v)
        elif p.type == "POINTER":
            if v is not None and hasattr(v, "name"):
                props[pid] = v.name

    if node.type == "TEX_IMAGE" and getattr(node, "image", None) is not None:
        try:
            props["filepath"] = bpy.path.abspath(node.image.filepath)
        except Exception:
            props["filepath"] = ""
        props["colorspace"] = getattr(node.image.colorspace_settings, "name", "")

    # ColorRamp / CurveMapping は POINTER なので通常の props には乗らない。
    # GLSL 側で評価できるよう、内容を配列として書き出す。
    props.update(_color_ramp_props(node))
    props.update(_curve_props(node))
    return props


def _color_ramp_props(node):
    """ColorRamp (VALTORGB) の内容を配列にする"""
    ramp = getattr(node, "color_ramp", None)
    if ramp is None:
        return {}
    positions = []
    colors = []
    for e in ramp.elements:
        positions.append(float(e.position))
        colors.extend(srgb_rgba(e.color))  # 色なので sRGB へ
    if not positions:
        return {}
    return {
        "ramp_positions": positions,
        "ramp_colors": colors,
        "ramp_interpolation": str(getattr(ramp, "interpolation", "LINEAR")),
    }


CURVE_LUT_SIZE = 16


def _curve_props(node):
    """CurveMapping (RGB カーブ / ベクトルカーブ / フロートカーブ) を
    0..1 を等間隔にサンプルしたテーブルにする"""
    mapping = getattr(node, "mapping", None)
    if mapping is None or not hasattr(mapping, "curves"):
        return {}
    try:
        mapping.update()
    except Exception:
        pass

    curves = list(mapping.curves)
    if not curves:
        return {}

    lut = []
    used = 0
    for cu in curves:
        try:
            samples = [
                float(cu.evaluate(i / (CURVE_LUT_SIZE - 1)))
                for i in range(CURVE_LUT_SIZE)
            ]
        except Exception:
            return {}
        lut.extend(samples)
        used += 1

    return {
        "curve_lut": lut,
        "curve_lut_size": CURVE_LUT_SIZE,
        "curve_lut_channels": used,
    }


def _socket_info(sock, owner):
    return {
        "name": sock.name,
        "type": _enum_code(_SHADER_SOCKET_TYPE_CODE, sock.type, "socket type", owner),
        "default": _socket_default(sock),
    }


# 書き出さないノード。
#   FRAME はノードを囲むだけの飾りで、ソケットもリンクも持たない。
#   持っていても再生側で使い道が無いので落とす。
_SKIP_NODE_TYPES = {"FRAME"}


def _graph_nodes_and_links(nt, owner):
    """ノードツリーを nodes / links の配列にする (グループも同じ形)

    飛ばしたノードのぶん番号がずれるので、リンクは
    「残したノードの中での番号」に振り直す。
    """
    nodes = []
    links = []
    index_of = {}
    for n in nt.nodes:
        if n.type in _SKIP_NODE_TYPES:
            continue
        who = "%s node '%s'" % (owner, n.name)
        index_of[n.name] = len(nodes)

        # OSL スクリプトは中身を書き出せず、GLSL へも変換できない。
        # 黙って既定値に落とすと見た目が変わったことに気づけないので、
        # ここで書き出しを止める。
        if n.type == "SCRIPT":
            raise YbrExportError(
                "%s: OSL script nodes cannot be exported (OSL runs on the CPU in "
                "Cycles and has no GLSL equivalent). Replace it with regular "
                "shader nodes, or bake it to a texture." % who
            )

        nodes.append(
            {
                "name": n.name,
                "type": _enum_code(_SHADER_NODE_TYPE_CODE, n.type, "node type", who),
                "label": n.label,
                "inputs": [_socket_info(s, who) for s in n.inputs],
                "outputs": [_socket_info(s, who) for s in n.outputs],
                "props": _node_props(n),
            }
        )

    for l in nt.links:
        try:
            fi = index_of[l.from_node.name]
            ti = index_of[l.to_node.name]
            fs = list(l.from_node.outputs).index(l.from_socket)
            ts = list(l.to_node.inputs).index(l.to_socket)
        except (KeyError, ValueError):
            continue
        links.append(
            {"from_node": fi, "from_socket": fs, "to_node": ti, "to_socket": ts}
        )

    return nodes, links


def _group_interface(nt, owner):
    """グループのインターフェース (入力 / 出力ソケット) を取り出す。

    中身は展開せずに持つので、GROUP_INPUT / GROUP_OUTPUT ノードの
    ソケット並びをそのまま使う。Blender 4.0 以降の interface API では
    並び順が保証されないため、実ノードから読むほうが確実。
    """
    gin, gout = [], []
    for n in nt.nodes:
        if n.type == "GROUP_INPUT" and not gin:
            # 末尾の空ソケット (仮想ソケット) は除く
            gin = [_socket_info(s, owner) for s in n.outputs if s.type != "CUSTOM"]
        elif n.type == "GROUP_OUTPUT" and not gout:
            gout = [_socket_info(s, owner) for s in n.inputs if s.type != "CUSTOM"]
    return gin, gout


def collect_node_groups(materials):
    """マテリアルが使っているノードグループを (入れ子も含めて) 集める"""
    found = []
    seen = set()

    def walk(nt):
        if nt is None:
            return
        for n in nt.nodes:
            if n.type != "GROUP":
                continue
            sub = getattr(n, "node_tree", None)
            if sub is None:
                continue
            key = sub.as_pointer()  # 名前ではなくポインタで見る
            if key in seen:
                continue
            seen.add(key)
            walk(sub)  # 内側から先に登録する
            found.append(sub)

    for mat in materials:
        nt = material_nodes(mat)
        if nt is not None:
            walk(nt)
    return found


def build_node_group(nt):
    owner = "node group '%s'" % nt.name
    gin, gout = _group_interface(nt, owner)
    nodes, links = _graph_nodes_and_links(nt, owner)
    return {
        "kind": "NODEGROUP",
        "id": nt.name,
        "inputs": gin,
        "outputs": gout,
        "nodes": nodes,
        "links": links,
    }


def build_material_pro(mat):
    block = {
        "kind": "MATERIAL",
        "id": mat.name,
        "mode": "PRO",
        "render_method": str(getattr(mat, "surface_render_method", "")),
        "backface_culling": bool(getattr(mat, "use_backface_culling", False)),
        "transparent": _material_is_transparent(mat),
        "nodes": [],
        "links": [],
    }

    nt = material_nodes(mat)
    if nt is None:
        warn(
            "material '%s': node tree not used, PRO mode exports an empty graph"
            % mat.name
        )
        return block

    block["nodes"], block["links"] = _graph_nodes_and_links(
        nt, "material '%s'" % mat.name
    )
    return block


# ---------------------------------------------------------------------------
# テクスチャ
# ---------------------------------------------------------------------------
def collect_images(materials):
    """マテリアルが参照している Image Texture ノードの画像を集める。

    ノードグループの中も見る (グループは展開せずに書き出すが、
    中で使っている画像は TEXTURE ブロックとして必要なため)。

    ※ ノードツリーの重複チェックは**名前ではなくポインタ**で行う。
       マテリアルに埋め込まれたノードツリーは、どのマテリアルでも
       name が "Shader Nodetree" になるため、名前で覚えてしまうと
       2 つめ以降のマテリアルが丸ごとスキップされてしまう。
    """
    imgs = []
    seen_imgs = set()
    seen_trees = set()

    def walk(nt):
        if nt is None:
            return
        key = nt.as_pointer()
        if key in seen_trees:
            return
        seen_trees.add(key)
        for n in nt.nodes:
            if n.type == "TEX_IMAGE" and n.image is not None:
                ik = n.image.as_pointer()
                if ik not in seen_imgs:
                    seen_imgs.add(ik)
                    imgs.append(n.image)
            elif n.type == "GROUP":
                walk(getattr(n, "node_tree", None))

    for mat in materials:
        nt = material_nodes(mat)
        if nt is not None:
            walk(nt)
    return imgs


def _image_rgba8(img):
    """画像を RGBA8 (上原点) のバイト列にする。失敗時は None"""
    w, h = int(img.size[0]), int(img.size[1])
    total = len(img.pixels)
    if w <= 0 or h <= 0 or total == 0:
        warn(
            "texture '%s': no pixel data available (unloaded or generated?)" % img.name
        )
        return None

    ch = total // (w * h)
    if ch not in (1, 3, 4):
        warn("texture '%s': unsupported channel count %d" % (img.name, ch))
        return None

    _cs, to_srgb = _resolve_colorspace(
        getattr(img.colorspace_settings, "name", "sRGB"), "texture '%s'" % img.name
    )

    if np is not None:
        buf = np.empty(total, dtype=np.float32)
        img.pixels.foreach_get(buf)
        buf = buf.reshape(h, w, ch)[::-1]  # Blender は下原点なので反転

        if ch == 1:
            rgb = np.repeat(buf, 3, axis=2)
            alpha = np.ones((h, w, 1), dtype=np.float32)
        elif ch == 3:
            rgb = buf
            alpha = np.ones((h, w, 1), dtype=np.float32)
        else:
            rgb = buf[:, :, :3]
            alpha = buf[:, :, 3:4]

        rgb = np.clip(rgb, 0.0, 1.0)
        if to_srgb:
            rgb = np.where(
                rgb <= 0.0031308, rgb * 12.92, 1.055 * np.power(rgb, 1.0 / 2.4) - 0.055
            )
        rgba = np.concatenate((rgb, np.clip(alpha, 0.0, 1.0)), axis=2)
        return (rgba * 255.0 + 0.5).astype(np.uint8).tobytes()

    # --- numpy が無い場合のフォールバック (遅い) ---
    warn("texture '%s': numpy unavailable, using the slow pixel path" % img.name)
    px = array.array("f", bytes(total * 4))
    img.pixels.foreach_get(px)

    out = bytearray(w * h * 4)
    for y in range(h):
        src = (h - 1 - y) * w * ch
        dst = y * w * 4
        for x in range(w):
            s = src + x * ch
            d = dst + x * 4
            if ch == 1:
                r = g = b = px[s]
                a = 1.0
            elif ch == 3:
                r, g, b = px[s], px[s + 1], px[s + 2]
                a = 1.0
            else:
                r, g, b, a = px[s], px[s + 1], px[s + 2], px[s + 3]
            if to_srgb:
                out[d] = linear_to_srgb_byte(r)
                out[d + 1] = linear_to_srgb_byte(g)
                out[d + 2] = linear_to_srgb_byte(b)
            else:
                out[d] = max(0, min(255, int(r * 255.0 + 0.5)))
                out[d + 1] = max(0, min(255, int(g * 255.0 + 0.5)))
                out[d + 2] = max(0, min(255, int(b * 255.0 + 0.5)))
            out[d + 3] = max(0, min(255, int(a * 255.0 + 0.5)))
    return bytes(out)


def _encode_image_from_source(img, fmt, quality):
    """元の画像を PNG / JPEG のバイト列にする。失敗時は None

    元ファイルが使えないとき (生成画像 / Blender 内で編集済み) 用の道。

    ポイントは「自分で色変換しない」こと。
    Blender の Image.pixels は **その画像のカラースペースからシーンリニアへ
    変換した float** を返す。同じカラースペースの一時イメージへその float を
    そのまま書き戻せば、書き戻し時の変換が読み出し時の変換のちょうど裏返しに
    なるので、Blender がどの曲線をどこで適用していても元の表示値が復元される。

    自前で linear -> sRGB してから Non-Color の一時イメージに入れる方法だと、
    Blender 側でもう一度エンコードが掛かる環境で色が浅く (濁って) しまう。
    """
    w, h = int(img.size[0]), int(img.size[1])
    total = len(img.pixels)
    if w <= 0 or h <= 0 or total < w * h * 4:
        warn("texture '%s': cannot re-encode (no pixel data)" % img.name)
        return None

    tmp_img = None
    tmp_dir = None
    try:
        tmp_img = bpy.data.images.new(
            "YBR_ENCODE_TMP", width=w, height=h, alpha=True, float_buffer=False
        )
        # 読み出し元と同じカラースペースにするのが肝
        try:
            tmp_img.colorspace_settings.name = img.colorspace_settings.name
        except Exception:
            pass

        if np is not None:
            buf = np.empty(total, dtype=np.float32)
            img.pixels.foreach_get(buf)
            tmp_img.pixels.foreach_set(buf)
        else:
            buf = array.array("f", bytes(total * 4))
            img.pixels.foreach_get(buf)
            tmp_img.pixels.foreach_set(buf)

        tmp_img.file_format = fmt

        tmp_dir = tempfile.mkdtemp(prefix="ybr_tex_")
        path = os.path.join(tmp_dir, "tex.%s" % ("png" if fmt == "PNG" else "jpg"))
        tmp_img.filepath_raw = path
        try:
            tmp_img.save(quality=quality)
        except TypeError:  # 古い API 用のフォールバック
            tmp_img.save()

        with open(path, "rb") as f:
            return f.read()
    except Exception as e:
        warn("texture '%s': %s encoding failed (%s)" % (img.name, fmt, e))
        return None
    finally:
        if tmp_img is not None:
            try:
                bpy.data.images.remove(tmp_img)
            except Exception:
                pass
        if tmp_dir is not None:
            try:
                for f in os.listdir(tmp_dir):
                    os.remove(os.path.join(tmp_dir, f))
                os.rmdir(tmp_dir)
            except Exception:
                pass


def resolve_compression(img, default_comp, default_quality):
    """画像ごとの設定を解決する。未設定ならエクスポーターの既定値"""
    st = getattr(img, "ybr", None)
    comp = getattr(st, "compression", "DEFAULT") if st else "DEFAULT"
    quality = getattr(st, "quality", default_quality) if st else default_quality
    if comp == "DEFAULT":
        comp = default_comp
        quality = default_quality
    return comp, int(quality)


def _original_file_bytes(img):
    """元の画像ファイルのバイト列をそのまま取り出す。

    PNG / JPEG ならそのまま埋め込めるので、再エンコードもカラースペース変換も
    要らない。**これがいちばん確実**で、色が変わる余地が無い。

    Blender の Image.pixels は「シーンリニアに変換された float」を返し、
    書き戻すときにどの変換が掛かるかはバージョンによって挙動が違う。
    元ファイルをそのまま使えば、その不確かさを丸ごと避けられる。

    戻り値は (bytes, "PNG"/"JPEG") か (None, None)。
    Blender 内で編集されている (is_dirty) 場合はファイルが古いので使わない。
    """
    if getattr(img, "is_dirty", False):
        return None, None
    if getattr(img, "source", "") in {"GENERATED", "VIEWER", "MOVIE", "SEQUENCE"}:
        return None, None

    data = None
    pf = getattr(img, "packed_file", None)
    if pf is not None and getattr(pf, "data", None):
        try:
            data = bytes(pf.data)
        except Exception:
            data = None
    if data is None:
        try:
            path = bpy.path.abspath(img.filepath_raw or img.filepath)
        except Exception:
            path = ""
        if path and os.path.isfile(path):
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except Exception:
                data = None
    if not data:
        return None, None

    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return data, "PNG"
    if data[:3] == b"\xff\xd8\xff":
        return data, "JPEG"
    return None, None


def build_texture_block(img, embed, default_comp, default_quality):
    try:
        path = bpy.path.abspath(img.filepath) if img.filepath else ""
    except Exception:
        path = ""

    comp, quality = resolve_compression(img, default_comp, default_quality)

    block = {
        "kind": "TEXTURE",
        "id": img.name,
        "name": img.name,
        "width": int(img.size[0]),
        "height": int(img.size[1]),
        # 格納されるバイト列に合わせたコードを書く (raylib 基準)
        "colorspace": _resolve_colorspace(
            getattr(img.colorspace_settings, "name", ""), "texture '%s'" % img.name
        )[0],
        "filepath": path,
        "format": "RGBA8",  # compression が NONE のときの pixels 形式
        "compression": "NONE",
        "embedded": False,
    }

    if not embed:
        if not path:
            warn(
                "texture '%s': no file path (packed or generated image). "
                "Enable 'Embed Textures' to include the pixel data" % img.name
            )
        return block

    if comp != "NONE":
        # 1) 元ファイル (PNG / JPEG) をそのまま埋め込めるならそれが最良。
        #    再エンコードもカラースペース変換も挟まないので色が一切変わらない。
        raw, fmt = _original_file_bytes(img)
        if raw is not None:
            block["data"] = raw
            block["compression"] = fmt
            block["quality"] = quality
            block["embedded"] = True
            block["_verbatim"] = True  # 集計用 (書き出し前に外す)
            return block

        # 2) 生成画像や編集済みの画像は Blender に再エンコードさせる
        if comp == "JPEG" and img.alpha_mode != "NONE":
            warn(
                "texture '%s': JPEG does not support alpha, the alpha channel "
                "will be dropped" % img.name
            )
        data = _encode_image_from_source(img, comp, quality)
        if data is not None:
            block["data"] = data
            block["compression"] = comp
            block["quality"] = quality
            block["embedded"] = True
            return block
        # エンコードに失敗したら無圧縮で残す

    # 3) 無圧縮 (RGBA8)。ここだけは自前で linear -> sRGB する
    pixels = _image_rgba8(img)
    if pixels is None:
        return block

    block["pixels"] = pixels
    block["compression"] = "NONE"
    block["embedded"] = True
    return block


# ---------------------------------------------------------------------------
# アニメーション (必ずベイクして書き出す)
# ---------------------------------------------------------------------------
# フレームタイプ : 将来の拡張用。今はトランスフォームのみ
FRAME_TRANSFORM = 0

# 補間方法 (ベイク済みなので書き出しは常に STEP)
INTERP_STEP = 0
INTERP_LINEAR = 1
INTERP_CUBIC = 2  # 非一様 Catmull-Rom
INTERP_SINC = 3  # Lanczos-a 再構成
INTERP_HERMITE = 4  # 3 次エルミート (キーに接線を持つ)
# ベイクは全フレーム行うのでここでは常に STEP を書き出す。
# CUBIC / SINC / HERMITE への変換とキー削減は ybr_tool --anime-opt で行う。

# SINC (Lanczos) の a。アニメーションごとにファイルへ保存する。
SINC_A_DEFAULT = 3

_MATRIX_EPS = 1e-6


def iter_fcurves(action, slot=None):
    """Blender 4.4+ のスロット付きアクションと従来アクションの両対応"""
    layers = getattr(action, "layers", None)
    if layers:
        for layer in layers:
            for strip in layer.strips:
                bags = []
                if slot is not None and hasattr(strip, "channelbag"):
                    bag = strip.channelbag(slot)
                    if bag is not None:
                        bags = [bag]
                if not bags:
                    bags = list(getattr(strip, "channelbags", []))
                for bag in bags:
                    for fc in bag.fcurves:
                        yield fc
        return
    for fc in getattr(action, "fcurves", []):
        yield fc


def _split_bone_path(path):
    """pose.bones["Bone"].location -> ("Bone", "location")"""
    if path.startswith('pose.bones["'):
        end = path.find('"]', 12)
        if 0 < end:
            return path[12:end], path[end + 2 :].lstrip(".")
    return None, path


def collect_animated_targets(action, slot=None):
    """(オブジェクトレベルのアニメがあるか, アニメのあるボーン名の集合)"""
    obj_level = False
    bones = set()
    for fc in iter_fcurves(action, slot):
        bone, _ = _split_bone_path(fc.data_path)
        if bone:
            bones.add(bone)
        else:
            obj_level = True
    return obj_level, bones


def _same_matrix(a, b):
    if b is None:
        return False
    for x, y in zip(a, b):
        if _MATRIX_EPS < abs(x - y):
            return False
    return True


def _object_local_matrix(eval_obj):
    """親空間における自分のローカル行列 (親が無ければ world)。

    matrix_local ではなく world から明示的に求める。ボーンペアレントや
    頂点ペアレントでは matrix_local が親オブジェクト基準の実際の位置を
    表さないため、こちらの方が常に正しい。
    """
    parent = eval_obj.parent
    if parent is None:
        return eval_obj.matrix_world.copy()
    return parent.matrix_world.inverted() @ eval_obj.matrix_world


def _pose_bone_local_matrix(pose_bone, included, rel=None):
    """親ボーン空間における自分のローカル行列。

    included は書き出し対象のボーン名集合。除外された親は飛ばして
    最も近い「含まれる祖先」からの相対にする。

    rel は「このアーマチュアを合成後アーマチュアの基準空間へ持っていく行列」。
    親を持たないボーンは合成後アーマチュアのルートになるので、その分だけ
    ずらしてやらないとレスト姿勢と噛み合わない。親を持つボーンは親との
    相対なので rel は打ち消し合い、掛ける必要がない。
    """
    parent = pose_bone.parent
    while parent is not None and parent.name not in included:
        parent = parent.parent
    if parent is None:
        m = pose_bone.matrix.copy()
        return (rel @ m) if rel is not None else m
    return parent.matrix.inverted() @ pose_bone.matrix


def _strip_slot(strip, action):
    """ストリップが使っているアクションスロットを得る (Blender 4.4+ のスロット対応)"""
    slot = getattr(strip, "action_slot", None)
    if slot is not None:
        return slot
    slots = getattr(action, "slots", None)
    if not slots:
        return None
    for sl in slots:
        if getattr(sl, "target_id_type", "OBJECT") == "OBJECT":
            return sl
    return slots[0]


def _action_frame_range(action, slot):
    """アクションのフレーム範囲。

    Blender の frame_range は、キーが 1 本しか無いなどの場合に
    幅 1 の範囲を返してくることがある (Blender bug #107030)。
    その場合は F カーブの範囲から取り直す。
    """
    fr = action.frame_range
    f0, f1 = int(math.floor(fr[0])), int(math.ceil(fr[1]))

    if f1 - f0 == 1:
        try:
            ranges = [fc.range() for fc in iter_fcurves(action, slot)]
            ranges = [r for r in ranges if r is not None]
            if ranges:
                f0 = int(math.floor(min(r[0] for r in ranges)))
                f1 = int(math.ceil(max(r[1] for r in ranges)))
        except Exception:
            pass

    if f1 < f0:
        f1 = f0
    return f0, f1


def _reset_pose(obj):
    """前のアクションのポーズが残らないようボーンをレストに戻す"""
    if obj.type != "ARMATURE" or obj.pose is None:
        return
    ident = Matrix()
    for pb in obj.pose.bones:
        pb.matrix_basis = ident.copy()


def constraint_target_objects(obj):
    """オブジェクトとポーズボーンのコンストレイントが参照しているオブジェクト"""
    targets = []

    def collect(constraints):
        for con in constraints:
            for attr in ("target", "pole_target"):
                tgt = getattr(con, attr, None)
                if tgt is not None and tgt not in targets and tgt is not obj:
                    targets.append(tgt)

    try:
        collect(obj.constraints)
        if obj.type == "ARMATURE" and obj.pose is not None:
            for pb in obj.pose.bones:
                collect(pb.constraints)
    except Exception:
        pass
    return targets


def _object_targets(ob):
    """そのオブジェクト自身のコンストレイントが参照しているオブジェクト"""
    targets = []
    try:
        for con in ob.constraints:
            for attr in ("target", "pole_target"):
                tgt = getattr(con, attr, None)
                if tgt is not None and tgt not in targets:
                    targets.append(tgt)
    except Exception:
        pass
    return targets


def collect_follower_objects(driver, candidates):
    """driver に追従しているだけのオブジェクトを集める (推移的)。

    自前のアニメーションを持たず、コンストレイントやボーンペアレントで
    他のオブジェクトに追従しているものは、そのままでは 1 フレームも
    ベイクされない。駆動元のアニメーションに相乗りさせる。
    """
    followers = []
    frontier = [driver]
    seen = {driver}

    while frontier:
        current = frontier.pop()
        for ob in candidates:
            if ob in seen:
                continue
            follows = current in _object_targets(ob)
            if not follows and ob.parent is current:
                # 通常のペアレントは親の行列で追従できるが、
                # ボーンペアレントは親オブジェクト基準の行列では表せない
                follows = getattr(ob, "parent_type", "OBJECT") == "BONE"
            if follows:
                seen.add(ob)
                followers.append(ob)
                frontier.append(ob)

    return followers


def _prepare_object_for_bake(context, obj, extra_objects=()):
    """glTF エクスポーターと同じ手順でベイク可能な状態にし、復元用の情報を返す。

    ・オブジェクトやコレクションが非表示 / 除外されていると depsgraph で
      評価されず、行列を読んでも動かないので一時的に表示に戻す
      (コンストレイントのターゲットも評価されないと結果が変わるので一緒に戻す)
    ・tweak mode 中は action が readonly なので解除する
    ・solo (星印) の NLA トラックがあると他が評価されないので解除する
    ・NLA の合成が乗らないよう use_nla を切る
    """
    state = {"obj": obj, "layers": [], "colls": [], "solo": [], "hidden": []}

    # 自分自身 / コンストレイントのターゲット / 追従オブジェクトを表示状態に戻す
    unhide = [obj] + constraint_target_objects(obj)
    for ob in extra_objects:
        if ob not in unhide:
            unhide.append(ob)
        for tgt in constraint_target_objects(ob):
            if tgt not in unhide:
                unhide.append(tgt)
    for ob in unhide:
        state["hidden"].append((ob, ob.hide_viewport))
        ob.hide_viewport = False

    names = set(ob.name for ob in unhide)
    view_layer = context.view_layer
    seen_layers = set()

    def walk(layer_coll, chain):
        chain = chain + [layer_coll]
        if names & set(layer_coll.collection.objects.keys()):
            for lc in chain:
                if id(lc) in seen_layers:
                    continue
                seen_layers.add(id(lc))
                state["layers"].append((lc, lc.exclude, lc.hide_viewport))
                try:
                    lc.exclude = False
                    lc.hide_viewport = False
                except Exception:
                    pass
                coll = lc.collection
                state["colls"].append((coll, coll.hide_viewport))
                coll.hide_viewport = False
        for child in layer_coll.children:
            walk(child, chain)

    try:
        walk(view_layer.layer_collection, [])
    except Exception as e:
        warn("object '%s': could not unhide its collections (%s)" % (obj.name, e))

    ad = obj.animation_data
    state["use_tweak_mode"] = ad.use_tweak_mode
    if ad.use_tweak_mode:
        ad.use_tweak_mode = False

    for track in ad.nla_tracks:
        if track.is_solo:
            state["solo"].append(track)
            track.is_solo = False

    state["use_nla"] = ad.use_nla
    ad.use_nla = False

    state["action"] = ad.action
    state["action_slot"] = getattr(ad, "action_slot", None)
    state["matrix_basis"] = obj.matrix_basis.copy()

    return state


def _restore_object_after_bake(state):
    obj = state["obj"]
    ad = obj.animation_data

    if ad is not None:
        try:
            ad.action = state["action"]
            if state["action_slot"] is not None and hasattr(ad, "action_slot"):
                ad.action_slot = state["action_slot"]
        except Exception:
            pass
        for track in state["solo"]:
            try:
                track.is_solo = True
            except Exception:
                pass
        ad.use_nla = state["use_nla"]
        ad.use_tweak_mode = state["use_tweak_mode"]

    try:
        obj.matrix_basis = state["matrix_basis"]
    except Exception:
        pass

    for coll, hide in state["colls"]:
        try:
            coll.hide_viewport = hide
        except Exception:
            pass
    for lc, exclude, hide in reversed(state["layers"]):
        try:
            lc.exclude = exclude
            lc.hide_viewport = hide
        except Exception:
            pass

    for ob, hide in state["hidden"]:
        try:
            ob.hide_viewport = hide
        except Exception:
            pass


def _assign_action(obj, action, slot):
    """アクションをアクティブにする。失敗したら False"""
    ad = obj.animation_data
    if ad.is_property_readonly("action"):
        ad.use_tweak_mode = False
    try:
        _reset_pose(obj)
        ad.action = action
        if slot is not None and hasattr(ad, "action_slot"):
            ad.action_slot = slot
    except Exception as e:
        warn(
            "action '%s' on object '%s' could not be made active (%s). "
            "Check the NLA editor" % (action.name, obj.name, e)
        )
        return False
    return True


def action_pose_markers(action, f0, f1):
    """アクションのポーズマーカーを「先頭からの相対フレーム」で書き出す。

    足音やエフェクトの発火タイミングを再生側へ渡すのに使う。
    アクションの範囲外にあるマーカーは出さない。
    """
    markers = getattr(action, "pose_markers", None)
    if not markers:
        return []

    out, skipped = [], 0
    for m in markers:
        f = int(round(m.frame))
        if f < f0 or f1 < f:
            skipped += 1
            continue
        out.append({"name": m.name, "frame": f - f0})
    if skipped:
        warn(
            "action '%s': %d pose marker(s) outside the frame range were skipped"
            % (action.name, skipped)
        )
    out.sort(key=lambda m: m["frame"])
    return out


def bake_action_animation(
    context,
    scene,
    obj,
    action,
    slot,
    fps,
    deform_only,
    merged=None,
    followers=(),
):
    """アクティブにしたアクションを 1 フレームずつ評価して ANIMATION ブロックにする"""
    if not _assign_action(obj, action, slot):
        return None

    f0, f1 = _action_frame_range(action, slot)

    # glTF エクスポーターと同じく、F カーブの有無に関わらず
    # オブジェクトと「書き出す全ボーン」をサンプリングする。
    #
    # F カーブを持つボーンだけを対象にすると、コンストレイント (IK や
    # Copy Transforms など) だけで動く deform ボーンが完全に抜け落ちる。
    # 制御ボーンは deform オフで除外されるため、結果として何も動かなくなる。
    # 全ボーンを実際の評価結果から読めば、非 deform ボーンの影響も
    # 畳み込まれた状態で入る。
    # 合成後アーマチュアに入っているボーンだけを対象にする。
    # トラックに書き出す名前は合成後の名前 (衝突時に改名されている)。
    included = None
    rel = None
    if obj.type == "ARMATURE" and merged is not None and obj in merged.objects:
        included = merged.included(obj)
        rel = merged.relative(obj)
    elif obj.type == "ARMATURE":
        # 書き出し対象のメッシュがどれも使っていないアーマチュア。
        # ボーンのトラックは出さない (参照先の ARMATURE ブロックが無いため)。
        included = None
    else:
        _, bone_names = collect_animated_targets(action, slot)
        if bone_names:
            warn(
                "action '%s': bone channels on non-armature object '%s' were skipped"
                % (action.name, obj.name)
            )

    tracks = [
        {
            "object": obj.name,
            "bone": None,
            "_out_bone": None,
            "_obj": obj,
            "_included": None,
            "_rel": None,
            "frames": [],
            "transforms": [],
            "_last": None,
        }
    ]

    if included is not None:
        for name in sorted(included):
            tracks.append(
                {
                    "object": obj.name,
                    "bone": name,
                    "_out_bone": merged.bone_out_name(obj, name),
                    "_obj": obj,
                    "_included": included,
                    "_rel": rel,
                    "frames": [],
                    "transforms": [],
                    "_last": None,
                }
            )

    # コンストレイントやボーンペアレントで追従しているだけのオブジェクト
    for follower in followers:
        tracks.append(
            {
                "object": follower.name,
                "bone": None,
                "_out_bone": None,
                "_obj": follower,
                "_included": None,
                "_rel": None,
                "frames": [],
                "transforms": [],
                "_last": None,
            }
        )

    # --- フレーム走査 (ここがベイク) ---
    for f in range(f0, f1 + 1):
        scene.frame_set(f)
        depsgraph = context.evaluated_depsgraph_get()

        for tr in tracks:
            eval_obj = tr["_obj"].evaluated_get(depsgraph)
            if tr["bone"] is None:
                mat = _object_local_matrix(eval_obj)
            else:
                pb = eval_obj.pose.bones.get(tr["bone"])
                if pb is None:
                    continue
                mat = _pose_bone_local_matrix(pb, tr["_included"], tr.get("_rel"))

            values = mat_to_column_major(conv_matrix(mat))
            if _same_matrix(values, tr["_last"]):
                continue  # 前フレームと同じ値なら書かない
            tr["_last"] = values
            tr["frames"].append(f - f0)
            tr["transforms"].extend(values)

    out_tracks = []
    for tr in tracks:
        n = len(tr["frames"])
        if n == 0:
            continue  # ベイクの結果 空になったトラックは出さない
        out_tracks.append(
            {
                "object": tr["object"],
                "bone": tr.get("_out_bone", tr["bone"]),
                "frame_count": n,
                "frames": _u32(tr["frames"]),
                "types": _u8([FRAME_TRANSFORM] * n),
                "interps": _u8([INTERP_STEP] * n),
                "transforms": _f32(tr["transforms"]),
            }
        )

    markers = action_pose_markers(action, f0, f1)

    if not out_tracks and not markers:
        return None

    return {
        "kind": "ANIMATION",
        "id": action.name,
        "object": obj.name,
        "fps": float(fps),
        "frame_count": f1 - f0 + 1,
        "sinc_a": SINC_A_DEFAULT,
        "space": "GL",  # 行列は変換済み (値は Blender 空間ではない)
        "tracks": out_tracks,
        "markers": markers,
    }


def build_animation_blocks(context, scene, src, fps, deform_only, merged=None):
    """NLA トラック内で使われている action strip をすべて書き出す。

    オブジェクトに直接設定されている Action (animation_data.action) は対象外。
    """
    saved_frame = scene.frame_current
    blocks = []

    # 自前の NLA ストリップを持つオブジェクト
    def has_strips(ob):
        ad = ob.animation_data
        if ad is None:
            return False
        for track in ad.nla_tracks:
            for strip in track.strips:
                if strip.type == "CLIP" and strip.action is not None:
                    return True
        return False

    animated = set(ob for ob in src if has_strips(ob))
    candidates = [ob for ob in src if ob not in animated]

    try:
        for obj in src:
            ad = obj.animation_data
            if ad is None:
                continue

            # このオブジェクトの NLA で使われている (アクション, スロット) を集める
            entries = []
            seen = set()
            for nla_track in ad.nla_tracks:
                for strip in nla_track.strips:
                    if strip.type != "CLIP" or strip.action is None:
                        continue
                    slot = _strip_slot(strip, strip.action)
                    key = (strip.action.name, getattr(slot, "handle", None))
                    if key in seen:
                        continue
                    seen.add(key)
                    entries.append((strip.action, slot))

            if not entries:
                if ad.action is not None:
                    warn(
                        "object '%s': the active action '%s' is not exported. "
                        "Only actions used by NLA strips are written out"
                        % (obj.name, ad.action.name)
                    )
                continue

            followers = collect_follower_objects(obj, candidates)
            if followers:
                warn(
                    "object '%s': %d object(s) following it via constraints or "
                    "bone parenting are baked into its animations: %s"
                    % (
                        obj.name,
                        len(followers),
                        ", ".join(sorted(o.name for o in followers)[:5]),
                    )
                )

            state = _prepare_object_for_bake(context, obj, followers)
            try:
                for action, slot in entries:
                    block = bake_action_animation(
                        context,
                        scene,
                        obj,
                        action,
                        slot,
                        fps,
                        deform_only,
                        merged,
                        followers,
                    )
                    if block is not None:
                        blocks.append(block)
            finally:
                _restore_object_after_bake(state)
    finally:
        scene.frame_set(saved_frame)

    return blocks


# ---------------------------------------------------------------------------
# エンプティ
# ---------------------------------------------------------------------------
def build_empty_block(obj):
    return {
        "kind": "EMPTY",
        "id": obj.name,
        "display_type": obj.empty_display_type,
        "display_size": float(obj.empty_display_size),
    }


# ---------------------------------------------------------------------------
# シーンツリー
# ---------------------------------------------------------------------------
def data_id_of(obj, apply_modifiers):
    if obj.type == "EMPTY":
        return obj.name
    if obj.data is None:
        return None
    if obj.type == "MESH" and apply_modifiers and 0 < len(obj.modifiers):
        return "%s#%s" % (obj.data.name, obj.name)
    return obj.data.name


IDENTITY_MATRIX = [
    1.0,
    0.0,
    0.0,
    0.0,
    0.0,
    1.0,
    0.0,
    0.0,
    0.0,
    0.0,
    1.0,
    0.0,
    0.0,
    0.0,
    0.0,
    1.0,
]


def build_object_node(
    obj, export_set, placed, apply_modifiers, is_root, custom_properties
):
    """オブジェクトノード。is_root が True なら world 行列を使う"""
    placed.add(obj)
    mat = obj.matrix_world if is_root else obj.matrix_local

    children = []
    for c in sorted(obj.children, key=lambda x: x.name):
        if c in export_set and c not in placed:
            children.append(
                build_object_node(
                    c, export_set, placed, apply_modifiers, False, custom_properties
                )
            )

    return {
        "name": obj.name,
        "type": _NODE_TYPE_CODE.get(obj.type, NODE_TYPE_OBJECT),
        "data": data_id_of(obj, apply_modifiers),
        "matrix": mat_to_column_major(conv_matrix(mat.copy())),
        "custom_properties": build_custom_properties(obj) if custom_properties else [],
        "children": children,
    }


def _layer_collection_map(layer_coll, out):
    out[layer_coll.collection.name] = layer_coll
    for c in layer_coll.children:
        _layer_collection_map(c, out)


def _collection_hidden(coll, layer_map, skip_viewport, skip_render):
    lc = layer_map.get(coll.name)

    # ビューレイヤーから除外されたコレクションは表示もレンダリングもされない
    if (skip_viewport or skip_render) and lc is not None and lc.exclude:
        return True
    if skip_viewport:
        if lc is not None and lc.hide_viewport:  # 目のアイコン
            return True
        if coll.hide_viewport:  # モニターのアイコン
            return True
    if skip_render and coll.hide_render:  # カメラのアイコン
        return True
    return False


def render_visible_objects(scene, layer_map):
    """レンダリングされるオブジェクトの集合。

    1 つでも有効なコレクションからリンクされていれば表示対象とする。
    """
    visible = set()

    def rec(coll):
        lc = layer_map.get(coll.name)
        if coll.hide_render or (lc is not None and lc.exclude):
            return
        for o in coll.objects:
            if not o.hide_render:
                visible.add(o)
        for c in coll.children:
            rec(c)

    rec(scene.collection)
    return visible


def build_collection_node(
    coll,
    layer_map,
    export_set,
    placed,
    apply_modifiers,
    skip_viewport,
    skip_render,
    custom_properties,
):
    children = []

    for child in sorted(coll.children, key=lambda c: c.name):
        if _collection_hidden(child, layer_map, skip_viewport, skip_render):
            continue
        children.append(
            build_collection_node(
                child,
                layer_map,
                export_set,
                placed,
                apply_modifiers,
                skip_viewport,
                skip_render,
                custom_properties,
            )
        )

    for obj in sorted(coll.objects, key=lambda o: o.name):
        if obj not in export_set or obj in placed:
            continue
        # 親オブジェクトがある場合はその下にぶら下げるのでここでは出さない
        if obj.parent is not None and obj.parent in export_set:
            continue
        children.append(
            build_object_node(
                obj, export_set, placed, apply_modifiers, True, custom_properties
            )
        )

    return {
        "name": coll.name,
        "type": NODE_TYPE_COLLECTION,
        "data": None,
        "matrix": list(IDENTITY_MATRIX),
        "custom_properties": [],
        "children": children,
    }


def build_scene_tree(
    context,
    scene,
    export_set,
    apply_modifiers,
    skip_viewport,
    skip_render,
    custom_properties,
):
    """シーンコレクションの階層をそのままツリーにする。

    オブジェクトは重複しないよう 1 度だけ配置する:
      - 親オブジェクトが書き出し対象なら、その子として
      - そうでなければ、リンクされているコレクションの下に
    """
    layer_map = {}
    _layer_collection_map(context.view_layer.layer_collection, layer_map)

    placed = set()
    master = build_collection_node(
        scene.collection,
        layer_map,
        export_set,
        placed,
        apply_modifiers,
        skip_viewport,
        skip_render,
        custom_properties,
    )
    tree = [master]

    # どのコレクションからも配置されなかったオブジェクトを拾う
    leftover = [o for o in export_set if o not in placed]
    for obj in sorted(leftover, key=lambda o: o.name):
        if obj in placed:
            continue
        if obj.parent is not None and obj.parent in export_set:
            continue
        tree.append(
            build_object_node(
                obj, export_set, placed, apply_modifiers, True, custom_properties
            )
        )

    still = [o for o in export_set if o not in placed]
    if still:
        warn(
            "%d object(s) could not be placed in the scene tree: %s"
            % (len(still), ", ".join(sorted(o.name for o in still)[:5]))
        )

    return tree


def retarget_armature_nodes(tree, merged):
    """シーンツリーのアーマチュアノードを合成後アーマチュアに向け直す。

    ARMATURE ブロックは 1 つしか出さないので、基準アーマチュアのノードだけが
    その id を指す。合成された側のノードは data を外す (ボーンは基準の空間へ
    移してあるので、そのノードの行列はもう使われない)。参照先の無いデータ ID を
    残さないためでもある。
    """
    code = _NODE_TYPE_CODE.get("ARMATURE")
    primary = merged.primary.name if merged is not None else None
    data_id = merged.data_id if merged is not None else None

    def rec(node):
        if node.get("type") == code:
            node["data"] = data_id if node.get("name") == primary else None
        for c in node.get("children", ()):
            rec(c)

    for root in tree:
        rec(root)


# ---------------------------------------------------------------------------
# 本体
# ---------------------------------------------------------------------------
def export_scene(
    context,
    filepath,
    use_selection,
    skip_hidden_viewport,
    skip_hidden_render,
    apply_modifiers,
    rest_pose,
    material_mode,
    export_animations,
    keep_raw_groups,
    embed_textures,
    texture_compression,
    texture_quality,
    export_custom_properties,
    deform_bones_only,
):
    del _warnings[:]

    scene = context.scene
    fps = scene.render.fps / scene.render.fps_base

    layer_map = {}
    _layer_collection_map(context.view_layer.layer_collection, layer_map)

    src = list(context.selected_objects) if use_selection else list(scene.objects)
    if skip_hidden_viewport:
        src = [o for o in src if o.visible_get()]
    if skip_hidden_render:
        renderable = render_visible_objects(scene, layer_map)
        src = [o for o in src if o in renderable]
    src.sort(key=lambda o: o.name)

    # メッシュが参照しているアーマチュアは、ビューポートで非表示だったり
    # 選択されていなかったりしても必ず書き出し対象に含める。
    # 含めないと ARMATURE ブロックが出ず (skin の joint index の参照先が消える)、
    # NLA ストリップの探索範囲からも外れてアニメーションが落ちてしまう。
    present = set(src)
    forced = []
    used_armatures = []
    for ob in list(src):
        if ob.type != "MESH":
            continue
        for arm_object in mesh_armature_objects(ob):
            if arm_object not in used_armatures:
                used_armatures.append(arm_object)
            if arm_object not in present:
                present.add(arm_object)
                forced.append(arm_object)
    if forced:
        warn(
            "armature(s) referenced by exported meshes were added to the export "
            "even though they are hidden or not selected: %s"
            % ", ".join(sorted(o.name for o in forced))
        )
        src.extend(forced)
        src.sort(key=lambda o: o.name)

    export_set = set(src)

    # アーマチュアモディファイアを一時的に無効化してレストポーズを書き出す
    muted = []
    if rest_pose and apply_modifiers:
        for ob in src:
            for m in getattr(ob, "modifiers", []):
                if m.type == "ARMATURE" and m.show_viewport:
                    m.show_viewport = False
                    muted.append(m)
        if muted:
            context.view_layer.update()

    try:
        depsgraph = context.evaluated_depsgraph_get()

        # 書き出すアーマチュアは 0 個か 1 個。メッシュのアーマチュア
        # モディファイアで使われているものが対象で、複数あれば合成する。
        merged = build_merged_armature(used_armatures, deform_bones_only)

        tree = build_scene_tree(
            context,
            scene,
            export_set,
            apply_modifiers,
            skip_hidden_viewport,
            skip_hidden_render,
            export_custom_properties,
        )
        retarget_armature_nodes(tree, merged)

        blocks = []
        done = set()
        material_objs = []

        # ARMATURE ブロックは合成後の 1 つだけ
        if merged is not None:
            blocks.append(build_merged_armature_block(merged))

        for obj in src:
            did = data_id_of(obj, apply_modifiers)

            if obj.type == "MESH" and did is not None:
                for slot in obj.material_slots:
                    if slot.material and slot.material not in material_objs:
                        material_objs.append(slot.material)
                if ("MESH", did) not in done:
                    done.add(("MESH", did))
                    owner = obj.evaluated_get(depsgraph) if apply_modifiers else obj
                    me = owner.to_mesh()
                    try:
                        mats = [
                            s.material.name for s in obj.material_slots if s.material
                        ]
                        blocks.append(
                            build_mesh_block(
                                did,
                                obj,
                                me,
                                mats,
                                keep_raw_groups,
                                deform_bones_only,
                                merged,
                            )
                        )
                    finally:
                        owner.to_mesh_clear()

            elif obj.type in {"CURVE", "SURFACE", "FONT"} and did is not None:
                if ("CURVE", did) not in done:
                    done.add(("CURVE", did))
                    blocks.append(build_curve_block(did, obj.data))

            elif obj.type == "LIGHT" and did is not None:
                if ("LIGHT", did) not in done:
                    done.add(("LIGHT", did))
                    blocks.append(build_light_block(did, obj.data))

            elif obj.type == "CAMERA" and did is not None:
                if ("CAMERA", did) not in done:
                    done.add(("CAMERA", did))
                    blocks.append(build_camera_block(did, obj.data))

            elif obj.type == "EMPTY":
                if ("EMPTY", did) not in done:
                    done.add(("EMPTY", did))
                    blocks.append(build_empty_block(obj))

        # --- マテリアル ---
        if material_mode == "NONE":
            # マテリアルを書き出さない。全メッシュを 1 つのダミーに向ける。
            # 再生側は「マテリアルが必ずある」前提で書けるので、
            # 参照無しにするより扱いやすい。
            blocks.append(build_material_dummy())
            for b in blocks:
                if b.get("kind") != "MESH":
                    continue
                b["materials"] = [YBR_DUMMY_MATERIAL]
                # 全三角形をマテリアル 0 に向ける
                if b.get("triangle_count"):
                    b["material_indices"] = _u32([0] * b["triangle_count"])
            material_objs = []
        else:
            for mat in material_objs:
                if material_mode == "PRO":
                    blocks.append(build_material_pro(mat))
                else:
                    blocks.append(build_material_simple(mat))

        # --- ノードグループ (PRO のみ / 展開せずそのまま書き出す) ---
        if material_mode == "PRO":
            for nt in collect_node_groups(material_objs):
                blocks.append(build_node_group(nt))

        # --- テクスチャ ---
        images = collect_images(material_objs)
        embedded_count = 0
        tex_lines = []
        for img in images:
            tb = build_texture_block(
                img, embed_textures, texture_compression, texture_quality
            )
            verbatim = tb.pop("_verbatim", False)
            if tb.get("embedded"):
                embedded_count += 1
            tex_lines.append(
                "[ybr]   - %-24s %4dx%-4d cs=%s %s%s"
                % (
                    img.name,
                    img.size[0],
                    img.size[1],
                    "Non-Color" if tb["colorspace"] == COLORSPACE_NON_COLOR else "sRGB",
                    tb["compression"] if tb.get("embedded") else "not embedded",
                    " (original file, no re-encode)" if verbatim else "",
                )
            )
            blocks.append(tb)
        if images:
            print(
                "[ybr] textures: %d found / %d embedded%s"
                % (
                    len(images),
                    embedded_count,
                    "" if embed_textures else " (Embed Textures is off)",
                )
            )
            for line in tex_lines:
                print(line)

        # --- アニメーション (必ずベイクする) ---
        if export_animations:
            blocks.extend(
                build_animation_blocks(
                    context, scene, src, fps, deform_bones_only, merged
                )
            )

        root = [MAGIC, FORMAT_VERSION, tree, blocks]

        w = CBORWriter()
        w.write(root)
        with open(filepath, "wb") as f:
            f.write(w.getvalue())

        return len(tree), len(blocks)

    finally:
        for m in muted:
            m.show_viewport = True
        if muted:
            context.view_layer.update()


_COMPRESSION_ITEMS = (
    ("NONE", "None (Raw RGBA8)", "無圧縮。読み込みは速いがサイズは最大"),
    ("PNG", "PNG", "可逆圧縮。アルファを保持する"),
    ("JPEG", "JPEG", "非可逆圧縮。アルファは失われる"),
)


class YBR_ImageSettings(bpy.types.PropertyGroup):
    """画像データブロックごとの .ybr 書き出し設定"""

    compression: EnumProperty(
        name="Compression",
        description="この画像を .ybr に埋め込むときの圧縮方式",
        items=(("DEFAULT", "Use Exporter Setting", "エクスポーター側の既定値に従う"),)
        + _COMPRESSION_ITEMS,
        default="DEFAULT",
    )
    quality: IntProperty(
        name="Quality",
        description="JPEG の品質 / PNG の圧縮レベル (0-100)",
        default=90,
        min=0,
        max=100,
    )


def _draw_image_ybr(layout, img):
    st = getattr(img, "ybr", None)
    if st is None:
        layout.label(text="YBR settings unavailable", icon="ERROR")
        return
    col = layout.column(align=True)
    col.prop(st, "compression")
    if st.compression in {"JPEG", "PNG"}:
        col.prop(st, "quality")
    elif st.compression == "DEFAULT":
        col.label(text="Follows the exporter's Texture Compression", icon="INFO")


class IMAGE_PT_ybr(bpy.types.Panel):
    bl_label = "Yui Blender to Raylib"
    bl_space_type = "IMAGE_EDITOR"
    bl_region_type = "UI"
    bl_category = "Image"

    @classmethod
    def poll(cls, context):
        return getattr(context.space_data, "image", None) is not None

    def draw(self, context):
        _draw_image_ybr(self.layout, context.space_data.image)


class NODE_PT_ybr(bpy.types.Panel):
    bl_label = "Yui Blender to Raylib"
    bl_space_type = "NODE_EDITOR"
    bl_region_type = "UI"
    bl_category = "Item"

    @classmethod
    def poll(cls, context):
        node = getattr(context, "active_node", None)
        return node is not None and node.type == "TEX_IMAGE" and node.image is not None

    def draw(self, context):
        _draw_image_ybr(self.layout, context.active_node.image)


class EXPORT_OT_yui_ybr(bpy.types.Operator, ExportHelper):
    """Export scene as Yui Blender to Raylib (.ybr)"""

    bl_idname = "export_scene.yui_ybr"
    bl_label = "Export Yui Blender to Raylib (.ybr)"
    bl_options = {"PRESET"}

    filename_ext = ".ybr"
    filter_glob: StringProperty(default="*.ybr", options={"HIDDEN"})

    use_selection: BoolProperty(
        name="Selection Only", description="選択オブジェクトのみ書き出す", default=False
    )
    skip_hidden_viewport: BoolProperty(
        name="Hide in Viewport",
        description="ビューポートで非表示のオブジェクト / コレクションを除外する",
        default=False,
    )
    skip_hidden_render: BoolProperty(
        name="Disable in Renders",
        description="レンダリングが無効なオブジェクト / コレクションを除外する",
        default=True,
    )
    apply_modifiers: BoolProperty(
        name="Apply Modifiers",
        description="モディファイア適用後の形状を書き出す",
        default=True,
    )
    rest_pose: BoolProperty(
        name="Rest Pose",
        description="アーマチュアモディファイアを無効化してレストポーズを書き出す",
        default=True,
    )
    material_mode: EnumProperty(
        name="Material Mode",
        items=(
            (
                "SIMPLE",
                "Simple",
                "BSDF から Base Color / Specular / Metallic / Roughness / Alpha を抽出",
            ),
            ("PRO", "Pro", "シェーダーノード構成をそのまま書き出す"),
            (
                "NONE",
                "None (dummy)",
                "マテリアルを書き出さない。全メッシュが 1 つのダミー"
                "マテリアルを参照する (テクスチャも出ない)",
            ),
        ),
        default="SIMPLE",
    )
    export_animations: BoolProperty(
        name="Animations",
        description="NLA トラック内の action strip をベイクして書き出す "
        "(オブジェクトに直接設定されている Action は対象外)",
        default=True,
    )
    export_custom_properties: BoolProperty(
        name="Custom Properties",
        description='"ybr_" で始まるカスタムプロパティを書き出す '
        "(プレフィックスは外して格納される)",
        default=True,
    )
    deform_bones_only: BoolProperty(
        name="Deform Bones Only",
        description="Deform が無効なボーンを書き出さない",
        default=True,
    )
    keep_raw_groups: BoolProperty(
        name="Keep Raw Vertex Groups",
        description="正規化前の頂点グループ生データも残す",
        default=False,
    )
    embed_textures: BoolProperty(
        name="Embed Textures",
        description="テクスチャの RGBA データをファイルに埋め込む "
        "(オフのときはパスのみ。再生側で画像を読めないと真っ白になる)",
        default=True,
    )
    texture_compression: EnumProperty(
        name="Texture Compression",
        description="埋め込むテクスチャの既定の圧縮方式 "
        "(画像ごとにサイドバーで上書きできる)",
        items=_COMPRESSION_ITEMS,
        default="PNG",
    )
    texture_quality: IntProperty(
        name="Texture Quality",
        description="JPEG の品質 / PNG の圧縮レベル (0-100)",
        default=90,
        min=0,
        max=100,
    )

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        col = layout.column(heading="Include")
        col.prop(self, "use_selection")
        col.prop(self, "export_animations")
        col.prop(self, "export_custom_properties")

        col = layout.column(heading="Exclude")
        col.prop(self, "skip_hidden_viewport")
        col.prop(self, "skip_hidden_render")

        col = layout.column(heading="Geometry")
        col.prop(self, "apply_modifiers")
        col.prop(self, "rest_pose")
        col.prop(self, "keep_raw_groups")
        col.prop(self, "deform_bones_only")

        col = layout.column()
        col.prop(self, "material_mode")

        col = layout.column(heading="Textures")
        col.prop(self, "embed_textures")
        sub = col.column()
        sub.enabled = self.embed_textures
        sub.prop(self, "texture_compression")
        sub.prop(self, "texture_quality")

    def execute(self, context):
        try:
            n_root, n_data = export_scene(
                context,
                self.filepath,
                self.use_selection,
                self.skip_hidden_viewport,
                self.skip_hidden_render,
                self.apply_modifiers,
                self.rest_pose,
                self.material_mode,
                self.export_animations,
                self.keep_raw_groups,
                self.embed_textures,
                self.texture_compression,
                self.texture_quality,
                self.export_custom_properties,
                self.deform_bones_only,
            )
        except YbrExportError as e:
            self.report({"ERROR"}, "YBR export failed: %s" % e)
            return {"CANCELLED"}
        except Exception as e:
            self.report({"ERROR"}, "YBR export failed: %s" % e)
            raise

        for msg in _warnings[:10]:
            self.report({"WARNING"}, msg)
        if 10 < len(_warnings):
            self.report(
                {"WARNING"},
                "... and %d more warnings (see console)" % (len(_warnings) - 10),
            )

        self.report(
            {"INFO"},
            "YBR export: %d roots / %d data blocks -> %s"
            % (n_root, n_data, os.path.basename(self.filepath)),
        )
        return {"FINISHED"}


def menu_func_export(self, context):
    self.layout.operator(
        EXPORT_OT_yui_ybr.bl_idname, text="Yui Blender to Raylib (.ybr)"
    )


classes = (YBR_ImageSettings, EXPORT_OT_yui_ybr, IMAGE_PT_ybr, NODE_PT_ybr)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Image.ybr = PointerProperty(type=YBR_ImageSettings)
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)


def unregister():
    """後始末。途中まで登録されている状態でも通るようにしておく
    (テストのように何度も登録 / 解除する使い方があるため)。"""
    try:
        bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)
    except Exception:
        pass
    if hasattr(bpy.types.Image, "ybr"):
        del bpy.types.Image.ybr
    for c in reversed(classes):
        try:
            bpy.utils.unregister_class(c)
        except Exception:
            pass


if __name__ == "__main__":
    register()
