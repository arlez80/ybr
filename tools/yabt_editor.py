#!/usr/bin/env python3
"""
Yui Blender to Raylib - YABT ノードエディタ

  python3 tools/yabt_editor.py

アニメーション合成ツリー (.yabt) を Blender のシェーダーノードエディタのような
見た目で組み立てる。編集を始める前に .ybr を読み込む必要がある
(ソースノードで選ぶアニメーションと、合成から外すボーンの一覧を .ybr から取る)。
アニメーションとボーンのコンボボックスは打ち込んで絞り込める。

ノードの位置はファイルに入らないので、読み込んだときだけ自動で並べる。
そのあとは自由に動かせる。

  マウス
    左ドラッグ (ソケット)  つなぐ / 外す
    左ドラッグ (ノード)    動かす
    左ドラッグ (背景)      スクロール
    中ドラッグ             スクロール
    ホイール               拡大 / 縮小
    右クリック (背景)      ノードを追加
  キー
    Shift+A                ノードを追加
    Delete                 選んでいるノードを消す
    Ctrl+N / O / S         新規 / 開く / 上書き保存
    Ctrl+Shift+S           保存 (名前を付けて)
    Ctrl+Z / Ctrl+Y        元に戻す / やり直す
"""

import copy
import os
import sys

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from yabt_format import (  # noqa: E402
    ABN_ADD, ABN_LERP, ABN_SOURCE, ABN_TRANSITION,
    ALM_LOOP, ALM_ONE_SHOT,
    LOOP_MODE_NAMES, NODE_TYPE_NAMES,
    YABT_MAX_DEPTH, YabtError,
    load_yabt, load_ybr_info, save_yabt,
)

APP_TITLE = "YABT ノードエディタ"

# 見た目 (Blender のダークテーマに寄せた色)
COL_BG = "#1d1d1d"
COL_GRID = "#282828"
COL_GRID_BIG = "#333333"
COL_NODE = "#303030"
COL_NODE_SEL = "#4a4a4a"
COL_OUTLINE = "#181818"
COL_OUTLINE_SEL = "#ffa028"
COL_TEXT = "#e8e8e8"
COL_TEXT_DIM = "#a0a0a0"
COL_LINK = "#c8c8c8"
COL_LINK_TEMP = "#ffa028"
COL_SOCKET = "#a1a1a1"
COL_SOCKET_OUT = "#c8a165"
COL_ERROR = "#e05a5a"

NODE_HEADER = {
    ABN_SOURCE: "#4a7a3a",
    ABN_LERP: "#3a5f8a",
    ABN_ADD: "#6a4a8a",
    ABN_TRANSITION: "#8a6030",
}
COL_OUTPUT_HEADER = "#8a3030"

NODE_W = 200
HEADER_H = 24
ROW_H = 20
LINE_H = 16
PAD = 6
COL_GAP = 70
ROW_GAP = 26
SOCKET_R = 5

TYPE_OUTPUT = -1  # エディタの中だけで使う「出力」ノード


# ----------------------------------------------------------------------------
# データ


class ENode:
    """編集中のノード。ファイルに書くときは仕様どおりの dict へ直す。"""

    def __init__(self, ntype, nid):
        self.nid = nid
        self.type = ntype
        self.unique_id = ""
        self.x = 0.0
        self.y = 0.0
        self.children = []  # 入力ソケットにつながっているノード (None は空)

        # ソース
        self.name = ""
        self.loop_mode = ALM_ONE_SHOT
        self.play_speed = 1.0
        # 線形補間 / 加算
        self.weight = 0.0
        self.filtered_bones = []
        # 遷移
        self.index = 0
        self.transition_seconds = 0.0

        if ntype == TYPE_OUTPUT:
            self.children = [None]
        elif ntype in (ABN_LERP, ABN_ADD):
            self.children = [None, None]
        elif ntype == ABN_TRANSITION:
            self.children = [None, None]

    # --- 見た目 ---

    def title(self):
        if self.type == TYPE_OUTPUT:
            return "出力"
        return NODE_TYPE_NAMES.get(self.type, "?")

    def header_color(self):
        if self.type == TYPE_OUTPUT:
            return COL_OUTPUT_HEADER
        return NODE_HEADER.get(self.type, "#555555")

    def has_output(self):
        return self.type != TYPE_OUTPUT

    def socket_labels(self):
        if self.type == TYPE_OUTPUT:
            return ["ツリー"]
        if self.type == ABN_LERP:
            return ["入力", "ブレンド入力"]
        if self.type == ABN_ADD:
            return ["入力", "加算入力"]
        if self.type == ABN_TRANSITION:
            return ["入力 %d" % i for i in range(len(self.children))]
        return []

    def summary(self):
        """ノードの中に出す短い説明。"""
        if self.type == ABN_SOURCE:
            name = self.name if self.name else "(未設定)"
            return [name,
                    "%s  x%.2f" % (LOOP_MODE_NAMES[self.loop_mode],
                                   self.play_speed)]
        if self.type in (ABN_LERP, ABN_ADD):
            lines = ["重み %.2f" % self.weight]
            if self.filtered_bones:
                lines.append("除外 %d 本" % len(self.filtered_bones))
            return lines
        if self.type == ABN_TRANSITION:
            return ["今 %d / %d" % (self.index, len(self.children)),
                    "遷移 %.2f 秒" % self.transition_seconds]
        return []

    def height(self):
        rows = (1 if self.has_output() else 0) + len(self.children)
        return (HEADER_H + PAD + rows * ROW_H +
                len(self.summary()) * LINE_H + PAD)


