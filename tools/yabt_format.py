"""
Yui Blender to Raylib - Yabt (アニメーション合成ツリー) の読み書き

YABT_FORMAT.md と src/lib/ybr_yabt.c に合わせてある。
ノードは仕様のキーをそのまま持つ dict として扱う (データ構造は変えない)。
"""

import os

from yabt_cbor import CborError, dumps, loads

__all__ = [
    "YabtError",
    "YABT_MAGIC", "YABT_VERSION", "YABT_MAX_DEPTH",
    "ABN_SOURCE", "ABN_LERP", "ABN_ADD", "ABN_TRANSITION",
    "ALM_ONE_SHOT", "ALM_LOOP",
    "NODE_TYPE_NAMES", "LOOP_MODE_NAMES",
    "load_animation_names", "load_bone_names", "load_ybr_info",
    "load_yabt", "save_yabt", "dumps_yabt",
    "check_tree",
]

YABT_MAGIC = "YABT"
YABT_VERSION = 1
YABT_MAX_DEPTH = 64

YBR_MAGIC = "YUI"
YBR_VERSION = 1

ABN_SOURCE = 0
ABN_LERP = 1
ABN_ADD = 2
ABN_TRANSITION = 3

ALM_ONE_SHOT = 0
ALM_LOOP = 1

NODE_TYPE_NAMES = {
    ABN_SOURCE: "ソース",
    ABN_LERP: "線形補間",
    ABN_ADD: "加算",
    ABN_TRANSITION: "遷移",
}

LOOP_MODE_NAMES = {
    ALM_ONE_SHOT: "1回だけ",
    ALM_LOOP: "ループ",
}


class YabtError(Exception):
    pass


# ----------------------------------------------------------------------------
# .ybr からアニメーション名を集める


def load_ybr_info(path):
    """.ybr からアニメーション名とボーン名を並び順のまま取り出す。"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise YabtError("'%s' を開けません (%s)" % (os.path.basename(path), e))

    try:
        # バイト列 (メッシュや埋め込みテクスチャ) は読み飛ばす
        root = loads(data, skip_bytes=True)
    except CborError as e:
        raise YabtError("YBR: CBOR を読めません (%s)" % e)

    if not isinstance(root, list) or len(root) < 4:
        raise YabtError("YBR: 先頭が 4 要素の配列ではありません")
    if root[0] != YBR_MAGIC:
        raise YabtError("YBR: マジックが違います (\"%s\" のはず)" % YBR_MAGIC)
    if root[1] != YBR_VERSION:
        raise YabtError("YBR: 対応していないバージョンです (%s)" % root[1])

    blocks = root[3]
    if not isinstance(blocks, list):
        raise YabtError("YBR: データブロックが配列ではありません")

    animations = []
    bones = []
    for block in blocks:
        if not isinstance(block, dict):
            continue
        kind = block.get("kind")
        if kind == "ANIMATION":
            name = block.get("id")
            if isinstance(name, str) and name and name not in animations:
                animations.append(name)
        elif kind == "ARMATURE":
            # アーマチュアは 0 個か 1 個 (ybr.h の決まり)。
            # 2 つ以上あったら先頭だけを見る。
            if bones:
                continue
            for bone in block.get("bones") or []:
                if not isinstance(bone, dict):
                    continue
                name = bone.get("name")
                if isinstance(name, str) and name and name not in bones:
                    bones.append(name)
    return animations, bones


def load_animation_names(path):
    """.ybr に入っているアニメーションの id を並び順のまま返す。"""
    return load_ybr_info(path)[0]


def load_bone_names(path):
    """.ybr のアーマチュアに入っているボーン名を並び順のまま返す。"""
    return load_ybr_info(path)[1]


# ----------------------------------------------------------------------------
# 読み込み


def _as_int(value, default):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return default
    return int(value)


def _as_float(value, default):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return default
    return float(value)


def _load_node(value, depth):
    if YABT_MAX_DEPTH < depth:
        raise YabtError("ツリーが %d より深くなっています" % YABT_MAX_DEPTH)
    if not isinstance(value, dict):
        raise YabtError("ノードがマップではありません")

    ntype = _as_int(value.get("type"), -1)
    # 付けていないときは空文字。文字列以外が来たら空文字として扱う。
    unique_id = value.get("unique_id")
    if not isinstance(unique_id, str):
        unique_id = ""

    if ntype == ABN_SOURCE:
        name = value.get("name")
        if not isinstance(name, str) or not name:
            raise YabtError("ソースノードに 'name' がありません")
        loop = _as_int(value.get("loop_mode"), ALM_ONE_SHOT)
        if loop not in (ALM_ONE_SHOT, ALM_LOOP):
            raise YabtError("知らない loop_mode です (%d)" % loop)
        speed = _as_float(value.get("play_speed"), 1.0)
        if not (0.0 < speed):
            speed = 1.0
        return {
            "type": ABN_SOURCE,
            "unique_id": unique_id,
            "name": name,
            "loop_mode": loop,
            "play_speed": speed,
        }

    if ntype in (ABN_LERP, ABN_ADD):
        weight = _as_float(value.get("weight"), 0.0)
        weight = min(max(weight, 0.0), 1.0)

        bones = value.get("filtered_bones")
        filtered = []
        if bones is not None:
            if not isinstance(bones, list):
                raise YabtError("'filtered_bones' が配列でも null でもありません")
            for bone in bones:
                if not isinstance(bone, str):
                    raise YabtError("'filtered_bones' の要素が文字列ではありません")
                filtered.append(bone)

        if "input" not in value or "mix_input" not in value:
            raise YabtError("'input' と 'mix_input' の両方が要ります")
        return {
            "type": ntype,
            "unique_id": unique_id,
            "weight": weight,
            "filtered_bones": filtered,
            "input": _load_node(value["input"], depth + 1),
            "mix_input": _load_node(value["mix_input"], depth + 1),
        }

    if ntype == ABN_TRANSITION:
        inputs = value.get("inputs")
        if not isinstance(inputs, list) or not inputs:
            raise YabtError("遷移ノードには 1 個以上の 'inputs' が要ります")
        loaded = [_load_node(item, depth + 1) for item in inputs]

        index = _as_int(value.get("index"), 0)
        if index < 0 or len(loaded) <= index:
            index = 0
        seconds = _as_float(value.get("transition_seconds"), 0.0)
        if not (0.0 <= seconds):
            seconds = 0.0
        return {
            "type": ABN_TRANSITION,
            "unique_id": unique_id,
            "index": index,
            "transition_seconds": seconds,
            "inputs": loaded,
        }

    raise YabtError("知らないノード種別です (%d)" % ntype)


def load_yabt(path):
    """.yabt を読んで根のノードを返す。"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise YabtError("'%s' を開けません (%s)" % (os.path.basename(path), e))

    if len(data) < 4:
        raise YabtError("YABT: 中身がありません")

    try:
        root = loads(data)
    except CborError as e:
        raise YabtError("YABT: CBOR を読めません (%s)" % e)

    if not isinstance(root, list) or len(root) < 3:
        raise YabtError("YABT: 先頭が 3 要素の配列ではありません")
    if root[0] != YABT_MAGIC:
        raise YabtError("YABT: マジックが違います (\"%s\" のはず)" % YABT_MAGIC)
    if root[1] != YABT_VERSION:
        raise YabtError(
            "YABT: 対応していないバージョンです (%s / %d のはず)"
            % (root[1], YABT_VERSION))

    return _load_node(root[2], 0)


