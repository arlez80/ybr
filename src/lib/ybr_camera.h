/*
	Yui Blender to Raylib - カメラ
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_CAMERA_H
#define YBR_CAMERA_H

#include "raylib.h"
#include "raymath.h"
#include "ybr.h"
#include "ybr_frustum.h"

#ifdef __cplusplus
extern "C" {
#endif

// シーンのカメラを raylib の Camera3D として読み込む。
// cameraNodeName が NULL なら最初に見つかったカメラを使う。
// クリップ距離は .ybr の値をそのまま outNear / outFar へ入れる (NULL 可)。
// 見つかれば 1。
int YbrCameraToRaylib(const YbrScene* scene, const char* cameraNodeName,
					  float aspect, Camera3D* outCamera, float* outNear,
					  float* outFar);

// シーンのカメラから直接 YbrFrustum を作る。
// カメラが無いときは「何も切らない」視錐台を返す。
YbrFrustum YbrFrustumFromScene(const YbrScene* scene,
							   const char* cameraNodeName, float aspect);

#ifdef __cplusplus
}
#endif

#endif /* YBR_CAMERA_H */
