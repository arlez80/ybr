# tools

`.yabt` (アニメーション合成ツリー) を組み立てるためのエディタです。
Python 3.8 以降と Tkinter だけで動きます。

```
python3 tools/yabt_editor.py
```

| ファイル         | 中身                                                 |
| ---------------- | ---------------------------------------------------- |
| `yabt_editor.py` | エディタ本体 (これを実行する)                        |
| `yabt_format.py` | `.yabt` の読み書きと `.ybr` からのアニメーション一覧 |
| `yabt_cbor.py`   | 最小の CBOR リーダー / ライター                      |

`yabt_format.py` と `yabt_cbor.py` は単体でも使えるので、
アセットのビルドスクリプトから `.yabt` を作ることもできます。

```python
import sys; sys.path.insert(0, "tools")
import yabt_format as Y

Y.save_yabt("locomotion.yabt", {
    "type": Y.ABN_LERP, "unique_id": "mix", "weight": 0.5,
    "filtered_bones": [],
    "input":     {"type": Y.ABN_SOURCE, "unique_id": "walk", "name": "Walk",
                  "loop_mode": Y.ALM_LOOP, "play_speed": 1.0},
    "mix_input": {"type": Y.ABN_SOURCE, "unique_id": "run", "name": "Run",
                  "loop_mode": Y.ALM_LOOP, "play_speed": 1.0},
})
```

## 使い方

編集を始める前に `.ybr` を読み込みます。
「ファイル」→「新規」または「開く」のときに聞かれます。

いちばん右にある赤い「出力」ノードにつないだものが、ツリーの根として保存されます。
ルートまで接続が辿れないノードは保存されません。

ノードの位置はファイルに入らない情報なので、`.yabt` を開いたときだけ重ならないように自動で並べます。そのあとは自由に動かせます。

### マウス

| 操作                           |                                 |
| ------------------------------ | ------------------------------- |
| ソケットを左ドラッグ           | つなぐ / つなぎ替える           |
| つながっている入力を左ドラッグ | 外して持ち上げる                |
| ノードを左ドラッグ             | 選んで動かす (右側で中身を編集) |
| 背景を左ドラッグ / 中ドラッグ  | スクロール                      |
| ホイール                       | 拡大 / 縮小                     |
| 背景を右クリック               | ノードを追加                    |

### キー

| キー                  |                        |
| --------------------- | ---------------------- |
| Shift+A               | ノードを追加           |
| Delete                | 選んでいるノードを消す |
| Ctrl+N / Ctrl+O       | 新規 / 開く            |
| Ctrl+S / Ctrl+Shift+S | 上書き保存 / 保存      |
| Ctrl+Z / Ctrl+Y       | 元に戻す / やり直す    |
