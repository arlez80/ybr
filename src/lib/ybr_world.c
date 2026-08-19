/*
	Yui Blender to Raylib - 描画環境
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_world.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rlgl.h"
#include "ybr_camera.h"
#include "ybr_internal.h"
#include "ybr_model_internal.h"

static void shadow_map_unload(YbrShadowMap* sm);

// 生成 / 破棄

YbrWorldOptions YbrWorldOptionsDefaults(void)
{
	YbrWorldOptions o;
	memset(&o, 0, sizeof(o));
	o.shadows = 0;
	o.shadowResolution = 2048;
	o.shadowLights = 1;
	o.shadowBias = 0.002f;
	o.shadowDistance = 20.0f;
	return o;
}

YbrWorld* YbrWorldCreate(void) { return YbrWorldCreateEx(NULL); }

YbrWorld* YbrWorldCreateEx(const YbrWorldOptions* options)
{
	YbrWorld* w = (YbrWorld*)YBR_CALLOC(1, sizeof(YbrWorld));
	if (!w) return NULL;

	w->options = options ? *options : YbrWorldOptionsDefaults();
	if (w->options.shadowResolution <= 0) w->options.shadowResolution = 2048;
	if (w->options.shadowLights <= 0) w->options.shadowLights = 1;
	if (YBR_WORLD_MAX_SHADOWS < w->options.shadowLights)
		w->options.shadowLights = YBR_WORLD_MAX_SHADOWS;
	if (!(0.0f < w->options.shadowBias)) w->options.shadowBias = 0.002f;
	if (!(0.0f < w->options.shadowDistance)) w->options.shadowDistance = 20.0f;

	for (int i = 0; i < YBR_WORLD_MAX_SHADOWS; i++)
		w->shadows[i].lightIndex = -1;

	// シェーダー側の既定値と同じ向きを入れておく。
	static const Vector3 dirs[YBR_WORLD_MAX_ACTIVE_LIGHTS] = {
		{-0.577f, -0.577f, -0.577f},
		{0.577f, -0.577f, 0.577f},
		{0.000f, -1.000f, 0.000f},
		{0.577f, -0.302f, -0.759f},
	};
	for (int i = 0; i < YBR_WORLD_MAX_LIGHTS; i++) {
		w->lights[i].direction = dirs[i % YBR_WORLD_MAX_ACTIVE_LIGHTS];
		w->lights[i].color = (Vector4){1.0f, 1.0f, 1.0f, 1.0f};
		w->lights[i].kind = YBR_LIGHTKIND_DIRECTIONAL;
		w->lights[i].intensity = 1.0f;
	}
	w->ambientColor = (Vector4){0.1f, 0.1f, 0.1f, 1.0f};
	w->lightCount = 0;
	w->culling = 1;	 // カメラを入れたらカリングする
	return w;
}

void YbrWorldUnload(YbrWorld* world)
{
	if (!world) return;
	for (int i = 0; i < YBR_WORLD_MAX_SHADOWS; i++)
		shadow_map_unload(&world->shadows[i]);
	YBR_FREE(world->queue);
	YBR_FREE(world);
}

void YbrWorldSetShadowLight(YbrWorld* world, int slot, int lightIndex)
{
	if (!world || slot < 0 || YBR_WORLD_MAX_SHADOWS <= slot) return;
	if (YBR_WORLD_MAX_LIGHTS <= lightIndex) return;
	world->shadows[slot].lightIndex = (lightIndex < 0) ? -1 : lightIndex;
}

// ライト

int YbrWorldGetLightCount(const YbrWorld* world)
{
	return world ? world->lightCount : 0;
}

void YbrWorldSetLightCount(YbrWorld* world, int count)
{
	if (!world) return;
	if (count < 0) count = 0;
	if (YBR_WORLD_MAX_LIGHTS < count) count = YBR_WORLD_MAX_LIGHTS;
	world->lightCount = count;
}

// 触られていないフィールドで従来どおり動くように整える
static YbrWorldLight light_defaults(void)
{
	YbrWorldLight l;
	memset(&l, 0, sizeof(l));
	l.direction = (Vector3){0.0f, -1.0f, 0.0f};
	l.color = (Vector4){1.0f, 1.0f, 1.0f, 1.0f};
	l.kind = YBR_LIGHTKIND_DIRECTIONAL;
	l.intensity = 1.0f;
	return l;
}

// index 番の灯を使うことにする (必要なら lightCount を伸ばす)
static int world_slot(YbrWorld* world, int index)
{
	if (!world || index < 0 || YBR_WORLD_MAX_LIGHTS <= index) return 0;
	if (world->lightCount < index + 1) world->lightCount = index + 1;
	return 1;
}

void YbrWorldSetLightEx(YbrWorld* world, int index, const YbrWorldLight* light)
{
	if (!light || !world_slot(world, index)) return;
	world->lights[index] = *light;
	if (!(0.0f < world->lights[index].intensity))
		world->lights[index].intensity = 1.0f;
}

void YbrWorldSetLight(YbrWorld* world, int index, Vector3 direction,
					  Color color)
{
	YbrWorldLight l = light_defaults();
	l.direction = direction;
	l.color = (Vector4){color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
						color.a / 255.0f};
	YbrWorldSetLightEx(world, index, &l);
}

const YbrWorldLight* YbrWorldGetLight(const YbrWorld* world, int index)
{
	if (!world || index < 0 || YBR_WORLD_MAX_LIGHTS <= index) return NULL;
	return &world->lights[index];
}

void YbrWorldSetPointLight(YbrWorld* world, int index, Vector3 position,
						   Color color, float range, float intensity)
{
	YbrWorldLight l = light_defaults();
	l.kind = YBR_LIGHTKIND_POINT;
	l.position = position;
	l.range = (0.0f < range) ? range : 0.0f;
	l.intensity = (0.0f < intensity) ? intensity : 1.0f;
	l.color = (Vector4){color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
						color.a / 255.0f};
	YbrWorldSetLightEx(world, index, &l);
}

void YbrWorldSetSpotLight(YbrWorld* world, int index, Vector3 position,
						  Vector3 direction, Color color, float range,
						  float intensity, float inner, float outer)
{
	YbrWorldLight l = light_defaults();
	l.kind = YBR_LIGHTKIND_SPOT;
	l.position = position;
	l.direction = direction;
	l.range = (0.0f < range) ? range : 0.0f;
	l.intensity = (0.0f < intensity) ? intensity : 1.0f;
	l.color = (Vector4){color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
						color.a / 255.0f};

	// 外側 < 内側 だと縁がぼけないので、内側を少し内へ寄せる
	if (outer <= inner) inner = outer * 0.9f;
	l.spotInner = inner;
	l.spotOuter = outer;
	YbrWorldSetLightEx(world, index, &l);
}

void YbrWorldSetAmbient(YbrWorld* world, Color ambient)
{
	if (!world) return;
	world->ambientColor = (Vector4){ambient.r / 255.0f, ambient.g / 255.0f,
									ambient.b / 255.0f, ambient.a / 255.0f};
}

// シーンからの取り込み

// Blender のライトが照らす向き。ライトは自分の -Z 方向を照らす。
static Vector3 light_direction(Matrix world)
{
	Vector3 d = {-world.m8, -world.m9, -world.m10};
	if (Vector3LengthSqr(d) <= 1e-12f) return (Vector3){0.0f, -1.0f, 0.0f};
	return Vector3Normalize(d);
}

int YbrWorldApplySceneLights(YbrWorld* world, const YbrScene* scene)
{
	if (!world || !scene || scene->lightCount <= 0) return 0;

	int used = 0;
	for (int i = 0; i < scene->lightCount && used < YBR_WORLD_MAX_LIGHTS; i++) {
		const YbrLight* li = &scene->lights[i];

		Matrix node = MatrixIdentity();
		if (!YbrSceneFindNodeWorld(scene, YBR_NODE_LIGHT, li->id, NULL, &node))
			continue;

		YbrWorldLight l = light_defaults();
		l.direction = light_direction(node);
		l.position = (Vector3){node.m12, node.m13, node.m14};

		// 色は Blender のリニア RGB をそのまま。強さは intensity に分けるので、
		// 色は 0..1 に収まっていればよい。
		float maxc = li->color.x;
		if (maxc < li->color.y) maxc = li->color.y;
		if (maxc < li->color.z) maxc = li->color.z;
		if (1.0f < maxc) {
			l.color = (Vector4){li->color.x / maxc, li->color.y / maxc,
								li->color.z / maxc, 1.0f};
		}
		else {
			l.color = (Vector4){li->color.x, li->color.y, li->color.z, 1.0f};
		}

		float e = (0.0f < li->energy) ? li->energy : 1.0f;
		if (1.0f < maxc) e *= maxc;

		switch (li->type) {
			case YBR_LIGHT_SUN:
				// SUN の energy は W/m^2 なので、そのまま倍率として使える
				l.kind = YBR_LIGHTKIND_DIRECTIONAL;
				l.intensity = (e < 20.0f) ? e : 20.0f;
				break;

			case YBR_LIGHT_SPOT:
				l.kind = YBR_LIGHTKIND_SPOT;
				// Blender の spot_size はコーンの全角。半角へ直す。
				// spot_blend は「内側が外側の何割から始まるか」。
				l.spotOuter = li->spotSize * 0.5f;
				l.spotInner =
					l.spotOuter * (1.0f - Clamp(li->spotBlend, 0.0f, 1.0f));
				l.intensity = e / (4.0f * PI);
				break;

			case YBR_LIGHT_AREA:
				// 面光源。形は再現できないので点光源で近似するが、
				l.kind = YBR_LIGHTKIND_POINT;
				l.intensity = e / PI;
				break;

			default:
				// POINT。Blender の W は全方向へ広がるので P/(4pi)
				l.kind = YBR_LIGHTKIND_POINT;
				l.intensity = e / (4.0f * PI);
				break;
		}
		if (1000.0f < l.intensity) l.intensity = 1000.0f;
		if (li->hasCutoff && 0.0f < li->cutoffDistance)
			l.range = li->cutoffDistance;

		world->lights[used] = l;
		used++;
	}
	world->lightCount = used;
	return used;
}

// ライトを選ぶ
// 点が箱の中なら 0、外なら箱までの距離
static float box_distance(Vector3 p, Vector3 lo, Vector3 hi)
{
	float dx = (p.x < lo.x) ? lo.x - p.x : (hi.x < p.x) ? p.x - hi.x : 0.0f;
	float dy = (p.y < lo.y) ? lo.y - p.y : (hi.y < p.y) ? p.y - hi.y : 0.0f;
	float dz = (p.z < lo.z) ? lo.z - p.z : (hi.z < p.z) ? p.z - hi.z : 0.0f;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

int YbrWorldPickLights(const YbrWorld* world, const YbrModelInstance* inst,
					   int* out, int max)
{
	if (!world || !out || max <= 0) return 0;

	Vector3 lo = {0, 0, 0}, hi = {0, 0, 0};
	int hasBox = inst ? YbrModelInstanceGetBounds(inst, &lo, &hi) : 0;

	// 小さいほど「効く」スコア。平行光は 0 で最優先
	float score[YBR_WORLD_MAX_LIGHTS];
	int cand[YBR_WORLD_MAX_LIGHTS];
	int n = 0;

	for (int i = 0; i < world->lightCount && i < YBR_WORLD_MAX_LIGHTS; i++) {
		const YbrWorldLight* l = &world->lights[i];
		float s;

		if (l->kind == YBR_LIGHTKIND_DIRECTIONAL) {
			s = 0.0f;
		}
		else {
			float d = hasBox ? box_distance(l->position, lo, hi) : 0.0f;
			// 届く距離の外なら、この体には効かないので捨てる
			if (0.0f < l->range && l->range < d) continue;
			// 強い灯ほど遠くても効くので、強さで割って比べる
			float power = (0.0f < l->intensity) ? l->intensity : 1.0f;
			s = (d * d) / power + 0.001f;
		}
		cand[n] = i;
		score[n] = s;
		n++;
	}

	// 選ぶのはたかだか 4 個なので、単純な選択ソートで足りる
	int picked = 0;
	while (picked < max && picked < n) {
		int best = picked;
		for (int i = picked + 1; i < n; i++)
			if (score[i] < score[best]) best = i;
		float ts = score[picked];
		score[picked] = score[best];
		score[best] = ts;
		int tc = cand[picked];
		cand[picked] = cand[best];
		cand[best] = tc;
		out[picked] = cand[picked];
		picked++;
	}
	return picked;
}

// カメラと視錐台
void YbrWorldSetCamera(YbrWorld* world, Camera3D camera, float aspect,
					   float nearPlane, float farPlane)
{
	if (!world) return;
	if (!(0.0f < aspect)) aspect = 1.0f;

	world->camera = camera;
	world->aspect = aspect;
	world->nearPlane = nearPlane;
	world->farPlane = farPlane;
	world->hasCamera = 1;
	world->frustum = YbrFrustumFromCamera(camera, aspect, nearPlane, farPlane);
}

int YbrWorldSetCameraFromScene(YbrWorld* world, const YbrScene* scene,
							   const char* cameraNodeName, float aspect)
{
	if (!world) return 0;
	Camera3D cam;
	float nearPlane = 0.0f, farPlane = 0.0f;
	if (!YbrCameraToRaylib(scene, cameraNodeName, aspect, &cam, &nearPlane,
						   &farPlane))
		return 0;
	YbrWorldSetCamera(world, cam, aspect, nearPlane, farPlane);
	return 1;
}

int YbrWorldGetCamera(const YbrWorld* world, Camera3D* out)
{
	if (!world || !world->hasCamera) return 0;
	if (out) *out = world->camera;
	return 1;
}

const YbrFrustum* YbrWorldGetFrustum(const YbrWorld* world)
{
	if (!world || !world->culling || !world->hasCamera) return NULL;
	return &world->frustum;
}

void YbrWorldSetFrustum(YbrWorld* world, const YbrFrustum* frustum)
{
	if (!world || !frustum) return;
	world->frustum = *frustum;
	world->hasCamera = 1;  // 視錐台だけ差し替えても判定は効くようにする
}

void YbrWorldSetCulling(YbrWorld* world, int enable)
{
	if (world) world->culling = enable ? 1 : 0;
}

// 描画キュー : 世界全体で並べ替えてから描く
void YbrWorldBeginFrame(YbrWorld* world)
{
	if (world) world->queueCount = 0;
}

int YbrWorldGetQueueCount(const YbrWorld* world)
{
	return world ? world->queueCount : 0;
}

static int cmp_world_item(const void* a, const void* b)
{
	const YbrWorldItem* x = (const YbrWorldItem*)a;
	const YbrWorldItem* y = (const YbrWorldItem*)b;
	// 不透明が先。半透明どうしは遠い順
	if (x->transparent != y->transparent)
		return x->transparent - y->transparent;
	if (!x->transparent) return 0;
	if (x->depth < y->depth) return 1;
	if (y->depth < x->depth) return -1;
	return 0;
}

int YbrWorldSubmit(YbrWorld* world, const YbrModelInstance* instIn, Color tint)
{
	if (!world || !instIn || !instIn->model) return 0;
	YbrModelInstance* inst = (YbrModelInstance*)instIn;
	const YbrModel* model = inst->model;

	// まず体ごと。外れていれば 1 パートも積まない
	const YbrFrustum* frustum = YbrWorldGetFrustum(world);
	if (!YbrModelInstanceIsVisible(inst, frustum)) return 0;

	Vector3 eye =
		world->hasCamera ? world->camera.position : (Vector3){0, 0, 0};
	float t[4] = {tint.r / 255.0f, tint.g / 255.0f, tint.b / 255.0f,
				  tint.a / 255.0f};
	int added = 0;

	for (int i = 0; i < model->nodeCount; i++) {
		if (inst->nodeVisible && !inst->nodeVisible[i]) continue;
		const YbrModelNode* node = &model->nodes[i];
		Matrix transform = MatrixMultiply(node->transform, inst->transform);

		for (int k = 0; k < model->partCount; k++) {
			const YbrModelPart* p = &model->parts[k];
			if (p->meshIndex != node->meshIndex) continue;
			if (inst->partVisible && !inst->partVisible[k]) continue;

			// 動かないパートはパート単位でももう一度カリングする
			if (frustum && !p->skinned) {
				Vector3 lo, hi;
				YbrModelPartWorldBox(p, transform, &lo, &hi);
				if (!YbrFrustumContainsBox(frustum, lo, hi)) continue;
			}

			const YbrModelMaterial* mat = YbrModelResolveMaterial(inst, i, p);

			void* nb =
				YbrGrowBuffer(world->queue, &world->queueCap,
							  world->queueCount + 1, sizeof(YbrWorldItem));
			if (!nb) return added;
			world->queue = (YbrWorldItem*)nb;

			YbrWorldItem* it = &world->queue[world->queueCount];
			it->inst = inst;
			it->part = p;
			it->material = mat;
			it->partIndex = k;
			it->transform = transform;
			it->transparent =
				(model->hasTransparent && mat && mat->transparent) ? 1 : 0;
			// パートの中心をワールドへ持っていって、カメラからの距離を測る
			Vector3 c = Vector3Transform(
				Vector3Scale(Vector3Add(p->localMin, p->localMax), 0.5f),
				transform);
			it->depth = Vector3DistanceSqr(c, eye);
			for (int k2 = 0; k2 < 4; k2++) it->tint[k2] = t[k2];

			world->queueCount++;
			added++;
		}
	}
	return added;
}

void YbrWorldDrawQueue(YbrWorld* world)
{
	if (!world || world->queueCount <= 0) return;

	qsort(world->queue, (size_t)world->queueCount, sizeof(YbrWorldItem),
		  cmp_world_item);

	int i = 0;
	// 不透明
	for (; i < world->queueCount && !world->queue[i].transparent; i++) {
		YbrWorldItem* it = &world->queue[i];
		YbrModelDrawPart((YbrModelInstance*)it->inst, it->material, it->part,
						 it->partIndex, it->transform, world, it->tint);
	}

	// 半透明（奥から / 深度書き込みは止める）
	if (i < world->queueCount) {
		rlDrawRenderBatchActive();
		rlDisableDepthMask();
		for (; i < world->queueCount; i++) {
			YbrWorldItem* it = &world->queue[i];
			YbrModelDrawPart((YbrModelInstance*)it->inst, it->material,
							 it->part, it->partIndex, it->transform, world,
							 it->tint);
		}
		rlDrawRenderBatchActive();
		rlEnableDepthMask();
	}
}

// 影
// 影を落とせるライトか。点光源は全方位なのでキューブマップが要る。
int YbrWorldLightCastsShadow(const YbrWorld* world, int index)
{
	const YbrWorldLight* l = YbrWorldGetLight(world, index);
	if (!l || index < 0 || world->lightCount <= index) return 0;
	return (l->kind == YBR_LIGHTKIND_DIRECTIONAL ||
			l->kind == YBR_LIGHTKIND_SPOT);
}

// 影を落とすライトを決める。指定が無ければ先頭から順に選ぶ。
int YbrWorldResolveShadowLights(YbrWorld* world, int* out, int max)
{
	if (!world || !out || max <= 0) return 0;

	int n = 0;
	// まず明示的に指定されたものを尊重する
	for (int s = 0; s < YBR_WORLD_MAX_SHADOWS && n < max; s++) {
		int li = world->shadows[s].lightIndex;
		if (0 <= li && YbrWorldLightCastsShadow(world, li)) out[n++] = li;
	}
	if (0 < n) return n;

	// 指定が無ければ、影を落とせるライトを先頭から
	for (int i = 0; i < world->lightCount && n < max; i++)
		if (YbrWorldLightCastsShadow(world, i)) out[n++] = i;
	return n;
}

// ライト視点の view * projection を作る。
Matrix YbrWorldLightMatrix(const YbrWorld* world, int lightIndex)
{
	const YbrWorldLight* l = YbrWorldGetLight(world, lightIndex);
	if (!l) return MatrixIdentity();

	Vector3 up = {0.0f, 1.0f, 0.0f};
	Vector3 dir = Vector3Normalize(l->direction);
	if (0.99f < fabsf(dir.y)) up = (Vector3){1.0f, 0.0f, 0.0f};

	if (l->kind == YBR_LIGHTKIND_SPOT) {
		float far_ = (0.0f < l->range) ? l->range : 100.0f;
		float fovy = 2.0f * l->spotOuter;
		if (!(0.01f < fovy)) fovy = 1.0f;
		if (3.0f < fovy) fovy = 3.0f;  // 180 度に近いと投影が壊れる
		Matrix view =
			MatrixLookAt(l->position, Vector3Add(l->position, dir), up);
		Matrix proj = MatrixPerspective(fovy, 1.0f, 0.05f, far_);
		return MatrixMultiply(view, proj);
	}

	// 平行光
	float half = world->options.shadowDistance * 0.5f;
	if (!(0.0f < half)) half = 10.0f;

	// カメラの注視点を中心にする (見ているところに影を寄せる)
	Vector3 center =
		world->hasCamera ? world->camera.target : (Vector3){0, 0, 0};
	float depth = world->options.shadowDistance * 2.0f;
	Vector3 eye = Vector3Subtract(center, Vector3Scale(dir, depth * 0.5f));

	Matrix view = MatrixLookAt(eye, center, up);
	Matrix proj = MatrixOrtho(-half, half, -half, half, 0.01f, depth);
	return MatrixMultiply(view, proj);
}

// 焼いた深度テクスチャ
Texture2D YbrWorldGetShadowMap(const YbrWorld* world, int slot)
{
	Texture2D none = {0};
	if (!world || slot < 0 || YBR_WORLD_MAX_SHADOWS <= slot) return none;
	if (!world->shadows[slot].ready) return none;
	return world->shadows[slot].depth;
}

// ----------------------------------------------------------------------------
// 影

static void shadow_map_unload(YbrShadowMap* sm)
{
	if (!sm) return;
	if (sm->depth.id) rlUnloadTexture(sm->depth.id);
	if (sm->fbo) rlUnloadFramebuffer(sm->fbo);
	sm->depth.id = 0;
	sm->fbo = 0;
	sm->ready = 0;
}

static int shadow_map_ensure(YbrShadowMap* sm, int resolution)
{
	if (sm->ready && sm->resolution == resolution) return 1;
	shadow_map_unload(sm);

	sm->fbo = rlLoadFramebuffer();
	if (!sm->fbo) return 0;

	rlEnableFramebuffer(sm->fbo);
	// 深度「テクスチャ」で作る (レンダーバッファだとシェーダーから読めない)
	sm->depth.id = rlLoadTextureDepth(resolution, resolution, false);
	sm->depth.width = resolution;
	sm->depth.height = resolution;
	sm->depth.mipmaps = 1;
	// 深度専用の pixel format は無いので、1 チャンネル 32bit として扱う
	sm->depth.format = PIXELFORMAT_UNCOMPRESSED_R32;
	rlFramebufferAttach(sm->fbo, sm->depth.id, RL_ATTACHMENT_DEPTH,
						RL_ATTACHMENT_TEXTURE2D, 0);

	int ok = rlFramebufferComplete(sm->fbo);
	rlDisableFramebuffer();
	if (!ok) {
		TraceLog(
			LOG_WARNING,
			"YBR: could not create a %dx%d shadow map (framebuffer incomplete)",
			resolution, resolution);
		shadow_map_unload(sm);
		return 0;
	}

	// 影の外側を「日向」に倒すため、端は伸ばして拾う
	SetTextureWrap(sm->depth, TEXTURE_WRAP_CLAMP);
	SetTextureFilter(sm->depth, TEXTURE_FILTER_POINT);

	sm->resolution = resolution;
	sm->ready = 1;
	return 1;
}

int YbrWorldRenderShadows(YbrWorld* world)
{
	if (!world) return 0;
	// 焼き直すので、前のフレームの結果はここで無効にする。
	// (影を切ったフレームで古い影が残らないように)
	world->shadowCount = 0;
	if (!world->options.shadows) return 0;

	int lights[YBR_WORLD_MAX_SHADOWS];
	int want =
		YbrWorldResolveShadowLights(world, lights, world->options.shadowLights);
	if (want <= 0) return 0;

	// 今の状態を控えて、あとで戻す
	Matrix keepProj = rlGetMatrixProjection();
	Matrix keepView = rlGetMatrixModelview();

	for (int s = 0; s < want; s++) {
		YbrShadowMap* sm = &world->shadows[s];
		if (!shadow_map_ensure(sm, world->options.shadowResolution)) continue;

		sm->lightIndex = lights[s];
		sm->lightVP = YbrWorldLightMatrix(world, lights[s]);

		rlDrawRenderBatchActive();
		rlEnableFramebuffer(sm->fbo);
		rlViewport(0, 0, sm->resolution, sm->resolution);
		rlClearScreenBuffers();	 // 深度を 1.0 で埋める
		rlEnableDepthTest();

		// ライト視点の行列を入れて、キューの不透明ぶんをそのまま描く。
		// 深度だけ要るので色は書き捨てになる。
		rlSetMatrixProjection(sm->lightVP);
		rlSetMatrixModelview(MatrixIdentity());

		for (int i = 0; i < world->queueCount; i++) {
			YbrWorldItem* it = &world->queue[i];
			if (it->transparent) continue;	// 半透明は影を落とさない
			YbrModelDrawPart((YbrModelInstance*)it->inst, it->material,
							 it->part, it->partIndex, it->transform, NULL,
							 it->tint);
		}

		rlDrawRenderBatchActive();
		rlDisableFramebuffer();
		world->shadowCount++;
	}

	// 画面へ戻す
	rlViewport(0, 0, GetScreenWidth(), GetScreenHeight());
	rlSetMatrixProjection(keepProj);
	rlSetMatrixModelview(keepView);
	return world->shadowCount;
}
