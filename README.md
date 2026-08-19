# Yui Blender to Raylib (.ybr)

Blender のシーンを **raylib** 向けに書き出して読み込むためのフォーマットとライブラリです。次の3つでできています。

- **Blender アドオン**
  - シーンを `.ybr`（CBOR ベース）として書き出す
- **C ライブラリ**
  - `.ybr` の読み書き、描画、アニメーション、当たり判定、シェーダーノードからGLSLへの変換
- **ybr_tool**
  - `.ybr` の中身を見たり、メッシュ / アニメーションを軽量化したりするコマンドラインツール

フォーマットの詳細は [YBR_FORMAT.md](YBR_FORMAT.md) にあります。

## できること

- メッシュ
  - 三角形ポリゴン
    - 3 < nのポリゴンは自動で分割されます
  - 法線 / 接線
  - UV
  - 頂点カラー
- スケルトンとスキニング
  - GPU & CPU 自動切り替え
- 合成ツリー付きアニメーション
- マテリアル
  - ノードグラフからGLSLへの変換
- ライトとシャドウマップ
  - 平行光
  - 点光源
  - スポット
- 視錐台カリング
- 半透明の並べ替え
- 当たり判定
  - 静的な8分木
  - ボーン追従
- カーブ
  - レールや経路の取得

## ビルド & インストール

### 必要なもの

- **raylib 5.0 以降**（`raylib.h` / `raymath.h` / `rlgl.h`）
- **C11が通るコンパイラ**
- **Python 3** + **SCons**
  - `pip install scons`

### ビルド

ビルド時のコマンドは以下の通りです。

```sh
  scons                ビルドする (= scons all)
  scons all            ライブラリ / ツール / テスト / サンプルを全部
  scons lib            ライブラリだけ
  scons tool           ツールだけ (bin/ybr_tool)
  scons test           テストだけ (bin/ybr_test)
  scons example        サンプルだけ
  scons -c             クリーン
```

以下のオプションが付いています。

```sh
  mode=release         最適化する (既定)
  mode=debug           デバッグ情報を付けて最適化しない

  raylib_src=<dir>     raylib.h / raymath.h / rlgl.h の場所 (RAYLIB_SRC でも可)
  raylib_lib=<dir>     raylib のライブラリの場所 (RAYLIB_LIB でも可)
```

### Blender アドオン

`blender/io_export_yui_ybr.py` を Blender のアドオンとしてインストールし、有効にすると **File > Export > Yui Blender to Raylib (.ybr)** がメニューに追加されます。
Blender 4.5, 5.0 以降で動きます。

## サンプルコード

```c
#include "ybr.h"
#include "ybr_model.h"

YbrScene *scene = YbrLoad("scene.ybr");
if (!scene) { printf("%s\n", YbrGetError()); return 1; }

YbrModel         *model = YbrModelLoad(scene, NULL);
YbrModelInstance *inst  = YbrModelInstanceCreate(model);

YbrWorld *world = YbrWorldCreate();
YbrWorldApplySceneLights(world, scene);   // .ybr のライトをそのまま使う

while (!WindowShouldClose()) {
    YbrWorldSetCamera(world, camera,
                      (float)GetScreenWidth() / GetScreenHeight(), 0, 0);
    BeginDrawing();
        BeginMode3D(camera);
            YbrModelInstanceDraw(inst, world, WHITE);
        EndMode3D();
    EndDrawing();
}

YbrWorldUnload(world);
YbrModelInstanceUnload(inst);
YbrModelUnload(model);
YbrUnload(scene);
```

動作するサンプルは `src/examples/` にあります。（準備中）

# API

## 基本

### 読み込み

```c
YbrScene *YbrLoad(const char *fileName);
```

| 引数       | 説明          |
| ---------- | ------------- |
| `fileName` | `.ybr` のパス |

成功でシーン、失敗で `NULL` が返ってきます。
エラーメッセージは `YbrGetError()` で取れます。
使い終えたら `YbrUnload()` で解放します。

```c
YbrScene *YbrLoadFromMemory(const unsigned char *data, size_t size);
```

