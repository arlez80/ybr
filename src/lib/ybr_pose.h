/*
	Yui Blender to Raylib - ボーンの姿勢とアニメーション合成
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_POSE_H
#define YBR_POSE_H

#include "raylib.h"
#include "raymath.h"
#include "ybr.h"
#include "ybr_anim.h"

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// ボーンの状態

typedef struct YbrPoseBone {
	// 入力 : 親ボーン相対の姿勢
	Vector3 translation;
	Quaternion rotation;
	Vector3 scale;

	// 出力 : YbrPoseUpdate() が計算する
	Matrix pose;  // アーマチュア空間での行列
	Matrix skin;  // pose * inverse(rest) 頂点用

	int parent;	 // 親 index / ルートは -1
} YbrPoseBone;

typedef struct YbrPose {
	const YbrArmature* armature;
	int boneCount;
	YbrPoseBone* bones;
	Matrix* invRest;		  // rest の逆行列（事前計算）
	YbrTransform* restLocal;  // rest_parent を TRS にしたもの
} YbrPose;

int YbrPoseInit(YbrPose* pose, const YbrArmature* armature);
void YbrPoseUnload(YbrPose* pose);
// すべてのボーンをレストポーズへ戻す
void YbrPoseReset(YbrPose* pose);
// bones[].translation / rotation / scale から pose / skin を計算する
void YbrPoseUpdate(YbrPose* pose);
int YbrPoseFindBone(const YbrPose* pose, const char* name);

// アニメーションを 1 本そのまま流し込む
// frame は float でよい。トラックに無いボーンはレストのままになる。
void YbrPoseApplyAnimation(YbrPose* pose, const YbrAnimation* anim,
						   float frame);

// ----------------------------------------------------------------------------
// アニメーション合成ツリー

typedef enum {
	YBR_ALM_ONE_SHOT = 0,  // 1回だけ再生
	YBR_ALM_LOOP		   // ループ
} YbrAnimLoopMode;

typedef enum {
	YBR_ABN_SOURCE = 0,	 // アニメーションそのもの
	YBR_ABN_LERP,		 // 線形補間
	YBR_ABN_ADD,		 // 加算
	YBR_ABN_TRANSITION	 // 遷移
} YbrAnimBlendNodeType;

typedef struct YbrAnimBlendNode YbrAnimBlendNode;

typedef struct YbrAnimBlendSource {
	const char* name;  // アニメーション名
	YbrAnimLoopMode loopMode;
	float playSpeed;  // 既定 1.0

	// 以下は内部で使用している
	const YbrAnimation* animation;
	float position;	 // 再生位置（秒）
	float length;	 // 総再生時間（秒）
} YbrAnimBlendSource;

typedef struct YbrAnimBlendInput2 {
	YbrAnimBlendNode* input;
	YbrAnimBlendNode* mixInput;
	float weight;				 // 0.0 - 1.0
	const char** filteredBones;	 // 合成から外すボーン名（NULL可）
	int filteredBoneCount;
} YbrAnimBlendInput2;

typedef struct YbrAnimBlendTransition {
	YbrAnimBlendNode* inputs;
	int inputCount;
	int index;		// 現在の入力
	int nextIndex;	// 遷移先（-1 で遷移なし）
	float transitionSeconds;
	float position;	 // 遷移中の経過時間
} YbrAnimBlendTransition;

struct YbrAnimBlendNode {
	const char* uniqueId;  // 検索用（使わないなら "" か NULL）
	YbrAnimBlendNodeType type;
	int playedPreviousEvaluate;
	union {
		YbrAnimBlendSource source;
		YbrAnimBlendInput2 lerp;
		YbrAnimBlendInput2 add;
		YbrAnimBlendTransition transition;
	};
};

// 合成中の1ボーン分の値
typedef struct YbrAnimDelta {
	Vector3 translation;  // レスト位置からのずれ
	Quaternion rotation;  // レスト姿勢からの回転
	Vector3 scale;		  // 親相対のスケールそのもの
} YbrAnimDelta;

#define YBR_ABT_MAX_STACK 32

typedef struct YbrAnimBlendTree {
	YbrAnimBlendNode* root;
	int validated;

	const YbrScene* scene;
	const YbrPose* pose;  // ボーン構成の参照元
	int boneCount;

	// トラックの索引 : animation ごとに boneCount ぶんのトラック番号
	int animationCount;
	int** trackIndex;  // [anim][bone] / 無ければ -1

	YbrAnimDelta* stack[YBR_ABT_MAX_STACK];
	int stackPointer;
} YbrAnimBlendTree;

// pose は先に YbrPoseInit() しておくこと (ボーン構成を参照する)。
// root は呼び出し側が持つツリー。成功で 1。
int YbrAnimBlendTreeInit(YbrAnimBlendTree* tree, const YbrScene* scene,
						 const YbrPose* pose, YbrAnimBlendNode* root);
void YbrAnimBlendTreeUnload(YbrAnimBlendTree* tree);
YbrAnimBlendNode* YbrAnimBlendTreeFind(const YbrAnimBlendTree* tree,
									   const char* uniqueId);
// delta 秒ぶん進めて outPose へ書き込む (YbrPoseUpdate まで行う)。成功で 1。
int YbrAnimBlendTreeEval(YbrAnimBlendTree* tree, float delta, YbrPose* outPose);

#ifdef __cplusplus
}
#endif

#endif /* YBR_POSE_H */
