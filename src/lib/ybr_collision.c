/*
	Yui Blender to Raylib - 当たり判定の共通部分
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_collision_internal.h"

YbrQueryOptions YbrQueryOptionsDefaults(void)
{
	YbrQueryOptions o;
	o.tagMask = 0;
	o.cullBackFace = 0;
	return o;
}

// ----------------------------------------------------------------------------
// 幾何

Vector3 YbrClosestPointOnTriangle(Vector3 p, Vector3 a, Vector3 b, Vector3 c)
{
	Vector3 ab = v3sub(b, a), ac = v3sub(c, a), ap = v3sub(p, a);
	float d1 = v3dot(ab, ap), d2 = v3dot(ac, ap);
	if (d1 <= 0.0f && d2 <= 0.0f) return a;

	Vector3 bp = v3sub(p, b);
	float d3 = v3dot(ab, bp), d4 = v3dot(ac, bp);
	if (0.0f <= d3 && d4 <= d3) return b;

	float vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0f && 0.0f <= d1 && d3 <= 0.0f) {
		float t = d1 / (d1 - d3);
		return v3add(a, v3mul(ab, t));
	}

	Vector3 cp = v3sub(p, c);
	float d5 = v3dot(ab, cp), d6 = v3dot(ac, cp);
	if (0.0f <= d6 && d5 <= d6) return c;

	float vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0f && 0.0f <= d2 && d6 <= 0.0f) {
		float t = d2 / (d2 - d6);
		return v3add(a, v3mul(ac, t));
	}

	float va = d3 * d6 - d5 * d4;
	if (va <= 0.0f && 0.0f <= (d4 - d3) && 0.0f <= (d5 - d6)) {
		float t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		return v3add(b, v3mul(v3sub(c, b), t));
	}

	{
		float denom = va + vb + vc;
		if (denom <= YBR_COL_EPS) return a; // 退化三角形
		denom = 1.0f / denom;
		return v3add(a, v3add(v3mul(ab, vb * denom), v3mul(ac, vc * denom)));
	}
}

// 線分どうしの最近接点 (Ericson)
static float closest_seg_seg(Vector3 p1, Vector3 q1, Vector3 p2, Vector3 q2,
							 Vector3* c1, Vector3* c2)
{
	Vector3 d1 = v3sub(q1, p1), d2 = v3sub(q2, p2), r = v3sub(p1, p2);
	float a = v3dot(d1, d1), e = v3dot(d2, d2), f = v3dot(d2, r);
	float s, t;

	if (a <= YBR_COL_EPS && e <= YBR_COL_EPS) {
		s = t = 0.0f;
	}
	else if (a <= YBR_COL_EPS) {
		s = 0.0f;
		t = Clamp(f / e, 0.0f, 1.0f);
	}
	else {
		float c = v3dot(d1, r);
		if (e <= YBR_COL_EPS) {
			t = 0.0f;
			s = Clamp(-c / a, 0.0f, 1.0f);
		}
		else {
			float b = v3dot(d1, d2);
			float denom = a * e - b * b;
			s = (YBR_COL_EPS < denom)
					? Clamp((b * f - c * e) / denom, 0.0f, 1.0f)
					: 0.0f;
			t = (b * s + f) / e;
			if (t < 0.0f) {
				t = 0.0f;
				s = Clamp(-c / a, 0.0f, 1.0f);
			}
			else if (1.0f < t) {
				t = 1.0f;
				s = Clamp((b - c) / a, 0.0f, 1.0f);
			}
		}
	}
	*c1 = v3add(p1, v3mul(d1, s));
	*c2 = v3add(p2, v3mul(d2, t));
	return Vector3DistanceSqr(*c1, *c2);
}

int YbrSegmentTriangleHit(Vector3 a, Vector3 b, Vector3 t0, Vector3 t1,
						  Vector3 t2, int cullBackFace, float* outT,
						  float* outU, float* outV)
{
	Vector3 d = v3sub(b, a);
	Vector3 e1 = v3sub(t1, t0), e2 = v3sub(t2, t0);
	Vector3 pv = v3cross(d, e2);
	float det = v3dot(e1, pv);

	// det > 0 は「面の表側から入ってきた」を意味する
	if (cullBackFace) {
		if (det < YBR_COL_EPS) return 0;
	}
	else if (-YBR_COL_EPS < det && det < YBR_COL_EPS)
		return 0;

	{
		float inv = 1.0f / det;
		Vector3 tv = v3sub(a, t0);
		float u = v3dot(tv, pv) * inv;
		if (u < 0.0f || 1.0f < u) return 0;

		Vector3 qv = v3cross(tv, e1);
		float v = v3dot(d, qv) * inv;
		if (v < 0.0f || 1.0f < u + v) return 0;

		float t = v3dot(e2, qv) * inv;
		if (t < 0.0f || 1.0f < t) return 0;

		if (outT) *outT = t;
		if (outU) *outU = u;
		if (outV) *outV = v;
		return 0.0f < det ? 1 : -1; // -1 = 裏面から
	}
}

float YbrSegmentTriangleDistance(Vector3 a, Vector3 b, Vector3 t0, Vector3 t1,
								 Vector3 t2, Vector3* outSeg, Vector3* outTri)
{
	float t;
	if (YbrSegmentTriangleHit(a, b, t0, t1, t2, 0, &t, NULL, NULL)) {
		Vector3 p = Vector3Lerp(a, b, t);
		if (outSeg) *outSeg = p;
		if (outTri) *outTri = p;
		return 0.0f;
	}

	// 交差していないときの最短距離は
	float best = 1e30f;
	Vector3 bs = a, bt = t0;

	const Vector3 e[3][2] = {{t0, t1}, {t1, t2}, {t2, t0}};
	for (int i = 0; i < 3; i++) {
		Vector3 c1, c2;
		float d2 = closest_seg_seg(a, b, e[i][0], e[i][1], &c1, &c2);
		if (d2 < best) {
			best = d2;
			bs = c1;
			bt = c2;
		}
	}
	{
		Vector3 ends[2];
		ends[0] = a;
		ends[1] = b;
		for (int i = 0; i < 2; i++) {
			Vector3 q = YbrClosestPointOnTriangle(ends[i], t0, t1, t2);
			float d2 = Vector3DistanceSqr(ends[i], q);
			if (d2 < best) {
				best = d2;
				bs = ends[i];
				bt = q;
			}
		}
	}

	if (outSeg) *outSeg = bs;
	if (outTri) *outTri = bt;
	return sqrtf(best);
}

// 三角形と AABB の交差 (Akenine-Moller の 13 軸 SAT)
static int tri_aabb_overlap(Vector3 a, Vector3 b, Vector3 c, Aabb box)
{
	Vector3 center = v3mul(v3add(box.min, box.max), 0.5f);
	Vector3 h = v3mul(v3sub(box.max, box.min), 0.5f);

	Vector3 v0 = v3sub(a, center), v1 = v3sub(b, center), v2 = v3sub(c, center);
	Vector3 f0 = v3sub(v1, v0), f1 = v3sub(v2, v1), f2 = v3sub(v0, v2);

	const float fv[3][3] = {
		{f0.x, f0.y, f0.z}, {f1.x, f1.y, f1.z}, {f2.x, f2.y, f2.z}};
	const float pv[3][3] = {
		{v0.x, v0.y, v0.z}, {v1.x, v1.y, v1.z}, {v2.x, v2.y, v2.z}};
	const float hv[3] = {h.x, h.y, h.z};

	// 9 本の交差軸 : e_i x f_j
	for (int i = 0; i < 3; i++) {	  // 箱の軸
		for (int j = 0; j < 3; j++) { // 三角形の辺
			int i1 = (i + 1) % 3, i2 = (i + 2) % 3;
			float ax1 = -fv[j][i2], ax2 = fv[j][i1]; // e_i x f_j の非零成分
			float p0 = pv[0][i1] * ax1 + pv[0][i2] * ax2;
			float p1 = pv[1][i1] * ax1 + pv[1][i2] * ax2;
			float p2 = pv[2][i1] * ax1 + pv[2][i2] * ax2;
			float r = hv[i1] * fabsf(ax1) + hv[i2] * fabsf(ax2);
			float mn = p0 < p1 ? (p0 < p2 ? p0 : p2) : (p1 < p2 ? p1 : p2);
			float mx = p1 < p0 ? (p2 < p0 ? p0 : p2) : (p2 < p1 ? p1 : p2);
			if (r + YBR_COL_EPS < mn || mx < -r - YBR_COL_EPS) return 0;
		}
	}

	// 箱の 3 軸
	for (int i = 0; i < 3; i++) {
		float mn = pv[0][i], mx = pv[0][i];
		for (int k = 1; k < 3; k++) {
			if (pv[k][i] < mn) mn = pv[k][i];
			if (mx < pv[k][i]) mx = pv[k][i];
		}
		if (hv[i] < mn || mx < -hv[i]) return 0;
	}

	// 三角形の面
	{
		Vector3 n = v3cross(f0, f1);
		float d = v3dot(n, v0);
		float r = hv[0] * fabsf(n.x) + hv[1] * fabsf(n.y) + hv[2] * fabsf(n.z);
		if (r < d || d < -r) return 0;
	}
	return 1;
}

// ----------------------------------------------------------------------------
// スイープ

// レイと球。もっとも手前の交点 t を返す (t < 0 は無視)
static int ray_sphere(Vector3 o, Vector3 d, Vector3 c, float r, float* outT)
{
	Vector3 m = v3sub(o, c);
	float a = v3dot(d, d);
	if (a < 1e-12f) return 0;
	float b = 2.0f * v3dot(m, d);
	float cc = v3dot(m, m) - r * r;
	float disc = b * b - 4.0f * a * cc;
	if (disc < 0.0f) return 0;
	float sq = sqrtf(disc);
	float t0 = (-b - sq) / (2.0f * a);
	float t1 = (-b + sq) / (2.0f * a);
	float t = (0.0f <= t0) ? t0 : t1;
	if (t < 0.0f) return 0;
	*outT = t;
	return 1;
}

// レイと「線分 p-q を軸にした無限円柱」。軸方向のパラメータが 0..1 の
// ときだけ有効 (端は呼び出し側で球として扱う)。
static int ray_cylinder(Vector3 o, Vector3 d, Vector3 p, Vector3 q, float r,
						float* outT)
{
	Vector3 ab = v3sub(q, p);
	Vector3 ao = v3sub(o, p);
	float abab = v3dot(ab, ab);
	if (abab < 1e-12f) return 0;

	float abd = v3dot(ab, d);
	float abao = v3dot(ab, ao);

	float a = v3dot(d, d) - abd * abd / abab;
	float b = 2.0f * (v3dot(d, ao) - abd * abao / abab);
	float c = v3dot(ao, ao) - abao * abao / abab - r * r;

	if (fabsf(a) < 1e-12f) return 0; // 軸と平行
	float disc = b * b - 4.0f * a * c;
	if (disc < 0.0f) return 0;
	float sq = sqrtf(disc);
	float t0 = (-b - sq) / (2.0f * a);
	float t1 = (-b + sq) / (2.0f * a);

	float ts[2] = {t0, t1};
	for (int i = 0; i < 2; i++) {
		float t = ts[i];
		if (t < 0.0f) continue;
		float u = (abao + t * abd) / abab;
		if (u < 0.0f || 1.0f < u) continue; // 端は球で拾う
		*outT = t;
		return 1;
	}
	return 0;
}

int YbrSweepSphereTriangle(Vector3 from, Vector3 to, float radius, Vector3 t0,
						   Vector3 t1, Vector3 t2, float* outT,
						   Vector3* outPoint, Vector3* outNormal)
{
	Vector3 d = v3sub(to, from);
	float r = 0.0f < radius ? radius : 0.0f;

	// 出発時点ですでに接触している
	{
		Vector3 q = YbrClosestPointOnTriangle(from, t0, t1, t2);
		float dist2 = Vector3DistanceSqr(from, q);
		if (dist2 <= r * r) {
			Vector3 n = v3sub(from, q);
			float len = Vector3Length(n);
			n = (1e-6f < len)
					? v3mul(n, 1.0f / len)
					: Vector3Normalize(v3cross(v3sub(t1, t0), v3sub(t2, t0)));
			if (outT) *outT = 0.0f;
			if (outPoint) *outPoint = q;
			if (outNormal) *outNormal = n;
			return 1;
		}
	}
	if (Vector3LengthSqr(d) < 1e-12f) return 0;

	Vector3 nrm = v3cross(v3sub(t1, t0), v3sub(t2, t0));
	float nlen = Vector3Length(nrm);
	// 面積0の三角形？
	if (nlen <= 1e-12f) return 0;
	nrm = v3mul(nrm, 1.0f / nlen);

	float best = 2.0f;
	int found = 0;

	// 面 : 三角形を法線方向にrだけ押し出した平面との交差
	{
		float s0 = v3dot(nrm, v3sub(from, t0));
		float sd = v3dot(nrm, d);
		float sign = (0.0f <= s0) ? 1.0f : -1.0f;
		if (1e-9f < fabsf(sd)) {
			float t = (sign * r - s0) / sd;
			if (0.0f <= t && t <= 1.0f) {
				Vector3 center = v3add(from, v3mul(d, t));
				Vector3 onPlane = v3sub(center, v3mul(nrm, sign * r));
				// 平面上の点が三角形の内側にあるか
				Vector3 q = YbrClosestPointOnTriangle(onPlane, t0, t1, t2);
				if (Vector3DistanceSqr(onPlane, q) <= 1e-8f) {
					best = t;
					found = 1;
				}
			}
		}
	}

	// 辺（円柱）と頂点（球）
	{
		const Vector3 vs[3] = {t0, t1, t2};
		for (int i = 0; i < 3; i++) {
			float t;
			if (ray_cylinder(from, d, vs[i], vs[(i + 1) % 3], r, &t) &&
				0.0f <= t && t <= 1.0f && (!found || t < best)) {
				best = t;
				found = 1;
			}
			if (ray_sphere(from, d, vs[i], r, &t) && 0.0f <= t && t <= 1.0f &&
				(!found || t < best)) {
				best = t;
				found = 1;
			}
		}
	}

	if (!found) return 0;

	Vector3 center = v3add(from, v3mul(d, best));
	Vector3 q = YbrClosestPointOnTriangle(center, t0, t1, t2);
	Vector3 n = v3sub(center, q);
	float len = Vector3Length(n);
	n = (1e-6f < len) ? v3mul(n, 1.0f / len) : nrm;

	if (outT) *outT = best;
	if (outPoint) *outPoint = q;
	if (outNormal) *outNormal = n;
	return 1;
}

int YbrTriangleBoxOverlap(Vector3 a, Vector3 b, Vector3 c, Vector3 boxMin,
						  Vector3 boxMax)
{
	Aabb box;
	box.min = Vector3Min(boxMin, boxMax);
	box.max = Vector3Max(boxMin, boxMax);
	return tri_aabb_overlap(a, b, c, box);
}