| 引数   | 説明                        |
| ------ | --------------------------- |
| `data` | メモリ上に読み込んだ `.ybr` |
| `size` | データのサイズ              |

成功でシーン、失敗で `NULL` が返ってきます。
エラーメッセージは `YbrGetError()` で取れます。
使い終えたら `YbrUnload()` で解放します。

### モデル生成

```c
YbrModel *YbrModelLoad(const YbrScene *scene, const YbrModelOptions *options);
```

| 引数 | 説明 |
|---|---|
| `scene` | 元になるシーン |
| `options` | 生成設定。**`NULL` なら全項目が既定値**（`YbrModelOptionsDefaults()`） |

成功で `YbrModel`、失敗で `NULL`。
シーンから描画で使えるデータを作り出します。描画用のインスタンス生成は `YbrModelInstanceCreate()` を使ってください。
YbrModel内で使用しているので、`scene`を先に解放してはいけません。

主な設定項目は次のとおりです。

| 項目              | 既定 | 内容                                                   |
| ----------------- | ---- | ------------------------------------------------------ |
| `materialShaders` | 1    | マテリアルごとにシェーダーを作るか                     |
| `loadTextures`    | 1    | テクスチャをGPUへ載せるか                              |
| `gpuSkinning`     | 1    | GPUスキニングを使うか（ボーン数が多いと CPU に落ちる） |
| `maxBones`        | 64   | GPU スキニングのボーン上限                             |

### 描画

```c
void YbrModelInstanceDraw(const YbrModelInstance *inst, const YbrWorld *world,
                          Color tint);
int  YbrModelInstanceDrawCulled(const YbrModelInstance *inst, const YbrWorld *world,
                                Color tint);
```

| 引数    | 説明                                                                                             |
| ------- | ------------------------------------------------------------------------------------------------ |
| `inst`  | 描くモデルインスタンス                                                                           |
| `world` | ライトなど描画環境設定。 `NULL` ならRaylibのように指定した色がそのまま表示される状態で描画する。 |
| `tint`  | 全体に掛ける色                                                                                   |

`BeginMode3D()` の中で呼びます。マテリアルが半透明を含む場合は「不透明 → 半透明（奥から）」の2パスを内部で行います。`DrawCulled()` は `world` の視錐台で描画した場合`1`を返し、画面外の場合は`0`を返します。

### 描画環境作成

```c
YbrWorld *YbrWorldCreateEx(const YbrWorldOptions *options);
```

| 引数      | 説明                                                 |
| --------- | ---------------------------------------------------- |
| `options` | 描画環境オプション設定。`NULL` で既定が設定されます。 |
| `scene`   | ライトを取り込む元のシーン                           |

`YbrWorld` はライト・カメラ・視錐台・描画キュー・シャドウマップをまとめて管理するためのものです。
`YbrWorldUnload()` で解放します。

```c
int       YbrWorldApplySceneLights(YbrWorld *world, const YbrScene *scene);
```

| 引数    | 説明                       |
| ------- | -------------------------- |
| `world` | 設定する描画環境           |
| `scene` | ライトを取り込む元のシーン |

取り込んだ灯数を返します。
`YbrWorldUnload()` で解放します。

YbrWorldOptionsで設定できる内容は以下の通りです。

| 影の設定           | 既定  | 内容                                 |
| ------------------ | ----- | ------------------------------------ |
| `shadows`          | 0     | 影を使うか                           |
| `shadowResolution` | 2048  | 深度テクスチャの一辺                 |
| `shadowLights`     | 1     | 影を落とすライトの数（1〜2）         |
| `shadowBias`       | 0.002 | 深度をずらす量（縞が出るなら上げる） |
| `shadowDistance`   | 20.0  | 平行光の影が届く広さ                 |

### シェーダー変換

```c
YbrShaderResult YbrShaderFromMaterialEx(const YbrMaterial *material,
                                        const YbrShaderOptions *options);
```

| 引数       | 説明                               |
| ---------- | ---------------------------------- |
| `material` | 変換するマテリアル                 |
| `options`  | 生成設定。`NULL`で既定値が選ばれる |

