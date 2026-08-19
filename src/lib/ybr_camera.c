/*
	Yui Blender to Raylib - カメラ
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_camera.h"

#include <math.h>
#include <string.h>

// ----------------------------------------------------------------------------
// カメラ系

// カメラを設定する
int YbrCameraToRaylib(const YbrScene* scene, const char* cameraNodeName,
					  float aspect, Camera3D* outCamera, float* outNear,
					  float* outFar)
{
	if (!scene || scene->cameraCount <= 0 || !outCamera) return 0;

	Matrix world = MatrixIdentity();
	const YbrCamera* cam = NULL;

	for (int i = 0; i < scene->cameraCount && !cam; i++)
		if (YbrSceneFindNodeWorld(scene, YBR_NODE_CAMERA, scene->cameras[i].id,
								  cameraNodeName, &world))
			cam = &scene->cameras[i];
	if (!cam) return 0;

	// クリップ距離は Blender の値をそのまま返す。
	if (outNear) *outNear = (0.0f < cam->clipStart) ? cam->clipStart : 0.01f;
	if (outFar) *outFar = (0.0f < cam->clipEnd) ? cam->clipEnd : 1000.0f;

	// Blender のカメラは -Z を向き、+Y が上。書き出し時に Y-up へ変換済み。
	Vector3 pos = {world.m12, world.m13, world.m14};
	Vector3 forward =
		Vector3Normalize((Vector3){-world.m8, -world.m9, -world.m10});
	Vector3 up = Vector3Normalize((Vector3){world.m4, world.m5, world.m6});

	outCamera->position = pos;
	outCamera->target = Vector3Add(pos, forward);
	outCamera->up = up;
	outCamera->projection = (cam->type == YBR_CAMERA_ORTHO)
								? CAMERA_ORTHOGRAPHIC
								: CAMERA_PERSPECTIVE;

	if (cam->type == YBR_CAMERA_ORTHO) {
		// raylib は fovy を「縦の大きさ」として使う
		outCamera->fovy = (0.0f < cam->orthoScale) ? cam->orthoScale : 10.0f;
		return 1;
	}

	// fovy を決める
	if (!(0.0f < aspect)) aspect = 1.0f;

	float fovy = cam->fovY;
	int fitHorizontal;
	switch (cam->sensorFit) {
		case YBR_SENSOR_FIT_HORIZONTAL:
			fitHorizontal = 1;
			break;
		case YBR_SENSOR_FIT_VERTICAL:
			fitHorizontal = 0;
			break;
		default:
			fitHorizontal = (1.0f <= aspect);
			break;
	}
	if (fitHorizontal && 0.0f < cam->fovX) {
		// 横基準の画角から縦の画角を出す
		fovy = 2.0f * atanf(tanf(cam->fovX * 0.5f) / aspect);
	}
	else if (!(0.0f < fovy) && 0.0f < cam->fovX) {
		fovy = cam->fovX;
	}
	if (!(0.0f < fovy)) fovy = 45.0f * DEG2RAD;

	outCamera->fovy = fovy * RAD2DEG;
	return 1;
}

// ----------------------------------------------------------------------------
// シーンのカメラから作る視錐台
// (組み立てそのものは ybr_frustum.c。ここは .ybr のカメラを引くだけ)

YbrFrustum YbrFrustumFromScene(const YbrScene* scene,
							   const char* cameraNodeName, float aspect)
{
	Camera3D cam;
	float nearPlane = 0.0f, farPlane = 0.0f;

	if (!YbrCameraToRaylib(scene, cameraNodeName, aspect, &cam, &nearPlane,
						   &farPlane))
		return YbrFrustumInfinite();
	return YbrFrustumFromCamera(cam, aspect, nearPlane, farPlane);
}
