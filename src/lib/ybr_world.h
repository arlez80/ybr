/*
	Yui Blender to Raylib - 描画環境
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_WORLD_H
#define YBR_WORLD_H

#include "raylib.h"
#include "raymath.h"
#include "ybr.h"
#include "ybr_frustum.h"
#include "ybr_shader.h"

#ifdef __cplusplus
extern "C" {
#endif

// ybr_model.h の型を指すだけなので前方宣言でよい。
// (ybr_model.h 側は struct タグ形式で定義するので二重 typedef にならない)
#ifndef YBR_MODEL_FWD_DECLARED
#define YBR_MODEL_FWD_DECLARED
typedef struct YbrModelInstance YbrModelInstance;
typedef struct YbrModelPart YbrModelPart;
typedef struct YbrModelMaterial YbrModelMaterial;
#endif

// ----------------------------------------------------------------------------
// ライト

// world が持てるライトの数。シェーダーが一度に扱えるのは
// YBR_WORLD_MAX_ACTIVE_LIGHTS
// までなので、体ごとに「効きそうな順」で選んで送る。
#define YBR_WORLD_MAX_LIGHTS 32

// 1 体を描くときにシェーダーへ送れる灯の数 (焼き込み済みの上限)
#define YBR_WORLD_MAX_ACTIVE_LIGHTS YBR_SHADER_MAX_LIGHTS

// 影を落とせるライトの数 (深度テクスチャの枚数)
#define YBR_WORLD_MAX_SHADOWS YBR_SHADER_MAX_SHADOWS

// ライトの種類
typedef enum {
	YBR_LIGHTKIND_DIRECTIONAL = 0,	// 平行光（減衰なし）
	YBR_LIGHTKIND_POINT = 1,		// 点光源
	YBR_LIGHTKIND_SPOT = 2			// スポット
} YbrLightKind;

// 1灯分の設定
typedef struct YbrWorldLight {
	Vector3 direction;	// 光が進む向き（平行光 / スポットの軸）
	Vector4 color;

	// 点光源 / スポット
	YbrLightKind kind;
	Vector3 position;  // ワールド座標での位置
	float range;	   // 届く距離。0 なら打ち切らない
	float intensity;   // color に掛かる倍率。0 以下は 1 とみなす
	float spotInner;   // コーン内側の半角（ラジアン）
	float spotOuter;   // コーン外側の半角（ラジアン）
} YbrWorldLight;

// ----------------------------------------------------------------------------
// 描画キュー / 影

// 描画キューの 1 件 (内部用)。世界ぜんたいで並べ替えるために持つ。
typedef struct YbrWorldItem {
	const YbrModelInstance* inst;
	const YbrModelPart* part;
	const YbrModelMaterial* material;
	int partIndex;
	Matrix transform;  // ノードのワールド行列
	float depth;	   // カメラからの距離 (半透明の並べ替え用)
	float tint[4];
	int transparent;
} YbrWorldItem;

// 深度テクスチャ 1 枚ぶん (内部用)。GPU 資源なので YbrWorld が持つ。
typedef struct YbrShadowMap {
	unsigned int fbo;  // 深度だけのフレームバッファ
	Texture2D depth;   // サンプルできる深度テクスチャ
	int resolution;
	Matrix lightVP;	 // ライト視点の view * projection
	int lightIndex;	 // world->lights の添字（-1 なら未使用）
	int ready;
} YbrShadowMap;

// YbrWorld を作るときの設定
typedef struct YbrWorldOptions {
	// 影を使うか。1 にすると深度テクスチャを作り、YbrWorldRenderShadows()
	// で影を焼けるようになる。
	int shadows;
	// 深度テクスチャの一辺（既定 2048）。大きいほど輪郭が細かくなるが重い。
	int shadowResolution;
	// 影を落とすライトの数（1..YBR_WORLD_MAX_SHADOWS / 既定 1）
	int shadowLights;
	// 深度の比較をずらす量（既定 0.002）
	// 小さすぎると自分の影で縞が出て、大きすぎると影が浮く
	float shadowBias;
	// 平行光の影が届く広さ（既定 20）
	// 光には位置が無いので、この大きさの箱をカメラの注視点にかぶせて焼く
	float shadowDistance;
} YbrWorldOptions;

// YbrWorldの既定値を返す関数
YbrWorldOptions YbrWorldOptionsDefaults(void);

typedef struct YbrWorld {
	// ライト
	int lightCount;
	YbrWorldLight lights[YBR_WORLD_MAX_LIGHTS];
	Vector4 ambientColor;

	// カメラと視錐台
	Camera3D camera;
	int hasCamera;
	float aspect, nearPlane, farPlane;
	YbrFrustum frustum;
	int culling;  // 0 にするとカリングしない

	// 描画キュー
	YbrWorldItem* queue;
	int queueCount, queueCap;

	// 影
	YbrWorldOptions options;
	YbrShadowMap shadows[YBR_WORLD_MAX_SHADOWS];
	int shadowCount;  // 実際に焼いた枚数
} YbrWorld;

// 影なしの world を作る (YbrWorldCreateEx(NULL) と同じ)
YbrWorld* YbrWorldCreate(void);
// 設定を指定して作る。options が NULL なら既定 (影なし)。
YbrWorld* YbrWorldCreateEx(const YbrWorldOptions* options);
void YbrWorldUnload(YbrWorld* world);

// 影
int YbrWorldRenderShadows(YbrWorld* world);
// 影を落とすライトを選び直す（既定は平行光 / スポットを先頭から）
// lightIndex に -1 を渡すとその枠を使わない。
void YbrWorldSetShadowLight(YbrWorld* world, int slot, int lightIndex);
// 焼いた深度テクスチャ (デバッグ表示用 / 無ければ id が 0)
Texture2D YbrWorldGetShadowMap(const YbrWorld* world, int slot);

// そのライトが影を落とせるか（平行光とスポットだけ）
int YbrWorldLightCastsShadow(const YbrWorld* world, int index);
// 影を落とすライトの添字を集める。戻り値は数。
int YbrWorldResolveShadowLights(YbrWorld* world, int* out, int max);
// ライト視点の view * projection
Matrix YbrWorldLightMatrix(const YbrWorld* world, int lightIndex);

// 有効な灯数 (上限 YBR_WORLD_MAX_LIGHTS)。SetLight 系は必要に応じて
// ここを自動で伸ばす。減らすと、その先の灯は消える。
// 実際にシェーダーへ送られるのは、この中から体ごとに選ばれた
// YBR_WORLD_MAX_ACTIVE_LIGHTS 灯まで。
int YbrWorldGetLightCount(const YbrWorld* world);
void YbrWorldSetLightCount(YbrWorld* world, int count);

// ライトを置く
void YbrWorldSetLight(YbrWorld* world, int index, Vector3 direction,
					  Color color);
// 種類まで含めてまとめて設定する。light が NULL なら何もしない。
void YbrWorldSetLightEx(YbrWorld* world, int index, const YbrWorldLight* light);
// 今の設定を読む (範囲外なら NULL)
const YbrWorldLight* YbrWorldGetLight(const YbrWorld* world, int index);

// 点光源。range が 0 なら距離で打ち切らない（逆二乗のみ）。
// intensity は color に掛かる倍率で、0 以下なら 1 とみなす。
void YbrWorldSetPointLight(YbrWorld* world, int index, Vector3 position,
						   Color color, float range, float intensity);
// スポット。direction は光が進む向き、inner / outer
// はコーンの半角（ラジアン）。 outer <= inner のときは inner
// を少し内側へ寄せて縁をぼかす。
void YbrWorldSetSpotLight(YbrWorld* world, int index, Vector3 position,
						  Vector3 direction, Color color, float range,
						  float intensity, float inner, float outer);

void YbrWorldSetAmbient(YbrWorld* world, Color ambient);

// シーンの LIGHT ブロックからライトを取り込む。
int YbrWorldApplySceneLights(YbrWorld* world, const YbrScene* scene);

// この体を照らすライトを「効きそうな順」に最大 max 灯選ぶ。
int YbrWorldPickLights(const YbrWorld* world, const YbrModelInstance* inst,
					   int* out, int max);

// カメラ
void YbrWorldSetCamera(YbrWorld* world, Camera3D camera, float aspect,
					   float nearPlane, float farPlane);
// .ybr に置いてあるカメラをそのまま使う（clip 距離も .ybr の値）
// 見つかれば 1
int YbrWorldSetCameraFromScene(YbrWorld* world, const YbrScene* scene,
							   const char* cameraNodeName, float aspect);
// raylib へ渡すための Camera3D。カメラを入れていなければ 0 を返す。
int YbrWorldGetCamera(const YbrWorld* world, Camera3D* out);
// 今の視錐台。カメラを入れていない / カリングを切っていれば NULL。
const YbrFrustum* YbrWorldGetFrustum(const YbrWorld* world);
// 視錐台を直接入れる (影のパスなどで、カメラとは別の視錐台を使いたいとき)
void YbrWorldSetFrustum(YbrWorld* world, const YbrFrustum* frustum);
// カリングの入り切り (既定は入り)
void YbrWorldSetCulling(YbrWorld* world, int enable);

// 描画キュー
void YbrWorldBeginFrame(YbrWorld* world);
int YbrWorldSubmit(YbrWorld* world, const YbrModelInstance* inst, Color tint);
void YbrWorldDrawQueue(YbrWorld* world);
int YbrWorldGetQueueCount(const YbrWorld* world);

#ifdef __cplusplus
}
#endif

#endif /* YBR_WORLD_H */