`error` が `YBR_SHADER_OK` なら `vertexCode` / `fragmentCode` に変換したシェーダーコードが文字列で入ります。
失敗時は両方 `NULL` で、`errorMessage` にエラーメッセージが入ります。
どちらの場合も `YbrUnloadShaderResult()` で解放してください。

### 当たり判定（固定物）

```c
YbrSolid *YbrSolidBuild(const YbrScene *scene, const YbrSolidOptions *options);
```

| 引数      | 説明                                 |
| --------- | ------------------------------------ |
| `scene`   | 元になるシーン                       |
| `options` | 構築設定。`NULL`で既定値を設定します |

`YbrSolid` はレストポーズ基準の前計算なので、動かないもの（地形・建物）向けです。ボーンで動くものは `YbrDynamic` を使います。

## APIリスト

### ファイルの読み書き

```c
YbrScene *YbrLoad(const char *fileName);
YbrScene *YbrLoadFromMemory(const unsigned char *data, size_t size);
void YbrUnload(YbrScene *scene);
int YbrSave(const YbrScene *scene, const char *fileName);
unsigned char *YbrSaveToMemory(const YbrScene *scene, size_t *outSize);
const char *YbrGetError(void);
const YbrMesh *YbrFindMesh(const YbrScene *scene, const char *id);
const YbrArmature *YbrGetArmature(const YbrScene *scene);
const YbrCurve *YbrFindCurve(const YbrScene *scene, const char *id);
const YbrLight *YbrFindLight(const YbrScene *scene, const char *id);
const YbrEmpty *YbrFindEmpty(const YbrScene *scene, const char *id);
const YbrMaterial *YbrFindMaterial(const YbrScene *scene, const char *id);
const YbrAnimation *YbrFindAnimation(const YbrScene *scene, const char *id);
const YbrNodeGroup *YbrFindNodeGroup(const YbrScene *scene, const char *id);
const YbrTextureData *YbrFindTexture(const YbrScene *scene, const char *id);
const YbrCamera *YbrFindCamera(const YbrScene *scene, const char *id);
const YbrNode *YbrFindNode(const YbrScene *scene, const char *name);
const YbrCustomProperty *YbrFindCustomProperty(const YbrNode *node, const char *key);
double YbrGetCustomNumber(const YbrNode *node, const char *key, double fallback);
const char *YbrGetCustomText(const YbrNode *node, const char *key, const char *fallback);
const char *YbrTextureFileExt(const YbrTextureData *tex);
void YbrWalkNodes(const YbrScene *scene, YbrNodeVisitor visitor, void *userData);
```

### カーブ補助

Curveをパスとして使用するためのユーティリティ関数。

```c
Vector3 YbrCurveGetPoint(const YbrCurve *curve, int splineIndex, float weight, int resolution);
Vector3 YbrCurveGetTangent(const YbrCurve *curve, int splineIndex, float weight, int resolution);
float YbrCurveGetLength(const YbrCurve *curve, int splineIndex, int resolution);
float YbrCurveGetWeightAtDistance(const YbrCurve *curve, int splineIndex, float distance, int resolution, int iterations);
Vector3 YbrCurveGetPointAtDistance(const YbrCurve *curve, int splineIndex, float distance, int resolution, int iterations);
```

### アニメーションツリー

```c
int YbrAnimBlendTreeInit(YbrAnimBlendTree *tree, const YbrScene *scene, const YbrPose *pose, YbrAnimBlendNode *root);
void YbrAnimBlendTreeUnload(YbrAnimBlendTree *tree);
YbrAnimBlendNode *YbrAnimBlendTreeFind(const YbrAnimBlendTree *tree, int uniqueId);
int YbrAnimBlendTreeEval(YbrAnimBlendTree *tree, float delta, YbrPose *outPose);
```

### モデル関連

