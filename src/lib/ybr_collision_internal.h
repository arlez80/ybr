/*
	Yui Blender to Raylib - 当たり判定ユーティリティ
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_COLLISION_INTERNAL_H
#define YBR_COLLISION_INTERNAL_H

#include <math.h>
#include <string.h>

#include "ybr_collision.h"
#include "ybr_internal.h"

#define YBR_COL_EPS 1e-8f
#define YBR_COL_STACK 256

// 小物

static inline Vector3 v3(float x, float y, float z)
{
	Vector3 r;
	r.x = x;
	r.y = y;
	r.z = z;
	return r;
}
static inline Vector3 v3add(Vector3 a, Vector3 b) { return Vector3Add(a, b); }
static inline Vector3 v3sub(Vector3 a, Vector3 b)
{
	return Vector3Subtract(a, b);
}
static inline Vector3 v3mul(Vector3 a, float s) { return Vector3Scale(a, s); }
static inline float v3dot(Vector3 a, Vector3 b)
{
	return Vector3DotProduct(a, b);
}
static inline Vector3 v3cross(Vector3 a, Vector3 b)
{
	return Vector3CrossProduct(a, b);
}

typedef struct Aabb {
	Vector3 min, max;
} Aabb;

static inline Aabb aabb_empty(void)
{
	Aabb b;
	b.min = v3(1e30f, 1e30f, 1e30f);
	b.max = v3(-1e30f, -1e30f, -1e30f);
	return b;
}

static inline void aabb_add_point(Aabb* b, Vector3 p)
{
	b->min = Vector3Min(b->min, p);
	b->max = Vector3Max(b->max, p);
}

static inline void aabb_add_box(Aabb* b, Aabb o)
{
	b->min = Vector3Min(b->min, o.min);
	b->max = Vector3Max(b->max, o.max);
}

static inline int aabb_valid(Aabb b)
{
	return b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z;
}

static inline Aabb tri_aabb(const YbrTriangle* t)
{
	Aabb b = aabb_empty();
	aabb_add_point(&b, t->v[0]);
	aabb_add_point(&b, t->v[1]);
	aabb_add_point(&b, t->v[2]);
	return b;
}

static inline int aabb_contains(Aabb outer, Aabb inner)
{
	return outer.min.x <= inner.min.x && inner.max.x <= outer.max.x &&
		   outer.min.y <= inner.min.y && inner.max.y <= outer.max.y &&
		   outer.min.z <= inner.min.z && inner.max.z <= outer.max.z;
}

static inline int aabb_overlap(Aabb a, Aabb b)
{
	return !(a.max.x < b.min.x || b.max.x < a.min.x || a.max.y < b.min.y ||
			 b.max.y < a.min.y || a.max.z < b.min.z || b.max.z < a.min.z);
}

// 点から AABB までの距離の 2 乗 (内部なら 0)
static inline float aabb_dist2_point(Aabb b, Vector3 p)
{
	float d = 0.0f, t;
	t = p.x < b.min.x ? b.min.x - p.x : (b.max.x < p.x ? p.x - b.max.x : 0.0f);
	d += t * t;
	t = p.y < b.min.y ? b.min.y - p.y : (b.max.y < p.y ? p.y - b.max.y : 0.0f);
	d += t * t;
	t = p.z < b.min.z ? b.min.z - p.z : (b.max.z < p.z ? p.z - b.max.z : 0.0f);
	d += t * t;
	return d;
}

// 線分 a-b と AABB がかすめるか (slab 法 / 保守的)
static inline int aabb_overlap_segment(Aabb b, Vector3 a, Vector3 e)
{
	float t0 = 0.0f, t1 = 1.0f;
	const float pa[3] = {a.x, a.y, a.z};
	const float pe[3] = {e.x, e.y, e.z};
	const float bmin[3] = {b.min.x, b.min.y, b.min.z};
	const float bmax[3] = {b.max.x, b.max.y, b.max.z};
	for (int i = 0; i < 3; i++) {
		float o = pa[i], d = pe[i] - o;
		if (-YBR_COL_EPS < d && d < YBR_COL_EPS) {
			if (o < bmin[i] || bmax[i] < o) return 0;
		}
		else {
			float inv = 1.0f / d;
			float n = (bmin[i] - o) * inv;
			float f = (bmax[i] - o) * inv;
			if (f < n) {
				float tmp = n;
				n = f;
				f = tmp;
			}
			if (t0 < n) t0 = n;
			if (f < t1) t1 = f;
			if (t1 < t0) return 0;
		}
	}
	return 1;
}

static inline Aabb aabb_expand(Aabb b, float r)
{
	b.min = v3(b.min.x - r, b.min.y - r, b.min.z - r);
	b.max = v3(b.max.x + r, b.max.y + r, b.max.z + r);
	return b;
}

#endif /* YBR_COLLISION_INTERNAL_H */
