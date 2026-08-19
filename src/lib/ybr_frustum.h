/*
	Yui Blender to Raylib - 視錐台 (フラスタム)
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_FRUSTUM_H
#define YBR_FRUSTUM_H

#include "raylib.h"
#include "raymath.h"

#ifdef __cplusplus
extern "C" {
#endif

// 視錐台カリング
typedef struct YbrFrustum {
	// 0:左 1:右 2:下 3:上 4:近 5:遠
	float planes[6][4];
} YbrFrustum;

// view * projection をまとめた行列から作る。
// raylib の並びなので YbrFrustumFromMatrix(MatrixMultiply(view, proj))。
YbrFrustum YbrFrustumFromMatrix(Matrix viewProjection);

// raylib の Camera3D から作る。aspect は画面の横 / 縦。
YbrFrustum YbrFrustumFromCamera(Camera3D camera, float aspect, float nearPlane,
								float farPlane);

// BeginMode3D() ... EndMode3D() の中で、今の行列から作る。
// 平行投影でもそのまま使える (rlgl の行列をそのまま読むため)。
YbrFrustum YbrFrustumCurrent(void);

// 何も切らない視錐台 (カメラが無いときの既定値)。
YbrFrustum YbrFrustumInfinite(void);

// 視錐台の中に入っているか (かすっていれば 1)。
int YbrFrustumContainsPoint(const YbrFrustum* f, Vector3 point);
int YbrFrustumContainsSphere(const YbrFrustum* f, Vector3 center, float radius);
int YbrFrustumContainsBox(const YbrFrustum* f, Vector3 boxMin, Vector3 boxMax);

#ifdef __cplusplus
}
#endif

#endif /* YBR_FRUSTUM_H */
