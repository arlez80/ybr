/*
	Yui Blender to Raylib - 視錐台 (フラスタム)
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_frustum.h"

#include <math.h>
#include <string.h>

#include "rlgl.h"

// 平面を正規化しておくと、球の判定で半径をそのまま使える
static void plane_set(float* p, float a, float b, float c, float d)
{
	float len = sqrtf(a * a + b * b + c * c);
	if (0.0f < len) {
		a /= len;
		b /= len;
		c /= len;
		d /= len;
	}
	p[0] = a;
	p[1] = b;
	p[2] = c;
	p[3] = d;
}

YbrFrustum YbrFrustumFromMatrix(Matrix m)
{
	YbrFrustum f;

	// 行ベクトル
	float r0[4] = {m.m0, m.m4, m.m8, m.m12};
	float r1[4] = {m.m1, m.m5, m.m9, m.m13};
	float r2[4] = {m.m2, m.m6, m.m10, m.m14};
	float r3[4] = {m.m3, m.m7, m.m11, m.m15};

	// 左 / 右 : -w <= x <= w
	plane_set(f.planes[0], r3[0] + r0[0], r3[1] + r0[1], r3[2] + r0[2],
			  r3[3] + r0[3]);
	plane_set(f.planes[1], r3[0] - r0[0], r3[1] - r0[1], r3[2] - r0[2],
			  r3[3] - r0[3]);
	// 下 / 上 : -w <= y <= w
	plane_set(f.planes[2], r3[0] + r1[0], r3[1] + r1[1], r3[2] + r1[2],
			  r3[3] + r1[3]);
	plane_set(f.planes[3], r3[0] - r1[0], r3[1] - r1[1], r3[2] - r1[2],
			  r3[3] - r1[3]);
	// 近 / 遠 : OpenGL の深度は -w <= z <= w
	plane_set(f.planes[4], r3[0] + r2[0], r3[1] + r2[1], r3[2] + r2[2],
			  r3[3] + r2[3]);
	plane_set(f.planes[5], r3[0] - r2[0], r3[1] - r2[1], r3[2] - r2[2],
			  r3[3] - r2[3]);
	return f;
}

YbrFrustum YbrFrustumFromCamera(Camera3D camera, float aspect, float nearPlane,
								float farPlane)
{
	// raylib の BeginMode3D() と同じ組み立て方をする
	if (!(0.0f < aspect)) aspect = 1.0f;
#ifdef RL_CULL_DISTANCE_NEAR
	if (!(0.0f < nearPlane)) nearPlane = (float)RL_CULL_DISTANCE_NEAR;
	if (!(0.0f < farPlane)) farPlane = (float)RL_CULL_DISTANCE_FAR;
#else
	if (!(0.0f < nearPlane)) nearPlane = 0.01f;
	if (!(0.0f < farPlane)) farPlane = 1000.0f;
#endif

	Matrix proj;
	if (camera.projection == CAMERA_ORTHOGRAPHIC) {
		// raylib は fovy を「縦の大きさ」として使う
		double top = camera.fovy / 2.0;
		double right = top * aspect;
		proj = MatrixOrtho(-right, right, -top, top, nearPlane, farPlane);
	}
	else {
		proj = MatrixPerspective(camera.fovy * DEG2RAD, aspect, nearPlane,
								 farPlane);
	}
	Matrix view = MatrixLookAt(camera.position, camera.target, camera.up);
	return YbrFrustumFromMatrix(MatrixMultiply(view, proj));
}

static float plane_distance(const float* p, Vector3 v)
{
	return p[0] * v.x + p[1] * v.y + p[2] * v.z + p[3];
}

int YbrFrustumContainsPoint(const YbrFrustum* f, Vector3 point)
{
	if (!f) return 1;
	for (int i = 0; i < 6; i++)
		if (plane_distance(f->planes[i], point) < 0.0f) return 0;
	return 1;
}

int YbrFrustumContainsSphere(const YbrFrustum* f, Vector3 center, float radius)
{
	if (!f) return 1;
	if (radius < 0.0f) radius = 0.0f;
	for (int i = 0; i < 6; i++)
		if (plane_distance(f->planes[i], center) < -radius) return 0;
	return 1;
}

int YbrFrustumContainsBox(const YbrFrustum* f, Vector3 boxMin, Vector3 boxMax)
{
	if (!f) return 1;

	// 箱が空 (min > max) なら中身が無いので外扱い
	if (boxMax.x < boxMin.x || boxMax.y < boxMin.y || boxMax.z < boxMin.z)
		return 0;

	for (int i = 0; i < 6; i++) {
		const float* p = f->planes[i];
		// 平面の法線ともっとも同じ向きの角 (positive vertex) だけを見る。
		// それが外側なら、箱全体が確実に外側にある。
		Vector3 v;
		v.x = (0.0f <= p[0]) ? boxMax.x : boxMin.x;
		v.y = (0.0f <= p[1]) ? boxMax.y : boxMin.y;
		v.z = (0.0f <= p[2]) ? boxMax.z : boxMin.z;
		if (plane_distance(p, v) < 0.0f) return 0;
	}
	// 6 枚すべてで外に出ていない = 中にあるか、かすっている。
	return 1;
}

// 何も切らない視錐台。
// 平面の法線を 0 にしておくと、距離が常に d (= 1) になって中判定になる。
YbrFrustum YbrFrustumInfinite(void)
{
	YbrFrustum f;
	memset(&f, 0, sizeof(f));
	for (int i = 0; i < 6; i++) f.planes[i][3] = 1.0f;
	return f;
}

// BeginMode3D() の中でだけ意味がある (rlgl の今の行列を読む)。
YbrFrustum YbrFrustumCurrent(void)
{
	return YbrFrustumFromMatrix(
		MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection()));
}