```c
Texture2D YbrLoadTexture(const YbrTextureData *tex);
YbrModelOptions YbrModelOptionsDefaults(void);
YbrModel *YbrModelLoad(const YbrScene *scene, const YbrModelOptions *opts);
void YbrModelUnload(YbrModel *model);
const YbrArmature *YbrModelGetArmature(const YbrModel *model);
int YbrModelHasTransparent(const YbrModel *model);
int YbrModelIsPartTransparent(const YbrModel *model, int part);
int YbrModelGetLocalBounds(const YbrModel *model, Vector3 *outMin, Vector3 *outMax);
const YbrNode *YbrModelGetNodeSource(const YbrModel *model, int node);
int YbrModelFindNode(const YbrModel *model, const char *name);
void YbrModelSetShader(YbrModel *model, int materialIndex, Shader shader);
YbrModelMaterial YbrModelMakeMaterial(Shader shader, Texture2D texture, Color baseColor, int transparent);
YbrModelInstance *YbrModelInstanceCreate(const YbrModel *model);
void YbrModelInstanceUnload(YbrModelInstance *inst);
const YbrModel *YbrModelInstanceGetModel(const YbrModelInstance *inst);
int YbrModelInstanceIsVisible(const YbrModelInstance *inst, const YbrFrustum *frustum);
void YbrModelInstanceSetTransform(YbrModelInstance *inst, Matrix transform);
Matrix YbrModelInstanceGetTransform(const YbrModelInstance *inst);
YbrPose *YbrModelInstanceGetPose(YbrModelInstance *inst, const char *armatureId);
int YbrModelInstanceHasPose(const YbrModelInstance *inst);
void YbrModelInstanceApplyPose(YbrModelInstance *inst);
void YbrModelInstanceSetPartVisible(YbrModelInstance *inst, int part, int visible);
int YbrModelInstanceIsPartVisible(const YbrModelInstance *inst, int part);
void YbrModelInstanceSetAllPartsVisible(YbrModelInstance *inst, int visible);
int YbrModelInstanceSetPartVisibleByMesh(YbrModelInstance *inst, const char *meshId, int visible);
int YbrModelInstanceSetPartVisibleByMaterial(YbrModelInstance *inst, const char *materialId, int visible);
void YbrModelInstanceSetNodeVisible(YbrModelInstance *inst, int node, int visible);
int YbrModelInstanceIsNodeVisible(const YbrModelInstance *inst, int node);
void YbrModelInstanceSetAllNodesVisible(YbrModelInstance *inst, int visible);
int YbrModelInstanceSetNodeVisibleByName(YbrModelInstance *inst, const char *name, int visible);
void YbrModelInstanceSetNodeMaterial(YbrModelInstance *inst, int node, const YbrModelMaterial *material);
const YbrModelMaterial *YbrModelInstanceGetNodeMaterial(const YbrModelInstance *inst, int node);
int YbrModelInstanceSetNodeMaterialByName(YbrModelInstance *inst, const char *name, const YbrModelMaterial *material);
void YbrModelInstanceClearNodeMaterials(YbrModelInstance *inst);
int YbrModelInstanceGetBounds(const YbrModelInstance *inst, Vector3 *outMin, Vector3 *outMax);
void YbrModelInstanceUpdateBounds(YbrModelInstance *inst);
void YbrModelInstanceSetSkinBounds(YbrModelInstance *inst, const YbrSkinBounds *bounds);
const YbrSkinBounds *YbrModelInstanceGetSkinBounds(const YbrModelInstance *inst);
int YbrModelBuildRestBounds(YbrModel *model);
void YbrModelSkinPart(const YbrModelPart *p, const YbrPose *pose, float *dstPos, float *dstNrm);
int YbrModelInstanceGetBoneWorld(const YbrModelInstance *inst, const char *boneName, Matrix *out);
int YbrModelInstanceGetNodeWorld(const YbrModelInstance *inst, const char *nodeName, Matrix *out);
void YbrModelInstanceSetUserData(YbrModelInstance *inst, void *userData);
void *YbrModelInstanceGetUserData(const YbrModelInstance *inst);
void YbrModelInstanceDraw(const YbrModelInstance *inst, const YbrWorld *world, Color tint);
void YbrModelInstanceDrawWires(const YbrModelInstance *inst, const YbrWorld *world, Color tint);
void YbrModelInstanceDrawPass(const YbrModelInstance *inst, const YbrWorld *world, Color tint, unsigned int pass);
void YbrModelInstanceDrawWiresPass(const YbrModelInstance *inst, const YbrWorld *world, Color tint, unsigned int pass);
int YbrModelInstanceDrawCulled(const YbrModelInstance *inst, const YbrWorld *world, Color tint);
int YbrModelInstanceDrawPassCulled(const YbrModelInstance *inst, const YbrWorld *world, Color tint, unsigned int pass);
```