class Document:
    def __init__(self, ybr_path, animations, bones):
        self.ybr_path = ybr_path
        self.animations = animations
        self.bones = bones
        self.path = None
        self.dirty = False
        self._next_nid = 1
        self.output = ENode(TYPE_OUTPUT, self.new_nid())
        self.output.x = 520.0
        self.output.y = 40.0
        self.nodes = [self.output]

    def new_nid(self):
        nid = self._next_nid
        self._next_nid += 1
        return nid

    def find(self, nid):
        for node in self.nodes:
            if node.nid == nid:
                return node
        return None

    def parent_of(self, node):
        """(親ノード, ソケット番号) を返す。つながっていなければ (None, -1)。"""
        for other in self.nodes:
            for i, child in enumerate(other.children):
                if child is node:
                    return other, i
        return None, -1

    def detach(self, node):
        parent, slot = self.parent_of(node)
        if parent:
            parent.children[slot] = None

    def connect(self, parent, slot, child):
        self.detach(child)
        parent.children[slot] = child

    def is_descendant(self, node, maybe):
        """maybe が node の子孫 (または node 自身) か。輪を作らないための確認。"""
        if node is maybe:
            return True
        for child in node.children:
            if child is not None and self.is_descendant(child, maybe):
                return True
        return False

    def remove(self, node):
        if node is self.output:
            return
        self.detach(node)
        # 子はつながりが切れて浮くだけ
        node.children = [None] * len(node.children)
        self.nodes.remove(node)

    def reachable(self):
        seen = set()

        def walk(node):
            if node is None or node.nid in seen:
                return
            seen.add(node.nid)
            for child in node.children:
                walk(child)

        walk(self.output)
        return seen

    def root(self):
        return self.output.children[0]

    # --- ファイルとのやりとり ---

    def to_tree(self, node):
        if node is None:
            raise YabtError("つながっていない入力があります")
        if node.type == ABN_SOURCE:
            return {
                "type": ABN_SOURCE,
                "unique_id": node.unique_id or "",
                "name": node.name,
                "loop_mode": node.loop_mode,
                "play_speed": node.play_speed,
            }
        if node.type in (ABN_LERP, ABN_ADD):
            return {
                "type": node.type,
                "unique_id": node.unique_id or "",
                "weight": node.weight,
                "filtered_bones": list(node.filtered_bones),
                "input": self.to_tree(node.children[0]),
                "mix_input": self.to_tree(node.children[1]),
            }
        if node.type == ABN_TRANSITION:
            if not node.children:
                raise YabtError("入力の無い遷移ノードがあります")
            return {
                "type": ABN_TRANSITION,
                "unique_id": node.unique_id or "",
                "index": node.index,
                "transition_seconds": node.transition_seconds,
                "inputs": [self.to_tree(c) for c in node.children],
            }
        raise YabtError("知らないノード種別です (%s)" % node.type)

    def from_tree(self, tree, depth=0):
        if YABT_MAX_DEPTH < depth:
            raise YabtError("ツリーが %d より深くなっています" % YABT_MAX_DEPTH)
        node = ENode(tree["type"], self.new_nid())
        node.unique_id = tree.get("unique_id") or ""
        if node.type == ABN_SOURCE:
            node.name = tree.get("name", "")
            node.loop_mode = tree.get("loop_mode", ALM_ONE_SHOT)
            node.play_speed = tree.get("play_speed", 1.0)
        elif node.type in (ABN_LERP, ABN_ADD):
            node.weight = tree.get("weight", 0.0)
            node.filtered_bones = list(tree.get("filtered_bones") or [])
            node.children = [self.from_tree(tree["input"], depth + 1),
                             self.from_tree(tree["mix_input"], depth + 1)]
        elif node.type == ABN_TRANSITION:
            node.index = tree.get("index", 0)
            node.transition_seconds = tree.get("transition_seconds", 0.0)
            node.children = [self.from_tree(t, depth + 1)
                             for t in tree["inputs"]]
        self.nodes.append(node)
        return node


# ----------------------------------------------------------------------------
# 位置の自動計算


def layout(doc):
    """ノードの位置を自動で決める。読み込んだ直後にだけ呼ぶ。

    根 (出力ノード) をいちばん右に置き、入力をたどるごとに左へずらす。
    縦は葉から順に詰めていき、親は子の真ん中に置く。
    最後に列ごとに重なりをほどくので、必ず離れて並ぶ。
    """
    depth = {}
    order = []  # 葉から順に見た並び

    def walk(node, d, seen):
        if node.nid in seen:
            return
        seen.add(node.nid)
        depth[node.nid] = d
        for child in node.children:
            if child is not None:
                walk(child, d + 1, seen)
        order.append(node)

    seen = set()
    walk(doc.output, 0, seen)
    # つながっていないノードも、それぞれの根として並べる
    for node in list(doc.nodes):
        if node.nid not in seen and doc.parent_of(node)[0] is None:
            walk(node, 0, seen)
    for node in list(doc.nodes):  # 念のため取りこぼしを拾う
        if node.nid not in seen:
            walk(node, 0, seen)

    # 縦 : 葉に順番に場所を割り当て、親は子の中央へ
    cursor = [0.0]
    y = {}

    def place(node, done):
        if node.nid in done:
            return
        done.add(node.nid)
        kids = [c for c in node.children if c is not None]
        for child in kids:
            place(child, done)
        if kids:
            top = min(y[c.nid] for c in kids)
            bottom = max(y[c.nid] + c.height() for c in kids)
            y[node.nid] = (top + bottom) * 0.5 - node.height() * 0.5
        else:
            y[node.nid] = cursor[0]
            cursor[0] += node.height() + ROW_GAP

    done = set()
    for node in order:
        place(node, done)

    # 横 : 深さで列を決める
    max_depth = max(depth.values()) if depth else 0
    for node in doc.nodes:
        d = depth.get(node.nid, 0)
        node.x = (max_depth - d) * (NODE_W + COL_GAP)
        node.y = y.get(node.nid, 0.0)

    # 列ごとに重なりをほどく
    columns = {}
    for node in doc.nodes:
        columns.setdefault(depth.get(node.nid, 0), []).append(node)
    for nodes in columns.values():
        nodes.sort(key=lambda n: n.y)
        bottom = None
        for node in nodes:
            if bottom is not None and node.y < bottom + ROW_GAP:
                node.y = bottom + ROW_GAP
            bottom = node.y + node.height()


