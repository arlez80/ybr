"""
Yui Blender to Raylib - 最小 CBOR (RFC 8949) リーダー / ライター

src/lib/ybr_cbor.c と同じ範囲だけを扱う (定義長のみ / 不定長は扱わない)。
標準ライブラリ以外は使わない。
"""

import struct

__all__ = ["CborError", "CborBytes", "loads", "dumps", "MAX_DEPTH"]

# 入れ子の深さの上限 (ybr_cbor.h の CBOR_MAX_DEPTH と同じ)
MAX_DEPTH = 64


class CborError(Exception):
    pass


class CborBytes:
    """skip_bytes=True のときのバイト列の代わり。長さだけ覚えておく。

    .ybr には埋め込みテクスチャのような大きなバイト列が入っている。
    アニメーション名を数えるだけのときに丸ごと複製したくないので、
    読み飛ばして長さだけ持つ。
    """

    __slots__ = ("length",)

    def __init__(self, length):
        self.length = length

    def __repr__(self):
        return "CborBytes(%d)" % self.length


# ----------------------------------------------------------------------------
# リーダー


def loads(data, skip_bytes=False):
    """data 全体を 1 個の CBOR 値として読む。"""
    mv = memoryview(bytes(data))
    value, off = _decode(mv, 0, 0, skip_bytes)
    if off != len(mv):
        raise CborError("末尾に余分なデータがあります (%d バイト)" % (len(mv) - off))
    return value


def _need(mv, off, count):
    if len(mv) < off + count:
        raise CborError("データが途中で終わっています")


def _uint(mv, off, size):
    _need(mv, off, size)
    v = 0
    for i in range(size):
        v = (v << 8) | mv[off + i]
    return v, off + size


def _half_to_float(bits):
    exp = (bits >> 10) & 0x1F
    mant = bits & 0x3FF
    if exp == 0:
        value = mant * 2.0 ** -24
    elif exp == 31:
        value = float("inf") if mant == 0 else float("nan")
    else:
        value = (mant + 1024) * 2.0 ** (exp - 25)
    return -value if (bits & 0x8000) else value


def _decode(mv, off, depth, skip_bytes):
    if MAX_DEPTH < depth:
        raise CborError("入れ子が %d より深くなっています" % MAX_DEPTH)
    _need(mv, off, 1)

    ib = mv[off]
    off += 1
    major = ib >> 5
    minor = ib & 0x1F

    if minor < 24:
        arg = minor
    elif minor == 24:
        arg, off = _uint(mv, off, 1)
    elif minor == 25:
        arg, off = _uint(mv, off, 2)
    elif minor == 26:
        arg, off = _uint(mv, off, 4)
    elif minor == 27:
        arg, off = _uint(mv, off, 8)
    elif minor == 31:
        raise CborError("不定長のデータは扱いません")
    else:
        raise CborError("壊れたヘッダです (0x%02X)" % ib)

    if major == 0:
        return arg, off
    if major == 1:
        return -1 - arg, off

    if major == 2:
        _need(mv, off, arg)
        if skip_bytes:
            return CborBytes(arg), off + arg
        return bytes(mv[off:off + arg]), off + arg

    if major == 3:
        _need(mv, off, arg)
        raw = bytes(mv[off:off + arg])
        try:
            return raw.decode("utf-8"), off + arg
        except UnicodeDecodeError:
            raise CborError("文字列が UTF-8 ではありません")

    if major == 4:
        items = []
        for _ in range(arg):
            value, off = _decode(mv, off, depth + 1, skip_bytes)
            items.append(value)
        return items, off

    if major == 5:
        out = {}
        for _ in range(arg):
            key, off = _decode(mv, off, depth + 1, skip_bytes)
            value, off = _decode(mv, off, depth + 1, skip_bytes)
            if isinstance(key, (str, int, bytes)):
                out[key] = value
        return out, off

    if major == 7:
        if minor == 20:
            return False, off
        if minor == 21:
            return True, off
        if minor == 22:
            return None, off
        if minor == 23:
            return None, off
        if minor == 25:
            return _half_to_float(arg), off
        if minor == 26:
            return struct.unpack(">f", struct.pack(">I", arg))[0], off
        if minor == 27:
            return struct.unpack(">d", struct.pack(">Q", arg))[0], off
        raise CborError("扱えない単純値です (%d)" % minor)

    raise CborError("扱えないメジャータイプです (%d)" % major)


# ----------------------------------------------------------------------------
# ライター


def dumps(value):
    """CBOR のバイト列にする。ybr_cbor.c の書き方に合わせてある。"""
    out = bytearray()
    _encode(out, value, 0)
    return bytes(out)


def _head(out, major, arg):
    if arg < 24:
        out.append((major << 5) | arg)
    elif arg < 0x100:
        out.append((major << 5) | 24)
        out.append(arg)
    elif arg < 0x10000:
        out.append((major << 5) | 25)
        out += struct.pack(">H", arg)
    elif arg < 0x100000000:
        out.append((major << 5) | 26)
        out += struct.pack(">I", arg)
    else:
        out.append((major << 5) | 27)
        out += struct.pack(">Q", arg)


def _encode(out, value, depth):
    if MAX_DEPTH < depth:
        raise CborError("入れ子が %d より深くなっています" % MAX_DEPTH)

    # bool は int より先に見ること (Python では bool は int の一種)
    if value is True:
        out.append(0xF5)
        return
    if value is False:
        out.append(0xF4)
        return
    if value is None:
        out.append(0xF6)
        return

    if isinstance(value, int):
        if 0 <= value:
            _head(out, 0, value)
        else:
            _head(out, 1, -1 - value)
        return

    if isinstance(value, float):
        # 書き出しは常に倍精度 (CborWriteFloat と同じ)
        out.append(0xFB)
        out += struct.pack(">d", value)
        return

    if isinstance(value, str):
        raw = value.encode("utf-8")
        _head(out, 3, len(raw))
        out += raw
        return

    if isinstance(value, (bytes, bytearray)):
        _head(out, 2, len(value))
        out += value
        return

    if isinstance(value, (list, tuple)):
        _head(out, 4, len(value))
        for item in value:
            _encode(out, item, depth + 1)
        return

    if isinstance(value, dict):
        _head(out, 5, len(value))
        for key, item in value.items():
            _encode(out, key, depth + 1)
            _encode(out, item, depth + 1)
        return

    raise CborError("書き出せない型です (%s)" % type(value).__name__)
