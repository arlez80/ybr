# Yui Blender to Raylib (.ybr) フォーマット仕様 v1

エンコード: CBOR (RFC 8949) / definite-length のみ / 拡張子 `.ybr`

## トップレベル

```
["YUI", 1, <scene_tree>, <data_blocks>]
```

- `scene_tree` : ルートノードの配列
- `data_blocks`: データブロックの配列

## 座標系

- Blender (Z-up, RH) → raylib/OpenGL (Y-up, RH) へ変換済み
  `(x, y, z)_blender → (x, z, -y)_gl`
- 行列は **float 16個の column-major**（OpenGL 準拠 / index 12,13,14 が平行移動）
  raylib `Matrix` へは `m.m0 = v[0]; m.m1 = v[1]; ... m.m15 = v[15];` で入る
- 三角形の巻き方向は CCW（変換で保存される）
- バイト列に詰めた数値は **リトルエンディアン固定**（float32 / uint16 / uint32 / uint8）

## 型コード

タイプは文字列ではなく整数で格納する。未対応の値は `null` を書き出し、
ローダー側では `*_UNKNOWN` (-1) になる。

| 種類                     | コード                                                                                             |
| ------------------------ | -------------------------------------------------------------------------------------------------- |
| ノードタイプ             | 0 UNKNOWN / 1 EMPTY / 2 MESH / 3 ARMATURE / 4 CURVE / 5 LIGHT / 6 CAMERA / 7 OBJECT / 8 COLLECTION |
| スプラインタイプ         | 0 POLY / 1 BEZIER / 2 NURBS                                                                        |
| ライトタイプ             | 0 POINT / 1 SUN / 2 SPOT / 3 AREA                                                                  |
| カラースペース           | 0 sRGB / 1 Linear / 2 Non-Color                                                                    |
| テクスチャ extension     | 0 REPEAT / 1 EXTEND / 2 CLIP / 3 MIRROR                                                            |
| テクスチャ interpolation | 0 Linear / 1 Closest / 2 Cubic / 3 Smart                                                           |
| カメラタイプ             | 0 PERSP / 1 ORTHO / 2 PANORAMIC                                                                    |
| センサーフィット         | 0 AUTO / 1 HORIZONTAL / 2 VERTICAL                                                                 |
| カスタムプロパティ型     | 0 BOOL / 1 INT / 2 FLOAT / 3 STRING / 4 ARRAY                                                      |
| アニメのフレームタイプ   | 0 TRANSFORM                                                                                        |
| アニメの補間方法         | 0 STEP / 1 LINEAR / 2 CUBIC / 3 SINC                                                               |

Blender の `SURFACE` / `FONT` はノードタイプ 4 (CURVE) に丸められる。
個別のコードを持たないオブジェクト（`SPEAKER` / `VOLUME` / `LATTICE` など）は
7 (OBJECT) になる。

## シーンツリー / ノード (map)

シーンツリーは **シーンコレクションの階層をそのまま反映する**。
ルートは常に 1 個のコレクションノード（`Scene Collection`）で、その下に
子コレクションとオブジェクトが並ぶ。

| key                 | type        | 内容                                       |
| ------------------- | ----------- | ------------------------------------------ |
| `name`              | tstr        | オブジェクト名 / コレクション名            |
| `type`              | uint        | ノードタイプコード                         |
| `data`              | tstr / null | 参照するデータ ID（コレクションは null）   |
| `matrix`            | [16 float]  | 下記参照（コレクションは単位行列）         |
| `custom_properties` | [custom]    | カスタムプロパティ（コレクションは常に空） |
| `children`          | [node]      | 子コレクション / 子オブジェクト            |

custom (map): `key`(tstr) / `type`(uint) / `value`

- Blender 側で **`ybr_` で始まるカスタムプロパティだけ** が対象。
  `key` はプレフィックスを外した名前。
- `value` は型に応じて bool / int / float / tstr、
  ARRAY のときは float32 の bstr（要素数は `count`）。
- `Custom Properties` オプションが OFF のときは常に空。