def filter_names(names, query):
    """検索文字で候補を絞る。空なら全部。大文字小文字は区別しない。
    前方一致を先に、その後に部分一致を並べる。"""
    q = query.strip().lower()
    if not q:
        return list(names)
    head = [n for n in names if n.lower().startswith(q)]
    tail = [n for n in names if q in n.lower() and n not in head]
    return head + tail


def resolve_name(names, query):
    """検索文字から 1 つに決める。決まらなければ None。"""
    text = query.strip()
    for name in names:
        if name == text:
            return name
    found = filter_names(names, text)
    return found[0] if len(found) == 1 else None


def free_spot(doc, x, y):
    """どのノードとも重ならない場所を探す。ノードを足すときに使う。"""
    for _ in range(400):
        hit = False
        for node in doc.nodes:
            if (x < node.x + NODE_W and node.x < x + NODE_W and
                    y < node.y + node.height() and node.y < y + HEADER_H * 3):
                hit = True
                break
        if not hit:
            return x, y
        y += ROW_GAP + HEADER_H
    return x, y


# ----------------------------------------------------------------------------
# アプリ


class Editor:
    def __init__(self, root):
        self.root = root
        self.doc = None
        self.selected = None
        self.undo_stack = []
        self.redo_stack = []

        self.zoom = 1.0
        self.pan_x = 60.0
        self.pan_y = 60.0

        self._drag = None      # 背景スクロール
        self._move = None      # ノードの移動
        self._link = None      # つなぎ替え中
        self._last_menu_pos = None
        self._sockets = []     # 当たり判定用 (node, kind, slot, wx, wy)
        self._panel_widgets = []
        self._suspend = False  # パネルを作り直している間は反映しない

        root.title(APP_TITLE)
        root.geometry("1280x800")
        root.protocol("WM_DELETE_WINDOW", self.on_quit)

        self._build_menu()
        self._build_body()
        self._bind_keys()
        self.refresh()

    # --- 組み立て ---

    def _build_menu(self):
        bar = tk.Menu(self.root)

        self.file_menu = tk.Menu(bar, tearoff=0)
        self.file_menu.add_command(label="新規", accelerator="Ctrl+N",
                                   command=self.on_new)
        self.file_menu.add_command(label="開く", accelerator="Ctrl+O",
                                   command=self.on_open)
        self.file_menu.add_separator()
        self.file_menu.add_command(label="上書き保存", accelerator="Ctrl+S",
                                   command=self.on_save)
        self.file_menu.add_command(label="保存", accelerator="Ctrl+Shift+S",
                                   command=self.on_save_as)
        self.file_menu.add_separator()
        self.file_menu.add_command(label="閉じる", command=self.on_close)
        bar.add_cascade(label="ファイル", menu=self.file_menu)

        self.edit_menu = tk.Menu(bar, tearoff=0)
        self.edit_menu.add_command(label="元に戻す", accelerator="Ctrl+Z",
                                   command=self.on_undo)
        self.edit_menu.add_command(label="やり直す", accelerator="Ctrl+Y",
                                   command=self.on_redo)
        bar.add_cascade(label="編集", menu=self.edit_menu)

        self.root.config(menu=bar)

        self.add_menu = tk.Menu(self.root, tearoff=0)
        for ntype in (ABN_SOURCE, ABN_LERP, ABN_ADD, ABN_TRANSITION):
            self.add_menu.add_command(
                label=NODE_TYPE_NAMES[ntype],
                command=lambda t=ntype: self.add_node(t))

    def _build_body(self):
        body = tk.Frame(self.root)
        body.pack(fill="both", expand=True)

        self.canvas = tk.Canvas(body, bg=COL_BG, highlightthickness=0,
                                takefocus=1)
        self.canvas.pack(side="left", fill="both", expand=True)

        side = tk.Frame(body, width=280, bg="#252525")
        side.pack(side="right", fill="y")
        side.pack_propagate(False)
        self.side = side

        tk.Label(side, text="プロパティ", bg="#252525", fg=COL_TEXT,
                 anchor="w", padx=8, pady=6).pack(fill="x")
        self.panel = tk.Frame(side, bg="#252525")
        self.panel.pack(fill="both", expand=True, padx=8)

        self.status = tk.Label(self.root, text="", anchor="w", padx=8,
                               bg="#202020", fg=COL_TEXT_DIM)
        self.status.pack(fill="x", side="bottom")

        self.canvas.bind("<Configure>", lambda e: self.redraw())
        self.canvas.bind("<ButtonPress-1>", self.on_press)
        self.canvas.bind("<B1-Motion>", self.on_motion)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.canvas.bind("<ButtonPress-2>", self.on_pan_start)
        self.canvas.bind("<B2-Motion>", self.on_pan_move)
        self.canvas.bind("<ButtonPress-3>", self.on_context)
        self.canvas.bind("<MouseWheel>", self.on_wheel)
        self.canvas.bind("<Button-4>", lambda e: self.on_wheel(e, 120))
        self.canvas.bind("<Button-5>", lambda e: self.on_wheel(e, -120))

    def _bind_keys(self):
        self.root.bind("<Control-n>", lambda e: self.on_new())
        self.root.bind("<Control-o>", lambda e: self.on_open())
        self.root.bind("<Control-s>", lambda e: self.on_save())
        self.root.bind("<Control-S>", lambda e: self.on_save_as())
        self.root.bind("<Control-z>", lambda e: self.on_undo())
        self.root.bind("<Control-y>", lambda e: self.on_redo())
        self.root.bind("<Control-Z>", lambda e: self.on_redo())
        self.canvas.bind("<Delete>", lambda e: self.delete_selected())
        self.canvas.bind("<KeyPress-A>", lambda e: self.popup_add(None))

    # --- 状態 ---

    def refresh(self):
        editable = self.doc is not None
        state = "normal" if editable else "disabled"
        for label in ("上書き保存", "保存", "閉じる"):
            self.file_menu.entryconfig(label, state=state)
        self.edit_menu.entryconfig(
            "元に戻す", state="normal" if self.undo_stack else "disabled")
        self.edit_menu.entryconfig(
            "やり直す", state="normal" if self.redo_stack else "disabled")

        title = APP_TITLE
        if self.doc:
            name = os.path.basename(self.doc.path) if self.doc.path else "無題"
            title = "%s%s - %s [%s]" % (
                "*" if self.doc.dirty else "", name, APP_TITLE,
                os.path.basename(self.doc.ybr_path))
        self.root.title(title)

        if self.doc:
            floating = len(self.doc.nodes) - len(self.doc.reachable())
            msg = "ノード %d 個" % (len(self.doc.nodes) - 1)
            if self.doc.root() is None:
                msg += "  /  出力に何もつながっていません"
            if 0 < floating:
                msg += "  /  つながっていないノード %d 個 (保存されません)" % floating
            msg += "  /  アニメーション %d 本" % len(self.doc.animations)
        else:
            msg = "「ファイル」→「新規」か「開く」で .ybr を選ぶと編集できます"
        self.status.config(text=msg)

        self.build_panel()
        self.redraw()

    def push_undo(self):
        if not self.doc:
            return
        self.undo_stack.append(self.snapshot())
        if 200 < len(self.undo_stack):
            self.undo_stack.pop(0)
        self.redo_stack.clear()
        self.doc.dirty = True

    def snapshot(self):
        sel = self.selected.nid if self.selected else None
        return (copy.deepcopy((self.doc.nodes, self.doc.output)),
                self.doc._next_nid, self.doc.path, self.doc.dirty, sel)

    def restore(self, snap):
        (nodes, output), next_nid, path, dirty, sel = snap
        nodes, output = copy.deepcopy((nodes, output))
        self.doc.nodes = nodes
        self.doc.output = output
        self.doc._next_nid = next_nid
        self.doc.path = path
        self.doc.dirty = dirty
        self.selected = self.doc.find(sel) if sel is not None else None

    # --- ファイルメニュー ---

    def ask_ybr(self):
        path = filedialog.askopenfilename(
            title="YBR を選ぶ (アニメーションの一覧を読みます)",
            filetypes=[("YBR ファイル", "*.ybr"), ("すべて", "*.*")])
        if not path:
            return None
        try:
            animations, bones = load_ybr_info(path)
        except YabtError as e:
            messagebox.showerror(APP_TITLE, str(e))
            return None
        if not animations:
            if not messagebox.askyesno(
                    APP_TITLE,
                    "この YBR にアニメーションが入っていません。\n"
                    "ソースノードで名前を選べませんが、続けますか?"):
                return None
        return path, animations, bones

    def confirm_discard(self):
        if not self.doc or not self.doc.dirty:
            return True
        answer = messagebox.askyesnocancel(
            APP_TITLE, "保存していない変更があります。保存しますか?")
        if answer is None:
            return False
        if answer:
            return self.on_save()
        return True

    def on_new(self):
        if not self.confirm_discard():
            return
        picked = self.ask_ybr()
        if not picked:
            return
        path, animations, bones = picked
        self.doc = Document(path, animations, bones)
        self.selected = None
        self.undo_stack.clear()
        self.redo_stack.clear()
        self.reset_view()
        self.refresh()

    def on_open(self):
        if not self.confirm_discard():
            return
        picked = self.ask_ybr()
        if not picked:
            return
        ybr_path, animations, bones = picked

        yabt_path = filedialog.askopenfilename(
            title="YABT を開く",
            filetypes=[("YABT ファイル", "*.yabt"), ("すべて", "*.*")])
        if not yabt_path:
            return
        try:
            tree = load_yabt(yabt_path)
        except YabtError as e:
            messagebox.showerror(APP_TITLE, str(e))
            return

        doc = Document(ybr_path, animations, bones)
        try:
            doc.output.children[0] = doc.from_tree(tree)
        except (YabtError, KeyError, TypeError) as e:
            messagebox.showerror(APP_TITLE, "読み込めません (%s)" % e)
            return
        doc.path = yabt_path
        layout(doc)

        self.doc = doc
        self.selected = None
        self.undo_stack.clear()
        self.redo_stack.clear()
        self.reset_view()
        self.refresh()

        missing = sorted({n.name for n in doc.nodes
                          if n.type == ABN_SOURCE and
                          n.name not in doc.animations})
        if missing:
            messagebox.showwarning(
                APP_TITLE,
                "この YBR に無いアニメーションを指しています:\n  " +
                "\n  ".join(missing))

    def _write(self, path):
        try:
            root = self.doc.root()
            if root is None:
                raise YabtError("出力に何もつながっていません")
            save_yabt(path, self.doc.to_tree(root))
        except YabtError as e:
            messagebox.showerror(APP_TITLE, "保存できません\n\n%s" % e)
            return False
        self.doc.path = path
        self.doc.dirty = False
        self.refresh()
        return True

    def _warn_before_save(self):
        floating = len(self.doc.nodes) - len(self.doc.reachable())
        if 0 < floating:
            if not messagebox.askyesno(
                    APP_TITLE,
                    "出力につながっていないノードが %d 個あります。\n"
                    "これらは保存されません。続けますか?" % floating):
                return False
        missing = sorted({n.name for n in self.doc.nodes
                          if n.type == ABN_SOURCE and n.name and
                          n.name not in self.doc.animations})
        if missing:
            if not messagebox.askyesno(
                    APP_TITLE,
                    "この YBR に無いアニメーションを指しています:\n  " +
                    "\n  ".join(missing) + "\n\n続けますか?"):
                return False
        return True

    def on_save(self):
        if not self.doc:
            return False
        if not self._warn_before_save():
            return False
        if self.doc.path:
            return self._write(self.doc.path)
        return self.on_save_as(check=False)

    def on_save_as(self, check=True):
        if not self.doc:
            return False
        if check and not self._warn_before_save():
            return False
        path = filedialog.asksaveasfilename(
            title="名前を付けて保存", defaultextension=".yabt",
            filetypes=[("YABT ファイル", "*.yabt"), ("すべて", "*.*")])
        if not path:
            return False
        return self._write(path)

    def on_close(self):
        if not self.doc:
            return
        if not self.confirm_discard():
            return
        self.doc = None
        self.selected = None
        self.undo_stack.clear()
        self.redo_stack.clear()
        self.refresh()

    def on_quit(self):
        if self.confirm_discard():
            self.root.destroy()

    # --- 編集メニュー ---

    def on_undo(self):
        if not self.doc or not self.undo_stack:
            return
        self.redo_stack.append(self.snapshot())
        self.restore(self.undo_stack.pop())
        self.refresh()

    def on_redo(self):
        if not self.doc or not self.redo_stack:
            return
        self.undo_stack.append(self.snapshot())
        self.restore(self.redo_stack.pop())
        self.refresh()

    # --- ノードの操作 ---

    def add_node(self, ntype):
        if not self.doc:
            return
        self.push_undo()
        node = ENode(ntype, self.doc.new_nid())
        if ntype == ABN_SOURCE and self.doc.animations:
            node.name = self.doc.animations[0]
        if self._last_menu_pos is not None:
            x, y = self._last_menu_pos
        else:
            x, y = self.to_world(60, 60)
        node.x, node.y = free_spot(self.doc, x, y)
        self.doc.nodes.append(node)
        self.selected = node
        self.refresh()

    def delete_selected(self):
        if not self.doc or not self.selected:
            return
        if self.selected is self.doc.output:
            return
        self.push_undo()
        self.doc.remove(self.selected)
        self.selected = None
        self.refresh()

    # --- 座標 ---

    def to_screen(self, wx, wy):
        return (wx + self.pan_x) * self.zoom, (wy + self.pan_y) * self.zoom

    def to_world(self, sx, sy):
        return sx / self.zoom - self.pan_x, sy / self.zoom - self.pan_y

    def reset_view(self):
        self.zoom = 1.0
        self.pan_x = 60.0
        self.pan_y = 60.0

    # --- 入力 ---

    def socket_at(self, wx, wy):
        best = None
        best_d = (SOCKET_R * 2.5) ** 2
        for node, kind, slot, sx, sy in self._sockets:
            d = (sx - wx) ** 2 + (sy - wy) ** 2
            if d < best_d:
                best_d = d
                best = (node, kind, slot)
        return best

    def node_at(self, wx, wy):
        for node in reversed(self.doc.nodes):
            if (node.x <= wx <= node.x + NODE_W and
                    node.y <= wy <= node.y + node.height()):
                return node
        return None

    def on_press(self, event):
        self.canvas.focus_set()
        if not self.doc:
            return
        wx, wy = self.to_world(event.x, event.y)

        hit = self.socket_at(wx, wy)
        if hit:
            node, kind, slot = hit
            if kind == "in":
                child = node.children[slot]
                if child is not None:
                    # つながっているものを持ち上げて、つなぎ替える
                    self.push_undo()
                    node.children[slot] = None
                    self._link = {"child": child, "from": (wx, wy),
                                  "pushed": True}
                    self.redraw()
                    return
                self._link = {"parent": (node, slot), "from": (wx, wy),
                              "pushed": False}
                return
            self._link = {"child": node, "from": (wx, wy), "pushed": False}
            return

        node = self.node_at(wx, wy)
        if node:
            self.selected = node
            self._move = {"node": node, "dx": wx - node.x, "dy": wy - node.y,
                          "moved": False}
            self.build_panel()
            self.redraw()
            return

        self.selected = None
        self.build_panel()
        self._drag = (event.x, event.y)
        self.redraw()

    def on_motion(self, event):
        if self._link is not None:
            self._link["to"] = self.to_world(event.x, event.y)
            self.redraw()
            return
        if self._move is not None:
            wx, wy = self.to_world(event.x, event.y)
            if not self._move["moved"]:
                # 動かし始めたときだけ 1 回だけ元に戻せるようにする
                self.push_undo()
                self._move["moved"] = True
            node = self._move["node"]
            node.x = wx - self._move["dx"]
            node.y = wy - self._move["dy"]
            self.redraw()
            return
        if self._drag is not None:
            dx = (event.x - self._drag[0]) / self.zoom
            dy = (event.y - self._drag[1]) / self.zoom
            self.pan_x += dx
            self.pan_y += dy
            self._drag = (event.x, event.y)
            self.redraw()

    def on_release(self, event):
        self._drag = None
        if self._move is not None:
            moved = self._move["moved"]
            self._move = None
            if moved:
                self.refresh()
            return
        if self._link is None:
            return
        link = self._link
        self._link = None

        wx, wy = self.to_world(event.x, event.y)
        hit = self.socket_at(wx, wy)
        if hit:
            node, kind, slot = hit
            if "child" in link and kind == "in":
                self.try_connect(node, slot, link["child"], link["pushed"])
            elif "parent" in link and kind == "out":
                parent, pslot = link["parent"]
                self.try_connect(parent, pslot, node, link["pushed"])
        self.refresh()

    def try_connect(self, parent, slot, child, pushed=False):
        if child is parent:
            return
        if self.doc.is_descendant(child, parent):
            messagebox.showwarning(APP_TITLE, "輪になるつなぎ方はできません。")
            return
        if not pushed:
            self.push_undo()
        else:
            self.doc.dirty = True
        self.doc.connect(parent, slot, child)

    def on_pan_start(self, event):
        self._drag = (event.x, event.y)

    def on_pan_move(self, event):
        if self._drag is None:
            return
        self.pan_x += (event.x - self._drag[0]) / self.zoom
        self.pan_y += (event.y - self._drag[1]) / self.zoom
        self._drag = (event.x, event.y)
        self.redraw()

    def on_wheel(self, event, delta=None):
        if not self.doc:
            return
        if delta is None:
            delta = event.delta
        before = self.to_world(event.x, event.y)
        self.zoom *= 1.1 if 0 < delta else 1 / 1.1
        self.zoom = min(max(self.zoom, 0.25), 3.0)
        after = self.to_world(event.x, event.y)
        self.pan_x += after[0] - before[0]
        self.pan_y += after[1] - before[1]
        self.redraw()

    def on_context(self, event):
        self.popup_add(event)

    def popup_add(self, event):
        if not self.doc:
            return
        if event is None:
            x = self.canvas.winfo_rootx() + 40
            y = self.canvas.winfo_rooty() + 40
            self._last_menu_pos = self.to_world(40, 40)
        else:
            x, y = event.x_root, event.y_root
            self._last_menu_pos = self.to_world(event.x, event.y)
        self.add_menu.tk_popup(x, y)

    # --- 描画 ---

    def socket_points(self, node):
        """(kind, slot, wx, wy) を上から順に返す。"""
        x, y = node.x, node.y
        row = y + HEADER_H + PAD
        out = []
        if node.has_output():
            out.append(("out", 0, x + NODE_W, row + ROW_H * 0.5))
            row += ROW_H
        for i in range(len(node.children)):
            out.append(("in", i, x, row + ROW_H * 0.5))
            row += ROW_H
        return out

    def draw_grid(self):
        w = self.canvas.winfo_width()
        h = self.canvas.winfo_height()
        step = 40 * self.zoom
        if step < 8:
            return
        big = step * 5
        ox = (self.pan_x * self.zoom) % step
        oy = (self.pan_y * self.zoom) % step
        bx = (self.pan_x * self.zoom) % big
        by = (self.pan_y * self.zoom) % big
        x = ox
        while x < w:
            self.canvas.create_line(x, 0, x, h, fill=COL_GRID)
            x += step
        y = oy
        while y < h:
            self.canvas.create_line(0, y, w, y, fill=COL_GRID)
            y += step
        x = bx
        while x < w:
            self.canvas.create_line(x, 0, x, h, fill=COL_GRID_BIG)
            x += big
        y = by
        while y < h:
            self.canvas.create_line(0, y, w, y, fill=COL_GRID_BIG)
            y += big

    def draw_link(self, x1, y1, x2, y2, color):
        """Blender ふうの横に伸びるベジェ曲線。"""
        dx = max(abs(x2 - x1) * 0.5, 30 * self.zoom)
        points = []
        steps = 16
        for i in range(steps + 1):
            t = i / steps
            mt = 1 - t
            px = (mt ** 3 * x1 + 3 * mt * mt * t * (x1 + dx) +
                  3 * mt * t * t * (x2 - dx) + t ** 3 * x2)
            py = (mt ** 3 * y1 + 3 * mt * mt * t * y1 +
                  3 * mt * t * t * y2 + t ** 3 * y2)
            points += [px, py]
        self.canvas.create_line(*points, fill=color,
                                width=max(1, int(2 * self.zoom)), smooth=True)

    def font(self, size, bold=False):
        px = max(7, int(size * self.zoom))
        return ("TkDefaultFont", px, "bold") if bold else ("TkDefaultFont", px)

    def redraw(self):
        self.canvas.delete("all")
        self._sockets = []

        if not self.doc:
            w = max(self.canvas.winfo_width(), 1)
            h = max(self.canvas.winfo_height(), 1)
            self.canvas.create_text(
                w // 2, h // 2, fill=COL_TEXT_DIM, justify="center",
                font=("TkDefaultFont", 12),
                text="編集を始めるには YBR を読み込みます。\n\n"
                     "「ファイル」→「新規」または「開く」")
            return

        self.draw_grid()

        for node in self.doc.nodes:
            for kind, slot, wx, wy in self.socket_points(node):
                self._sockets.append((node, kind, slot, wx, wy))

        # つながり
        for node in self.doc.nodes:
            for kind, slot, wx, wy in self.socket_points(node):
                if kind != "in":
                    continue
                child = node.children[slot]
                if child is None:
                    continue
                out = [p for p in self.socket_points(child) if p[0] == "out"]
                if not out:
                    continue
                sx, sy = self.to_screen(out[0][2], out[0][3])
                ex, ey = self.to_screen(wx, wy)
                self.draw_link(sx, sy, ex, ey, COL_LINK)

        if self._link is not None and "to" in self._link:
            tx, ty = self.to_screen(*self._link["to"])
            if "child" in self._link:
                out = [p for p in self.socket_points(self._link["child"])
                       if p[0] == "out"]
                if out:
                    sx, sy = self.to_screen(out[0][2], out[0][3])
                    self.draw_link(sx, sy, tx, ty, COL_LINK_TEMP)
            else:
                parent, slot = self._link["parent"]
                pts = [p for p in self.socket_points(parent)
                       if p[0] == "in" and p[1] == slot]
                if pts:
                    ex, ey = self.to_screen(pts[0][2], pts[0][3])
                    self.draw_link(tx, ty, ex, ey, COL_LINK_TEMP)

        for node in self.doc.nodes:
            self.draw_node(node)

    def draw_node(self, node):
        x, y = node.x, node.y
        sx, sy = self.to_screen(x, y)
        w = NODE_W * self.zoom
        h = node.height() * self.zoom
        selected = node is self.selected

        self.canvas.create_rectangle(
            sx, sy, sx + w, sy + h,
            fill=COL_NODE_SEL if selected else COL_NODE,
            outline=COL_OUTLINE_SEL if selected else COL_OUTLINE,
            width=2 if selected else 1)
        self.canvas.create_rectangle(
            sx, sy, sx + w, sy + HEADER_H * self.zoom,
            fill=node.header_color(), outline="")
        self.canvas.create_text(
            sx + 8 * self.zoom, sy + HEADER_H * self.zoom * 0.5,
            anchor="w", text=node.title(), fill=COL_TEXT,
            font=self.font(10, bold=True))

        if node.unique_id:
            self.canvas.create_text(
                sx + w - 8 * self.zoom, sy + HEADER_H * self.zoom * 0.5,
                anchor="e", text=node.unique_id, fill=COL_TEXT,
                font=self.font(8))

        labels = node.socket_labels()
        for kind, slot, wx, wy in self.socket_points(node):
            px, py = self.to_screen(wx, wy)
            r = SOCKET_R * self.zoom
            color = COL_SOCKET_OUT if kind == "out" else COL_SOCKET
            filled = kind == "out" or node.children[slot] is not None
            self.canvas.create_oval(
                px - r, py - r, px + r, py + r,
                fill=color if filled else COL_NODE, outline=color, width=1)
            if kind == "out":
                self.canvas.create_text(
                    px - 10 * self.zoom, py, anchor="e", text="出力",
                    fill=COL_TEXT_DIM, font=self.font(8))
            else:
                text = labels[slot] if slot < len(labels) else "入力"
                fill = COL_TEXT_DIM
                if node.children[slot] is None:
                    fill = COL_ERROR
                self.canvas.create_text(
                    px + 10 * self.zoom, py, anchor="w", text=text,
                    fill=fill, font=self.font(8))

        rows = (1 if node.has_output() else 0) + len(node.children)
        top = y + HEADER_H + PAD + rows * ROW_H
        for i, line in enumerate(node.summary()):
            lx, ly = self.to_screen(x + 10, top + i * LINE_H + LINE_H * 0.5)
            fill = COL_TEXT
            if node.type == ABN_SOURCE and i == 0:
                if not node.name or node.name not in self.doc.animations:
                    fill = COL_ERROR
            self.canvas.create_text(lx, ly, anchor="w", text=line, fill=fill,
                                    font=self.font(9))

    # --- プロパティパネル ---

    def clear_panel(self):
        for widget in self.panel.winfo_children():
            widget.destroy()

    def build_panel(self):
        self._suspend = True
        self.clear_panel()

        if not self.doc:
            tk.Label(self.panel, text="YBR を読み込んでください",
                     bg="#252525", fg=COL_TEXT_DIM, anchor="w",
                     wraplength=250, justify="left").pack(fill="x", pady=4)
            self._suspend = False
            return

        node = self.selected
        if node is None:
            tk.Label(self.panel, text="ノードを選ぶとここで編集できます",
                     bg="#252525", fg=COL_TEXT_DIM, anchor="w",
                     wraplength=250, justify="left").pack(fill="x", pady=4)
            self._suspend = False
            return

        tk.Label(self.panel, text=node.title(), bg="#252525", fg=COL_TEXT,
                 anchor="w", font=("TkDefaultFont", 11, "bold")
                 ).pack(fill="x", pady=(4, 8))

        if node.type == TYPE_OUTPUT:
            tk.Label(self.panel,
                     text="ここにつないだノードがツリーの根になります。",
                     bg="#252525", fg=COL_TEXT_DIM, anchor="w",
                     wraplength=250, justify="left").pack(fill="x")
            self._suspend = False
            return

        self._row_text(node, "unique_id", "unique_id",
                       "YbrAnimBlendTreeFind() で引くための名前。"
                       "使わないなら空のまま")

        if node.type == ABN_SOURCE:
            self._row_anim(node)
            self._row_choice(node, "loop_mode", "ループ方法",
                             [(ALM_ONE_SHOT, LOOP_MODE_NAMES[ALM_ONE_SHOT]),
                              (ALM_LOOP, LOOP_MODE_NAMES[ALM_LOOP])])
            self._row_float(node, "play_speed", "再生速度", 0.01, 100.0)

        elif node.type in (ABN_LERP, ABN_ADD):
            self._row_float(node, "weight", "重み", 0.0, 1.0)
            self._row_bones(node)

        elif node.type == ABN_TRANSITION:
            self._row_int(node, "index", "今の入力", 0,
                          max(0, len(node.children) - 1))
            self._row_float(node, "transition_seconds", "遷移の秒数", 0.0, 60.0)
            self._row_inputs(node)

        self._suspend = False

    def _label(self, text, hint=None):
        tk.Label(self.panel, text=text, bg="#252525", fg=COL_TEXT_DIM,
                 anchor="w").pack(fill="x", pady=(8, 0))
        if hint:
            tk.Label(self.panel, text=hint, bg="#252525", fg="#707070",
                     anchor="w", wraplength=250, justify="left",
                     font=("TkDefaultFont", 8)).pack(fill="x")

    def _commit(self, node, attr, value):
        if self._suspend:
            return
        if getattr(node, attr) == value:
            return
        self.push_undo()
        setattr(node, attr, value)
        # <FocusOut> の最中にパネルを壊さないよう、後回しにする
        self.root.after_idle(self.refresh)

    def _row_text(self, node, attr, label, hint=None):
        self._label(label, hint)
        var = tk.StringVar(value=getattr(node, attr) or "")
        entry = tk.Entry(self.panel, textvariable=var)
        entry.pack(fill="x")

        def apply(_event=None):
            self._commit(node, attr, var.get().strip())

        entry.bind("<Return>", apply)
        entry.bind("<FocusOut>", apply)

    def _row_int(self, node, attr, label, lo=None, hi=None, hint=None):
        self._label(label, hint)
        var = tk.StringVar(value=str(getattr(node, attr)))
        entry = tk.Entry(self.panel, textvariable=var)
        entry.pack(fill="x")

        def apply(_event=None):
            try:
                value = int(float(var.get()))
            except ValueError:
                var.set(str(getattr(node, attr)))
                return
            if lo is not None:
                value = max(lo, value)
            if hi is not None:
                value = min(hi, value)
            var.set(str(value))
            self._commit(node, attr, value)

        entry.bind("<Return>", apply)
        entry.bind("<FocusOut>", apply)

    def _row_float(self, node, attr, label, lo, hi, hint=None):
        self._label(label, hint)
        var = tk.StringVar(value="%.4g" % getattr(node, attr))
        entry = tk.Entry(self.panel, textvariable=var)
        entry.pack(fill="x")

        def apply(_event=None):
            try:
                value = float(var.get())
            except ValueError:
                var.set("%.4g" % getattr(node, attr))
                return
            value = min(max(value, lo), hi)
            var.set("%.4g" % value)
            self._commit(node, attr, value)

        entry.bind("<Return>", apply)
        entry.bind("<FocusOut>", apply)

    def _row_choice(self, node, attr, label, choices):
        self._label(label)
        names = [name for _, name in choices]
        current = dict(choices).get(getattr(node, attr), names[0])
        box = ttk.Combobox(self.panel, values=names, state="readonly")
        box.set(current)
        box.pack(fill="x")

        def apply(_event=None):
            for value, name in choices:
                if name == box.get():
                    self._commit(node, attr, value)
                    return

        box.bind("<<ComboboxSelected>>", apply)

    def _search_box(self, names, initial, kind, empty_msg, on_pick,
                    keep_text=False, restore_on_leave=False, current=None):
        """打ち込んで絞り込めるコンボボックス。1 つに決まったら on_pick(名前)。

        戻り値は絞り込みの案内を書き換える関数。
        """
        var = tk.StringVar(value=initial)
        box = ttk.Combobox(self.panel, values=names, textvariable=var)
        box.pack(fill="x", pady=(4, 0))

        hint = tk.Label(self.panel, text="", bg="#252525", fg=COL_TEXT_DIM,
                        anchor="w", wraplength=250, justify="left",
                        font=("TkDefaultFont", 8))
        hint.pack(fill="x")

        def show(text, error=False):
            hint.config(text=text, fg=COL_ERROR if error else COL_TEXT_DIM)

        def count_hint(found):
            if not names:
                return empty_msg
            if not found:
                return "見つかりません"
            if len(found) == len(names):
                return "%d 本 / 文字を打つと絞り込めます" % len(names)
            return "%d 本に絞り込み (↓ で候補)" % len(found)

        def refilter(event=None):
            # 候補の中を移動しているときは絞り込み直さない
            if event is not None and event.keysym in (
                    "Up", "Down", "Return", "KP_Enter", "Escape", "Tab",
                    "Left", "Right", "Shift_L", "Shift_R"):
                return
            found = filter_names(names, var.get())
            box["values"] = found
            show(count_hint(found))

        def apply(_event=None):
            if not names:
                show(empty_msg, True)
                return
            picked = resolve_name(names, var.get())
            if picked is None:
                found = filter_names(names, var.get())
                if found:
                    show("%d 本あります。もう少し絞り込んでください" % len(found),
                         True)
                else:
                    show("その%sはありません" % kind, True)
                return
            if keep_text:
                var.set(picked)
            on_pick(picked)

        def leave(_event=None):
            # 打ちかけのまま離れたら元に戻す
            if resolve_name(names, var.get()) is None:
                var.set(current() if current else "")
                box["values"] = names
                show(count_hint(names))
            else:
                apply()

        box.bind("<KeyRelease>", refilter)
        box.bind("<Return>", apply)
        box.bind("<<ComboboxSelected>>", apply)
        if restore_on_leave:
            box.bind("<FocusOut>", leave)

        show(count_hint(names))
        return show, apply

    def _row_anim(self, node):
        self._label("アニメーション", "YBR に入っているものから選びます")
        names = list(self.doc.animations)

        self._search_box(
            names, node.name or "", "アニメーション",
            "この YBR にアニメーションがありません",
            lambda picked: self._commit(node, "name", picked),
            keep_text=True, restore_on_leave=True,
            current=lambda: node.name or "")

        if node.name and node.name not in names:
            tk.Label(self.panel,
                     text="この YBR に '%s' はありません" % node.name,
                     bg="#252525", fg=COL_ERROR, anchor="w", wraplength=250,
                     justify="left").pack(fill="x")

    def _row_bones(self, node):
        self._label("合成から外すボーン", "YBR のアーマチュアから選びます")

        listbox = tk.Listbox(self.panel, height=5, exportselection=False)
        for bone in node.filtered_bones:
            listbox.insert("end", bone)
        listbox.pack(fill="x")

        remain = [b for b in self.doc.bones if b not in node.filtered_bones]

        def add_bone(picked):
            self._commit(node, "filtered_bones",
                         list(node.filtered_bones) + [picked])

        show, add = self._search_box(
            remain, "", "ボーン", "追加できるボーンがありません", add_bone)

        def remove():
            picked = listbox.curselection()
            if not picked:
                show("消すボーンを上のリストから選んでください", True)
                return
            rest = [b for i, b in enumerate(node.filtered_bones)
                    if i not in picked]
            self._commit(node, "filtered_bones", rest)

        row = tk.Frame(self.panel, bg="#252525")
        row.pack(fill="x")
        tk.Button(row, text="追加", command=add).pack(side="left")
        tk.Button(row, text="削除", command=remove).pack(side="left")

        if not self.doc.bones:
            tk.Label(self.panel, text="この YBR にアーマチュアがありません",
                     bg="#252525", fg=COL_TEXT_DIM, anchor="w",
                     wraplength=250, justify="left").pack(fill="x")

        missing = [b for b in node.filtered_bones if b not in self.doc.bones]
        if missing:
            tk.Label(self.panel,
                     text="この YBR に無いボーン: " + ", ".join(missing),
                     bg="#252525", fg=COL_ERROR, anchor="w",
                     wraplength=250, justify="left").pack(fill="x")

    def _row_inputs(self, node):
        self._label("入力の数", "遷移で切り替える先の数")
        row = tk.Frame(self.panel, bg="#252525")
        row.pack(fill="x")
        tk.Label(row, text=str(len(node.children)), bg="#252525",
                 fg=COL_TEXT, width=4).pack(side="left")
        tk.Button(row, text="追加",
                  command=lambda: self._add_input(node)).pack(side="left")
        tk.Button(row, text="末尾削除",
                  command=lambda: self._del_input(node)).pack(side="left")

    def _add_input(self, node):
        self.push_undo()
        node.children.append(None)
        self.root.after_idle(self.refresh)

    def _del_input(self, node):
        if len(node.children) <= 1:
            return
        self.push_undo()
        node.children.pop()
        if len(node.children) <= node.index:
            node.index = len(node.children) - 1
        self.root.after_idle(self.refresh)


def main():
    root = tk.Tk()
    Editor(root)
    root.mainloop()


if __name__ == "__main__":
    main()
