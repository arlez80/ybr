/*
	Yui Blender to Raylib - アニメーションの最適化
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_anim_opt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// C11 スレッド
#if defined(YBR_NO_THREADS)
#define YBR_USE_C11_THREADS 0
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && \
	!defined(__STDC_NO_THREADS__) && __has_include(<threads.h>)
#include <threads.h>
#define YBR_USE_C11_THREADS 1
#else
#define YBR_USE_C11_THREADS 0
#endif

#if YBR_USE_C11_THREADS
#include <stdio.h>
#endif

#ifndef YBR_PI
#define YBR_PI 3.14159265358979323846f
#endif

// オプション

YbrAnimOptOptions YbrAnimOptDefaults(void)
{
	YbrAnimOptOptions o;
	o.posEps = 0.0005f;
	o.rotEps = 0.05f;
	o.scaleEps = 0.001f;
	o.interpMask = YBR_INTERP_ALL;
	o.subsample = 2;
	o.maxRounds = 4;
	o.interp = YbrInterpParamsDefault();
	o.threads = YBR_ANIM_OPT_THREADS_AUTO;
	return o;
}

static YbrAnimOptOptions sanitize(const YbrAnimOptOptions* in)
{
	YbrAnimOptOptions o = in ? *in : YbrAnimOptDefaults();
	if (!(0.0f < o.posEps)) o.posEps = 1e-9f;
	if (!(0.0f < o.rotEps)) o.rotEps = 1e-9f;
	if (!(0.0f < o.scaleEps)) o.scaleEps = 1e-9f;
	if (o.interpMask == 0u) o.interpMask = YBR_INTERP_BIT(YBR_INTERP_LINEAR);
	if (o.subsample < 1) o.subsample = 1;
	if (16 < o.subsample) o.subsample = 16;
	if (o.maxRounds < 1) o.maxRounds = 1;
	if (8 < o.maxRounds) o.maxRounds = 8;
	if (o.threads < 0) o.threads = 0;
	if (YBR_ANIM_OPT_MAX_THREADS < o.threads)
		o.threads = YBR_ANIM_OPT_MAX_THREADS;
	o.interp = YbrInterpParamsSanitize(&o.interp);
	return o;
}

static void stats_merge(YbrAnimOptStats* dst, const YbrAnimOptStats* src);

// 誤差が並んだときの好み。実行時に安く、素直な順
static int pref_order(YbrInterp m)
{
	switch (m) {
		case YBR_INTERP_LINEAR:
			return 0;
		case YBR_INTERP_HERMITE:
			return 1; /* 区間の両端しか見ないので扱いやすい */
		case YBR_INTERP_CUBIC:
			return 2;
		case YBR_INTERP_SINC:
			return 3;
		case YBR_INTERP_STEP:
			return 4;
		default:
			return 5;
	}
}

// 候補 (err=e, mode=m) が現在の最良 (bestErr, best) より良いか。
// 誤差がほぼ同じなら pref_order の小さいほうを採る。
static int better(float e, int m, float bestErr, int best)
{
	if (best < 0) return 1;
	if (e < bestErr - 1e-6f) return 1;
	if (e <= bestErr + 1e-6f &&
		pref_order((YbrInterp)m) < pref_order((YbrInterp)best))
		return 1;
	return 0;
}

// 作業用のキー列 (skip で「1 本抜いた状態」を表現する)

typedef struct Keys {
	int m;
	int* frame;
	unsigned char* ip;
	YbrTransform* val;
	YbrAnimTangent* tan; /* HERMITE 用 : 2 * m 個 / 使わないなら NULL */
	int* src;			 /* 元の frames[] のインデックス */
	int skip;			 /* -1 なら間引き無し */
} Keys;

static int keys_map(const Keys* K, int v)
{
	return (0 <= K->skip && K->skip <= v) ? v + 1 : v;
}

static int kv_frame(int i, const void* ud)
{
	const Keys* K = (const Keys*)ud;
	return K->frame[keys_map(K, i)];
}
static YbrInterp kv_interp(int i, const void* ud)
{
	const Keys* K = (const Keys*)ud;
	return (YbrInterp)K->ip[keys_map(K, i)];
}
static YbrTransform kv_value(int i, const void* ud)
{
	const Keys* K = (const Keys*)ud;
	return K->val[keys_map(K, i)];
}
static YbrAnimTangent kv_tangent(int i, int out, const void* ud)
{
	const Keys* K = (const Keys*)ud;
	return K->tan[keys_map(K, i) * 2 + (out ? 1 : 0)];
}