オブジェクトが重複しないよう、各オブジェクトはツリー中に **1 度だけ** 現れる。

| 配置先             | 条件                   | `matrix`             |
| ------------------ | ---------------------- | -------------------- |
| 親オブジェクトの子 | 親も書き出し対象のとき | 親相対のローカル行列 |
| コレクションの子   | 上記以外               | world 行列           |

- 複数のコレクションにリンクされたオブジェクトは、最初に見つかったコレクションの
  下に 1 度だけ出力される。
- `Visible Only` が ON のときは、ビューレイヤーで除外 (`exclude`) / 非表示にされた
  コレクションはツリーごとスキップされる。
- どのコレクションからも配置できなかったオブジェクトは、保険としてルート直下に
  追加され、警告が出る。

## データブロック共通

`kind` と `id`（= データ ID）を必ず持つ。
`kind` は `MESH` / `ARMATURE` / `CURVE` / `LIGHT` / `MATERIAL` / `NODEGROUP` /
`ANIMATION` / `TEXTURE` / `CAMERA` / `EMPTY`。

---

### MESH

| key                | type            | 内容                                                                         |
| ------------------ | --------------- | ---------------------------------------------------------------------------- |
| `vertex_count`     | uint            | 頂点数                                                                       |
| `triangle_count`   | uint            | 三角形数                                                                     |
| `positions`        | bstr            | float32 × 3 × vertex_count                                                   |
| `normals`          | bstr            | float32 × 3 × vertex_count                                                   |
| `uvs`              | bstr (optional) | float32 × 2（V は反転済み）                                                  |
| `tangents`         | bstr (optional) | float32 × 4（xyz が接線 / w が従法線の向き ±1）。UV があるメッシュにだけ出る |
| `colors`           | bstr (optional) | uint8 RGBA × 4（sRGB）                                                       |
| `indices`          | bstr            | **uint32** × 3 × triangle_count                                              |
| `material_indices` | bstr            | uint32 × triangle_count（`materials` への添字）                              |
| `vertex_map`       | bstr            | uint32 × vertex_count（出力頂点 → Blender の元頂点）                         |
| `materials`        | [tstr]          | 参照マテリアル ID                                                            |
| `armature`         | tstr / null     | アーマチュア**オブジェクト名**（合成後の基準アーマチュア）                   |
| `armature_data`    | tstr / null     | アーマチュアのデータ ID（唯一の ARMATURE ブロックの `id`）                   |
| `skin`             | skin / null     | ウェイト上位 4 本に正規化したスキニングデータ                                |
| `vertex_groups`    | [vgroup]        | 頂点グループ生データ（オプション ON のときのみ）                             |

skin (map):

| key          | type | 内容                                                           |
| ------------ | ---- | -------------------------------------------------------------- |
| `influences` | uint | 常に 4                                                         |
| `joints`     | bstr | uint16 × 4 × vertex_count / **ARMATURE の bones 配列の index** |
| `weights`    | bstr | float32 × 4 × vertex_count / **各頂点で合計 1.0**              |

- joint は書き出し時に頂点グループ名とボーン名の一致で解決済み。
  対応するボーンが無い頂点グループはスキンから除外され、警告が出る。
- **アーマチュアモディファイアを持たないメッシュには `skin` は付かない**（`null`）。
  ペアレントしただけのメッシュは Blender でも変形しないので対象外。
- メッシュに複数のアーマチュアモディファイアが載っている場合、頂点グループ名は
  モディファイアの並び順で照合し、最初に見つかったボーンに割り当てる。

- 5 本以上のウェイトを持つ頂点は上位 4 本を残して切り捨て、残りを再正規化する
  （書き出し時に警告が出る）。影響が 4 本未満の枠は joint 0 / weight 0.0 で埋める。
- ウェイト合計が 0 の頂点は 4 枠すべて weight 0.0。

vgroup (map): `Keep Raw Vertex Groups` オプションが ON のときのみ出力される。