### カメラ

```c
int YbrCameraToRaylib(const YbrScene *scene, const char *cameraNodeName, float aspect, Camera3D *outCamera, float *outNear, float *outFar);
YbrFrustum YbrFrustumFromMatrix(Matrix viewProjection);
YbrFrustum YbrFrustumFromCamera(Camera3D camera, float aspect, float nearPlane, float farPlane);
YbrFrustum YbrFrustumCurrent(void);
int YbrFrustumContainsPoint (const YbrFrustum *f, Vector3 point);
int YbrFrustumContainsSphere(const YbrFrustum *f, Vector3 center, float radius);
int YbrFrustumContainsBox (const YbrFrustum *f, Vector3 boxMin, Vector3 boxMax);
YbrFrustum YbrFrustumFromScene(const YbrScene *scene, const char *cameraNodeName, float aspect);
```

### 描画環境

```c
int YbrSceneFindNodeWorld(const YbrScene *scene, YbrNodeType type, const char *dataId, const char *nodeName, Matrix *out);
YbrWorldOptions YbrWorldOptionsDefaults(void);
YbrWorld *YbrWorldCreate(void);
YbrWorld *YbrWorldCreateEx(const YbrWorldOptions *options);
void YbrWorldUnload(YbrWorld *world);
int YbrWorldRenderShadows(YbrWorld *world);
void YbrWorldSetShadowLight(YbrWorld *world, int slot, int lightIndex);
Texture2D YbrWorldGetShadowMap(const YbrWorld *world, int slot);
int YbrWorldLightCastsShadow(const YbrWorld *world, int index);
int YbrWorldResolveShadowLights(YbrWorld *world, int *out, int max);
Matrix YbrWorldLightMatrix(const YbrWorld *world, int lightIndex);
int YbrWorldGetLightCount(const YbrWorld *world);
void YbrWorldSetLightCount(YbrWorld *world, int count);
void YbrWorldSetLight(YbrWorld *world, int index, Vector3 direction, Color color);
void YbrWorldSetLightEx(YbrWorld *world, int index, const YbrWorldLight *light);
const YbrWorldLight *YbrWorldGetLight(const YbrWorld *world, int index);
void YbrWorldSetPointLight(YbrWorld *world, int index, Vector3 position, Color color, float range, float intensity);
void YbrWorldSetSpotLight(YbrWorld *world, int index, Vector3 position, Vector3 direction, Color color, float range, float intensity, float inner, float outer);
void YbrWorldSetAmbient(YbrWorld *world, Color ambient);
int YbrWorldApplySceneLights(YbrWorld *world, const YbrScene *scene);
int YbrWorldPickLights(const YbrWorld *world, const YbrModelInstance *inst, int *out, int max);
void YbrWorldSetCamera(YbrWorld *world, Camera3D camera, float aspect, float nearPlane, float farPlane);
int YbrWorldSetCameraFromScene(YbrWorld *world, const YbrScene *scene, const char *cameraNodeName, float aspect);
int YbrWorldGetCamera(const YbrWorld *world, Camera3D *out);
const YbrFrustum *YbrWorldGetFrustum(const YbrWorld *world);
void YbrWorldSetFrustum(YbrWorld *world, const YbrFrustum *frustum);
void YbrWorldSetCulling(YbrWorld *world, int enable);
void YbrWorldBeginFrame(YbrWorld *world);
int YbrWorldSubmit(YbrWorld *world, const YbrModelInstance *inst, Color tint);
void YbrWorldDrawQueue(YbrWorld *world);
int YbrWorldGetQueueCount(const YbrWorld *world);
```

### 当たり判定：共通

