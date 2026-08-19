/*
	Yui Blender to Raylib - アニメーションの補間 / サンプリング
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_anim.h"

#include <math.h>
#include <string.h>

#ifndef YBR_PI
#define YBR_PI 3.14159265358979323846f
#endif

YbrInterpParams YbrInterpParamsDefault(void)
{
	YbrInterpParams p;
	p.sincA = YBR_SINC_A_DEFAULT;
	return p;
}

YbrInterpParams YbrInterpParamsSanitize(const YbrInterpParams* p)
{
	YbrInterpParams o = YbrInterpParamsDefault();
	if (!p) return o;
	o = *p;
	if (o.sincA < 1) o.sincA = 1;
	if (YBR_INTERP_MAX_RADIUS < o.sincA) o.sincA = YBR_INTERP_MAX_RADIUS;
	return o;
}

YbrInterpParams YbrInterpParamsFromAnimation(const YbrAnimation* a)
{
	YbrInterpParams p = YbrInterpParamsDefault();
	if (!a) return p;
	p.sincA = a->sincA;
	return YbrInterpParamsSanitize(&p);
}

const char* YbrInterpName(YbrInterp i)
{
	switch (i) {
		case YBR_INTERP_STEP:
			return "STEP";
		case YBR_INTERP_LINEAR:
			return "LINEAR";
		case YBR_INTERP_CUBIC:
			return "CUBIC";
		case YBR_INTERP_SINC:
			return "SINC";
		case YBR_INTERP_HERMITE:
			return "HERMITE";
		default:
			return "UNKNOWN";
	}
}

static int ci_equal(const char* a, const char* b)
{
	for (; *a && *b; a++, b++) {
		char ca = *a, cb = *b;
		if ('A' <= ca && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
		if ('A' <= cb && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
		if (ca != cb) return 0;
	}
	return *a == 0 && *b == 0;
}

int YbrInterpParse(const char* name, YbrInterp* out)
{
	if (!name || !out) return 0;
	if (ci_equal(name, "step")) {
		*out = YBR_INTERP_STEP;
		return 1;
	}
	if (ci_equal(name, "linear")) {
		*out = YBR_INTERP_LINEAR;
		return 1;
	}
	if (ci_equal(name, "cubic")) {
		*out = YBR_INTERP_CUBIC;
		return 1;
	}
	if (ci_equal(name, "sinc")) {
		*out = YBR_INTERP_SINC;
		return 1;
	}
	if (ci_equal(name, "lanczos")) {
		*out = YBR_INTERP_SINC;
		return 1;
	}
	if (ci_equal(name, "hermite")) {
		*out = YBR_INTERP_HERMITE;
		return 1;
	}
	if (ci_equal(name, "bezier")) {
		*out = YBR_INTERP_HERMITE;
		return 1;
	}
	return 0;
}

int YbrInterpRadius(YbrInterp i, const YbrInterpParams* p)
{
	YbrInterpParams q = YbrInterpParamsSanitize(p);
	switch (i) {
		case YBR_INTERP_STEP:
			return 0;
		case YBR_INTERP_LINEAR:
			return 0;
		case YBR_INTERP_CUBIC:
			return 1;
		case YBR_INTERP_SINC:
			return q.sincA;
		case YBR_INTERP_HERMITE:
			return 0; // 区間の両端キーしか見ない
		default:
			return 0;
	}
}

// TRS <-> Matrix

// raylib の Matrix は「float 配列にしたとき column-major」なので、
// 基底ベクトルは列 = (m0,m1,m2) / (m4,m5,m6) / (m8,m9,m10)、
// 平行移動は (m12,m13,m14)。raymath の MatrixDecompose には頼らず、
// 分解 -> 合成が確実に往復するよう自前で実装する。

YbrTransform YbrTransformIdentity(void)
{
	YbrTransform t;
	t.translation.x = t.translation.y = t.translation.z = 0.0f;
	t.rotation.x = t.rotation.y = t.rotation.z = 0.0f;
	t.rotation.w = 1.0f;
	t.scale.x = t.scale.y = t.scale.z = 1.0f;
	return t;
}

YbrTransform YbrTransformFromMatrix(Matrix m)
{
	YbrTransform t = YbrTransformIdentity();

	t.translation.x = m.m12;
	t.translation.y = m.m13;
	t.translation.z = m.m14;

	// 列ベクトル
	float cx[3] = {m.m0, m.m1, m.m2};
	float cy[3] = {m.m4, m.m5, m.m6};
	float cz[3] = {m.m8, m.m9, m.m10};

	float sx = sqrtf(cx[0] * cx[0] + cx[1] * cx[1] + cx[2] * cx[2]);
	float sy = sqrtf(cy[0] * cy[0] + cy[1] * cy[1] + cy[2] * cy[2]);
	float sz = sqrtf(cz[0] * cz[0] + cz[1] * cz[1] + cz[2] * cz[2]);

	// 行列式が負 = 鏡映。符号は x スケールにまとめる
	float det = cx[0] * (cy[1] * cz[2] - cy[2] * cz[1]) -
				cy[0] * (cx[1] * cz[2] - cx[2] * cz[1]) +
				cz[0] * (cx[1] * cy[2] - cx[2] * cy[1]);
	if (det < 0.0f) sx = -sx;

	t.scale.x = sx;
	t.scale.y = sy;
	t.scale.z = sz;

	if (fabsf(sx) < 1e-12f || sy < 1e-12f || sz < 1e-12f) return t; // 退化

	// 正規直交化した回転行列 r[col][row]
	float r00 = cx[0] / sx, r10 = cx[1] / sx, r20 = cx[2] / sx;
	float r01 = cy[0] / sy, r11 = cy[1] / sy, r21 = cy[2] / sy;
	float r02 = cz[0] / sz, r12 = cz[1] / sz, r22 = cz[2] / sz;

	float trace = r00 + r11 + r22;
	float qx, qy, qz, qw;
	if (0.0f < trace) {
		float s = sqrtf(trace + 1.0f) * 2.0f;
		qw = 0.25f * s;
		qx = (r21 - r12) / s;
		qy = (r02 - r20) / s;
		qz = (r10 - r01) / s;
	}
	else if (r11 < r00 && r22 < r00) {
		float s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
		qw = (r21 - r12) / s;
		qx = 0.25f * s;
		qy = (r01 + r10) / s;
		qz = (r02 + r20) / s;
	}
	else if (r22 < r11) {
		float s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
		qw = (r02 - r20) / s;
		qx = (r01 + r10) / s;
		qy = 0.25f * s;
		qz = (r12 + r21) / s;
	}
	else {
		float s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
		qw = (r10 - r01) / s;
		qx = (r02 + r20) / s;
		qy = (r12 + r21) / s;
		qz = 0.25f * s;
	}

	float len = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
	if (1e-12f < len) {
		qx /= len;
		qy /= len;
		qz /= len;
		qw /= len;
	}
	else {
		qx = qy = qz = 0.0f;
		qw = 1.0f;
	}

	// w >= 0 に正規化しておくと符号ぞろえが安定する
	if (qw < 0.0f) {
		qx = -qx;
		qy = -qy;
		qz = -qz;
		qw = -qw;
	}

	t.rotation.x = qx;
	t.rotation.y = qy;
	t.rotation.z = qz;
	t.rotation.w = qw;
	return t;
}

Matrix YbrTransformToMatrix(YbrTransform t)
{
	float x = t.rotation.x, y = t.rotation.y, z = t.rotation.z,
		  w = t.rotation.w;
	float len = sqrtf(x * x + y * y + z * z + w * w);
	if (1e-12f < len) {
		x /= len;
		y /= len;
		z /= len;
		w /= len;
	}
	else {
		x = y = z = 0.0f;
		w = 1.0f;
	}

	float r00 = 1.0f - 2.0f * (y * y + z * z), r01 = 2.0f * (x * y - w * z),
		  r02 = 2.0f * (x * z + w * y);
	float r10 = 2.0f * (x * y + w * z), r11 = 1.0f - 2.0f * (x * x + z * z),
		  r12 = 2.0f * (y * z - w * x);
	float r20 = 2.0f * (x * z - w * y), r21 = 2.0f * (y * z + w * x),
		  r22 = 1.0f - 2.0f * (x * x + y * y);

	Matrix m;
	m.m0 = r00 * t.scale.x;
	m.m1 = r10 * t.scale.x;
	m.m2 = r20 * t.scale.x;
	m.m3 = 0.0f;
	m.m4 = r01 * t.scale.y;
	m.m5 = r11 * t.scale.y;
	m.m6 = r21 * t.scale.y;
	m.m7 = 0.0f;
	m.m8 = r02 * t.scale.z;
	m.m9 = r12 * t.scale.z;
	m.m10 = r22 * t.scale.z;
	m.m11 = 0.0f;
	m.m12 = t.translation.x;
	m.m13 = t.translation.y;
	m.m14 = t.translation.z;
	m.m15 = 1.0f;
	return m;
}

YbrTransform YbrTransformLerp(YbrTransform a, YbrTransform b, float s)
{
	YbrTransform o;
	o.translation.x = a.translation.x + (b.translation.x - a.translation.x) * s;
	o.translation.y = a.translation.y + (b.translation.y - a.translation.y) * s;
	o.translation.z = a.translation.z + (b.translation.z - a.translation.z) * s;
	o.scale.x = a.scale.x + (b.scale.x - a.scale.x) * s;
	o.scale.y = a.scale.y + (b.scale.y - a.scale.y) * s;
	o.scale.z = a.scale.z + (b.scale.z - a.scale.z) * s;

	float d = a.rotation.x * b.rotation.x + a.rotation.y * b.rotation.y +
			  a.rotation.z * b.rotation.z + a.rotation.w * b.rotation.w;
	float sign = (d < 0.0f) ? -1.0f : 1.0f;
	float qx = a.rotation.x + (b.rotation.x * sign - a.rotation.x) * s;
	float qy = a.rotation.y + (b.rotation.y * sign - a.rotation.y) * s;
	float qz = a.rotation.z + (b.rotation.z * sign - a.rotation.z) * s;
	float qw = a.rotation.w + (b.rotation.w * sign - a.rotation.w) * s;
	float len = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
	if (1e-12f < len) {
		qx /= len;
		qy /= len;
		qz /= len;
		qw /= len;
	}
	else {
		qx = qy = qz = 0.0f;
		qw = 1.0f;
	}
	o.rotation.x = qx;
	o.rotation.y = qy;
	o.rotation.z = qz;
	o.rotation.w = qw;
	return o;
}

// 重みの計算

// どの補間方法も「キー lo .. lo+n-1 の重み付き和」で表せる形にする。
// 重みの総和は必ず 1 (アフィン不変) になるようにしてある。

static float sinc_pi(float x)
{
	if (fabsf(x) < 1e-6f) return 1.0f;
	float px = YBR_PI * x;
	return sinf(px) / px;
}

static float lanczos(float x, int a)
{
	float ax = fabsf(x);
	if ((float)a <= ax) return 0.0f;
	return sinc_pi(x) * sinc_pi(x / (float)a);
}

// i     : 区間の終端キー番号 (1 .. count-1)
static int build_weights(int count, const int* segFrames, YbrInterp mode, int i,
						 float s, const YbrInterpParams* p, int* outLo,
						 float* w)
{
	// segFrames は「必要な範囲のフレーム番号を引く」ための関数ではなく、
	int lo, hi, n, k;

	switch (mode) {
		case YBR_INTERP_STEP:
			*outLo = i - 1;
			w[0] = 1.0f;
			return 1;

		case YBR_INTERP_LINEAR:
			*outLo = i - 1;
			w[0] = 1.0f - s;
			w[1] = s;
			return 2;

		case YBR_INTERP_CUBIC: {
			// 非一様 Catmull-Rom を Hermite 基底で展開し、
			// 4キーぶんの重みへ落とす
			float s2 = s * s, s3 = s2 * s;
			float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
			float h10 = s3 - 2.0f * s2 + s;
			float h01 = -2.0f * s3 + 3.0f * s2;
			float h11 = s3 - s2;

			int im2 = i - 2, im1 = i - 1, ip1 = i + 1;
			float fm2 = (float)segFrames[0]; // key i-2（無効なら未使用）
			float fm1 = (float)segFrames[1]; // key i-1
			float f0 = (float)segFrames[2];	 // key i
			float fp1 = (float)segFrames[3]; // key i+1（無効なら未使用）
			float dt = f0 - fm1;
			if (dt <= 0.0f) dt = 1.0f;

			lo = (0 <= im2) ? im2 : im1;
			hi = (ip1 <= count - 1) ? ip1 : i;
			n = hi - lo + 1;
			for (k = 0; k < n; k++) w[k] = 0.0f;

			w[im1 - lo] += h00;
			w[i - lo] += h01;

			// 接線は Bessel (Overhauser) 型で取る。
			{
				float c0 = h10 * dt;
				if (0 <= im2) {
					float dtp = fm1 - fm2;
					if (dtp <= 0.0f) dtp = dt;
					float dtn = dt;
					float A = dtn / (dtp * (dtp + dtn));
					float B = dtp / (dtn * (dtp + dtn));
					w[im2 - lo] -= c0 * A;
					w[im1 - lo] += c0 * (A - B);
					w[i - lo] += c0 * B;
				}
				else {
					w[i - lo] += c0 / dt;
					w[im1 - lo] -= c0 / dt;
				}
			}
			{
				float c1 = h11 * dt;
				if (ip1 <= count - 1) {
					float dtp = dt;
					float dtn = fp1 - f0;
					if (dtn <= 0.0f) dtn = dt;
					float A = dtn / (dtp * (dtp + dtn));
					float B = dtp / (dtn * (dtp + dtn));
					w[im1 - lo] -= c1 * A;
					w[i - lo] += c1 * (A - B);
					w[ip1 - lo] += c1 * B;
				}
				else {
					w[i - lo] += c1 / dt;
					w[im1 - lo] -= c1 / dt;
				}
			}
			*outLo = lo;
			return n;
		}

		case YBR_INTERP_SINC:
		default: {
			// キー番号を軸にしたリサンプリング。u は小数のキー番号
			YbrInterpParams q = YbrInterpParamsSanitize(p);
			float u = (float)(i - 1) + s;
			int k0 = i - q.sincA;
			int k1 = i + q.sincA - 1;

			lo = k0 < 0 ? 0 : k0;
			hi = count - 1 < k1 ? count - 1 : k1;
			n = hi - lo + 1;
			for (k = 0; k < n; k++) w[k] = 0.0f;

			{
				float sum = 0.0f;
				for (k = k0; k <= k1; k++) {
					float d = u - (float)k;
					float wk = lanczos(d, q.sincA);
					int kc = k < lo ? lo : (hi < k ? hi : k); // エッジ複製
					w[kc - lo] += wk;
					sum += wk;
				}
				if (fabsf(sum) < 1e-20f) {
					for (k = 0; k < n; k++) w[k] = 0.0f;
					w[(i - 1) - lo] = 1.0f;
				}
				else {
					for (k = 0; k < n; k++) w[k] /= sum;
				}
			}
			*outLo = lo;
			return n;
		}
	}
}

// キー列の評価

// frames[i-1] <= t < frames[i] となる i を返す。
// t が先頭より前なら 0、末尾以降なら count を返す。
static int find_segment(const YbrKeySource* ks, float t)
{
	int lo = 0, hi = ks->count - 1;
	if (t < (float)ks->frameAt(0, ks->ud)) return 0;
	if ((float)ks->frameAt(hi, ks->ud) <= t) return ks->count;

	// frameAt(lo) <= t < frameAt(hi) を保ちながら狭める
	while (1 < hi - lo) {
		int mid = lo + (hi - lo) / 2;
		if ((float)ks->frameAt(mid, ks->ud) <= t)
			lo = mid;
		else
			hi = mid;
	}
	return hi;
}

YbrTransform YbrEvaluateKeySource(const YbrKeySource* ks, float frame,
								  const YbrInterpParams* p)
{
	if (!ks || ks->count <= 0) return YbrTransformIdentity();
	if (ks->count == 1) return ks->valueAt(0, ks->ud);

	int i = find_segment(ks, frame);
	if (i <= 0) return ks->valueAt(0, ks->ud);
	if (ks->count <= i) return ks->valueAt(ks->count - 1, ks->ud);

	int f0 = ks->frameAt(i - 1, ks->ud);
	int f1 = ks->frameAt(i, ks->ud);
	float span = (float)(f1 - f0);
	float s = (0.0f < span) ? (frame - (float)f0) / span : 0.0f;
	if (s < 0.0f) s = 0.0f;
	if (1.0f < s) s = 1.0f;

	YbrInterp mode = ks->interpAt(i, ks->ud);
	if (mode < YBR_INTERP_STEP || YBR_INTERP_COUNT <= mode)
		mode = YBR_INTERP_LINEAR;
	if (mode == YBR_INTERP_HERMITE && !ks->tangentAt) mode = YBR_INTERP_LINEAR;

	if (mode == YBR_INTERP_HERMITE) {
		// 3次エルミート : p(s) = h00 p0 + h10 dt m0 + h01 p1 + h11 dt m1
		// 接線は1フレームあたりの変化量なのでdt（フレーム差）を掛ける。
		YbrTransform p0 = ks->valueAt(i - 1, ks->ud);
		YbrTransform p1 = ks->valueAt(i, ks->ud);
		YbrAnimTangent m0 =
			ks->tangentAt(i - 1, 1, ks->ud);			 // 前のキーの out
		YbrAnimTangent m1 = ks->tangentAt(i, 0, ks->ud); // 今のキーの in

		// クォータニオンの符号をそろえる
		float dot =
			p0.rotation.x * p1.rotation.x + p0.rotation.y * p1.rotation.y +
			p0.rotation.z * p1.rotation.z + p0.rotation.w * p1.rotation.w;
		if (dot < 0.0f) {
			p1.rotation.x = -p1.rotation.x;
			p1.rotation.y = -p1.rotation.y;
			p1.rotation.z = -p1.rotation.z;
			p1.rotation.w = -p1.rotation.w;
			m1.rotation.x = -m1.rotation.x;
			m1.rotation.y = -m1.rotation.y;
			m1.rotation.z = -m1.rotation.z;
			m1.rotation.w = -m1.rotation.w;
		}

		float s2 = s * s, s3 = s2 * s;
		float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
		float h10 = s3 - 2.0f * s2 + s;
		float h01 = -2.0f * s3 + 3.0f * s2;
		float h11 = s3 - s2;
		float dt = span;

		YbrTransform o;
		o.translation.x = h00 * p0.translation.x + h10 * dt * m0.translation.x +
						  h01 * p1.translation.x + h11 * dt * m1.translation.x;
		o.translation.y = h00 * p0.translation.y + h10 * dt * m0.translation.y +
						  h01 * p1.translation.y + h11 * dt * m1.translation.y;
		o.translation.z = h00 * p0.translation.z + h10 * dt * m0.translation.z +
						  h01 * p1.translation.z + h11 * dt * m1.translation.z;
		o.scale.x = h00 * p0.scale.x + h10 * dt * m0.scale.x +
					h01 * p1.scale.x + h11 * dt * m1.scale.x;
		o.scale.y = h00 * p0.scale.y + h10 * dt * m0.scale.y +
					h01 * p1.scale.y + h11 * dt * m1.scale.y;
		o.scale.z = h00 * p0.scale.z + h10 * dt * m0.scale.z +
					h01 * p1.scale.z + h11 * dt * m1.scale.z;

		// 回転は glTF の CUBICSPLINE と同じくクォータニオン成分で補間して正規化
		float qx = h00 * p0.rotation.x + h10 * dt * m0.rotation.x +
				   h01 * p1.rotation.x + h11 * dt * m1.rotation.x;
		float qy = h00 * p0.rotation.y + h10 * dt * m0.rotation.y +
				   h01 * p1.rotation.y + h11 * dt * m1.rotation.y;
		float qz = h00 * p0.rotation.z + h10 * dt * m0.rotation.z +
				   h01 * p1.rotation.z + h11 * dt * m1.rotation.z;
		float qw = h00 * p0.rotation.w + h10 * dt * m0.rotation.w +
				   h01 * p1.rotation.w + h11 * dt * m1.rotation.w;
		float len = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
		if (1e-12f < len) {
			float inv = 1.0f / len;
			qx *= inv;
			qy *= inv;
			qz *= inv;
			qw *= inv;
		}
		else {
			qx = qy = qz = 0.0f;
			qw = 1.0f;
		}
		if (qw < 0.0f) {
			qx = -qx;
			qy = -qy;
			qz = -qz;
			qw = -qw;
		}
		o.rotation.x = qx;
		o.rotation.y = qy;
		o.rotation.z = qz;
		o.rotation.w = qw;
		return o;
	}

	// CUBIC 用に i-2 .. i+1 のフレーム番号を集めておく
	int segFrames[4];
	segFrames[0] = (0 <= i - 2) ? ks->frameAt(i - 2, ks->ud) : f0;
	segFrames[1] = f0;
	segFrames[2] = f1;
	segFrames[3] = (i + 1 <= ks->count - 1) ? ks->frameAt(i + 1, ks->ud) : f1;

	float w[YBR_INTERP_MAX_TAPS];
	int lo = 0;
	int n = build_weights(ks->count, segFrames, mode, i, s, p, &lo, w);
	if (n <= 0) return ks->valueAt(i - 1, ks->ud);
	if (YBR_INTERP_MAX_TAPS < n) n = YBR_INTERP_MAX_TAPS;

	// 値を集める
	YbrTransform v[YBR_INTERP_MAX_TAPS];
	int k;
	for (k = 0; k < n; k++) v[k] = ks->valueAt(lo + k, ks->ud);

	// クォータニオンの符号を区間始点からそろえる
	{
		int r = (i - 1) - lo;
		if (r < 0) r = 0;
		if (n - 1 < r) r = n - 1;
		for (k = r + 1; k < n; k++) {
			float d = v[k - 1].rotation.x * v[k].rotation.x +
					  v[k - 1].rotation.y * v[k].rotation.y +
					  v[k - 1].rotation.z * v[k].rotation.z +
					  v[k - 1].rotation.w * v[k].rotation.w;
			if (d < 0.0f) {
				v[k].rotation.x = -v[k].rotation.x;
				v[k].rotation.y = -v[k].rotation.y;
				v[k].rotation.z = -v[k].rotation.z;
				v[k].rotation.w = -v[k].rotation.w;
			}
		}
		for (k = r - 1; 0 <= k; k--) {
			float d = v[k + 1].rotation.x * v[k].rotation.x +
					  v[k + 1].rotation.y * v[k].rotation.y +
					  v[k + 1].rotation.z * v[k].rotation.z +
					  v[k + 1].rotation.w * v[k].rotation.w;
			if (d < 0.0f) {
				v[k].rotation.x = -v[k].rotation.x;
				v[k].rotation.y = -v[k].rotation.y;
				v[k].rotation.z = -v[k].rotation.z;
				v[k].rotation.w = -v[k].rotation.w;
			}
		}
	}

	// 重み付き和
	YbrTransform o;
	o.translation.x = o.translation.y = o.translation.z = 0.0f;
	o.scale.x = o.scale.y = o.scale.z = 0.0f;
	float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 0.0f;
	for (k = 0; k < n; k++) {
		float a = w[k];
		o.translation.x += v[k].translation.x * a;
		o.translation.y += v[k].translation.y * a;
		o.translation.z += v[k].translation.z * a;
		o.scale.x += v[k].scale.x * a;
		o.scale.y += v[k].scale.y * a;
		o.scale.z += v[k].scale.z * a;
		qx += v[k].rotation.x * a;
		qy += v[k].rotation.y * a;
		qz += v[k].rotation.z * a;
		qw += v[k].rotation.w * a;
	}
	{
		float len = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
		if (1e-8f < len) {
			qx /= len;
			qy /= len;
			qz /= len;
			qw /= len;
		}
		else {
			YbrTransform ref = ks->valueAt(i - 1, ks->ud);
			qx = ref.rotation.x;
			qy = ref.rotation.y;
			qz = ref.rotation.z;
			qw = ref.rotation.w;
		}
		o.rotation.x = qx;
		o.rotation.y = qy;
		o.rotation.z = qz;
		o.rotation.w = qw;
	}
	return o;
}

// 接線ユーティリティ

YbrAnimTangent YbrAnimTangentZero(void)
{
	YbrAnimTangent t;
	memset(&t, 0, sizeof(t));
	return t;
}

YbrAnimTangent YbrAnimTangentFromDelta(YbrTransform a, YbrTransform b,
									   float dtFrames)
{
	YbrAnimTangent t = YbrAnimTangentZero();
	if (!(0.0f < dtFrames)) return t;
	float inv = 1.0f / dtFrames;

	// 回転はクォータニオン成分の差。符号をそろえてから引く
	float dot = a.rotation.x * b.rotation.x + a.rotation.y * b.rotation.y +
				a.rotation.z * b.rotation.z + a.rotation.w * b.rotation.w;
	if (dot < 0.0f) {
		b.rotation.x = -b.rotation.x;
		b.rotation.y = -b.rotation.y;
		b.rotation.z = -b.rotation.z;
		b.rotation.w = -b.rotation.w;
	}

	t.translation.x = (b.translation.x - a.translation.x) * inv;
	t.translation.y = (b.translation.y - a.translation.y) * inv;
	t.translation.z = (b.translation.z - a.translation.z) * inv;
	t.rotation.x = (b.rotation.x - a.rotation.x) * inv;
	t.rotation.y = (b.rotation.y - a.rotation.y) * inv;
	t.rotation.z = (b.rotation.z - a.rotation.z) * inv;
	t.rotation.w = (b.rotation.w - a.rotation.w) * inv;
	t.scale.x = (b.scale.x - a.scale.x) * inv;
	t.scale.y = (b.scale.y - a.scale.y) * inv;
	t.scale.z = (b.scale.z - a.scale.z) * inv;
	return t;
}

YbrAnimTangent YbrAnimTangentFromBezier(YbrTransform key, YbrTransform handle,
										float dtFrames)
{
	// ベジェの制御点 c は p + m*dt/3 の位置にあるので、m = 3*(c - p)/dt
	YbrAnimTangent t = YbrAnimTangentFromDelta(key, handle, dtFrames);
	t.translation.x *= 3.0f;
	t.translation.y *= 3.0f;
	t.translation.z *= 3.0f;
	t.rotation.x *= 3.0f;
	t.rotation.y *= 3.0f;
	t.rotation.z *= 3.0f;
	t.rotation.w *= 3.0f;
	t.scale.x *= 3.0f;
	t.scale.y *= 3.0f;
	t.scale.z *= 3.0f;
	return t;
}

// サンプラ

typedef struct SamplerUD {
	const int* frames;
	const unsigned char* interps;
	const YbrTransform* values;
	const YbrAnimTangent* tangents;
} SamplerUD;

static int smp_frame(int i, const void* ud)
{
	return ((const SamplerUD*)ud)->frames[i];
}
static YbrInterp smp_interp(int i, const void* ud)
{
	return (YbrInterp)((const SamplerUD*)ud)->interps[i];
}
static YbrTransform smp_value(int i, const void* ud)
{
	return ((const SamplerUD*)ud)->values[i];
}
static YbrAnimTangent smp_tangent(int i, int out, const void* ud)
{
	return ((const SamplerUD*)ud)->tangents[i * 2 + (out ? 1 : 0)];
}

int YbrAnimSamplerInitFromAnimation(YbrAnimSampler* s, const YbrAnimation* anim,
									const YbrAnimTrack* track)
{
	YbrInterpParams p = YbrInterpParamsFromAnimation(anim);
	return YbrAnimSamplerInit(s, track, &p);
}

int YbrAnimSamplerInit(YbrAnimSampler* s, const YbrAnimTrack* track,
					   const YbrInterpParams* params)
{
	if (!s) return 0;
	memset(s, 0, sizeof(*s));
	s->params = YbrInterpParamsSanitize(params);
	if (!track || track->frameCount <= 0 || !track->frames) return 1;

	int n = track->frameCount;
	s->frames = (int*)YBR_MALLOC((size_t)n * sizeof(int));
	s->interps = (unsigned char*)YBR_MALLOC((size_t)n);
	s->values = (YbrTransform*)YBR_MALLOC((size_t)n * sizeof(YbrTransform));
	if (!s->frames || !s->interps || !s->values) {
		YbrAnimSamplerUnload(s);
		return 0;
	}
	if (track->tangents) {
		s->tangents =
			(YbrAnimTangent*)YBR_MALLOC((size_t)n * 2 * sizeof(YbrAnimTangent));
		if (!s->tangents) {
			YbrAnimSamplerUnload(s);
			return 0;
		}
		memcpy(s->tangents, track->tangents,
			   (size_t)n * 2 * sizeof(YbrAnimTangent));
	}
	for (int i = 0; i < n; i++) {
		s->frames[i] = track->frames[i].frame;
		s->interps[i] = (unsigned char)track->frames[i].interp;
		s->values[i] = YbrTransformFromMatrix(track->frames[i].transform);
	}
	s->frameCount = n;
	return 1;
}

void YbrAnimSamplerUnload(YbrAnimSampler* s)
{
	if (!s) return;
	YBR_FREE(s->frames);
	YBR_FREE(s->interps);
	YBR_FREE(s->values);
	YBR_FREE(s->tangents);
	memset(s, 0, sizeof(*s));
}

YbrTransform YbrAnimSamplerEvaluate(const YbrAnimSampler* s, float frame)
{
	if (!s || s->frameCount <= 0) return YbrTransformIdentity();
	SamplerUD ud;
	ud.frames = s->frames;
	ud.interps = s->interps;
	ud.values = s->values;
	ud.tangents = s->tangents;
	YbrKeySource ks;
	ks.count = s->frameCount;
	ks.frameAt = smp_frame;
	ks.interpAt = smp_interp;
	ks.valueAt = smp_value;
	ks.tangentAt = s->tangents ? smp_tangent : NULL;
	ks.ud = &ud;
	return YbrEvaluateKeySource(&ks, frame, &s->params);
}

Matrix YbrAnimSamplerMatrix(const YbrAnimSampler* s, float frame)
{
	return YbrTransformToMatrix(YbrAnimSamplerEvaluate(s, frame));
}

// 前処理なし版

static int trk_frame(int i, const void* ud)
{
	return ((const YbrAnimTrack*)ud)->frames[i].frame;
}
static YbrInterp trk_interp(int i, const void* ud)
{
	return ((const YbrAnimTrack*)ud)->frames[i].interp;
}
static YbrTransform trk_value(int i, const void* ud)
{
	return YbrTransformFromMatrix(
		((const YbrAnimTrack*)ud)->frames[i].transform);
}
static YbrAnimTangent trk_tangent(int i, int out, const void* ud)
{
	return ((const YbrAnimTrack*)ud)->tangents[i * 2 + (out ? 1 : 0)];
}

YbrTransform YbrAnimTrackEvaluate(const YbrAnimTrack* tr, float frame,
								  const YbrInterpParams* params)
{
	if (!tr || tr->frameCount <= 0 || !tr->frames)
		return YbrTransformIdentity();
	YbrInterpParams p = YbrInterpParamsSanitize(params);
	YbrKeySource ks;
	ks.count = tr->frameCount;
	ks.frameAt = trk_frame;
	ks.interpAt = trk_interp;
	ks.valueAt = trk_value;
	ks.tangentAt = tr->tangents ? trk_tangent : NULL;
	ks.ud = tr;
	return YbrEvaluateKeySource(&ks, frame, &p);
}

Matrix YbrAnimTrackMatrix(const YbrAnimTrack* tr, float frame,
						  const YbrInterpParams* params)
{
	return YbrTransformToMatrix(YbrAnimTrackEvaluate(tr, frame, params));
}

// ポーズマーカー (アニメーションイベント)

// (from, to] に入るマーカーを集める内部ヘルパー
static int markers_in(const YbrAnimation* a, float from, float to,
					  const YbrAnimMarker** out, int max, int found)
{
	for (int i = 0; i < a->markerCount; i++) {
		float f = (float)a->markers[i].frame;
		if (f <= from || to < f) continue;
		if (out && found < max) out[found] = &a->markers[i];
		found++;
	}
	return found;
}

int YbrAnimMarkersInRange(const YbrAnimation* anim, float fromFrame,
						  float toFrame, const YbrAnimMarker** out, int max)
{
	if (!anim || anim->markerCount <= 0) return 0;
	if (fromFrame == toFrame) return 0;

	if (fromFrame < toFrame)
		return markers_in(anim, fromFrame, toFrame, out, max, 0);

	// ループで巻き戻った : (from, 最後] と [先頭, to] の 2 区間
	float last = (float)anim->frameCount;
	int n = markers_in(anim, fromFrame, last, out, max, 0);
	return markers_in(anim, -1.0f, toFrame, out, max, n);
}