| key       | type | 内容                                       |
| --------- | ---- | ------------------------------------------ |
| `name`    | tstr | グループ名（ボーン名と対応）               |
| `index`   | uint | Blender 上のグループ番号                   |
| `count`   | uint | 登録頂点数                                 |
| `indices` | bstr | uint32 × count（**出力頂点インデックス**） |
| `weights` | bstr | float32 × count                            |

- 三角形化済み。法線 / UV / 頂点カラーが異なるコーナーは頂点分割済み。
  頂点グループは分割後のインデックスへ展開済みなのでそのまま使える。
- `Rest Pose` オプション ON のときはアーマチュアモディファイアを一時無効化して
  書き出すため、形状はレストポーズになる。
- raylib の `Mesh.indices` は `unsigned short` なので、C 側で 65536 未満なら
  uint16 へ変換、超える場合はメッシュ分割が必要。

### ARMATURE

**ARMATURE ブロックはファイル全体で 0 個か 1 個**。読み込み側もそれを前提にしている
（`YbrScene.armature` は単一のポインタ）。

| key     | type   | 内容                       |
| ------- | ------ | -------------------------- |
| `bones` | [bone] | 親が必ず子より先に並ぶ順序 |

書き出し対象は「メッシュのアーマチュアモディファイアが指しているアーマチュア」。
それが複数あるときは **1 つに合成してから書き出す**:

- オブジェクト名順で先頭のものを「基準」にし、その `id`（データ名）を使う
- 基準以外のボーンは `rel = 基準.matrix_world⁻¹ @ その他.matrix_world` を掛けて
  基準アーマチュアの空間へ移し、合成後アーマチュアのルートとしてぶら下げる
  （つまり合成後はマルチルートになりうる）
- ボーン名が衝突したときだけ `<アーマチュアオブジェクト名>/<ボーン名>` に改名する。
  アニメーションのトラック名にも同じ改名が適用される
- シーンツリー上では基準アーマチュアのノードだけが `data` にこの `id` を持ち、
  合成された側のノードは `data` が `null` になる（ボーンは基準の空間へ移して
  あるので、そのノードの行列はもう使われない）

`Deform Bones Only` オプション（既定 ON）では `use_deform` が OFF のボーンを
書き出さない。除外されたボーンの子は、最も近い「含まれる祖先」に付け替えられる
（`parent` と `rest_parent` もその祖先基準になる）。

bone (map): `name`(tstr) / `parent`(int, ルートは -1) / `rest`([16 float] アーマチュア空間) /
`rest_parent`([16 float] 親ボーン相対) / `length`(float)

### CURVE

スプラインのみ対応（ベベル / テーパー / フォント変換は未対応）。

| key       | type     | 内容               |
| --------- | -------- | ------------------ |
| `is_3d`   | bool     | `True` = 3D カーブ |
| `splines` | [spline] | スプライン配列     |

spline (map):

| key                                        | type | 内容                                                          |
| ------------------------------------------ | ---- | ------------------------------------------------------------- |
| `type`                                     | uint | スプラインタイプコード                                        |
| `cyclic`                                   | bool | 閉じているか                                                  |
| `order`                                    | uint | NURBS の `order_u`                                            |
| `point_count`                              | uint | 制御点数                                                      |
| `points`                                   | bstr | float32 × 3 × point_count                                     |
| `handles_left` / `handles_right`           | bstr | BEZIER のみ / float32 × 3                                     |
| `handle_types_left` / `handle_types_right` | bstr | BEZIER のみ / uint8（0 FREE / 1 AUTO / 2 VECTOR / 3 ALIGNED） |
| `weights`                                  | bstr | POLY / NURBS のみ / float32                                   |
| `tilts` / `radii`                          | bstr | float32                                                       |

### LIGHT