```c
YbrQueryOptions YbrQueryOptionsDefaults(void);
int YbrSweepSphereTriangle(Vector3 from, Vector3 to, float radius, Vector3 t0, Vector3 t1, Vector3 t2, float *outT, Vector3 *outPoint, Vector3 *outNormal);
int YbrTriangleBoxOverlap(Vector3 a, Vector3 b, Vector3 c, Vector3 boxMin, Vector3 boxMax);
Vector3 YbrClosestPointOnTriangle(Vector3 p, Vector3 a, Vector3 b, Vector3 c);
float YbrSegmentTriangleDistance(Vector3 a, Vector3 b, Vector3 t0, Vector3 t1, Vector3 t2, Vector3 *outSeg, Vector3 *outTri);
int YbrSegmentTriangleHit(Vector3 a, Vector3 b, Vector3 t0, Vector3 t1, Vector3 t2, int cullBackFace, float *outT, float *outU, float *outV);
```

### 当たり判定：固定物

8分木で前計算した静的コリジョン。

```c
YbrSolidBuildOptions YbrSolidBuildDefaults(void);
YbrSolid *YbrSolidBuild(const YbrScene *scene, const YbrSolidBuildOptions *opts);
YbrSolid *YbrSolidBuildFromNode(const YbrScene *scene, const YbrNode *node, Matrix parentWorld, const YbrSolidBuildOptions *opts);
YbrSolid *YbrSolidBuildFromTriangles(const YbrTriangle *tris, int count, const YbrSolidBuildOptions *opts);
void YbrSolidUnload(YbrSolid *col);
int YbrSolidGetTriangleCount(const YbrSolid *col);
const YbrTriangle *YbrSolidGetTriangle(const YbrSolid *col, int index);
int YbrSolidGetBounds(const YbrSolid *col, Vector3 *outMin, Vector3 *outMax);
int YbrSolidGetNodeCount(const YbrSolid *col);
int YbrSolidGetDepth(const YbrSolid *col);
int YbrSolidSegment(const YbrSolid *col, Vector3 a, Vector3 b, const YbrQueryOptions *opts, YbrRayHit *out);
int YbrSolidRay(const YbrSolid *col, Vector3 origin, Vector3 direction, float maxDistance, const YbrQueryOptions *opts, YbrRayHit *out);
int YbrSolidSphere(const YbrSolid *col, Vector3 center, float radius, const YbrQueryOptions *opts, YbrShapeHit *out);
int YbrSolidCapsule(const YbrSolid *col, Vector3 a, Vector3 b, float radius, const YbrQueryOptions *opts, YbrShapeHit *out);
int YbrSolidTriangle(const YbrSolid *col, Vector3 v0, Vector3 v1, Vector3 v2, const YbrQueryOptions *opts, YbrTriHit *out);
int YbrSolidSweepSphere(const YbrSolid *col, Vector3 from, Vector3 to, float radius, const YbrQueryOptions *opts, YbrRayHit *out);
int YbrSolidOverlapSphere(const YbrSolid *col, Vector3 center, float radius, const YbrQueryOptions *opts, YbrTriangleVisitor visitor, void *userData);
int YbrSolidOverlapCapsule(const YbrSolid *col, Vector3 a, Vector3 b, float radius, const YbrQueryOptions *opts, YbrTriangleVisitor visitor, void *userData);
int YbrSolidOverlapBox(const YbrSolid *col, Vector3 boxMin, Vector3 boxMax, const YbrQueryOptions *opts, YbrTriangleVisitor visitor, void *userData);
```

### 当たり判定：ボーン追従あり

ボーンに追従するコリジョン。

