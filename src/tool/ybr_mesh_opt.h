/*
	ybr_mesh_opt.h - メッシュの最適化
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_MESH_OPT_H
#define YBR_MESH_OPT_H

#include "ybr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct YbrMeshOptOptions {
	int mergeVertices; /* 重複頂点をまとめる            既定 1 */
	int dropUnused;	   /* 未使用頂点を捨てる            既定 1 */
	int optimizeCache; /* 三角形を並べ替える            既定 1 */
	int optimizeFetch; /* 頂点を並べ替える              既定 1 */
	int cacheSize;	   /* 想定する頂点キャッシュの段数  既定 32 */

	// 量子化 (0 なら丸めない)。値は「1 単位あたりの分割数」で、
	// 大きいほど細かい。位置はシーン単位なので、モデルの大きさに合わせる。
	float positionGrid; /* 例: 1024 なら 1/1024 単位に丸める 既定 0 */
	int normalBits;		/* 法線を 2^bits 段階に丸める        既定 0 */
	int uvBits;			/* UV を 2^bits 段階に丸める         既定 0 */

	// テクスチャの長辺の上限 (0 なら縮小しない)。RAW のみ有効。
	int maxTextureSize;
} YbrMeshOptOptions;

YbrMeshOptOptions YbrMeshOptDefaults(void);

typedef struct YbrMeshOptStats {
	int meshCount;
	int verticesBefore, verticesAfter;
	int trianglesBefore, trianglesAfter;
	// ACMR = Average Cache Miss Ratio (三角形 1 枚あたりのキャッシュミス数)。
	// 小さいほど良い。理想は 0.5 付近、最悪は 3.0。
	float acmrBefore, acmrAfter;

	int textureCount, texturesResized;
} YbrMeshOptStats;

// メッシュ 1 つを最適化する。成功なら 1。
int YbrOptimizeMesh(YbrMesh* mesh, const YbrMeshOptOptions* opts,
					YbrMeshOptStats* stats);

// シーン全体 (メッシュとテクスチャ) を最適化する。
int YbrOptimizeScene(YbrScene* scene, const YbrMeshOptOptions* opts,
					 YbrMeshOptStats* stats);

// 三角形の並びに対する ACMR を測る (最適化の効果確認用)
float YbrMeshComputeACMR(const unsigned int* indices, int triangleCount,
						 int vertexCount, int cacheSize);

#ifdef __cplusplus
}
#endif

#endif /* YBR_MESH_OPT_H */