| key                         | type             | 内容                                                 |
| --------------------------- | ---------------- | ---------------------------------------------------- |
| `type`                      | uint             | ライトタイプコード                                   |
| `color`                     | [3 float]        | リニア RGB                                           |
| `energy`                    | float            | Blender の W（SUN は irradiance）                    |
| `use_shadow`                | bool             |                                                      |
| `radius`                    | float            | `shadow_soft_size`                                   |
| `angle`                     | float            | SUN のみ / ラジアン                                  |
| `spot_size` / `spot_blend`  | float            | SPOT のみ / `spot_size` はコーン**全角**（ラジアン） |
| `shape` / `size` / `size_y` | tstr, float      | AREA のみ                                            |
| `cutoff_distance`           | float (optional) | カスタム距離が有効なときのみ                         |

### MATERIAL

`mode` が `SIMPLE`（既定）か `PRO`。共通で `render_method` / `backface_culling` /
`transparent` を持つ。

`transparent` は「このマテリアルが半透明を含むか」を**書き出し側で判定**した結果。
再生側は不透明を先に、半透明をカメラから遠い順にあとで描く。次のいずれかで真になる。

1. Blender のブレンド設定が不透明でない
   （4.2 以降の `surface_render_method`、それ以前の `blend_method`）
2. Alpha が 1.0 だと言い切れない
3. 半透明を作るノード（Transparent / Glass / Mix Shader など）がある

2 は、Alpha にリンクがあっても**遡って 1.0 と分かるものは不透明**として扱う
（1.0 の Value ノード、アルファチャンネルを持たない画像の `Alpha` 出力、
定数どうしの Math、Reroute、素通しのノードグループなど。
色を Alpha につないだ場合は Blender と同じく輝度に直して判定する）。
Noise のように値がその場で決まらないものは半透明に倒す。
誤って真にしても描く順が変わるだけで見た目は正しいので、迷ったら真にする。

Material Mode には `SIMPLE` / `PRO` のほかに **`None (dummy)`** がある。
`None` のときはマテリアルを一切書き出さず、`YbrDefault` という白いだけの
`MATERIAL` ブロックを 1 つだけ出して、全メッシュがそれを参照する
（テクスチャもノードグループも出ない）。再生側は「マテリアルが必ずある」
前提で書けるので、参照無しにするより扱いやすい。

**SIMPLE** — 出力ノードの Surface を辿って `* BSDF` ノードから値を抽出する。
接続が Image Texture なら `*_map`、RGB / Value ノードなら値として畳み込む。
それ以外のノードが繋がっている場合は値を読めないので**警告**を出し、
ソケットのデフォルト値を格納する。

`*_map` があるチャンネルは、値のほうを**中立値**（色なら白、スカラーなら 1.0）
にする。Blender はリンクがあるとソケットの値を使わないが、再生側は
「値 × テクスチャ」で掛けるため、そのまま入れるとテクスチャが余計に暗くなる。

| key                                             | type           | 内容                      |
| ----------------------------------------------- | -------------- | ------------------------- |
| `base_color`                                    | [4 float]      | RGBA                      |
| `specular` / `metallic` / `roughness` / `alpha` | float          |                           |
| `base_color_map` ほか `*_map`                   | texture / null | 各チャンネルのテクスチャ  |
| `normal_map` / `normal_strength`                | texture, float | Normal Map ノード経由も可 |

texture (map): `image`(tstr / TEXTURE ブロックの id) / `filepath`(tstr) /
`colorspace` / `extension` / `interpolation`（いずれも型コード、未対応は null）

**PRO** — シェーダーノードグラフをそのまま格納。

| key     | type   | 内容                                                                |
| ------- | ------ | ------------------------------------------------------------------- |
| `nodes` | [node] | ノード配列（添字がノード ID）                                       |
| `links` | [link] | `from_node` / `from_socket` / `to_node` / `to_socket`（すべて添字） |

shader node (map): `name`(tstr) / `type`(uint) / `label`(tstr) /
`inputs`[socket] / `outputs`[socket] / `props`(map)

socket (map): `name`(tstr) / `type`(uint) / `default`（float, [float], null のいずれか）

**色の値はすべて sRGB で格納する。** Blender のマテリアルの色（ソケットの
デフォルト値、RGB ノード、Color Ramp の色、頂点カラー）はシーンリニアだが、
raylib は「バイト列がそのまま画面に出る」前提なので、テクスチャと同じく
表示用の値へ寄せてある。寄せないと色が暗く濁って見える。
ベクトル / スカラー（Roughness など）は色ではないので変換しない。

