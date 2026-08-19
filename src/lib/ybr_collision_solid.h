/*
	Yui Blender to Raylib - 固定物当たり判定
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_COLLISION_SOLID_H
#define YBR_COLLISION_SOLID_H

#include "ybr.h"
#include "ybr_collision.h"

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// 当たり判定情報の作成

typedef struct YbrSolidBuildOptions {
	int maxDepth;			  // 8分木の最大深さ 1～16
	int maxTrianglesPerNode;  // これ以下なら分割しない
	float looseness;		  // 子ノードの判定箱を広げる倍率 1.0 = ぴったり
	int (*filter)(const YbrNode* node, const YbrMesh* mesh,
				  unsigned int* outTag, void* userData);
	void* filterUserData;
} YbrSolidBuildOptions;

// 既定値
YbrSolidBuildOptions YbrSolidBuildDefaults(void);

typedef struct YbrSolid YbrSolid;

// シーン全体から作る。opts は NULL 可 (既定値)。
// 三角形が 1 枚も無い場合も空の YbrSolid を返す (NULL はメモリ不足)。
YbrSolid* YbrSolidBuild(const YbrScene* scene,
						const YbrSolidBuildOptions* opts);

// ツリーの一部だけから作る。parentWorld は node の親のワールド行列
YbrSolid* YbrSolidBuildFromNode(const YbrScene* scene, const YbrNode* node,
								Matrix parentWorld,
								const YbrSolidBuildOptions* opts);

// 三角形の配列から直接作る。tris は複製されるので呼び出し後に解放してよい。
// ybr_dynamic.h がローカル空間の木を作るのにも使う。
YbrSolid* YbrSolidBuildFromTriangles(const YbrTriangle* tris, int count,
									 const YbrSolidBuildOptions* opts);

void YbrSolidUnload(YbrSolid* col);

// 情報
int YbrSolidGetTriangleCount(const YbrSolid* col);
const YbrTriangle* YbrSolidGetTriangle(const YbrSolid* col, int index);
// 全体の AABB。三角形が無い場合は 0 を返し、out は書き換えない。
int YbrSolidGetBounds(const YbrSolid* col, Vector3* outMin, Vector3* outMax);
int YbrSolidGetNodeCount(const YbrSolid* col);
int YbrSolidGetDepth(const YbrSolid* col);

// ----------------------------------------------------------------------------
// 判定関数

// 線分 a -> b。もっとも手前の 1 枚を返す。
int YbrSolidSegment(const YbrSolid* col, Vector3 a, Vector3 b,
					const YbrQueryOptions* opts, YbrRayHit* out);

// レイ（方向 + 最大距離）。maxDistance <= 0 なら無限として扱う。
int YbrSolidRay(const YbrSolid* col, Vector3 origin, Vector3 direction,
				float maxDistance, const YbrQueryOptions* opts, YbrRayHit* out);

// 球
int YbrSolidSphere(const YbrSolid* col, Vector3 center, float radius,
				   const YbrQueryOptions* opts, YbrShapeHit* out);

// カプセル（線分 a-b の周り半径 radius）
int YbrSolidCapsule(const YbrSolid* col, Vector3 a, Vector3 b, float radius,
					const YbrQueryOptions* opts, YbrShapeHit* out);

// 三角形。coplanar（同一平面上）の重なりは検出しない。
int YbrSolidTriangle(const YbrSolid* col, Vector3 v0, Vector3 v1, Vector3 v2,
					 const YbrQueryOptions* opts, YbrTriHit* out);

// スイープ (動く球の連続判定)
int YbrSolidSweepSphere(const YbrSolid* col, Vector3 from, Vector3 to,
						float radius, const YbrQueryOptions* opts,
						YbrRayHit* out);

// 全ての接触ポリゴンを取得する
int YbrSolidOverlapSphere(const YbrSolid* col, Vector3 center, float radius,
						  const YbrQueryOptions* opts,
						  YbrTriangleVisitor visitor, void* userData);
int YbrSolidOverlapCapsule(const YbrSolid* col, Vector3 a, Vector3 b,
						   float radius, const YbrQueryOptions* opts,
						   YbrTriangleVisitor visitor, void* userData);
// AABB。三角形と箱が実際に交差するものだけを返す (SAT)。
int YbrSolidOverlapBox(const YbrSolid* col, Vector3 boxMin, Vector3 boxMax,
					   const YbrQueryOptions* opts, YbrTriangleVisitor visitor,
					   void* userData);

#ifdef __cplusplus
}
#endif

#endif /* YBR_COLLISION_SOLID_H */
