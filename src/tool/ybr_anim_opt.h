/*
	Yui Blender to Raylib - アニメーションの最適化
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_ANIM_OPT_H
#define YBR_ANIM_OPT_H

#include "ybr.h"
#include "ybr_anim.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YBR_INTERP_BIT(i) (1u << (unsigned)(i))
#define YBR_INTERP_ALL                                                     \
	(YBR_INTERP_BIT(YBR_INTERP_STEP) | YBR_INTERP_BIT(YBR_INTERP_LINEAR) | \
	 YBR_INTERP_BIT(YBR_INTERP_CUBIC) | YBR_INTERP_BIT(YBR_INTERP_SINC) |  \
	 YBR_INTERP_BIT(YBR_INTERP_HERMITE))

// .ybr でのキー 1 本ぶんのバイト数
// frames(u32) + types(u8) + interps(u8) + transforms(f32 x 16)
#define YBR_ANIM_KEY_BYTES (4 + 1 + 1 + 16 * 4)
// HERMITE を使うトラックはキーごとに接線も持つ。
// in == out (なめらか) なら 1 つ、折れているなら in / out の 2 つ。
#define YBR_ANIM_TANGENT_BYTES (YBR_TANGENT_FLOATS * 4)
#define YBR_ANIM_TANGENT_BYTES_MAX (2 * YBR_TANGENT_FLOATS * 4)

// トラックが占めるおおよそのバイト数 (接線を含む)
size_t YbrAnimTrackBytes(const YbrAnimTrack* tr);

// C11 スレッド (<threads.h>) が使えるか。使えないときは常に逐次で動く。
int YbrAnimOptHasThreads(void);
// 自動指定 (threads = 0) のときに使うスレッド数
int YbrAnimOptDefaultThreads(void);

// 同時に処理するトラック数。
#define YBR_ANIM_OPT_THREADS_AUTO 0
#define YBR_ANIM_OPT_MAX_THREADS 64

typedef struct YbrAnimOptOptions {
	float posEps;			 /* 平行移動の許容誤差 (シーン単位)   既定 0.0005 */
	float rotEps;			 /* 回転の許容誤差 (度)               既定 0.05   */
	float scaleEps;			 /* スケールの相対許容誤差            既定 0.001  */
	unsigned int interpMask; /* 使ってよい補間方法のビットマスク            */
	int subsample;			 /* 1 フレームあたりの検査点数 (1..16) 既定 2   */
	int maxRounds;			 /* 補間の選び直し <-> 間引きの往復回数 既定 4  */
	YbrInterpParams interp;	 /* SINC の a。結果は YbrAnimation.sincA に入る */
	int threads;			 /* 同時に処理するトラック数 (既定 0 = 自動)   */
} YbrAnimOptOptions;

typedef struct YbrAnimOptStats {
	int animCount;
	int trackCount;
	int keysBefore;
	int keysAfter;
	int interpUse[YBR_INTERP_COUNT]; /* 最適化後の区間数 (先頭キーは除く) */
	float maxPosErr;				 /* 実際に出た最大誤差 */
	float maxRotErr;				 /* 度 */
	float maxScaleErr;				 /* 相対 */
} YbrAnimOptStats;

YbrAnimOptOptions YbrAnimOptDefaults(void);

// トラック 1 本 / アニメーション 1 本 / シーン全体。
int YbrOptimizeAnimTrack(YbrAnimTrack* tr, const YbrAnimOptOptions* o,
						 YbrAnimOptStats* stats);
int YbrOptimizeAnimation(YbrAnimation* a, const YbrAnimOptOptions* o,
						 YbrAnimOptStats* stats);
int YbrOptimizeSceneAnimations(YbrScene* sc, const YbrAnimOptOptions* o,
							   YbrAnimOptStats* stats);

#ifdef __cplusplus
}
#endif

#endif /* YBR_ANIM_OPT_H */