`type` は文字列ではなく**整数コード**。アドオンが知らないタイプは 0 (UNKNOWN) に
なり、書き出し時に警告が出る。コードの一覧は C 側の `YbrShaderNodeType` /
`YbrShaderSocketType` を参照（`src/ybr.h`）。

Blender のバージョンでノード名が変わったものは **旧名のコードに寄せる**。
C 側の enum では新しい名前も同じ値のエイリアスとして定義してあり、
名前が変わったバージョンを `_BL<major><minor>` の形で末尾に付ける。

| コード名             | エイリアス                    | 内容                                 |
| -------------------- | ----------------------------- | ------------------------------------ |
| `YBR_SN_BSDF_VELVET` | `YBR_SN_BSDF_SHEEN_BL400`     | 4.0 で Velvet → Sheen                |
| `YBR_SN_SEPRGB`      | `YBR_SN_SEPARATE_COLOR_BL303` | 3.3 で Separate RGB → Separate Color |
| `YBR_SN_COMBRGB`     | `YBR_SN_COMBINE_COLOR_BL303`  | 3.3 で Combine RGB → Combine Color   |

`props` は RNA プロパティの汎用ダンプ（bool / int / float / 配列 / 文字列 / ポインタ名）。

### NODEGROUP

PRO モードのノードグループ。**展開せずグループのまま**別ブロックとして格納する。
マテリアル内の `GROUP` ノードは `props["node_tree"]` にこのブロックの `id` を持つ。

| key       | type     | 内容                                           |
| --------- | -------- | ---------------------------------------------- |
| `id`      | tstr     | ノードツリー名（`GROUP` ノードから参照される） |
| `inputs`  | [socket] | グループの入力インターフェース                 |
| `outputs` | [socket] | グループの出力インターフェース                 |
| `nodes`   | [node]   | MATERIAL の PRO と同じ形式                     |
| `links`   | [link]   | 同上                                           |

- `inputs` は中身の `GROUP_INPUT` ノードの**出力**ソケット、
  `outputs` は `GROUP_OUTPUT` ノードの**入力**ソケットと同じ並び。
  末尾の仮想ソケット（新しい入出力を足すための空欄）は含まない。
- グループの中でさらに別のグループを使っている場合、そのグループも
  独立した `NODEGROUP` ブロックとして書き出される（内側が先に並ぶ）。
- グループの中で使っている画像も `TEXTURE` ブロックとして書き出される。
- C 側では `YbrScene.nodeGroups` に入り、`YbrFindNodeGroup()` で引ける。
- GLSL 変換では**グループ 1 つが関数 1 つ**になる（README 参照）。

### CAMERA

| key                              | type        | 内容                   |
| -------------------------------- | ----------- | ---------------------- |
| `type`                           | uint        | カメラタイプコード     |
| `lens`                           | float       | 焦点距離 (mm)          |
| `sensor_width` / `sensor_height` | float       | センサーサイズ (mm)    |
| `sensor_fit`                     | uint / null | センサーフィットコード |
| `fov_x` / `fov_y`                | float       | 画角 (ラジアン)        |
| `clip_start` / `clip_end`        | float       | ニア / ファークリップ  |
| `ortho_scale`                    | float       | ORTHO のときのスケール |
| `shift_x` / `shift_y`            | float       | レンズシフト           |

raylib の `Camera3D.fovy` は**度**なので、`fov_y * RAD2DEG` を渡す。

### ANIMATION

**書き出し対象は NLA トラック内の action strip で使われているアクション。**
オブジェクトに直接設定されている Action (`animation_data.action`) は書き出さない
（見つかった場合は警告が出る）。同じ (オブジェクト, アクション, 範囲) の
ストリップが複数あっても 1 ブロックにまとめられる。

各ストリップは `use_nla` を切ってアクション単体で評価した結果になっている。

