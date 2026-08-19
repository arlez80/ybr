/*
	Yui Blender to Raylib - YbrCurve の評価ユーティリティ
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_curve.h"

#include <math.h>
#include <stdlib.h>

#define YBR_MAX_ORDER 16

static const Vector3 YBR_ZERO = {0.0f, 0.0f, 0.0f};

static const YbrSpline* get_spline(const YbrCurve* curve, int splineIndex)
{
	if (!curve || splineIndex < 0 || curve->splineCount <= splineIndex)
		return NULL;
	const YbrSpline* s = &curve->splines[splineIndex];
	if (s->pointCount <= 0 || !s->points) return NULL;
	return s;
}

static Vector3 point_at(const YbrSpline* s, int i)
{
	Vector3 v = {s->points[i * 3], s->points[i * 3 + 1], s->points[i * 3 + 2]};
	return v;
}

static Vector3 handle_at(const float* h, int i)
{
	Vector3 v = {h[i * 3], h[i * 3 + 1], h[i * 3 + 2]};
	return v;
}

// 区間数 (閉じているときは最後の点から始点へ戻る区間が増える)
static int segment_count(const YbrSpline* s)
{
	if (s->pointCount < 2) return 0;
	return s->cyclic ? s->pointCount : s->pointCount - 1;
}

// POLY
static Vector3 eval_poly(const YbrSpline* s, float t)
{
	int segs = segment_count(s);
	if (segs <= 0) return point_at(s, 0);

	float ft = t * (float)segs;
	int seg = (int)ft;
	if (segs <= seg) seg = segs - 1;
	float local = ft - (float)seg;

	int i0 = seg;
	int i1 = (seg + 1) % s->pointCount;
	Vector3 a = point_at(s, i0), b = point_at(s, i1);
	return Vector3Add(Vector3Scale(a, 1.0f - local), Vector3Scale(b, local));
}

// BEZIER
static Vector3 eval_bezier(const YbrSpline* s, float t)
{
	int segs = segment_count(s);
	if (segs <= 0 || !s->handlesLeft || !s->handlesRight)
		return eval_poly(s, t);

	float ft = t * (float)segs;
	int seg = (int)ft;
	if (segs <= seg) seg = segs - 1;
	float u = ft - (float)seg;

	int i0 = seg;
	int i1 = (seg + 1) % s->pointCount;

	Vector3 p0 = point_at(s, i0);
	Vector3 p1 = handle_at(s->handlesRight, i0);
	Vector3 p2 = handle_at(s->handlesLeft, i1);
	Vector3 p3 = point_at(s, i1);

	float mu = 1.0f - u;
	float w0 = mu * mu * mu;
	float w1 = 3.0f * mu * mu * u;
	float w2 = 3.0f * mu * u * u;
	float w3 = u * u * u;

	Vector3 r = Vector3Scale(p0, w0);
	r = Vector3Add(r, Vector3Scale(p1, w1));
	r = Vector3Add(r, Vector3Scale(p2, w2));
	r = Vector3Add(r, Vector3Scale(p3, w3));
	return r;
}

// ----------------------------------------------------------------------------
// NURBS（一様ノットの有理 B-spline / de Boor）

static Vector3 eval_nurbs(const YbrSpline* s, float t)
{
	int n = s->pointCount;
	int order = s->order;
	if (order < 2) order = 2;
	if (YBR_MAX_ORDER < order) order = YBR_MAX_ORDER;
	if (n < order) order = n;
	int p = order - 1; // 次数

	if (n < 2 || p < 1) return point_at(s, 0);

	double u;
	if (s->cyclic) {
		u = (double)p + (double)t * (double)n;
	}
	else {
		if (n <= p) return eval_poly(s, t);
		u = (double)p + (double)t * (double)(n - p);
	}

	int k = (int)floor(u);
	int hiKnot = s->cyclic ? (p + n) : n;
	if (hiKnot - 1 < k) k = hiKnot - 1;
	if (k < p) k = p;

	// 同次座標 (w*x, w*y, w*z, w) で de Boor を回す
	double dx[YBR_MAX_ORDER], dy[YBR_MAX_ORDER], dz[YBR_MAX_ORDER],
		dw[YBR_MAX_ORDER];
	for (int j = 0; j <= p; j++) {
		int idx = k - p + j;
		if (s->cyclic) {
			idx %= n;
			if (idx < 0) idx += n;
		}
		else {
			if (idx < 0) idx = 0;
			if (n - 1 < idx) idx = n - 1;
		}
		double w = s->weights ? (double)s->weights[idx] : 1.0;
		if (w <= 0.0) w = 1.0;
		dx[j] = (double)s->points[idx * 3] * w;
		dy[j] = (double)s->points[idx * 3 + 1] * w;
		dz[j] = (double)s->points[idx * 3 + 2] * w;
		dw[j] = w;
	}

	for (int r = 1; r <= p; r++) {
		for (int j = p; r <= j; j--) {
			double denom = (double)(p - r + 1); // 一様ノットなので定数
			double alpha = (u - (double)(j + k - p)) / denom;
			if (alpha < 0.0) alpha = 0.0;
			if (1.0 < alpha) alpha = 1.0;
			double ia = 1.0 - alpha;
			dx[j] = ia * dx[j - 1] + alpha * dx[j];
			dy[j] = ia * dy[j - 1] + alpha * dy[j];
			dz[j] = ia * dz[j - 1] + alpha * dz[j];
			dw[j] = ia * dw[j - 1] + alpha * dw[j];
		}
	}

	Vector3 out = YBR_ZERO;
	if (dw[p] != 0.0) {
		out.x = (float)(dx[p] / dw[p]);
		out.y = (float)(dy[p] / dw[p]);
		out.z = (float)(dz[p] / dw[p]);
	}
	return out;
}

// ----------------------------------------------------------------------------
// 公開API

static Vector3 eval_spline(const YbrSpline* s, float t)
{
	if (s->pointCount == 1) return point_at(s, 0);
	switch (s->type) {
		case YBR_SPLINE_BEZIER:
			return eval_bezier(s, t);
		case YBR_SPLINE_NURBS:
			return eval_nurbs(s, t);
		default:
			return eval_poly(s, t);
	}
}

Vector3 YbrCurveGetPoint(const YbrCurve* curve, int splineIndex, float weight,
						 int resolution)
{
	(void)resolution;
	const YbrSpline* s = get_spline(curve, splineIndex);
	if (!s) return YBR_ZERO;
	return eval_spline(s, Clamp(weight, 0.0f, 1.0f));
}

Vector3 YbrCurveGetTangent(const YbrCurve* curve, int splineIndex, float weight,
						   int resolution)
{
	const YbrSpline* s = get_spline(curve, splineIndex);
	if (!s) return YBR_ZERO;
	if (resolution < 1) resolution = 1;

	int segs = segment_count(s);
	if (segs <= 0) return YBR_ZERO;

	float t = Clamp(weight, 0.0f, 1.0f);
	float h = 1.0f / (float)(segs * resolution * 4);
	if (h <= 0.0f) h = 1e-4f;

	float t0 = Clamp(t - h, 0.0f, 1.0f);
	float t1 = Clamp(t + h, 0.0f, 1.0f);
	if (t1 - t0 <= 0.0f) return YBR_ZERO;

	Vector3 d = Vector3Subtract(eval_spline(s, t1), eval_spline(s, t0));
	if (Vector3Length(d) <= 0.0f) return YBR_ZERO;
	return Vector3Normalize(d);
}

// 折れ線近似の分割数
static int sample_count(const YbrSpline* s, int resolution)
{
	if (resolution < 1) resolution = 1;
	int segs = segment_count(s);
	if (segs <= 0) segs = 1;
	long n = (long)segs * (long)resolution;
	if (n < 1) n = 1;
	if (1000000L < n) n = 1000000L; // 暴走防止
	return (int)n;
}

float YbrCurveGetLength(const YbrCurve* curve, int splineIndex, int resolution)
{
	const YbrSpline* s = get_spline(curve, splineIndex);
	if (!s || s->pointCount < 2) return 0.0f;

	int n = sample_count(s, resolution);
	float total = 0.0f;
	Vector3 prev = eval_spline(s, 0.0f);
	for (int i = 1; i <= n; i++) {
		Vector3 cur = eval_spline(s, (float)i / (float)n);
		total += Vector3Distance(prev, cur);
		prev = cur;
	}
	return total;
}

float YbrCurveGetWeightAtDistance(const YbrCurve* curve, int splineIndex,
								  float distance, int resolution,
								  int iterations)
{
	const YbrSpline* s = get_spline(curve, splineIndex);
	if (!s || s->pointCount < 2) return 0.0f;
	if (iterations < 0) iterations = 0;

	int n = sample_count(s, resolution);

	// 折れ線を歩いて距離を含む区間を探す
	float acc = 0.0f;
	float t0 = 0.0f, t1 = 1.0f;
	float segLen = 0.0f;
	Vector3 prev = eval_spline(s, 0.0f);
	Vector3 p0 = prev, p1 = prev;
	int found = 0;

	if (distance <= 0.0f) return 0.0f;

	for (int i = 1; i <= n; i++) {
		float t = (float)i / (float)n;
		Vector3 cur = eval_spline(s, t);
		float d = Vector3Distance(prev, cur);
		if (!found && distance <= acc + d) {
			t0 = (float)(i - 1) / (float)n;
			t1 = t;
			p0 = prev;
			p1 = cur;
			segLen = d;
			distance -= acc;  // 区間先頭からの残り距離
			found = 1;
		}
		acc += d;
		prev = cur;
	}

	if (!found) return 1.0f;  // 全長を超えている -> 終端にクランプ
	if (segLen <= 0.0f) return t0;

	// 区間内を二分して近似を詰める
	float remain = distance;
	float lenLeft = segLen;
	for (int it = 0; it < iterations; it++) {
		float tm = 0.5f * (t0 + t1);
		Vector3 pm = eval_spline(s, tm);
		float dLeft = Vector3Distance(p0, pm);
		float dRight = Vector3Distance(pm, p1);
		if (remain <= dLeft) {
			t1 = tm;
			p1 = pm;
			lenLeft = dLeft;
		}
		else {
			remain -= dLeft;
			t0 = tm;
			p0 = pm;
			lenLeft = dRight;
		}
		if (lenLeft <= 0.0f) return t0;
	}

	float frac = (0.0f < lenLeft) ? (remain / lenLeft) : 0.0f;
	return Clamp(t0 + (t1 - t0) * Clamp(frac, 0.0f, 1.0f), 0.0f, 1.0f);
}

Vector3 YbrCurveGetPointAtDistance(const YbrCurve* curve, int splineIndex,
								   float distance, int resolution,
								   int iterations)
{
	float w = YbrCurveGetWeightAtDistance(curve, splineIndex, distance,
										  resolution, iterations);
	return YbrCurveGetPoint(curve, splineIndex, w, resolution);
}