static YbrKeySource keys_source(const Keys* K)
{
	YbrKeySource ks;
	ks.count = K->m - (0 <= K->skip ? 1 : 0);
	ks.frameAt = kv_frame;
	ks.interpAt = kv_interp;
	ks.valueAt = kv_value;
	ks.tangentAt = K->tan ? kv_tangent : NULL;
	ks.ud = K;
	return ks;
}

// 参照 (元のベイク結果)

typedef struct Ref {
	int f0, f1;
	int sub;		   /* 1 フレームあたりの標本数 */
	YbrTransform* val; /* (f1 - f0) * sub + 1 個 / index = (frame - f0) * sub */
} Ref;

// ベイクは整数フレームでしか定義されていないので、サブフレームの「正解」は
static YbrTransform ref_cr(const YbrTransform* val, int n, int i, float s)
{
	if (n <= 1) return val[0];
	if (i < 0) return val[0];
	if (n - 1 <= i) return val[n - 1];
	if (s <= 1e-6f) return val[i];

	float s2 = s * s, s3 = s2 * s;
	float w[4];
	w[0] = -0.5f * s3 + s2 - 0.5f * s;
	w[1] = 1.5f * s3 - 2.5f * s2 + 1.0f;
	w[2] = -1.5f * s3 + 2.0f * s2 + 0.5f * s;
	w[3] = 0.5f * s3 - 0.5f * s2;

	// 端は「複製」ではなく「線形外挿」で埋める。
	int idx[4];
	idx[0] = i - 1;
	idx[1] = i;
	idx[2] = i + 1;
	idx[3] = i + 2;
	if (idx[0] < 0) {
		w[1] += 2.0f * w[0];
		w[2] -= w[0];
		w[0] = 0.0f;
		idx[0] = i;
	}
	if (n - 1 < idx[3]) {
		w[2] += 2.0f * w[3];
		w[1] -= w[3];
		w[3] = 0.0f;
		idx[3] = i;
	}
	if (n - 1 < idx[2]) idx[2] = n - 1;

	YbrTransform v[4];
	for (int k = 0; k < 4; k++) v[k] = val[idx[k]];
	// クォータニオンの符号を v[1] を基準にそろえる
	for (int k = 2; k < 4; k++) {
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
	{
		float d = v[1].rotation.x * v[0].rotation.x +
				  v[1].rotation.y * v[0].rotation.y +
				  v[1].rotation.z * v[0].rotation.z +
				  v[1].rotation.w * v[0].rotation.w;
		if (d < 0.0f) {
			v[0].rotation.x = -v[0].rotation.x;
			v[0].rotation.y = -v[0].rotation.y;
			v[0].rotation.z = -v[0].rotation.z;
			v[0].rotation.w = -v[0].rotation.w;
		}
	}

	YbrTransform o;
	o.translation.x = o.translation.y = o.translation.z = 0.0f;
	o.scale.x = o.scale.y = o.scale.z = 0.0f;
	float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 0.0f;
	for (int k = 0; k < 4; k++) {
		o.translation.x += v[k].translation.x * w[k];
		o.translation.y += v[k].translation.y * w[k];
		o.translation.z += v[k].translation.z * w[k];
		o.scale.x += v[k].scale.x * w[k];
		o.scale.y += v[k].scale.y * w[k];
		o.scale.z += v[k].scale.z * w[k];
		qx += v[k].rotation.x * w[k];
		qy += v[k].rotation.y * w[k];
		qz += v[k].rotation.z * w[k];
		qw += v[k].rotation.w * w[k];
	}
	float len = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
	if (1e-8f < len) {
		qx /= len;
		qy /= len;
		qz /= len;
		qw /= len;
	}
	else {
		YbrTransform r = val[i];
		qx = r.rotation.x;
		qy = r.rotation.y;
		qz = r.rotation.z;
		qw = r.rotation.w;
	}
	o.rotation.x = qx;
	o.rotation.y = qy;
	o.rotation.z = qz;
	o.rotation.w = qw;
	return o;
}

// 誤差

static float norm_err(const YbrAnimOptOptions* o, YbrTransform a,
					  YbrTransform b, float* pe, float* re, float* se)
{
	float dx = a.translation.x - b.translation.x;
	float dy = a.translation.y - b.translation.y;
	float dz = a.translation.z - b.translation.z;
	float p = sqrtf(dx * dx + dy * dy + dz * dz);

	// 2*acos(dot) は角度が小さいところで float の精度が足りず、
	float bx = b.rotation.x, by = b.rotation.y, bz = b.rotation.z,
		  bw = b.rotation.w;
	float d = a.rotation.x * bx + a.rotation.y * by + a.rotation.z * bz +
			  a.rotation.w * bw;
	if (d < 0.0f) {
		bx = -bx;
		by = -by;
		bz = -bz;
		bw = -bw;
	}
	float sx = a.rotation.x - bx, sy = a.rotation.y - by;
	float sz = a.rotation.z - bz, sw = a.rotation.w - bw;
	float px = a.rotation.x + bx, py = a.rotation.y + by;
	float pz = a.rotation.z + bz, pw = a.rotation.w + bw;
	float sub = sqrtf(sx * sx + sy * sy + sz * sz + sw * sw);
	float add = sqrtf(px * px + py * py + pz * pz + pw * pw);
	float r = 2.0f * atan2f(sub, add) * (180.0f / YBR_PI);

	float s = 0.0f;
	{
		float as[3], bs[3];
		as[0] = a.scale.x;
		as[1] = a.scale.y;
		as[2] = a.scale.z;
		bs[0] = b.scale.x;
		bs[1] = b.scale.y;
		bs[2] = b.scale.z;
		for (int k = 0; k < 3; k++) {
			float diff = fabsf(as[k] - bs[k]);
			float base = fabsf(bs[k]);
			float rel = (1e-6f < base) ? diff / base : diff;
			if (s < rel) s = rel;
		}
	}

	if (pe) *pe = p;
	if (re) *re = r;
	if (se) *se = s;

	float e = p / o->posEps;
	float t = r / o->rotEps;
	if (e < t) e = t;
	t = s / o->scaleEps;
	if (e < t) e = t;
	return e;
}

// キー位置 loP..hiP が張るフレーム範囲で、現在の再構成と参照の最大誤差。
// mp / mr / ms は非 NULL なら成分ごとの最大値を「更新」する。
static float measure(const Keys* K, const Ref* R, const YbrAnimOptOptions* o,
					 int loP, int hiP, float* mp, float* mr, float* ms)
{
	int fa = K->frame[loP], fb = K->frame[hiP];
	if (fb < fa) {
		int t = fa;
		fa = fb;
		fb = t;
	}

	YbrKeySource ks = keys_source(K);
	long steps = (long)(fb - fa) * (long)R->sub;
	long base = (long)(fa - R->f0) * (long)R->sub;
	float worst = 0.0f;

	for (long j = 0; j <= steps; j++) {
		float t = (float)fa + (float)j / (float)R->sub;
		YbrTransform a = YbrEvaluateKeySource(&ks, t, &o->interp);
		YbrTransform b = R->val[base + j];
		float pe, re, se;
		float e = norm_err(o, a, b, &pe, &re, &se);
		if (worst < e) worst = e;
		if (mp && *mp < pe) *mp = pe;
		if (mr && *mr < re) *mr = re;
		if (ms && *ms < se) *ms = se;
	}
	return worst;
}

// 補間方法の選び直し

// 区間 q (キー q-1 -> q) の補間方法を選び直す。
// 区間の中身にしか影響しないので、見るのは [q-1, q] だけでよい。
static int refit_segment(Keys* K, const Ref* R, const YbrAnimOptOptions* o,
						 int q)
{
	int best = -1;
	float bestErr = 0.0f;
	unsigned char save = K->ip[q];

	for (int m = 0; m < YBR_INTERP_COUNT; m++) {
		if (!(o->interpMask & YBR_INTERP_BIT(m))) continue;
		K->ip[q] = (unsigned char)m;
		float e = measure(K, R, o, q - 1, q, NULL, NULL, NULL);
		if (better(e, m, bestErr, best)) {
			bestErr = e;
			best = m;
		}
	}
	K->ip[q] = (0 <= best) ? (unsigned char)best : save;
	return K->ip[q] != save;
}

static int refit_all(Keys* K, const Ref* R, const YbrAnimOptOptions* o)
{
	int changed = 0;
	for (int q = 1; q < K->m; q++) changed += refit_segment(K, R, o, q);
	return changed;
}

// 間引き

// キー pos を抜いたときの (最良の補間方法での) 誤差を求める。
// 抜いてよければ *accept に 1。
static void eval_removal(Keys* K, const Ref* R, const YbrAnimOptOptions* o,
						 int pos, int radius, float* outErr, int* outMode,
						 int* accept)
{
	int loP = pos - radius;
	if (loP < 0) loP = 0;
	int hiP = pos + radius;
	if (K->m - 1 < hiP) hiP = K->m - 1;

	K->skip = -1;
	float errBefore = measure(K, R, o, loP, hiP, NULL, NULL, NULL);

	unsigned char save = K->ip[pos + 1];
	int best = -1;
	float bestErr = 0.0f;

	K->skip = pos;
	for (int m = 0; m < YBR_INTERP_COUNT; m++) {
		if (!(o->interpMask & YBR_INTERP_BIT(m))) continue;
		K->ip[pos + 1] = (unsigned char)m;
		float e = measure(K, R, o, loP, hiP, NULL, NULL, NULL);
		if (better(e, m, bestErr, best)) {
			bestErr = e;
			best = m;
		}
	}
	K->ip[pos + 1] = save;
	K->skip = -1;

	*outErr = bestErr;
	*outMode = best;
	// 許容誤差に収まるときだけ抜く。
	*accept = (0 <= best) && (bestErr <= 1.0f);
	(void)errBefore;
}

static int decimate(Keys* K, const Ref* R, const YbrAnimOptOptions* o,
					int radius, float* err, int* mode, unsigned char* acc)
{
	int removed = 0;
	if (K->m < 3) return 0;

	for (int pos = 1; pos <= K->m - 2; pos++) {
		int a, md;
		float e;
		eval_removal(K, R, o, pos, radius, &e, &md, &a);
		err[pos] = e;
		mode[pos] = md;
		acc[pos] = (unsigned char)a;
	}

	for (;;) {
		int bestPos = -1;
		float bestErr = 0.0f;
		for (int pos = 1; pos <= K->m - 2; pos++) {
			if (!acc[pos]) continue;
			if (bestPos < 0 || err[pos] < bestErr) {
				bestPos = pos;
				bestErr = err[pos];
			}
		}
		if (bestPos < 0) break;

		// つながる区間の補間方法を確定してから抜く。
		// 抜くと旧 bestPos+1 が新 bestPos になる。
		if (0 <= mode[bestPos])
			K->ip[bestPos + 1] = (unsigned char)mode[bestPos];

		int tail = K->m - bestPos - 1;
		if (0 < tail) {
			memmove(K->frame + bestPos, K->frame + bestPos + 1,
					(size_t)tail * sizeof(int));
			memmove(K->ip + bestPos, K->ip + bestPos + 1, (size_t)tail);
			memmove(K->val + bestPos, K->val + bestPos + 1,
					(size_t)tail * sizeof(YbrTransform));
			if (K->tan)
				memmove(K->tan + bestPos * 2, K->tan + (bestPos + 1) * 2,
						(size_t)tail * 2 * sizeof(YbrAnimTangent));
			memmove(K->src + bestPos, K->src + bestPos + 1,
					(size_t)tail * sizeof(int));
			memmove(err + bestPos, err + bestPos + 1,
					(size_t)tail * sizeof(float));
			memmove(mode + bestPos, mode + bestPos + 1,
					(size_t)tail * sizeof(int));
			memmove(acc + bestPos, acc + bestPos + 1, (size_t)tail);
		}
		K->m--;
		removed++;
		if (K->m < 3) break;

		// 再構成が変わりうる範囲だけ計算し直す
		int w = 2 * radius + 1;
		int lo = bestPos - w;
		if (lo < 1) lo = 1;
		int hi = bestPos + w;
		if (K->m - 2 < hi) hi = K->m - 2;
		for (int pos = lo; pos <= hi; pos++) {
			int a, md;
			float e;
			eval_removal(K, R, o, pos, radius, &e, &md, &a);
			err[pos] = e;
			mode[pos] = md;
			acc[pos] = (unsigned char)a;
		}
	}
	return removed;
}

// トラック 1 本

// HERMITE 用の接線を、元のベイク結果 (参照テーブル) の微分から作る。
// in / out は同じ値にする (キーでなめらかにつながる)。
static void build_tangents(Keys* K, const Ref* R)
{
	for (int i = 0; i < K->m; i++) {
		int f = K->frame[i];
		int fa = f - 1, fb = f + 1;
		if (fa < R->f0) fa = f;
		if (R->f1 < fb) fb = f;
		float dt = (float)(fb - fa);
		YbrAnimTangent t;
		if (0.0f < dt) {
			YbrTransform a = R->val[(size_t)(fa - R->f0) * (size_t)R->sub];
			YbrTransform b = R->val[(size_t)(fb - R->f0) * (size_t)R->sub];
			t = YbrAnimTangentFromDelta(a, b, dt);
		}
		else {
			t = YbrAnimTangentZero();
		}
		K->tan[i * 2 + 0] = t;
		K->tan[i * 2 + 1] = t;
	}
}

size_t YbrAnimTrackBytes(const YbrAnimTrack* tr)
{
	if (!tr || tr->frameCount <= 0) return 0;
	size_t bytes = (size_t)tr->frameCount * YBR_ANIM_KEY_BYTES;
	if (tr->tangents) {
		for (int i = 0; i < tr->frameCount; i++) {
			if (tr->frames[i].interp == YBR_INTERP_HERMITE) {
				size_t per = YbrAnimTrackTangentsSymmetric(tr)
								 ? YBR_ANIM_TANGENT_BYTES
								 : YBR_ANIM_TANGENT_BYTES_MAX;
				bytes += (size_t)tr->frameCount * per;
				break;
			}
		}
	}
	return bytes;
}

static int optimize_pass(YbrAnimTrack* tr, const YbrAnimOptOptions* opts,
						 YbrAnimOptStats* st)
{
	if (!tr) return 1;
	YbrAnimOptOptions o = sanitize(opts);
	int useHermite = (o.interpMask & YBR_INTERP_BIT(YBR_INTERP_HERMITE)) != 0;

	int n = tr->frameCount;
	if (st) {
		st->trackCount++;
		st->keysBefore += (0 < n ? n : 0);
	}
	if (n <= 0 || !tr->frames) return 1;

	if (n == 1) {
		tr->frames[0].interp = YBR_INTERP_STEP;
		if (st) st->keysAfter += 1;
		return 1;
	}

	// 作業領域
	Keys K;
	K.m = n;
	K.skip = -1;
	K.frame = (int*)YBR_MALLOC((size_t)n * sizeof(int));
	K.ip = (unsigned char*)YBR_MALLOC((size_t)n);
	K.val = (YbrTransform*)YBR_MALLOC((size_t)n * sizeof(YbrTransform));
	K.tan = useHermite ? (YbrAnimTangent*)YBR_MALLOC((size_t)n * 2 *
													 sizeof(YbrAnimTangent))
					   : NULL;
	K.src = (int*)YBR_MALLOC((size_t)n * sizeof(int));

	int f0 = tr->frames[0].frame, f1 = tr->frames[n - 1].frame;
	if (f1 < f0) f1 = f0;
	int spanCount = f1 - f0 + 1;

	Ref R;
	R.f0 = f0;
	R.f1 = f1;
	R.sub = o.subsample;
	size_t refCount = (size_t)(f1 - f0) * (size_t)R.sub + 1;
	R.val = (YbrTransform*)YBR_MALLOC(refCount * sizeof(YbrTransform));
	YbrTransform* refInt =
		(YbrTransform*)YBR_MALLOC((size_t)spanCount * sizeof(YbrTransform));

	float* err = (float*)YBR_MALLOC((size_t)n * sizeof(float));
	int* mode = (int*)YBR_MALLOC((size_t)n * sizeof(int));
	unsigned char* acc = (unsigned char*)YBR_MALLOC((size_t)n);

	if (!K.frame || !K.ip || !K.val || !K.src || !R.val || !refInt || !err ||
		!mode || !acc || (useHermite && !K.tan)) {
		YBR_FREE(K.frame);
		YBR_FREE(K.ip);
		YBR_FREE(K.val);
		YBR_FREE(K.tan);
		YBR_FREE(K.src);
		YBR_FREE(R.val);
		YBR_FREE(refInt);
		YBR_FREE(err);
		YBR_FREE(mode);
		YBR_FREE(acc);
		return 0;
	}

	for (int i = 0; i < n; i++) {
		K.frame[i] = tr->frames[i].frame;
		K.ip[i] = (unsigned char)tr->frames[i].interp;
		K.val[i] = YbrTransformFromMatrix(tr->frames[i].transform);
		K.src[i] = i;
	}

	// 参照を作る
	// 元の補間方法のまま整数フレームで展開しサブフレームは Catmull-Rom
	// で埋めてテーブル化する
	{
		YbrKeySource ks = keys_source(&K);
		for (int f = f0; f <= f1; f++)
			refInt[f - f0] = YbrEvaluateKeySource(&ks, (float)f, &o.interp);

		for (size_t idx = 0; idx < refCount; idx++) {
			int i = (int)(idx / (size_t)R.sub);
			int r = (int)(idx % (size_t)R.sub);
			float sfrac = (float)r / (float)R.sub;
			R.val[idx] =
				(r == 0) ? refInt[i] : ref_cr(refInt, spanCount, i, sfrac);
		}
		YBR_FREE(refInt);
		refInt = NULL;
	}

	// HERMITE用の接線
	if (K.tan) build_tangents(&K, &R);

	// カーネルが前後何キーまで参照するか
	int radius = 1;
	for (int m = 0; m < YBR_INTERP_COUNT; m++) {
		if (!(o.interpMask & YBR_INTERP_BIT(m))) continue;
		int r = YbrInterpRadius((YbrInterp)m, &o.interp) + 1;
		if (radius < r) radius = r;
	}

	// 選び直しと間引き
	for (int round = 0; round < o.maxRounds; round++) {
		refit_all(&K, &R, &o);
		if (decimate(&K, &R, &o, radius, err, mode, acc) == 0) break;
	}
	refit_all(&K, &R, &o);

	// 全体が動いていないなら1キーまで落とす
	if (K.m == 2) {
		K.skip = 1;	 // 末尾を無視 = キー 1 本だけの状態
		float e = measure(&K, &R, &o, 0, 1, NULL, NULL, NULL);
		K.skip = -1;
		if (e <= 1.0f) K.m = 1;
	}

	// 実際に出た誤差を測る
	if (st && 2 <= K.m) {
		float mp = 0.0f, mr = 0.0f, ms = 0.0f;
		measure(&K, &R, &o, 0, K.m - 1, &mp, &mr, &ms);
		if (st->maxPosErr < mp) st->maxPosErr = mp;
		if (st->maxRotErr < mr) st->maxRotErr = mr;
		if (st->maxScaleErr < ms) st->maxScaleErr = ms;
	}

	// 書き戻し
	// 行列そのものは元の値のまま
	{
		YbrAnimFrame* nf =
			(YbrAnimFrame*)YBR_MALLOC((size_t)K.m * sizeof(YbrAnimFrame));
		if (!nf) {
			YBR_FREE(K.frame);
			YBR_FREE(K.ip);
			YBR_FREE(K.val);
			YBR_FREE(K.tan);
			YBR_FREE(K.src);
			YBR_FREE(R.val);
			YBR_FREE(refInt);
			YBR_FREE(err);
			YBR_FREE(mode);
			YBR_FREE(acc);
			return 0;
		}
		int usesHermite = 0;
		for (int j = 0; j < K.m; j++) {
			const YbrAnimFrame* o0 = &tr->frames[K.src[j]];
			nf[j].frame = K.frame[j];
			nf[j].type = o0->type;
			nf[j].interp = (j == 0) ? YBR_INTERP_STEP : (YbrInterp)K.ip[j];
			nf[j].transform = o0->transform;
			if (nf[j].interp == YBR_INTERP_HERMITE) usesHermite = 1;
			if (st && 0 < j) st->interpUse[K.ip[j]]++;
		}
		YBR_FREE(tr->frames);
		tr->frames = nf;
		tr->frameCount = K.m;

		YBR_FREE(tr->tangents);
		tr->tangents = NULL;
		if (usesHermite && K.tan) {
			tr->tangents = (YbrAnimTangent*)YBR_MALLOC((size_t)K.m * 2 *
													   sizeof(YbrAnimTangent));
			if (tr->tangents)
				memcpy(tr->tangents, K.tan,
					   (size_t)K.m * 2 * sizeof(YbrAnimTangent));
		}
		if (st) st->keysAfter += K.m;
	}

	YBR_FREE(K.frame);
	YBR_FREE(K.ip);
	YBR_FREE(K.val);
	YBR_FREE(K.tan);
	YBR_FREE(K.src);
	YBR_FREE(R.val);
	YBR_FREE(refInt);
	YBR_FREE(err);
	YBR_FREE(mode);
	YBR_FREE(acc);
	return 1;
}

// HERMITE 有り / 無しを試して、小さいほうを採る

static void stats_merge(YbrAnimOptStats* dst, const YbrAnimOptStats* src)
{
	if (!dst) return;
	dst->trackCount += src->trackCount;
	dst->keysBefore += src->keysBefore;
	dst->keysAfter += src->keysAfter;
	for (int i = 0; i < YBR_INTERP_COUNT; i++)
		dst->interpUse[i] += src->interpUse[i];
	if (dst->maxPosErr < src->maxPosErr) dst->maxPosErr = src->maxPosErr;
	if (dst->maxRotErr < src->maxRotErr) dst->maxRotErr = src->maxRotErr;
	if (dst->maxScaleErr < src->maxScaleErr)
		dst->maxScaleErr = src->maxScaleErr;
}

// frames / tangents を含めてトラックを複製する
static int track_clone(YbrAnimTrack* dst, const YbrAnimTrack* src)
{
	*dst = *src;
	dst->frames = NULL;
	dst->tangents = NULL;
	if (0 < src->frameCount && src->frames) {
		dst->frames = (YbrAnimFrame*)YBR_MALLOC((size_t)src->frameCount *
												sizeof(YbrAnimFrame));
		if (!dst->frames) return 0;
		memcpy(dst->frames, src->frames,
			   (size_t)src->frameCount * sizeof(YbrAnimFrame));
	}
	if (src->tangents && 0 < src->frameCount) {
		dst->tangents = (YbrAnimTangent*)YBR_MALLOC((size_t)src->frameCount *
													2 * sizeof(YbrAnimTangent));
		if (!dst->tangents) {
			YBR_FREE(dst->frames);
			dst->frames = NULL;
			return 0;
		}
		memcpy(dst->tangents, src->tangents,
			   (size_t)src->frameCount * 2 * sizeof(YbrAnimTangent));
	}
	return 1;
}

int YbrOptimizeAnimTrack(YbrAnimTrack* tr, const YbrAnimOptOptions* opts,
						 YbrAnimOptStats* st)
{
	if (!tr) return 1;
	YbrAnimOptOptions o = sanitize(opts);

	int hermiteAllowed =
		(o.interpMask & YBR_INTERP_BIT(YBR_INTERP_HERMITE)) != 0;
	int otherAllowed =
		(o.interpMask & ~YBR_INTERP_BIT(YBR_INTERP_HERMITE)) != 0;

	// 片方しか選べないなら 1 回で終わり
	if (!hermiteAllowed || !otherAllowed || tr->frameCount < 3) {
		YbrAnimOptStats s;
		memset(&s, 0, sizeof(s));
		int ok = optimize_pass(tr, &o, &s);
		stats_merge(st, &s);
		return ok;
	}

	// 接線ぶんのデータが増えるので、両方試して小さいほうを採る
	YbrAnimTrack alt;
	if (!track_clone(&alt, tr)) {
		YbrAnimOptStats s;
		memset(&s, 0, sizeof(s));
		int ok = optimize_pass(tr, &o, &s);
		stats_merge(st, &s);
		return ok;
	}

	YbrAnimOptOptions noHermite = o;
	noHermite.interpMask &= ~YBR_INTERP_BIT(YBR_INTERP_HERMITE);

	YbrAnimOptStats sH, sN;
	memset(&sH, 0, sizeof(sH));
	memset(&sN, 0, sizeof(sN));

	int okH = optimize_pass(tr, &o, &sH);			 // HERMITE 有り
	int okN = optimize_pass(&alt, &noHermite, &sN);	 // HERMITE 無し

	size_t bytesH = YbrAnimTrackBytes(tr);
	size_t bytesN = YbrAnimTrackBytes(&alt);

	if (okN && (!okH || bytesN <= bytesH)) {
		// 接線を持たないほうが小さい
		YBR_FREE(tr->frames);
		YBR_FREE(tr->tangents);
		tr->frames = alt.frames;
		tr->tangents = alt.tangents;
		tr->frameCount = alt.frameCount;
		stats_merge(st, &sN);
		return okN;
	}

	YBR_FREE(alt.frames);
	YBR_FREE(alt.tangents);
	stats_merge(st, &sH);
	return okH;
}

// 並列化（トラック単位）

int YbrAnimOptHasThreads(void) { return YBR_USE_C11_THREADS; }

int YbrAnimOptDefaultThreads(void)
{
#if YBR_USE_C11_THREADS
	const char* env = getenv("YBR_THREADS");
	if (env) {
		int n = atoi(env);
		if (0 < n)
			return n < YBR_ANIM_OPT_MAX_THREADS ? n : YBR_ANIM_OPT_MAX_THREADS;
	}
	return 8;
#else
	return 1;
#endif
}

#if YBR_USE_C11_THREADS

// トラックを1本ずつ取り出して処理する。取り出しだけ排他すればよい。
typedef struct Pool {
	YbrAnimTrack* tracks;
	int count;
	const YbrAnimOptOptions* opts;

	mtx_t lock;
	int next;
	int ok;
	YbrAnimOptStats stats;	// lockで守る
} Pool;

static int pool_worker(void* ud)
{
	Pool* p = (Pool*)ud;
	for (;;) {
		mtx_lock(&p->lock);
		int i = p->next++;
		mtx_unlock(&p->lock);
		if (p->count <= i) break;

		// 集計はスレッドごとに取り、最後にまとめる
		YbrAnimOptStats local;
		memset(&local, 0, sizeof(local));
		int ok = YbrOptimizeAnimTrack(&p->tracks[i], p->opts, &local);

		mtx_lock(&p->lock);
		stats_merge(&p->stats, &local);
		if (!ok) p->ok = 0;
		mtx_unlock(&p->lock);
	}
	return 0;
}

static int optimize_tracks_parallel(YbrAnimTrack* tracks, int count,
									const YbrAnimOptOptions* opts,
									YbrAnimOptStats* st, int threads)
{
	Pool p;
	memset(&p, 0, sizeof(p));
	p.tracks = tracks;
	p.count = count;
	p.opts = opts;
	p.next = 0;
	p.ok = 1;
	if (mtx_init(&p.lock, mtx_plain) != thrd_success) return -1;

	thrd_t th[YBR_ANIM_OPT_MAX_THREADS];
	int started = 0;
	for (int i = 0; i < threads; i++) {
		if (thrd_create(&th[i], pool_worker, &p) != thrd_success) break;
		started++;
	}
	if (started == 0) {
		mtx_destroy(&p.lock);
		return -1;
	}

	// 呼び出し元のスレッドも 1 本ぶん働く
	pool_worker(&p);
	for (int i = 0; i < started; i++) thrd_join(th[i], NULL);

	mtx_destroy(&p.lock);
	stats_merge(st, &p.stats);
	return p.ok;
}

#endif /* YBR_USE_C11_THREADS */

int YbrOptimizeAnimation(YbrAnimation* a, const YbrAnimOptOptions* o,
						 YbrAnimOptStats* st)
{
	if (!a) return 1;
	if (st) st->animCount++;

	// 使用した Lanczos の a をファイルへ持たせる (再生側が同じ形を再現できる)
	YbrAnimOptOptions q = sanitize(o);
	a->sincA = q.interp.sincA;

	int threads = 0 < q.threads ? q.threads : YbrAnimOptDefaultThreads();
	if (a->trackCount < threads) threads = a->trackCount;

#if YBR_USE_C11_THREADS
	if (1 < threads && 1 < a->trackCount) {
		YbrAnimOptStats local;
		memset(&local, 0, sizeof(local));
		// 呼び出し元も働くので、追加で作るのは threads - 1 本
		int r = optimize_tracks_parallel(a->tracks, a->trackCount, o, &local,
										 threads - 1);
		if (0 <= r) {
			stats_merge(st, &local);
			return r;
		}
		// スレッドを作れなかったら逐次へ落ちる
	}
#else
	(void)threads;
#endif

	int ok = 1;
	for (int i = 0; i < a->trackCount; i++)
		if (!YbrOptimizeAnimTrack(&a->tracks[i], o, st)) ok = 0;
	return ok;
}

int YbrOptimizeSceneAnimations(YbrScene* sc, const YbrAnimOptOptions* o,
							   YbrAnimOptStats* st)
{
	if (!sc) return 1;
	int ok = 1;
	for (int i = 0; i < sc->animationCount; i++)
		if (!YbrOptimizeAnimation(&sc->animations[i], o, st)) ok = 0;
	return ok;
}
