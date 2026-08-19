/*
	Yui Blender to Raylib - Yabtファイル読み書き
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_YABT_H
#define YBR_YABT_H

#include "ybr.h"
#include "ybr_pose.h"

#ifdef __cplusplus
extern "C" {
#endif

// .yabt は アニメーション合成ツリー (YbrAnimBlendNode) だけを入れる形式。
// .ybr とは別のファイルなので、同じツリーを別のシーンへ持って行ける。
// アニメーションの実体は持たず、YBR_ABN_SOURCE の名前で .ybr 側を指す。
#define YABT_MAGIC "YABT"
#define YABT_SUPPORTED_VERSION 1

// 入れ子の深さの上限。壊れた / 悪意のあるファイルで
// 再帰が深くなりすぎてスタックを食い潰さないようにする。
#define YABT_MAX_DEPTH 64

// 読み込んだツリー一式。root 以下のノードと文字列はこれが持つ。
typedef struct YabtTree {
	int version;
	YbrAnimBlendNode* root;

	// 以下は内部で使用している
	void** blocks;	// 確保したメモリの一覧
	int blockCount;
	int blockCap;
} YabtTree;

YabtTree* YabtLoad(const char* fileName);
YabtTree* YabtLoadFromMemory(const unsigned char* data, size_t size);
void YabtUnload(YabtTree* tree);

// root はツリーの根。YbrAnimBlendTree の中身を書くなら tree.root を渡す。
int YabtSave(const YbrAnimBlendNode* root, const char* fileName);
unsigned char* YabtSaveToMemory(const YbrAnimBlendNode* root, size_t* outSize);

const char* YabtGetError(void);

#ifdef __cplusplus
}
#endif

#endif /* YBR_YABT_H */
