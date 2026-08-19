/*
	Yui Blender to Raylib - 当たり判定の共通部分
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_COLLISION_H
#define YBR_COLLISION_H

#include "ybr.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YBR_MODEL_PART_DECLARED
#define YBR_MODEL_PART_DECLARED
typedef struct YbrModelPart YbrModelPart;
#endif

// 三角形
typedef struct YbrTriangle {
	Vector3 v[3];			   // ワールド空間の頂点）反時計回り）
	Vector3 normal;			   // 面法線（正規化済み）
	const YbrNode* sceneNode;  // 元のシーンノード / NULL 可
	const YbrModelPart* node;  // 元のモデルパート / NULL 可
	const YbrMesh* mesh;	   // 元のメッシュ / NULL 可
	int meshTriangle;		   // メッシュ内の三角形番号
	int materialIndex;		   // メッシュ内のマテリアル番号 / -1
	unsigned int tag;		   // filter が付けた任意の値（既定 0）
} YbrTriangle;

// 判定の共通オプション
typedef struct YbrQueryOptions {
	unsigned int
		tagMask;  // 0なら全部。非0なら (tag & mask) が非0の三角形だけを見る
	int cullBackFace;  // 線分判定のみ。1 で裏面を無視
} YbrQueryOptions;

YbrQueryOptions YbrQueryOptionsDefaults(void);

// ----------------------------------------------------------------------------
// 判定結果

// 線分（レイ）の結果
typedef struct YbrRayHit {
	int hit;			   // 当たったら1
	Vector3 point;		   // 衝突点（ワールド座標）
	Vector3 normal;		   // 衝突したポリゴンの面法線
	float distance;		   // 線分の始点からの距離
	float t;			   // 線分内の位置 0..1
	float u, v;			   // 重心座標
	int frontFace;		   // 表から当たったら 1
	YbrTriangle triangle;  // 衝突したポリゴン (3頂点 + 法線)
} YbrRayHit;

// 球 / カプセルの結果
typedef struct YbrShapeHit {
	int hit;			   // 1枚以上に接触したら1
	int count;			   // 接触したポリゴン数
	Vector3 point;		   // 一番深い接触点（ポリゴン側の面上）
	Vector3 normal;		   // 押し出すベクトル（形状 <- ポリゴン）
	float depth;		   // めり込み量
	Vector3 resolve;	   // 形状をこれだけ動かせば離れる
	YbrTriangle triangle;  // 一番深いポリゴン
} YbrShapeHit;

// 三角形どうしの結果
typedef struct YbrTriHit {
	int hit;				 // 当たったら1
	int count;				 // 交差したポリゴン数
	Vector3 pointA, pointB;	 // 交差線分（点で接する場合は同じ座標）
	Vector3 point;			 // 交差線分の中点
	YbrTriangle triangle;	 // 交差したポリゴン
} YbrTriHit;

// 列挙コールバック
// visitor が 0 を返すとそこで打ち切る。戻り値は呼ばれた回数。
typedef int (*YbrTriangleVisitor)(const YbrTriangle* tri, void* userData);

// 単体の幾何ユーティリティ（当たり判定情報が無くても使える）
// 半径 radius の球を from -> to へ動かしたときの、三角形との最初の接触。
int YbrSweepSphereTriangle(Vector3 from, Vector3 to, float radius, Vector3 t0,
						   Vector3 t1, Vector3 t2, float* outT,
						   Vector3* outPoint, Vector3* outNormal);

// 三角形と AABB が交差するか（13軸SAT）
int YbrTriangleBoxOverlap(Vector3 a, Vector3 b, Vector3 c, Vector3 boxMin,
						  Vector3 boxMax);

// 点にもっとも近い三角形上の点
Vector3 YbrClosestPointOnTriangle(Vector3 p, Vector3 a, Vector3 b, Vector3 c);

// 線分と三角形の最短距離。outSeg / outTri は NULL 可。
// 交差している場合は 0 を返し、両方に交点が入る。
float YbrSegmentTriangleDistance(Vector3 a, Vector3 b, Vector3 t0, Vector3 t1,
								 Vector3 t2, Vector3* outSeg, Vector3* outTri);

// 線分と三角形の交差。当たったら 1 を返し、t (0..1) と重心座標を返す。
int YbrSegmentTriangleHit(Vector3 a, Vector3 b, Vector3 t0, Vector3 t1,
						  Vector3 t2, int cullBackFace, float* outT,
						  float* outU, float* outV);

#ifdef __cplusplus
}
#endif

#endif /* YBR_COLLISION_H */