# ----------------------------------------------------------------------------
# 書き出し


def check_tree(node, depth=0):
    """書ける形か調べる。ybr_yabt.c の check_node() と同じ検査。"""
    if YABT_MAX_DEPTH < depth:
        raise YabtError("ツリーが %d より深くなっています" % YABT_MAX_DEPTH)
    if node is None:
        raise YabtError("つながっていない入力があります")

    ntype = node.get("type")
    if ntype == ABN_SOURCE:
        if not node.get("name"):
            raise YabtError("名前の決まっていないソースノードがあります")
        return
    if ntype in (ABN_LERP, ABN_ADD):
        check_tree(node.get("input"), depth + 1)
        check_tree(node.get("mix_input"), depth + 1)
        return
    if ntype == ABN_TRANSITION:
        inputs = node.get("inputs") or []
        if not inputs:
            raise YabtError("入力の無い遷移ノードがあります")
        for item in inputs:
            check_tree(item, depth + 1)
        return
    raise YabtError("知らないノード種別です (%s)" % ntype)


def _write_node(node):
    """ybr_yabt.c の write_node() と同じキーの順で並べる。"""
    ntype = node["type"]

    if ntype == ABN_SOURCE:
        return {
            "type": ABN_SOURCE,
            "unique_id": str(node.get("unique_id") or ""),
            "name": node["name"],
            "loop_mode": int(node.get("loop_mode", ALM_ONE_SHOT)),
            "play_speed": float(node.get("play_speed", 1.0)),
        }

    if ntype in (ABN_LERP, ABN_ADD):
        bones = list(node.get("filtered_bones") or [])
        return {
            "type": ntype,
            "unique_id": str(node.get("unique_id") or ""),
            "weight": float(node.get("weight", 0.0)),
            "filtered_bones": bones if bones else None,
            "input": _write_node(node["input"]),
            "mix_input": _write_node(node["mix_input"]),
        }

    if ntype == ABN_TRANSITION:
        return {
            "type": ABN_TRANSITION,
            "unique_id": str(node.get("unique_id") or ""),
            "index": int(node.get("index", 0)),
            "transition_seconds": float(node.get("transition_seconds", 0.0)),
            "inputs": [_write_node(item) for item in node["inputs"]],
        }

    raise YabtError("知らないノード種別です (%s)" % ntype)


def dumps_yabt(root):
    """根のノードを .yabt のバイト列にする。"""
    check_tree(root, 0)
    return dumps([YABT_MAGIC, YABT_VERSION, _write_node(root)])


def save_yabt(path, root):
    data = dumps_yabt(root)
    try:
        with open(path, "wb") as f:
            f.write(data)
    except OSError as e:
        raise YabtError("'%s' に書けません (%s)" % (os.path.basename(path), e))
