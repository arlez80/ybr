/*
	Yui Blender to Raylib - モデル描画
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_MODEL_H
#define YBR_MODEL_H

#include "raylib.h"
#include "ybr.h"
#include "ybr_anim.h"
#include "ybr_camera.h"
#include "ybr_frustum.h"
#include "ybr_pose.h"
#include "ybr_shader.h"
#include "ybr_world.h"

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// メッシュ分割ユーティリティ
// YbrMesh は 1 メッシュに複数マテリアルを持てて、index も uint32。
// GPU (と raylib の Mesh) は 1 メッシュ 1 マテリアル / uint16 index
// なので、マテリアル単位 + 65535 頂点単位で分割する。
typedef struct YbrSubmesh {
	int materialIndex;	// YbrMesh.materials への添字 / -1
	int vertexCount;
	int triangleCount;
	unsigned int* vertexMap;  // 分割後の頂点 -> 元メッシュの頂点
	unsigned short* indices;  // 3 * triangleCount
} YbrSubmesh;

int YbrSplitMesh(const YbrMesh* mesh, YbrSubmesh** out, int* outCount);
void YbrFreeSubmeshes(YbrSubmesh* subs, int count);

// ----------------------------------------------------------------------------
// モデルの生成設定

typedef struct YbrModelOptions {
	int loadTextures;	  // マテリアルのテクスチャを読む
	int uploadNormals;	  // 既定 1
	int uploadTexcoords;  // 既定 1
	int uploadColors;	  // 既定 1

	// マテリアルからシェーダーを生成する（0ならrlglの既定シェーダー）
	int materialShaders;
	// GPU スキニングを使う
	int gpuSkinning;
	// シェーダーに持たせるボーン数の上限 既定 YBR_SHADER_MAX_BONES
	int maxBones;
	// GLSL のバージョン。0 なら rlGetVersion() を使う
	int glVersion;

	// ライトの項目はここには無い。生成されるシェーダーは常に
	// YBR_WORLD_MAX_ACTIVE_LIGHTS 灯 / YBR_WORLD_MAX_SHADOWS 枚ぶんの
	// uniform を持ち、実際に何灯効かせるかは YbrWorld が毎フレーム決める。
} YbrModelOptions;

YbrModelOptions YbrModelOptionsDefaults(void);

// ----------------------------------------------------------------------------
// マテリアル

// .ybr のテクスチャブロックを raylib の Texture2D として読み込む
Texture2D YbrLoadTexture(const YbrTextureData* tex);

// シェーダーが使うテクスチャ1枚分の割り当て
#define YBR_MODEL_MAX_TEXTURES 8

typedef struct YbrModelTextureSlot {
	Texture2D texture;
	int loc;   // sampler uniform の location
	int slot;  // テクスチャユニット番号
} YbrModelTextureSlot;

#ifndef YBR_MODEL_FWD_DECLARED
#define YBR_MODEL_FWD_DECLARED
typedef struct YbrModelInstance YbrModelInstance;
typedef struct YbrModelPart YbrModelPart;
typedef struct YbrModelMaterial YbrModelMaterial;
#endif

struct YbrModelMaterial {
	const char* id;
	Vector4 baseColor;
	// ベースカラーのテクスチャ (textures[0] と同じもの / 無ければ id == 0)。
	// 法線 / ラフネスなどは textures[] に入る。
	Texture2D texture;
	int backfaceCulling;
	int transparent;  // 半透明を含む

	// 生成したシェーダー
	Shader shader;	// id == 0 なら rlgl の既定シェーダー
	int ownsShader;
	int skinning;  // シェーダーが GPU スキニング対応か
	int maxBones;
	int locBoneMatrices;  // -1 なら無し
	int attribBoneIds;
	int attribBoneWeights;

	// シェーダーが宣言している sampler をすべて割り当てておく。
	// ベースカラーだけでなく法線 / ラフネス / メタリック / アルファも入る。
	YbrModelTextureSlot textures[YBR_MODEL_MAX_TEXTURES];
	int textureCount;
	// シェーダー変数 (ライトの中身は YbrWorld が毎フレーム流し込む)
	int locLightDir[YBR_WORLD_MAX_ACTIVE_LIGHTS];
	int locLightColor[YBR_WORLD_MAX_ACTIVE_LIGHTS];
	int locLightPos[YBR_WORLD_MAX_ACTIVE_LIGHTS];
	int locLightParams[YBR_WORLD_MAX_ACTIVE_LIGHTS];
	int locShadowMap[YBR_WORLD_MAX_SHADOWS];
	int locLightVP[YBR_WORLD_MAX_SHADOWS];
	int locShadowParams[YBR_WORLD_MAX_SHADOWS];
	int locAmbient, locViewPos;
};

// GPUへ載せた1描画単位
struct YbrModelPart {
	int meshIndex;		// YbrModel.meshes への添字
	int materialIndex;	// YbrModel.materials への添字
	int vertexCount;
	int triangleCount;

	// このパートのローカル AABB。半透明の並べ替えと、レスト姿勢の
	// AABB を O(1) で出すのに使う。
	Vector3 localMin, localMax;

	float* positions;		// 3 * vertexCount（CPU 側の原本）
	float* normals;			// NULL可
	float* tangents;		// 4 * vertexCount / NULL可
	float* texcoords;		// NULL可
	unsigned char* colors;	// NULL可
	unsigned short* indices;

	unsigned short* boneIds;  // 4 * vertexCount / NULL可
	float* boneWeights;		  // 4 * vertexCount / NULL可
	int skinned;			  // スキンを持つか (= 唯一のアーマチュアに従う)
	int gpuSkin;			  // 1 なら頂点シェーダーでスキニングする
	// CPU スキニングのパートは、描画のたびにインスタンスの計算結果を
	// この VBO へ流し込む。共有バッファを一時置き場として使う形。
	int dynamicBuffers;

	unsigned int vaoId;
	// 0:pos 1:uv 2:normal 3:color 4:boneIds 5:boneWeights 6:tangent 7:index
	unsigned int vboId[8];
};

// シーンツリー上の「メッシュを持つノード」1つ分
typedef struct YbrModelNode {
	const char* name;
	const char* meshId;

	// 元になった YbrScene のノード。カスタムプロパティなどはここから引く
	// YbrScene 側を指しているので、YbrScene より先に YbrModel を解放すること
	const YbrNode* source;
	Matrix transform;  // ワールド行列
	int meshIndex;	   // YbrModel.meshes への添字
} YbrModelNode;

// モデルデータ
typedef struct YbrModel {
	const YbrScene* scene;

	int meshCount;
	const YbrMesh** meshes;	 // 元メッシュ

	int partCount;
	YbrModelPart* parts;

	int materialCount;
	YbrModelMaterial* materials;

	int nodeCount;
	YbrModelNode* nodes;  // トポロジ（レスト姿勢の行列）

	// 唯一のアーマチュア (無ければ NULL)。ポーズはインスタンスが持つ。
	const YbrArmature* armature;

	int ownTextures;  // 自前で読んだテクスチャを解放するか
	// 同じ画像を複数のマテリアルが使っても1回しか読まないようにする表
	struct YbrTexCacheEntry* texCache;
	int texCacheCount, texCacheCap;

	int gpuSkinning;	 // GPU スキニングを使っているか
	int hasTransparent;	 // 半透明のマテリアルがあるか

	// レスト姿勢のローカル AABB（ロード時に 1 回だけ計算する）
	Vector3 localMin, localMax;
	int hasBounds;
	Vector3 staticMin, staticMax;
	int hasStatic;
} YbrModel;

// YbrSkinBounds : ポーズ追従 AABB のための前計算
typedef struct YbrSkinBounds {
	const YbrModel* model;	// どのモデルから作ったか (取り違え防止)
	int boneCount;			// アーマチュアのボーン数
	Vector3* boneMin;		// boneCount 個
	Vector3* boneMax;
	unsigned char* boneValid;  // その骨が動かす頂点があったか
} YbrSkinBounds;

// 前計算する。スキンを持つパートが 1 つも無ければ NULL を返す
YbrSkinBounds* YbrSkinBoundsCreate(const YbrModel* model);
void YbrSkinBoundsUnload(YbrSkinBounds* bounds);

// モデルインスタンス：表示1体分
struct YbrModelInstance {
	const YbrModel* model;
	// ポーズ追従 AABB の前計算 (紐づけなければ NULL)。所有はしない。
	const YbrSkinBounds* skinBounds;
	Matrix transform;  // この体のワールド行列

	// 唯一のアーマチュアのポーズ。hasPose が 0 なら pose は未初期化。
	int hasPose;
	YbrPose pose;

	// 1体ごとの表示フラグ
	unsigned char* partVisible;				// model->partCount 個
	unsigned char* nodeVisible;				// model->nodeCount 個
	const YbrModelMaterial** nodeMaterial;	// model->nodeCount 個 / NULL可

	// CPU スキニングの計算結果（GPU への転送は描画時）
	float** animPositions;	// model->partCount 個 / 要らないものは NULL
	float** animNormals;

	// ポーズを反映したワールド AABB
	Vector3 worldMin, worldMax;
	int hasBounds;

	// 作業領域 (毎フレーム確保しないため)
	struct YbrDrawItem* drawOrder;
	int drawOrderCap;
	float* boneUpload;
	int boneUploadCap;

	void* userData;
};

YbrModel* YbrModelLoad(const YbrScene* scene, const YbrModelOptions* opts);
void YbrModelUnload(YbrModel* model);

// 半透明を含むマテリアルがあるか
int YbrModelHasTransparent(const YbrModel* model);
// そのパートが半透明マテリアルか
int YbrModelIsPartTransparent(const YbrModel* model, int part);
// レスト姿勢のローカル AABB
int YbrModelGetLocalBounds(const YbrModel* model, Vector3* outMin,
						   Vector3* outMax);
// 唯一のアーマチュア (無ければ NULL)
const YbrArmature* YbrModelGetArmature(const YbrModel* model);
// ノードの元になった YbrScene のノード
const YbrNode* YbrModelGetNodeSource(const YbrModel* model, int node);
int YbrModelFindNode(const YbrModel* model, const char* name);

// 描画に使うシェーダーを差し替える
// materialIndex < 0 なら全マテリアル
// 以前のシェーダーが自前生成なら解放する。
void YbrModelSetShader(YbrModel* model, int materialIndex, Shader shader);

// 差し替え用の YbrModelMaterial を組み立てる補助。
YbrModelMaterial YbrModelMakeMaterial(Shader shader, Texture2D texture,
									  Color baseColor, int transparent);

// 描画パスの指定
typedef enum {
	YBR_DRAW_OPAQUE = 1u << 0,		 // 不透明マテリアルのパートだけ
	YBR_DRAW_TRANSPARENT = 1u << 1,	 // 半透明マテリアルのパートだけ
	YBR_DRAW_ALL = YBR_DRAW_OPAQUE | YBR_DRAW_TRANSPARENT
} YbrDrawPass;

YbrModelInstance* YbrModelInstanceCreate(const YbrModel* model);
void YbrModelInstanceUnload(YbrModelInstance* inst);

const YbrModel* YbrModelInstanceGetModel(const YbrModelInstance* inst);

// この体が視錐台に入っているか (ポーズ反映後のワールド AABB で判定)。
// frustum が NULL なら常に 1。
int YbrModelInstanceIsVisible(const YbrModelInstance* inst,
							  const YbrFrustum* frustum);

// 配置
void YbrModelInstanceSetTransform(YbrModelInstance* inst, Matrix transform);
Matrix YbrModelInstanceGetTransform(const YbrModelInstance* inst);

// ポーズ関連。アーマチュアは 1 つなので armatureId は
// 「一致確認をしたいときだけ」渡せばよく、NULL でよい。
YbrPose* YbrModelInstanceGetPose(YbrModelInstance* inst,
								 const char* armatureId);
// ポーズを持っているか (0 か 1)
int YbrModelInstanceHasPose(const YbrModelInstance* inst);
void YbrModelInstanceApplyPose(YbrModelInstance* inst);

// 表示/非表示設定
void YbrModelInstanceSetPartVisible(YbrModelInstance* inst, int part,
									int visible);
int YbrModelInstanceIsPartVisible(const YbrModelInstance* inst, int part);
void YbrModelInstanceSetAllPartsVisible(YbrModelInstance* inst, int visible);
int YbrModelInstanceSetPartVisibleByMesh(YbrModelInstance* inst,
										 const char* meshId, int visible);
int YbrModelInstanceSetPartVisibleByMaterial(YbrModelInstance* inst,
											 const char* materialId,
											 int visible);
void YbrModelInstanceSetNodeVisible(YbrModelInstance* inst, int node,
									int visible);
int YbrModelInstanceIsNodeVisible(const YbrModelInstance* inst, int node);
void YbrModelInstanceSetAllNodesVisible(YbrModelInstance* inst, int visible);
int YbrModelInstanceSetNodeVisibleByName(YbrModelInstance* inst,
										 const char* name, int visible);

// マテリアルの差し替え
void YbrModelInstanceSetNodeMaterial(YbrModelInstance* inst, int node,
									 const YbrModelMaterial* material);
const YbrModelMaterial* YbrModelInstanceGetNodeMaterial(
	const YbrModelInstance* inst, int node);
int YbrModelInstanceSetNodeMaterialByName(YbrModelInstance* inst,
										  const char* name,
										  const YbrModelMaterial* material);
void YbrModelInstanceClearNodeMaterials(YbrModelInstance* inst);

// 当たり判定 / カリング用の AABB
int YbrModelInstanceGetBounds(const YbrModelInstance* inst, Vector3* outMin,
							  Vector3* outMax);
// AABB を計算し直す。ApplyPose() / SetTransform() / SetSkinBounds() が
// 内部で呼ぶので、普通は直接呼ばなくてよい。
void YbrModelInstanceUpdateBounds(YbrModelInstance* inst);
// 体に紐づける。NULL を渡すと外れる。
void YbrModelInstanceSetSkinBounds(YbrModelInstance* inst,
								   const YbrSkinBounds* bounds);
const YbrSkinBounds* YbrModelInstanceGetSkinBounds(
	const YbrModelInstance* inst);

// レスト姿勢の AABB (localMin/Max と staticMin/Max) を求め直す。
int YbrModelBuildRestBounds(YbrModel* model);

// CPU スキニングの計算 (内部用。インスタンスのバッファへ書き出す)
void YbrModelSkinPart(const YbrModelPart* p, const YbrPose* pose, float* dstPos,
					  float* dstNrm);

// アタッチメント
int YbrModelInstanceGetBoneWorld(const YbrModelInstance* inst,
								 const char* boneName, Matrix* out);
int YbrModelInstanceGetNodeWorld(const YbrModelInstance* inst,
								 const char* nodeName, Matrix* out);

// ユーザーデータ
void YbrModelInstanceSetUserData(YbrModelInstance* inst, void* userData);
void* YbrModelInstanceGetUserData(const YbrModelInstance* inst);

// ----------------------------------------------------------------------------
// 描画 (world は NULL 可。NULL ならシェーダーの既定ライトで描かれる)

void YbrModelInstanceDraw(const YbrModelInstance* inst, const YbrWorld* world,
						  Color tint);
void YbrModelInstanceDrawWires(const YbrModelInstance* inst,
							   const YbrWorld* world, Color tint);
void YbrModelInstanceDrawPass(const YbrModelInstance* inst,
							  const YbrWorld* world, Color tint,
							  unsigned int pass);
void YbrModelInstanceDrawWiresPass(const YbrModelInstance* inst,
								   const YbrWorld* world, Color tint,
								   unsigned int pass);

// 視錐台カリング付きの描画
int YbrModelInstanceDrawCulled(const YbrModelInstance* inst,
							   const YbrWorld* world, Color tint);
int YbrModelInstanceDrawPassCulled(const YbrModelInstance* inst,
								   const YbrWorld* world, Color tint,
								   unsigned int pass);

#ifdef __cplusplus
}
#endif

#endif /* YBR_MODEL_H */