| key           | type  | 内容                                    |
| ------------- | ----- | --------------------------------------- |
| `id`          | tstr  | アクション名（参照 ID）                 |
| `object`      | tstr  | 対象オブジェクト名                      |
| `fps`         | float | フレームレート                          |
| `frame_count` | uint  | 全フレーム数                            |
| `sinc_a`      | uint  | `SINC` の Lanczos-a（1..16 / 省略時 3） |
| `space`       | tstr  | 常に `"GL"`（行列は変換済み）           |
| `tracks`      | array | 下記のトラック                          |
| `markers`     | array | ポーズマーカー（無ければ空配列）        |

トラックは `object` / `bone` / `frame_count` /
`frames`(u32 配列) / `types`(u8 配列) / `interps`(u8 配列) /
`transforms`(f32 配列, 16 個ずつ) を持つ。

#### ポーズマーカー (`markers`)

Blender のアクションに付いているポーズマーカー。足音やエフェクトの発火
タイミングを再生側へ渡すのに使う。

| key     | type | 内容                   |
| ------- | ---- | ---------------------- |
| `name`  | tstr | マーカー名             |
| `frame` | uint | 先頭からの相対フレーム |

`frame` の昇順に並ぶ。アクションの範囲外にあるマーカーは書き出されない。
C 側は `YbrAnimMarkersInRange()` で「前のフレームから今のフレームまでに
跨いだマーカー」を拾える（ループの巻き戻しにも対応する）。

アニメーションは **必ずベイクして** 書き出す。ベイクの手順は
Blender の glTF エクスポーター (`io_scene_gltf2`) の
`blender/exp/animation/action.py` に倣っている。

#### 補間方法 (`interps`)

キー `i` の `interp` は「キー `i-1` からキー `i` へ向かう区間」の再構成方法を表す。
`frames[0]` にはそれ以前のキーが無いので、`interps[0]` は**参照されない**。

区間内の位置を `s`（0..1）、キー番号を `k`、
小数のキー番号を `u = (i-1) + s` とする。

| コード | 名前     | 参照するキー     | 内容                                                |
| ------ | -------- | ---------------- | --------------------------------------------------- |
| 0      | `STEP`   | `i-1`            | 区間中はキー `i-1` の値のままホールドする           |
| 1      | `LINEAR` | `i-1`, `i`       | 線形補間。回転は符号をそろえた nlerp                |
| 2      | `CUBIC`  | `i-2` .. `i+1`   | 非一様 Catmull-Rom（接線は Bessel / Overhauser 型） |
| 3      | `SINC`   | `i-a` .. `i+a-1` | Lanczos-a（`sinc(x)·sinc(x/a)`）による再構成        |

- 行列はそのまま混ぜず、**一度 TRS（平行移動 / クォータニオン / スケール）へ
  分解してから**補間して合成し直す。回転は区間の始点キーを基準に
  符号をそろえてから重み付き和を取り、最後に正規化する。
- どの方法も重みの総和が 1 になるよう正規化してある。
- 参照するキーが配列の外に出る場合は、先頭 / 末尾のキーを外側へ延長して扱う
  （`CUBIC` は端で片側差分の接線に切り替える）。
- すべての方法がキーを必ず通る（補間であって近似ではない）。

Lanczos の `a` はアニメーションブロックの `sinc_a`（uint, 1..16）に入っている。
`ybr_tool --anime-opt-sinc-a` で変えた値がそのまま保存されるので、
再生側で設定し直す必要はない。省略されている場合は `3` として扱う。

Blender のエクスポーターは全フレームをベイクするので、
書き出し直後の `interps` は常に `STEP`（前フレームと同じ値のフレームは省略）。
`CUBIC` / `SINC` への置き換えとキーの間引きは
`ybr_tool --anime-opt` が行う（README 参照）。

C 側での評価は `ybr_anim.h` の `YbrAnimSamplerInitFromAnimation()`
（`sinc_a` を自動で拾う）か `YbrAnimSampler` / `YbrAnimTrackEvaluate` を使う。

### TEXTURE

#### カラースペース

カラースペースは raylib が実際に使用する値をそのまま格納する。