```c
YbrDynamicBuildOptions YbrDynamicBuildDefaults(void);
YbrDynamic *YbrDynamicFromInstance(YbrModelInstance *inst, const YbrDynamicBuildOptions *opts);
void YbrDynamicUnload(YbrDynamic *dyn);
void YbrDynamicUpdate(YbrDynamic *dyn);
void YbrDynamicSetTransform(YbrDynamic *dyn, Matrix transform);
Matrix YbrDynamicGetTransform(const YbrDynamic *dyn);
void YbrDynamicSetUserData(YbrDynamic *dyn, void *userData);
void *YbrDynamicGetUserData(const YbrDynamic *dyn);
void YbrDynamicSetEnabled(YbrDynamic *dyn, int enabled);
int YbrDynamicIsEnabled(const YbrDynamic *dyn);
const YbrModelInstance *YbrDynamicGetInstance(const YbrDynamic *dyn);
int YbrDynamicGetTriangleCount(const YbrDynamic *dyn);
int YbrDynamicGetPartCount(const YbrDynamic *dyn);
int YbrDynamicGetBounds(const YbrDynamic *dyn, Vector3 *outMin, Vector3 *outMax);
Matrix YbrDynamicGetPartTransform(const YbrDynamic *dyn, int part);
int YbrDynamicGetPartBone(const YbrDynamic *dyn, int part);
int YbrDynamicGetPartTriangleCount(const YbrDynamic *dyn, int part);
const YbrTriangle *YbrDynamicGetPartTriangle(const YbrDynamic *dyn, int part, int index);
int YbrDynamicSegment(const YbrDynamic *dyn, Vector3 a, Vector3 b, const YbrQueryOptions *opts, YbrRayHit *out);
int YbrDynamicRay(const YbrDynamic *dyn, Vector3 origin, Vector3 direction, float maxDistance, const YbrQueryOptions *opts, YbrRayHit *out);
int YbrDynamicSphere(const YbrDynamic *dyn, Vector3 center, float radius, const YbrQueryOptions *opts, YbrShapeHit *out);
int YbrDynamicCapsule(const YbrDynamic *dyn, Vector3 a, Vector3 b, float radius, const YbrQueryOptions *opts, YbrShapeHit *out);
int YbrDynamicTriangle(const YbrDynamic *dyn, Vector3 v0, Vector3 v1, Vector3 v2, const YbrQueryOptions *opts, YbrTriHit *out);
int YbrDynamicSweepSphere(const YbrDynamic *dyn, Vector3 from, Vector3 to, float radius, const YbrQueryOptions *opts, YbrRayHit *out);
int YbrDynamicOverlapSphere(const YbrDynamic *dyn, Vector3 center, float radius, const YbrQueryOptions *opts, YbrTriangleVisitor visitor, void *userData);
int YbrDynamicOverlapCapsule(const YbrDynamic *dyn, Vector3 a, Vector3 b, float radius, const YbrQueryOptions *opts, YbrTriangleVisitor visitor, void *userData);
int YbrDynamicGetLastBone(const YbrDynamic *dyn);
const char *YbrDynamicGetLastBoneName(const YbrDynamic *dyn);
YbrDynamicWorld *YbrDynamicWorldCreate(void);
void YbrDynamicWorldUnload(YbrDynamicWorld *world);
int YbrDynamicWorldAdd(YbrDynamicWorld *world, YbrDynamic *dyn);
int YbrDynamicWorldRemove(YbrDynamicWorld *world, YbrDynamic *dyn);
int YbrDynamicWorldGetCount(const YbrDynamicWorld *world);
YbrDynamic *YbrDynamicWorldGet(const YbrDynamicWorld *world, int index);
void YbrDynamicWorldUpdate(YbrDynamicWorld *world);
int YbrDynamicWorldSegment(YbrDynamicWorld *world, Vector3 a, Vector3 b, const YbrQueryOptions *opts, YbrDynamicRayHit *out);
int YbrDynamicWorldSweepSphere(YbrDynamicWorld *world, Vector3 from, Vector3 to, float radius, const YbrQueryOptions *opts, YbrDynamicRayHit *out);
int YbrDynamicWorldSphere(YbrDynamicWorld *world, Vector3 center, float radius, const YbrQueryOptions *opts, YbrDynamicShapeHit *out);
int YbrDynamicWorldCapsule(YbrDynamicWorld *world, Vector3 a, Vector3 b, float radius, const YbrQueryOptions *opts, YbrDynamicShapeHit *out);
int YbrDynamicWorldOverlapBox(YbrDynamicWorld *world, Vector3 boxMin, Vector3 boxMax, YbrDynamicVisitor visitor, void *userData);
```
