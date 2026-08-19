/*
	Yui Blender to Raylib - モデル描画の内部インターフェース
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_MODEL_INTERNAL_H
#define YBR_MODEL_INTERNAL_H

#include "ybr_model.h"
#include "ybr_world.h"

#ifdef __cplusplus
extern "C" {
#endif

// テクスチャ (ybr_texture.c)
// 画像 id からテクスチャを取る。同じ id は 1 回しか読まない。
Texture2D YbrModelGetTexture(YbrModel* model, const YbrScene* scene,
							 const char* imageId, int wrap, int filter);
// キャッシュしたテクスチャをまとめて解放する。
void YbrModelUnloadTextures(YbrModel* model);

// 描画 (ybr_model.c)
// パート 1 枚を描く。mat は解決済みのマテリアル。world は NULL 可。
void YbrModelDrawPart(YbrModelInstance* inst, const YbrModelMaterial* mat,
					  const YbrModelPart* part, int partIndex,
					  Matrix transform, const YbrWorld* world,
					  const float tint[4]);
// ノードに差し替えがあればそれを、無ければパート本来のマテリアルを返す。
const YbrModelMaterial* YbrModelResolveMaterial(const YbrModelInstance* inst,
												int nodeIndex,
												const YbrModelPart* part);
// パートのワールド AABB (スキンを持たないパート専用)。
void YbrModelPartWorldBox(const YbrModelPart* part, Matrix world,
						  Vector3* outMin, Vector3* outMax);

#ifdef __cplusplus
}
#endif

#endif /* YBR_MODEL_INTERNAL_H */