#### 埋め込み方の優先順位

|     | 条件                                                                    | やること                     |
| --- | ----------------------------------------------------------------------- | ---------------------------- |
| 1   | 元ファイルが PNG / JPEG（パック済み or ディスク上、Blender 内で未編集） | **バイト列をそのままコピー** |
| 2   | 生成画像 / 編集済み画像 で PNG・JPEG 指定                               | Blender に再エンコードさせる |
| 3   | 圧縮なし（RAW RGBA8）指定                                               | 自前で 8bit に変換する       |

1 が使えるならそれが最良。再エンコードもカラースペース変換も挟まないので、
**色が変わる余地が無い**。

2 では自前で色変換をしない。Blender の `Image.pixels` は「その画像の
カラースペースからシーンリニアへ変換した float」を返すので、
**同じカラースペースの一時イメージへその float をそのまま書き戻す**。
書き戻し時の変換が読み出し時の変換のちょうど裏返しになるため、Blender が
どの曲線をどこで適用していても元の表示値が復元される。
（自前で linear → sRGB してから Non-Color の一時イメージに入れると、
Blender 側でもう一度エンコードが掛かる環境で色が浅く・濁って見える。）

3 だけは自前で変換する。`Image.pixels` はどの色空間の画像でも
シーンリニアの float を返すので、次の処理をする。

| Blender のカラースペース                                              | 書き出し時の処理                         | `colorspace`  |
| --------------------------------------------------------------------- | ---------------------------------------- | ------------- |
| `Non-Color` / `Raw` / `Data` を含む名前                               | 変換せずそのまま 8bit 化                 | 2 (Non-Color) |
| それ以外（`sRGB` / `Linear` / `Filmic sRGB` / `AgX` / `ACEScg` など） | リニア → sRGB エンコードしてから 8bit 化 | 0 (sRGB)      |

したがって Blender エクスポーターが書き出す `colorspace` は
**0 か 2 のどちらか**で、1 (Linear) にはならない
（他のツールが書き出す可能性があるのでコードとしては残してある）。

再生側は変換不要で、そのまま `LoadTextureFromImage()` に渡せばよい。

法線・ラフネス・メタリックなどのマップが `sRGB` のままになっていると、
sRGB エンコードがかかって値が変わってしまう。SIMPLE モードでは
この状態を検出して警告を出す。

#### ブロックの内容

マテリアルが参照している Image Texture ノードの画像ごとに 1 ブロック。
`id` は画像名で、マテリアルの `*_map.image` や PRO モードの `props.image` から引く。

| key                | type            | 内容                                                        |
| ------------------ | --------------- | ----------------------------------------------------------- |
| `name`             | tstr            | 画像名                                                      |
| `width` / `height` | uint            | 解像度                                                      |
| `colorspace`       | uint / null     | カラースペースコード                                        |
| `filepath`         | tstr            | 絶対パス（埋め込みでないときはこれを読む）                  |
| `compression`      | tstr            | `NONE` / `PNG` / `JPEG`                                     |
| `format`           | tstr            | `compression` が `NONE` のときの `pixels` の形式（`RGBA8`） |
| `quality`          | uint (optional) | JPEG 品質 / PNG 圧縮レベル                                  |
| `embedded`         | bool            | `pixels` か `data` があるか                                 |
| `pixels`           | bstr (optional) | `NONE` のとき / uint8 RGBA × width × height                 |
| `data`             | bstr (optional) | `PNG` / `JPEG` のとき / エンコード済みファイルそのもの      |

- `Embed Textures` オプション ON のときだけ中身が入る。OFF ならパスのみ。
- 圧縮方式は **画像ごとに指定できる**。画像エディタ / シェーダーエディタの
  サイドバーの "Yui Blender to Raylib" パネルで設定する。既定は
  `Use Exporter Setting` で、エクスポーターの `Texture Compression` に従う。
