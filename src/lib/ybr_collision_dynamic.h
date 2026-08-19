/*
	Yui Blender to Raylib - スキニングあり当たり判定
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_COLLISION_DYNAMIC_H
#define YBR_COLLISION_DYNAMIC_H

#include "ybr.h"
#include "ybr_collision.h"
#include "ybr_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct YbrDynamic YbrDynamic;

typedef struct YbrDynamicBuildOptions {
	int maxDepth;			  // ボーンごとの 8 分木の深さ
	int maxTrianglesPerNode;  // これ以下なら分割しない
	float looseness;		  // 子ノードの判定箱を広げる倍率
	int (*filter)(const YbrMesh* mesh, unsigned int* outTag, void* userData);
	void* filterUserData;
} YbrDynamicBuildOptions;

YbrDynamicBuildOptions YbrDynamicBuildDefaults(void);

// model のポリゴンから作る。model は解放しないこと。
YbrDynamic* YbrDynamicFromInstance(YbrModelInstance* inst,
								   const YbrDynamicBuildOptions* opts);
void YbrDynamicUnload(YbrDynamic* dyn);

// 毎フレーム
// ポーズを変えたら呼ぶ。ボーンの行列とワールド AABB を更新するだけ。
void YbrDynamicUpdate(YbrDynamic* dyn);

// モデル全体に掛かる行列
void YbrDynamicSetTransform(YbrDynamic* dyn, Matrix transform);
Matrix YbrDynamicGetTransform(const YbrDynamic* dyn);

void YbrDynamicSetUserData(YbrDynamic* dyn, void* userData);
void* YbrDynamicGetUserData(const YbrDynamic* dyn);
void YbrDynamicSetEnabled(YbrDynamic* dyn, int enabled);
int YbrDynamicIsEnabled(const YbrDynamic* dyn);

// 情報
const YbrModelInstance* YbrDynamicGetInstance(const YbrDynamic* dyn);
int YbrDynamicGetTriangleCount(const YbrDynamic* dyn);
// パート数 = ボーン数 + (ウェイトの無い三角形があれば 1)
int YbrDynamicGetPartCount(const YbrDynamic* dyn);
// 現在のポーズを反映したワールド AABB
int YbrDynamicGetBounds(const YbrDynamic* dyn, Vector3* outMin,
						Vector3* outMax);
// パート i の現在のワールド行列
Matrix YbrDynamicGetPartTransform(const YbrDynamic* dyn, int part);
// パート i がどのボーンか
int YbrDynamicGetPartBone(const YbrDynamic* dyn, int part);
int YbrDynamicGetPartTriangleCount(const YbrDynamic* dyn, int part);
// パート i の index 番目の三角形
const YbrTriangle* YbrDynamicGetPartTriangle(const YbrDynamic* dyn, int part,
											 int index);

// 判定
int YbrDynamicSegment(const YbrDynamic* dyn, Vector3 a, Vector3 b,
					  const YbrQueryOptions* opts, YbrRayHit* out);
int YbrDynamicRay(const YbrDynamic* dyn, Vector3 origin, Vector3 direction,
				  float maxDistance, const YbrQueryOptions* opts,
				  YbrRayHit* out);
int YbrDynamicSphere(const YbrDynamic* dyn, Vector3 center, float radius,
					 const YbrQueryOptions* opts, YbrShapeHit* out);
int YbrDynamicCapsule(const YbrDynamic* dyn, Vector3 a, Vector3 b, float radius,
					  const YbrQueryOptions* opts, YbrShapeHit* out);
int YbrDynamicTriangle(const YbrDynamic* dyn, Vector3 v0, Vector3 v1,
					   Vector3 v2, const YbrQueryOptions* opts, YbrTriHit* out);
int YbrDynamicSweepSphere(const YbrDynamic* dyn, Vector3 from, Vector3 to,
						  float radius, const YbrQueryOptions* opts,
						  YbrRayHit* out);

// 接触したポリゴンをすべて受け取る (三角形はワールド空間で渡される)
int YbrDynamicOverlapSphere(const YbrDynamic* dyn, Vector3 center, float radius,
							const YbrQueryOptions* opts,
							YbrTriangleVisitor visitor, void* userData);
int YbrDynamicOverlapCapsule(const YbrDynamic* dyn, Vector3 a, Vector3 b,
							 float radius, const YbrQueryOptions* opts,
							 YbrTriangleVisitor visitor, void* userData);

// どのボーンに当たったか
int YbrDynamicGetLastBone(const YbrDynamic* dyn);
const char* YbrDynamicGetLastBoneName(const YbrDynamic* dyn);

// ----------------------------------------------------------------------------
// 生成 / 破棄
typedef struct YbrDynamicWorld YbrDynamicWorld;

YbrDynamicWorld* YbrDynamicWorldCreate(void);
// dyn の所有権は移らない。Unload しても中身は解放しない。
void YbrDynamicWorldUnload(YbrDynamicWorld* world);

int YbrDynamicWorldAdd(YbrDynamicWorld* world, YbrDynamic* dyn);
int YbrDynamicWorldRemove(YbrDynamicWorld* world, YbrDynamic* dyn);
int YbrDynamicWorldGetCount(const YbrDynamicWorld* world);
YbrDynamic* YbrDynamicWorldGet(const YbrDynamicWorld* world, int index);

// 各 YbrDynamic の YbrDynamicUpdate() を呼び、広域判定の並びを作り直す。
// 動きが連続していれば並びはほとんど変わらないので、ここはほぼ O(n)。
void YbrDynamicWorldUpdate(YbrDynamicWorld* world);

typedef struct YbrDynamicRayHit {
	YbrRayHit hit;
	YbrDynamic* dynamic;
	int bone;  // 当たったボーン / -1 は静的パート
} YbrDynamicRayHit;

typedef struct YbrDynamicShapeHit {
	YbrShapeHit hit;
	YbrDynamic* dynamic;  // もっとも深く当たったもの
	int bone;
	int dynamicCount;  // 接触したYbrDynamicの数
} YbrDynamicShapeHit;

int YbrDynamicWorldSegment(YbrDynamicWorld* world, Vector3 a, Vector3 b,
						   const YbrQueryOptions* opts, YbrDynamicRayHit* out);
int YbrDynamicWorldSweepSphere(YbrDynamicWorld* world, Vector3 from, Vector3 to,
							   float radius, const YbrQueryOptions* opts,
							   YbrDynamicRayHit* out);
int YbrDynamicWorldSphere(YbrDynamicWorld* world, Vector3 center, float radius,
						  const YbrQueryOptions* opts, YbrDynamicShapeHit* out);
int YbrDynamicWorldCapsule(YbrDynamicWorld* world, Vector3 a, Vector3 b,
						   float radius, const YbrQueryOptions* opts,
						   YbrDynamicShapeHit* out);

// AABB が重なっている YbrDynamic を列挙する (広域判定だけ / 0 で打ち切り)
typedef int (*YbrDynamicVisitor)(YbrDynamic* dyn, void* userData);
int YbrDynamicWorldOverlapBox(YbrDynamicWorld* world, Vector3 boxMin,
							  Vector3 boxMax, YbrDynamicVisitor visitor,
							  void* userData);

#ifdef __cplusplus
}
#endif

#endif /* YBR_COLLISION_DYNAMIC_H */
