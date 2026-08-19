/*
	Yui Blender to Raylib - YbrCurve の評価ユーティリティ
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_CURVE_H
#define YBR_CURVE_H

#include "ybr.h"

#ifdef __cplusplus
extern "C" {
#endif

// ウェイト (0.0 = 始点 / 1.0 = 終点) からスプライン上の座標を求める
Vector3 YbrCurveGetPoint(const YbrCurve* curve, int splineIndex, float weight,
						 int resolution);

// 同じ位置での接線 (正規化済み)。方向が求まらないときは {0, 0, 0}
Vector3 YbrCurveGetTangent(const YbrCurve* curve, int splineIndex, float weight,
						   int resolution);

// スプラインの全長 (折れ線近似)。resolution が大きいほど正確。
float YbrCurveGetLength(const YbrCurve* curve, int splineIndex, int resolution);

// 始点からの移動距離に対応するウェイトを返す (近似値)。
float YbrCurveGetWeightAtDistance(const YbrCurve* curve, int splineIndex,
								  float distance, int resolution,
								  int iterations);

// 上の関数と YbrCurveGetPoint を続けて呼ぶだけのショートカット
Vector3 YbrCurveGetPointAtDistance(const YbrCurve* curve, int splineIndex,
								   float distance, int resolution,
								   int iterations);

#ifdef __cplusplus
}
#endif

#endif /* YBR_CURVE_H */