- JPEG はアルファを保持できないので、アルファを含む画像に指定すると警告が出る。
- エンコードに失敗した画像は `NONE`（生 RGBA8）にフォールバックする。
- `data` は普通の PNG / JPEG ファイルなので、raylib なら
  `LoadImageFromMemory(YbrTextureFileExt(t), t->data, t->dataSize)` でそのまま読める。
- ピクセルは **上原点**（Blender の下原点から反転済み）。UV の V も反転済みなので、
  通常のファイル読み込みと同じ向きでそのまま使える。
- カラースペースが `sRGB` 系の画像は linear→sRGB 変換して 8bit 化する。
  `Non-Color` / `Raw` / `Linear` はそのまま 8bit 化する。
- 無圧縮のときは raylib へ
  `Image img = { pixels, width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };`
  で渡せる（`pixels` はローダーが所有しているのでコピーするか、解放順に注意）。

### EMPTY

エンプティは Blender 上でデータを持たないため、**オブジェクト名を ID** とする。
`display_type`(tstr) / `display_size`(float)

---

## 拡張と前方互換

`.ybr` は「後から項目を足していく」前提のフォーマットで、
**読み込み側は知らないものを読み飛ばす**。version を上げるのは、既存の項目の意味が変わるような後方非互換の変更のときだけ。

読み込み側の決まり:

- マップの**知らないキーは無視する**
- **知らない `kind` のデータブロックは無視する**
- **知らない列挙値（型コード）は `*_UNKNOWN` になる。**
  ファイル側が `null` を書いていた場合も同じ扱い
- **知らないフレームタイプ (`types`) のキーは読み飛ばす。**
  詰めて格納されるので、残ったキーだけで補間が成立する
- CBOR の**タグは外して中身だけを読む**
- 配列 / マップは definite-length のみ（indefinite-length は読めない）

| 壊れ方                                           | 読み込み側の振る舞い                           |
| ------------------------------------------------ | ---------------------------------------------- |
| `indices` が `vertex_count` の外を指す           | **エラー**（読み込み失敗）                     |
| `material_indices` が `materials` の外を指す     | **エラー**                                     |
| ボーンの `parent` が自分以降を指す               | **エラー**                                     |
| キーのフレーム番号が単調増加でない               | **エラー**                                     |
| `vertex_count` が `positions` の長さより大きい   | 実データに合わせて**切り詰める**               |
| `normals` / `uvs` / `colors` / `tangents` が短い | その配列を**捨てる**（無いものとして扱う）     |
| `skin` の配列が足りない                          | スキンを**捨てる**（スキン無しとして扱う）     |
| RAW テクスチャの画素が足りない                   | 埋め込みを**捨てて** `filepath` に倒す         |
| CBOR の入れ子が 64 段より深い                    | **エラー**（再帰でスタックを食い潰さないため） |
| 実データより大きい要素数の宣言                   | **エラー**（確保する前に弾く）                 |

## 書き出しオプション

| 名前                   | 既定   | 内容                                                    |
| ---------------------- | ------ | ------------------------------------------------------- |
| Selection Only         | OFF    | 選択オブジェクトのみ                                    |
| Hide in Viewport       | ON     | ビューポートで非表示のオブジェクト / コレクションを除外 |
| Disable in Renders     | OFF    | レンダリングが無効なオブジェクト / コレクションを除外   |
| Apply Modifiers        | ON     | モディファイア適用後の形状                              |
| Rest Pose              | ON     | アーマチュアモディファイアを一時無効化                  |
| Material Mode          | SIMPLE | SIMPLE / PRO                                            |
| Animations             | ON     | アクションをベイクして書き出す                          |
| Custom Properties      | ON     | `ybr_` で始まるカスタムプロパティを書き出す             |
| Deform Bones Only      | ON     | Deform が無効なボーンを書き出さない                     |
| Keep Raw Vertex Groups | OFF    | 正規化前の頂点グループも残す                            |
| Embed Textures         | OFF    | テクスチャデータを埋め込む                              |
| Texture Compression    | PNG    | 埋め込み時の既定の圧縮方式（画像ごとに上書き可）        |
| Texture Quality        | 90     | JPEG 品質 / PNG 圧縮レベル                              |
