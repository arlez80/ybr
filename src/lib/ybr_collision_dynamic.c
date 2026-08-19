/*
	Yui Blender to Raylib - スキニングあり当たり判定
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_collision_dynamic.h"

#include "ybr_collision_internal.h"
#include "ybr_collision_solid.h"

// ----------------------------------------------------------------------------
// ユーティリティ

static void axis_scales(Matrix m, float* outMin, float* outMax)
{
	float sx = sqrtf(m.m0 * m.m0 + m.m1 * m.m1 + m.m2 * m.m2);
	float sy = sqrtf(m.m4 * m.m4 + m.m5 * m.m5 + m.m6 * m.m6);
	float sz = sqrtf(m.m8 * m.m8 + m.m9 * m.m9 + m.m10 * m.m10);
	float lo = sx < sy ? sx : sy;
	if (sz < lo) lo = sz;
	float hi = sy < sx ? sx : sy;
	if (hi < sz) hi = sz;
	if (!(1e-6f < lo)) lo = 1e-6f;
	if (!(1e-6f < hi)) hi = 1e-6f;
	*outMin = lo;
	*outMax = hi;
}

// ----------------------------------------------------------------------------
// ボーン1本ぶんの当たり判定
typedef struct Part {
	int bone;		 // -1 ならウェイトの無い静的パート
	YbrSolid* tree;	 // そのボーンのローカル空間の8分木

	Matrix transform;  // ローカル -> ワールド
	Matrix inverse;
	Matrix normalMatrix;
	float scaleMin, scaleMax;

	Vector3 localMin, localMax;
	Vector3 worldMin, worldMax;
	int hasBounds;
} Part;

struct YbrDynamic {
	YbrModelInstance* inst;
	const YbrModel* model;
	YbrPose* pose;	// model のポーズ（最初のアーマチュア）

	Part* parts;
	int partCount;

	Matrix transform;  // モデル全体に掛かる行列
	Vector3 worldMin, worldMax;
	int hasBounds;
	int triangleCount;

	void* userData;
	int enabled;
	int lastBone;  // 直前の判定で当たったボーン
};

YbrDynamicBuildOptions YbrDynamicBuildDefaults(void)
{
	YbrDynamicBuildOptions o;
	// ボーン 1 本あたりのポリゴンは少ないので浅い木で十分
	o.maxDepth = 5;
	o.maxTrianglesPerNode = 8;
	o.looseness = 1.2f;
	o.filter = NULL;
	o.filterUserData = NULL;
	return o;
}

// ----------------------------------------------------------------------------
// 三角形の振り分け

// 三角形をどのボーンに預けるか決める
static int pick_bone(const YbrModelPart* p, int i0, int i1, int i2,
					 int boneCount)
{
	if (!p->boneIds || !p->boneWeights) return -1;

	// 3頂点 x 4影響 = 12個ぶんを 1回なめて、ボーンごとに足し合わせる
	int bones[12];
	float sums[12];
	int used = 0;
	const int vi[3] = {i0, i1, i2};

	for (int a = 0; a < 3; a++) {
		for (int k = 0; k < 4; k++) {
			int b = (int)p->boneIds[vi[a] * 4 + k];
			float w = p->boneWeights[vi[a] * 4 + k];
			if (w <= 0.0f || b < 0 || boneCount <= b) continue;

			int slot = -1;
			for (int s = 0; s < used; s++)
				if (bones[s] == b) {
					slot = s;
					break;
				}
			if (slot < 0) {
				slot = used++;
				bones[slot] = b;
				sums[slot] = 0.0f;
			}
			sums[slot] += w;
		}
	}

	int best = -1;
	float bestW = 0.0f;
	for (int s = 0; s < used; s++)
		if (bestW < sums[s]) {
			bestW = sums[s];
			best = bones[s];
		}
	return (0.0f < bestW) ? best : -1;
}

typedef struct TriList {
	YbrTriangle* tris;
	int count, cap;
} TriList;

static int trilist_add(TriList* l, const YbrTriangle* t)
{
	void* nb =
		YbrGrowBuffer(l->tris, &l->cap, l->count + 1, sizeof(YbrTriangle));
	if (!nb) return 0;
	l->tris = (YbrTriangle*)nb;
	l->tris[l->count++] = *t;
	return 1;
}

// ----------------------------------------------------------------------------
// 構築

YbrDynamic* YbrDynamicFromInstance(YbrModelInstance* inst,
								   const YbrDynamicBuildOptions* opts)
{
	if (!inst || !inst->model) return NULL;
	const YbrModel* model = inst->model;

	YbrDynamicBuildOptions o = YbrDynamicBuildDefaults();
	if (opts) o = *opts;

	YbrDynamic* dyn = (YbrDynamic*)YBR_CALLOC(1, sizeof(YbrDynamic));
	if (!dyn) return NULL;
	dyn->inst = inst;
	dyn->model = model;
	dyn->transform = MatrixIdentity();
	dyn->enabled = 1;
	dyn->lastBone = -1;
	dyn->pose = YbrModelInstanceGetPose(inst, NULL);

	int boneCount = dyn->pose ? dyn->pose->boneCount : 0;

	// パートは「ボーン数 + 静的 1」ぶん用意し、空のものは後で捨てる
	int slots = boneCount + 1;
	TriList* lists = (TriList*)YBR_CALLOC((size_t)slots, sizeof(TriList));
	if (!lists) {
		YBR_FREE(dyn);
		return NULL;
	}

	// 三角形をボーンごとに振り分ける
	for (int i = 0; i < model->partCount; i++) {
		const YbrModelPart* mp = &model->parts[i];
		const YbrMesh* mesh =
			(0 <= mp->meshIndex && mp->meshIndex < model->meshCount)
				? model->meshes[mp->meshIndex]
				: NULL;

		unsigned int tag = 0;
		if (o.filter && !o.filter(mesh, &tag, o.filterUserData)) continue;

		// このパートを描いているノードのワールド行列。
		// スキンがある場合はスキン行列がすべてを担うので単位行列扱いにする。
		Matrix nodeWorld = MatrixIdentity();
		for (int n = 0; n < model->nodeCount; n++) {
			if (model->nodes[n].meshIndex != mp->meshIndex) continue;
			if (!mp->boneIds) nodeWorld = model->nodes[n].transform;
			break;
		}

		for (int t = 0; t < mp->triangleCount; t++) {
			int i0 = (int)mp->indices[t * 3 + 0];
			int i1 = (int)mp->indices[t * 3 + 1];
			int i2 = (int)mp->indices[t * 3 + 2];

			int bone = (mp->skinned && dyn->pose)
						   ? pick_bone(mp, i0, i1, i2, boneCount)
						   : -1;

			YbrTriangle tri;
			memset(&tri, 0, sizeof(tri));
			const int vi[3] = {i0, i1, i2};
			for (int k = 0; k < 3; k++) {
				Vector3 v = v3(mp->positions[vi[k] * 3 + 0],
							   mp->positions[vi[k] * 3 + 1],
							   mp->positions[vi[k] * 3 + 2]);
				if (0 <= bone) {
					// ボーンのローカル空間へ = skin 行列の逆 ... ではなく、
					v = Vector3Transform(v, dyn->pose->invRest[bone]);
				}
				else {
					v = Vector3Transform(v, nodeWorld);
				}
				tri.v[k] = v;
			}

			Vector3 n =
				v3cross(v3sub(tri.v[1], tri.v[0]), v3sub(tri.v[2], tri.v[0]));
			float len = Vector3Length(n);
			// 面積 0 は捨てる
			if (len <= 1e-12f) continue;
			tri.normal = v3mul(n, 1.0f / len);

			// YbrDynamic は YbrModel から作るのでシーンノードは無い
			tri.sceneNode = NULL;
			tri.node = mp;
			tri.mesh = mesh;
			tri.meshTriangle = t;
			tri.materialIndex = mp->materialIndex;
			tri.tag = tag;

			int slot = (0 <= bone) ? bone : boneCount;
			if (!trilist_add(&lists[slot], &tri)) {
				for (int s = 0; s < slots; s++) YBR_FREE(lists[s].tris);
				YBR_FREE(lists);
				YBR_FREE(dyn);
				return NULL;
			}
			dyn->triangleCount++;
		}
	}

	// 中身のあるスロットだけパートにする
	YbrSolidBuildOptions so = YbrSolidBuildDefaults();
	so.maxDepth = o.maxDepth;
	so.maxTrianglesPerNode = o.maxTrianglesPerNode;
	so.looseness = o.looseness;

	int used = 0;
	for (int s = 0; s < slots; s++)
		if (0 < lists[s].count) used++;

	if (0 < used) {
		dyn->parts = (Part*)YBR_CALLOC((size_t)used, sizeof(Part));
		if (!dyn->parts) {
			for (int s = 0; s < slots; s++) YBR_FREE(lists[s].tris);
			YBR_FREE(lists);
			YBR_FREE(dyn);
			return NULL;
		}
		for (int s = 0; s < slots; s++) {
			if (lists[s].count <= 0) continue;
			Part* pt = &dyn->parts[dyn->partCount];
			pt->bone = (s == boneCount) ? -1 : s;
			pt->tree =
				YbrSolidBuildFromTriangles(lists[s].tris, lists[s].count, &so);
			if (!pt->tree) {
				for (int k = 0; k < slots; k++) YBR_FREE(lists[k].tris);
				YBR_FREE(lists);
				YbrDynamicUnload(dyn);
				return NULL;
			}
			pt->hasBounds =
				YbrSolidGetBounds(pt->tree, &pt->localMin, &pt->localMax);
			pt->transform = MatrixIdentity();
			dyn->partCount++;
		}
	}

	for (int s = 0; s < slots; s++) YBR_FREE(lists[s].tris);
	YBR_FREE(lists);

	YbrDynamicUpdate(dyn);
	return dyn;
}

void YbrDynamicUnload(YbrDynamic* dyn)
{
	if (!dyn) return;
	for (int i = 0; i < dyn->partCount; i++) YbrSolidUnload(dyn->parts[i].tree);
	YBR_FREE(dyn->parts);
	YBR_FREE(dyn);
}

// ----------------------------------------------------------------------------
// 更新

static void part_set_transform(Part* pt, Matrix m)
{
	pt->transform = m;
	pt->inverse = MatrixInvert(m);
	pt->normalMatrix = MatrixTranspose(pt->inverse);
	axis_scales(m, &pt->scaleMin, &pt->scaleMax);

	if (!pt->hasBounds) return;
	Vector3 lo = v3(1e30f, 1e30f, 1e30f);
	Vector3 hi = v3(-1e30f, -1e30f, -1e30f);
	for (int i = 0; i < 8; i++) {
		Vector3 c;
		c.x = (i & 1) ? pt->localMax.x : pt->localMin.x;
		c.y = (i & 2) ? pt->localMax.y : pt->localMin.y;
		c.z = (i & 4) ? pt->localMax.z : pt->localMin.z;
		c = Vector3Transform(c, m);
		lo = Vector3Min(lo, c);
		hi = Vector3Max(hi, c);
	}
	pt->worldMin = lo;
	pt->worldMax = hi;
}

void YbrDynamicUpdate(YbrDynamic* dyn)
{
	if (!dyn) return;

	Vector3 lo = v3(1e30f, 1e30f, 1e30f);
	Vector3 hi = v3(-1e30f, -1e30f, -1e30f);
	int any = 0;

	for (int i = 0; i < dyn->partCount; i++) {
		Part* pt = &dyn->parts[i];
		Matrix m = dyn->transform;
		if (0 <= pt->bone && dyn->pose && pt->bone < dyn->pose->boneCount) {
			// 三角形は invRest を掛けた形で持っているので、
			// ワールドは pose 行列を掛けるだけで出る
			m = MatrixMultiply(dyn->pose->bones[pt->bone].pose, dyn->transform);
		}
		part_set_transform(pt, m);
		if (pt->hasBounds) {
			lo = Vector3Min(lo, pt->worldMin);
			hi = Vector3Max(hi, pt->worldMax);
			any = 1;
		}
	}

	dyn->hasBounds = any;
	if (any) {
		dyn->worldMin = lo;
		dyn->worldMax = hi;
	}
}

void YbrDynamicSetTransform(YbrDynamic* dyn, Matrix transform)
{
	if (!dyn) return;
	dyn->transform = transform;
	YbrDynamicUpdate(dyn);
}

Matrix YbrDynamicGetTransform(const YbrDynamic* dyn)
{
	return dyn ? dyn->transform : MatrixIdentity();
}

void YbrDynamicSetUserData(YbrDynamic* dyn, void* userData)
{
	if (dyn) dyn->userData = userData;
}
void* YbrDynamicGetUserData(const YbrDynamic* dyn)
{
	return dyn ? dyn->userData : NULL;
}
void YbrDynamicSetEnabled(YbrDynamic* dyn, int enabled)
{
	if (dyn) dyn->enabled = enabled ? 1 : 0;
}
int YbrDynamicIsEnabled(const YbrDynamic* dyn)
{
	return dyn ? dyn->enabled : 0;
}

const YbrModelInstance* YbrDynamicGetInstance(const YbrDynamic* dyn)
{
	return dyn ? dyn->inst : NULL;
}
int YbrDynamicGetTriangleCount(const YbrDynamic* dyn)
{
	return dyn ? dyn->triangleCount : 0;
}
int YbrDynamicGetPartCount(const YbrDynamic* dyn)
{
	return dyn ? dyn->partCount : 0;
}

int YbrDynamicGetBounds(const YbrDynamic* dyn, Vector3* outMin, Vector3* outMax)
{
	if (!dyn || !dyn->hasBounds) return 0;
	if (outMin) *outMin = dyn->worldMin;
	if (outMax) *outMax = dyn->worldMax;
	return 1;
}

Matrix YbrDynamicGetPartTransform(const YbrDynamic* dyn, int part)
{
	if (!dyn || part < 0 || dyn->partCount <= part) return MatrixIdentity();
	return dyn->parts[part].transform;
}

int YbrDynamicGetPartBone(const YbrDynamic* dyn, int part)
{
	if (!dyn || part < 0 || dyn->partCount <= part) return -1;
	return dyn->parts[part].bone;
}

int YbrDynamicGetPartTriangleCount(const YbrDynamic* dyn, int part)
{
	if (!dyn || part < 0 || dyn->partCount <= part) return 0;
	return YbrSolidGetTriangleCount(dyn->parts[part].tree);
}

const YbrTriangle* YbrDynamicGetPartTriangle(const YbrDynamic* dyn, int part,
											 int index)
{
	if (!dyn || part < 0 || dyn->partCount <= part) return NULL;
	return YbrSolidGetTriangle(dyn->parts[part].tree, index);
}

int YbrDynamicGetLastBone(const YbrDynamic* dyn)
{
	return dyn ? dyn->lastBone : -1;
}

const char* YbrDynamicGetLastBoneName(const YbrDynamic* dyn)
{
	if (!dyn || dyn->lastBone < 0 || !dyn->pose || !dyn->pose->armature)
		return NULL;
	if (dyn->pose->boneCount <= dyn->lastBone) return NULL;
	return dyn->pose->armature->bones[dyn->lastBone].name;
}

// ----------------------------------------------------------------------------
// ワールドへ変換する

static Vector3 part_dir_to_world(const Part* pt, Vector3 n)
{
	Vector3 r;
	r.x = pt->normalMatrix.m0 * n.x + pt->normalMatrix.m4 * n.y +
		  pt->normalMatrix.m8 * n.z;
	r.y = pt->normalMatrix.m1 * n.x + pt->normalMatrix.m5 * n.y +
		  pt->normalMatrix.m9 * n.z;
	r.z = pt->normalMatrix.m2 * n.x + pt->normalMatrix.m6 * n.y +
		  pt->normalMatrix.m10 * n.z;
	return Vector3Normalize(r);
}

static YbrTriangle part_tri_to_world(const Part* pt, YbrTriangle t)
{
	for (int i = 0; i < 3; i++)
		t.v[i] = Vector3Transform(t.v[i], pt->transform);
	t.normal = part_dir_to_world(pt, t.normal);
	return t;
}

static void ray_hit_to_world(const Part* pt, YbrRayHit* h, Vector3 a, Vector3 b)
{
	h->point = Vector3Transform(h->point, pt->transform);
	h->normal = part_dir_to_world(pt, h->normal);
	h->triangle = part_tri_to_world(pt, h->triangle);
	h->distance = Vector3Distance(a, b) * h->t;
}

static void shape_hit_to_world(const Part* pt, YbrShapeHit* h)
{
	h->point = Vector3Transform(h->point, pt->transform);
	h->normal = part_dir_to_world(pt, h->normal);
	h->triangle = part_tri_to_world(pt, h->triangle);
	h->depth *= pt->scaleMax;
	float len = Vector3Length(h->resolve);
	if (1e-9f < len)
		h->resolve = Vector3Scale(
			part_dir_to_world(pt, Vector3Scale(h->resolve, 1.0f / len)),
			len * pt->scaleMax);
}

// 問い合わせの AABB とパートの AABB が重なるか
static int part_overlaps(const Part* pt, Vector3 qmin, Vector3 qmax)
{
	if (!pt->hasBounds) return 0;
	return !(pt->worldMax.x < qmin.x || qmax.x < pt->worldMin.x ||
			 pt->worldMax.y < qmin.y || qmax.y < pt->worldMin.y ||
			 pt->worldMax.z < qmin.z || qmax.z < pt->worldMin.z);
}

// ----------------------------------------------------------------------------
// 判定

int YbrDynamicSegment(const YbrDynamic* dyn, Vector3 a, Vector3 b,
					  const YbrQueryOptions* opts, YbrRayHit* out)
{
	YbrRayHit best;
	memset(&best, 0, sizeof(best));
	best.t = 1.0f;
	if (out) *out = best;
	if (!dyn || !dyn->enabled) return 0;

	Vector3 qmin = Vector3Min(a, b), qmax = Vector3Max(a, b);
	int found = 0;
	int bone = -1;

	for (int i = 0; i < dyn->partCount; i++) {
		const Part* pt = &dyn->parts[i];
		if (!part_overlaps(pt, qmin, qmax)) continue;

		YbrRayHit h;
		Vector3 la = Vector3Transform(a, pt->inverse);
		Vector3 lb = Vector3Transform(b, pt->inverse);
		if (!YbrSolidSegment(pt->tree, la, lb, opts, &h)) continue;
		if (found && best.t <= h.t) continue;

		ray_hit_to_world(pt, &h, a, b);
		best = h;
		bone = pt->bone;
		found = 1;
	}

	((YbrDynamic*)dyn)->lastBone = bone;
	if (out) *out = best;
	return found;
}

int YbrDynamicRay(const YbrDynamic* dyn, Vector3 origin, Vector3 direction,
				  float maxDistance, const YbrQueryOptions* opts,
				  YbrRayHit* out)
{
	Vector3 d = Vector3Normalize(direction);
	if (!dyn || Vector3LengthSqr(d) <= 0.0f) {
		if (out) memset(out, 0, sizeof(*out));
		return 0;
	}
	if (!(0.0f < maxDistance)) {
		float len = 1.0f;
		if (dyn->hasBounds) {
			Vector3 c =
				Vector3Scale(Vector3Add(dyn->worldMin, dyn->worldMax), 0.5f);
			len = Vector3Distance(dyn->worldMin, dyn->worldMax) +
				  Vector3Distance(origin, c) + 1.0f;
		}
		maxDistance = len;
	}
	return YbrDynamicSegment(dyn, origin,
							 Vector3Add(origin, Vector3Scale(d, maxDistance)),
							 opts, out);
}

int YbrDynamicSweepSphere(const YbrDynamic* dyn, Vector3 from, Vector3 to,
						  float radius, const YbrQueryOptions* opts,
						  YbrRayHit* out)
{
	YbrRayHit best;
	memset(&best, 0, sizeof(best));
	best.t = 1.0f;
	if (out) *out = best;
	if (!dyn || !dyn->enabled) return 0;

	Vector3 r = v3(radius, radius, radius);
	Vector3 qmin = v3sub(Vector3Min(from, to), r);
	Vector3 qmax = v3add(Vector3Max(from, to), r);
	int found = 0, bone = -1;

	for (int i = 0; i < dyn->partCount; i++) {
		const Part* pt = &dyn->parts[i];
		if (!part_overlaps(pt, qmin, qmax)) continue;

		YbrRayHit h;
		Vector3 lf = Vector3Transform(from, pt->inverse);
		Vector3 lt = Vector3Transform(to, pt->inverse);
		if (!YbrSolidSweepSphere(pt->tree, lf, lt, radius / pt->scaleMin, opts,
								 &h))
			continue;
		if (found && best.t <= h.t) continue;

		ray_hit_to_world(pt, &h, from, to);
		best = h;
		bone = pt->bone;
		found = 1;
	}

	((YbrDynamic*)dyn)->lastBone = bone;
	if (out) *out = best;
	return found;
}

// 球 / カプセル共通
static int shape_query(const YbrDynamic* dyn, Vector3 a, Vector3 b,
					   float radius, int isCapsule, const YbrQueryOptions* opts,
					   YbrShapeHit* out)
{
	YbrShapeHit best;
	memset(&best, 0, sizeof(best));
	if (out) *out = best;
	if (!dyn || !dyn->enabled) return 0;

	Vector3 r = v3(radius, radius, radius);
	Vector3 qmin = v3sub(isCapsule ? Vector3Min(a, b) : a, r);
	Vector3 qmax = v3add(isCapsule ? Vector3Max(a, b) : a, r);

	Vector3 resolve = v3(0, 0, 0);
	int bone = -1;

	for (int i = 0; i < dyn->partCount; i++) {
		const Part* pt = &dyn->parts[i];
		if (!part_overlaps(pt, qmin, qmax)) continue;

		YbrShapeHit h;
		Vector3 la = Vector3Transform(a, pt->inverse);
		Vector3 lb = Vector3Transform(b, pt->inverse);
		float lr = radius / pt->scaleMin;
		int hit = isCapsule ? YbrSolidCapsule(pt->tree, la, lb, lr, opts, &h)
							: YbrSolidSphere(pt->tree, la, lr, opts, &h);
		if (!hit) continue;

		shape_hit_to_world(pt, &h);
		best.count += h.count;
		if (!best.hit || best.depth < h.depth) {
			best.hit = 1;
			best.depth = h.depth;
			best.point = h.point;
			best.normal = h.normal;
			best.triangle = h.triangle;
			bone = pt->bone;
		}
		best.hit = 1;

		// すでに解消済みの向きは足さない
		float len = Vector3Length(h.resolve);
		if (1e-6f < len) {
			Vector3 n = Vector3Scale(h.resolve, 1.0f / len);
			float already = Vector3DotProduct(n, resolve);
			if (already < len)
				resolve = Vector3Add(resolve, Vector3Scale(n, len - already));
		}
	}

	best.resolve = resolve;
	((YbrDynamic*)dyn)->lastBone = bone;
	if (out) *out = best;
	return best.hit;
}

int YbrDynamicSphere(const YbrDynamic* dyn, Vector3 center, float radius,
					 const YbrQueryOptions* opts, YbrShapeHit* out)
{
	return shape_query(dyn, center, center, radius, 0, opts, out);
}

int YbrDynamicCapsule(const YbrDynamic* dyn, Vector3 a, Vector3 b, float radius,
					  const YbrQueryOptions* opts, YbrShapeHit* out)
{
	return shape_query(dyn, a, b, radius, 1, opts, out);
}

int YbrDynamicTriangle(const YbrDynamic* dyn, Vector3 v0, Vector3 v1,
					   Vector3 v2, const YbrQueryOptions* opts, YbrTriHit* out)
{
	YbrTriHit best;
	memset(&best, 0, sizeof(best));
	if (out) *out = best;
	if (!dyn || !dyn->enabled) return 0;

	Vector3 qmin = Vector3Min(v0, Vector3Min(v1, v2));
	Vector3 qmax = Vector3Max(v0, Vector3Max(v1, v2));
	int bone = -1;

	for (int i = 0; i < dyn->partCount; i++) {
		const Part* pt = &dyn->parts[i];
		if (!part_overlaps(pt, qmin, qmax)) continue;

		YbrTriHit h;
		Vector3 l0 = Vector3Transform(v0, pt->inverse);
		Vector3 l1 = Vector3Transform(v1, pt->inverse);
		Vector3 l2 = Vector3Transform(v2, pt->inverse);
		if (!YbrSolidTriangle(pt->tree, l0, l1, l2, opts, &h)) continue;

		best.count += h.count;
		if (!best.hit) {
			best.hit = 1;
			best.pointA = Vector3Transform(h.pointA, pt->transform);
			best.pointB = Vector3Transform(h.pointB, pt->transform);
			best.point =
				Vector3Scale(Vector3Add(best.pointA, best.pointB), 0.5f);
			best.triangle = part_tri_to_world(pt, h.triangle);
			bone = pt->bone;
		}
	}

	((YbrDynamic*)dyn)->lastBone = bone;
	if (out) *out = best;
	return best.hit;
}

// ----------------------------------------------------------------------------
// 複数取得

typedef struct VisitCtx {
	const Part* part;
	YbrTriangleVisitor visitor;
	void* ud;
} VisitCtx;

static int visit_to_world(const YbrTriangle* tri, void* ud)
{
	VisitCtx* c = (VisitCtx*)ud;
	YbrTriangle w = part_tri_to_world(c->part, *tri);
	return c->visitor(&w, c->ud);
}

static int overlap_query(const YbrDynamic* dyn, Vector3 a, Vector3 b,
						 float radius, int isCapsule,
						 const YbrQueryOptions* opts,
						 YbrTriangleVisitor visitor, void* userData)
{
	if (!dyn || !dyn->enabled || !visitor) return 0;

	Vector3 r = v3(radius, radius, radius);
	Vector3 qmin = v3sub(isCapsule ? Vector3Min(a, b) : a, r);
	Vector3 qmax = v3add(isCapsule ? Vector3Max(a, b) : a, r);
	int total = 0;

	for (int i = 0; i < dyn->partCount; i++) {
		const Part* pt = &dyn->parts[i];
		if (!part_overlaps(pt, qmin, qmax)) continue;

		VisitCtx c;
		c.part = pt;
		c.visitor = visitor;
		c.ud = userData;
		Vector3 la = Vector3Transform(a, pt->inverse);
		Vector3 lb = Vector3Transform(b, pt->inverse);
		float lr = radius / pt->scaleMin;
		total += isCapsule ? YbrSolidOverlapCapsule(pt->tree, la, lb, lr, opts,
													visit_to_world, &c)
						   : YbrSolidOverlapSphere(pt->tree, la, lr, opts,
												   visit_to_world, &c);
	}
	return total;
}

int YbrDynamicOverlapSphere(const YbrDynamic* dyn, Vector3 center, float radius,
							const YbrQueryOptions* opts,
							YbrTriangleVisitor visitor, void* userData)
{
	return overlap_query(dyn, center, center, radius, 0, opts, visitor,
						 userData);
}

int YbrDynamicOverlapCapsule(const YbrDynamic* dyn, Vector3 a, Vector3 b,
							 float radius, const YbrQueryOptions* opts,
							 YbrTriangleVisitor visitor, void* userData)
{
	return overlap_query(dyn, a, b, radius, 1, opts, visitor, userData);
}

// ----------------------------------------------------------------------------
// 生成 / 破棄

typedef struct SapEntry {
	float minX;
	int index;
} SapEntry;

struct YbrDynamicWorld {
	YbrDynamic** items;
	int count, cap;
	SapEntry* order;
	int orderCap;
};

YbrDynamicWorld* YbrDynamicWorldCreate(void)
{
	return (YbrDynamicWorld*)YBR_CALLOC(1, sizeof(YbrDynamicWorld));
}

void YbrDynamicWorldUnload(YbrDynamicWorld* world)
{
	if (!world) return;
	YBR_FREE(world->items);
	YBR_FREE(world->order);
	YBR_FREE(world);
}

int YbrDynamicWorldAdd(YbrDynamicWorld* world, YbrDynamic* dyn)
{
	if (!world || !dyn) return 0;
	for (int i = 0; i < world->count; i++)
		if (world->items[i] == dyn) return 1;

	void* nb = YbrGrowBuffer(world->items, &world->cap, world->count + 1,
							 sizeof(YbrDynamic*));
	if (!nb) return 0;
	world->items = (YbrDynamic**)nb;

	void* no = YbrGrowBuffer(world->order, &world->orderCap, world->count + 1,
							 sizeof(SapEntry));
	if (!no) return 0;
	world->order = (SapEntry*)no;

	world->items[world->count] = dyn;
	world->order[world->count].index = world->count;
	world->order[world->count].minX = 0.0f;
	world->count++;
	YbrDynamicWorldUpdate(world);
	return 1;
}

int YbrDynamicWorldRemove(YbrDynamicWorld* world, YbrDynamic* dyn)
{
	if (!world || !dyn) return 0;
	for (int i = 0; i < world->count; i++) {
		if (world->items[i] != dyn) continue;
		int tail = world->count - i - 1;
		if (0 < tail)
			memmove(world->items + i, world->items + i + 1,
					(size_t)tail * sizeof(YbrDynamic*));
		world->count--;
		for (int k = 0; k < world->count; k++) world->order[k].index = k;
		YbrDynamicWorldUpdate(world);
		return 1;
	}
	return 0;
}

int YbrDynamicWorldGetCount(const YbrDynamicWorld* world)
{
	return world ? world->count : 0;
}

YbrDynamic* YbrDynamicWorldGet(const YbrDynamicWorld* world, int index)
{
	if (!world || index < 0 || world->count <= index) return NULL;
	return world->items[index];
}

void YbrDynamicWorldUpdate(YbrDynamicWorld* world)
{
	if (!world || world->count <= 0) return;

	for (int i = 0; i < world->count; i++) {
		int idx = world->order[i].index;
		if (idx < 0 || world->count <= idx) {
			idx = i;
			world->order[i].index = i;
		}
		YbrDynamic* d = world->items[idx];
		YbrDynamicUpdate(d);
		Vector3 lo, hi;
		world->order[i].minX = YbrDynamicGetBounds(d, &lo, &hi) ? lo.x : 1e30f;
	}

	// 動きが連続していれば並びはほとんど変わらないので挿入ソートで足りる
	for (int i = 1; i < world->count; i++) {
		SapEntry key = world->order[i];
		int j = i - 1;
		while (0 <= j && key.minX < world->order[j].minX) {
			world->order[j + 1] = world->order[j];
			j--;
		}
		world->order[j + 1] = key;
	}
}

typedef int (*BroadFn)(YbrDynamic* dyn, void* ud);

static int broadphase(YbrDynamicWorld* world, Vector3 qmin, Vector3 qmax,
					  BroadFn fn, void* ud)
{
	if (!world || world->count <= 0) return 0;
	int visited = 0;

	for (int i = 0; i < world->count; i++) {
		const SapEntry* e = &world->order[i];
		if (qmax.x < e->minX) break;  // minX 昇順なので以降は範囲外

		YbrDynamic* d = world->items[e->index];
		if (!d || !d->enabled) continue;

		Vector3 lo, hi;
		if (!YbrDynamicGetBounds(d, &lo, &hi)) continue;
		if (hi.x < qmin.x || hi.y < qmin.y || qmax.y < lo.y || hi.z < qmin.z ||
			qmax.z < lo.z)
			continue;

		visited++;
		if (!fn(d, ud)) break;
	}
	return visited;
}

typedef struct SegCtx {
	Vector3 a, b;
	const YbrQueryOptions* opts;
	YbrDynamicRayHit best;
	int found;
	float radius;
} SegCtx;

static int seg_fn(YbrDynamic* dyn, void* ud)
{
	SegCtx* c = (SegCtx*)ud;
	YbrRayHit h;
	int hit =
		(0.0f < c->radius)
			? YbrDynamicSweepSphere(dyn, c->a, c->b, c->radius, c->opts, &h)
			: YbrDynamicSegment(dyn, c->a, c->b, c->opts, &h);
	if (hit && (!c->found || h.t < c->best.hit.t)) {
		c->best.hit = h;
		c->best.dynamic = dyn;
		c->best.bone = YbrDynamicGetLastBone(dyn);
		c->found = 1;
	}
	return 1;
}

static int world_segment_common(YbrDynamicWorld* world, Vector3 a, Vector3 b,
								float radius, const YbrQueryOptions* opts,
								YbrDynamicRayHit* out)
{
	SegCtx c;
	memset(&c, 0, sizeof(c));
	c.a = a;
	c.b = b;
	c.opts = opts;
	c.radius = radius;
	c.best.hit.t = 1.0f;
	c.best.bone = -1;
	if (out) *out = c.best;
	if (!world) return 0;

	Vector3 qmin = Vector3Min(a, b), qmax = Vector3Max(a, b);
	if (0.0f < radius) {
		Vector3 r = v3(radius, radius, radius);
		qmin = v3sub(qmin, r);
		qmax = v3add(qmax, r);
	}

	broadphase(world, qmin, qmax, seg_fn, &c);
	if (out) *out = c.best;
	return c.found;
}

int YbrDynamicWorldSegment(YbrDynamicWorld* world, Vector3 a, Vector3 b,
						   const YbrQueryOptions* opts, YbrDynamicRayHit* out)
{
	return world_segment_common(world, a, b, 0.0f, opts, out);
}

int YbrDynamicWorldSweepSphere(YbrDynamicWorld* world, Vector3 from, Vector3 to,
							   float radius, const YbrQueryOptions* opts,
							   YbrDynamicRayHit* out)
{
	if (!(0.0f < radius)) radius = 1e-4f;
	return world_segment_common(world, from, to, radius, opts, out);
}

typedef struct WShapeCtx {
	Vector3 a, b;
	float radius;
	int isCapsule;
	const YbrQueryOptions* opts;
	YbrDynamicShapeHit best;
	Vector3 resolve;
} WShapeCtx;

static int wshape_fn(YbrDynamic* dyn, void* ud)
{
	WShapeCtx* c = (WShapeCtx*)ud;
	YbrShapeHit h;
	int hit = c->isCapsule
				  ? YbrDynamicCapsule(dyn, c->a, c->b, c->radius, c->opts, &h)
				  : YbrDynamicSphere(dyn, c->a, c->radius, c->opts, &h);
	if (!hit) return 1;

	c->best.dynamicCount++;
	c->best.hit.count += h.count;
	if (!c->best.hit.hit || c->best.hit.depth < h.depth) {
		c->best.hit.hit = 1;
		c->best.hit.depth = h.depth;
		c->best.hit.point = h.point;
		c->best.hit.normal = h.normal;
		c->best.hit.triangle = h.triangle;
		c->best.dynamic = dyn;
		c->best.bone = YbrDynamicGetLastBone(dyn);
	}
	c->best.hit.hit = 1;

	float len = Vector3Length(h.resolve);
	if (1e-6f < len) {
		Vector3 n = Vector3Scale(h.resolve, 1.0f / len);
		float already = Vector3DotProduct(n, c->resolve);
		if (already < len)
			c->resolve = Vector3Add(c->resolve, Vector3Scale(n, len - already));
	}
	return 1;
}

static int world_shape_common(YbrDynamicWorld* world, Vector3 a, Vector3 b,
							  float radius, int isCapsule,
							  const YbrQueryOptions* opts,
							  YbrDynamicShapeHit* out)
{
	WShapeCtx c;
	memset(&c, 0, sizeof(c));
	c.a = a;
	c.b = b;
	c.radius = radius;
	c.isCapsule = isCapsule;
	c.opts = opts;
	c.best.bone = -1;
	if (out) *out = c.best;
	if (!world) return 0;

	Vector3 r = v3(radius, radius, radius);
	Vector3 qmin = v3sub(isCapsule ? Vector3Min(a, b) : a, r);
	Vector3 qmax = v3add(isCapsule ? Vector3Max(a, b) : a, r);

	broadphase(world, qmin, qmax, wshape_fn, &c);
	c.best.hit.resolve = c.resolve;
	if (out) *out = c.best;
	return c.best.hit.hit;
}

int YbrDynamicWorldSphere(YbrDynamicWorld* world, Vector3 center, float radius,
						  const YbrQueryOptions* opts, YbrDynamicShapeHit* out)
{
	return world_shape_common(world, center, center, radius, 0, opts, out);
}

int YbrDynamicWorldCapsule(YbrDynamicWorld* world, Vector3 a, Vector3 b,
						   float radius, const YbrQueryOptions* opts,
						   YbrDynamicShapeHit* out)
{
	return world_shape_common(world, a, b, radius, 1, opts, out);
}

typedef struct BoxCtx {
	YbrDynamicVisitor visitor;
	void* ud;
} BoxCtx;

static int box_fn(YbrDynamic* dyn, void* ud)
{
	BoxCtx* c = (BoxCtx*)ud;
	return c->visitor(dyn, c->ud);
}

int YbrDynamicWorldOverlapBox(YbrDynamicWorld* world, Vector3 boxMin,
							  Vector3 boxMax, YbrDynamicVisitor visitor,
							  void* userData)
{
	if (!world || !visitor) return 0;
	BoxCtx c;
	c.visitor = visitor;
	c.ud = userData;
	return broadphase(world, Vector3Min(boxMin, boxMax),
					  Vector3Max(boxMin, boxMax), box_fn, &c);
}
