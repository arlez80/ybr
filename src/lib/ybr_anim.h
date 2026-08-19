/*
	Yui Blender to Raylib - アニメーションの補間 / サンプリング
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_ANIM_H
#define YBR_ANIM_H

#include "ybr.h"

#ifdef __cplusplus
extern "C" {
#endif

// 補間パラメータ
// YBR_SINC_A_DEFAULT / YBR_SINC_A_MAX は ybr.h にある（フォーマットの定数）
// 重みテーブルの上限。sincA はこの範囲に丸められる
#define YBR_INTERP_MAX_RADIUS YBR_SINC_A_MAX
#define YBR_INTERP_MAX_TAPS (2 * YBR_INTERP_MAX_RADIUS + 2)

typedef struct YbrInterpParams {
	int sincA;	// SINC (Lanczos) の a。1..YBR_SINC_A_MAX
} YbrInterpParams;

// フォーマット規定の既定値 (sincA = 3) を返す
YbrInterpParams YbrInterpParamsDefault(void);

// NULL や範囲外の値を既定値へ丸める (内部でも呼ばれる)
YbrInterpParams YbrInterpParamsSanitize(const YbrInterpParams* p);

// ファイルに保存されている値を取り出す。a が NULL なら既定値。
YbrInterpParams YbrInterpParamsFromAnimation(const YbrAnimation* a);

const char* YbrInterpName(YbrInterp i);
// "step" / "linear" / "cubic" / "sinc"(lanczos) を解釈する。
// 成功したら 1。
int YbrInterpParse(const char* name, YbrInterp* out);
// その補間方法が区間の外側を何キー分参照するか (片側)
int YbrInterpRadius(YbrInterp i, const YbrInterpParams* p);

// TRS
typedef struct YbrTransform {
	Vector3 translation;
	Quaternion rotation;  // 正規化済み
	Vector3 scale;		  // 鏡映は x にまとめられる
} YbrTransform;

YbrTransform YbrTransformIdentity(void);
YbrTransform YbrTransformFromMatrix(Matrix m);
Matrix YbrTransformToMatrix(YbrTransform t);
// 回転の符号をそろえた上での線形補間（回転は nlerp）
YbrTransform YbrTransformLerp(YbrTransform a, YbrTransform b, float t);

// キー列アクセサ経由の評価
// index は 0 .. count-1。frame は単調増加していること。
typedef struct YbrKeySource {
	int count;
	int (*frameAt)(int index, const void* ud);
	YbrInterp (*interpAt)(int index, const void* ud);
	YbrTransform (*valueAt)(int index, const void* ud);
	// HERMITE 用の接線。NULL なら HERMITE は LINEAR として扱う。
	// out が 0 なら in 接線、1 なら out 接線。
	YbrAnimTangent (*tangentAt)(int index, int out, const void* ud);
	const void* ud;
} YbrKeySource;

// 接線ユーティリティ
YbrAnimTangent YbrAnimTangentZero(void);
// 2 つの値の差から接線 (1 フレームあたりの変化量) を作る。
// dtFrames が 0 以下なら 0 を返す。
YbrAnimTangent YbrAnimTangentFromDelta(YbrTransform a, YbrTransform b,
									   float dtFrames);
// Blender の F-Curve のようなベジェハンドルから接線を作る。
// handle は「キーからハンドルまでのフレーム差と値差」。
YbrAnimTangent YbrAnimTangentFromBezier(YbrTransform key, YbrTransform handle,
										float dtFrames);

YbrTransform YbrEvaluateKeySource(const YbrKeySource* ks, float frame,
								  const YbrInterpParams* p);

// サンプラ
typedef struct YbrAnimSampler {
	int frameCount;
	int* frames;
	unsigned char* interps;
	YbrTransform* values;
	YbrAnimTangent* tangents;  // HERMITE を使わなければ NULL
	YbrInterpParams params;
} YbrAnimSampler;

// params が NULL なら既定値。成功で 1。
int YbrAnimSamplerInit(YbrAnimSampler* s, const YbrAnimTrack* track,
					   const YbrInterpParams* params);
// アニメーションに保存されている sincA をそのまま使う版。
// track は anim->tracks[i] を渡すこと。
int YbrAnimSamplerInitFromAnimation(YbrAnimSampler* s, const YbrAnimation* anim,
									const YbrAnimTrack* track);
void YbrAnimSamplerUnload(YbrAnimSampler* s);

YbrTransform YbrAnimSamplerEvaluate(const YbrAnimSampler* s, float frame);
Matrix YbrAnimSamplerMatrix(const YbrAnimSampler* s, float frame);

// 前処理なしで直接評価する版
// (必要なキーだけその場で分解するので、毎フレーム呼ぶなら YbrAnimSampler
// を使ったほうが速い)。
YbrTransform YbrAnimTrackEvaluate(const YbrAnimTrack* tr, float frame,
								  const YbrInterpParams* params);
Matrix YbrAnimTrackMatrix(const YbrAnimTrack* tr, float frame,
						  const YbrInterpParams* params);

// ポーズマーカーが区間内にあるか取得
int YbrAnimMarkersInRange(const YbrAnimation* anim, float fromFrame,
						  float toFrame, const YbrAnimMarker** out, int max);

#ifdef __cplusplus
}
#endif

#endif /* YBR_ANIM_H */
