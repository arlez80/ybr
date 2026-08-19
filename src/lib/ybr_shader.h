/*
	Yui Blender to Raylib - シェーダー
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_SHADER_H
#define YBR_SHADER_H

#include "raylib.h"
#include "rlgl.h"
#include "ybr.h"

// raylib 5.0 以降のみ対応
typedef char ybr_requires_raylib_5_or_later[(RL_OPENGL_ES_30 == 6) ? 1 : -1];

#ifdef __cplusplus
extern "C" {
#endif

// rlGlVersion は 1 始まりなので 0 を「不明」として使う
#define YBR_GL_UNKNOWN 0

// ライトの数。0 なら「ライト無し」= 指定した色をそのまま出す
// (raylib の既定シェーダーのような見え方)。1..4 で方向光を使う。
#define YBR_SHADER_MAX_LIGHTS 4
// 影を落とせるライトの数 (深度テクスチャの枚数)
#define YBR_SHADER_MAX_SHADOWS 2
#define YBR_SHADER_LIGHTS_DEFAULT 1

// ライトの uniform 名。i は 0..lightCount-1
#define YBR_SHADER_UNIFORM_LIGHT_DIR "ybrLightDir"		  // + i : vec3
#define YBR_SHADER_UNIFORM_LIGHT_COLOR "ybrLightColor"	  // + i : vec4
#define YBR_SHADER_UNIFORM_LIGHT_POS "ybrLightPos"		  // + i : vec3
#define YBR_SHADER_UNIFORM_LIGHT_PARAMS "ybrLightParams"  // + i : vec4
#define YBR_SHADER_UNIFORM_AMBIENT "ambient"			  // vec4

// 影の uniform 名。i は 0..shadowLights-1 (シェーダーの灯 i に対応する)
#define YBR_SHADER_UNIFORM_SHADOW_MAP "ybrShadowMap"		// + i : sampler2D
#define YBR_SHADER_UNIFORM_LIGHT_VP "ybrLightVP"			// + i : mat4
#define YBR_SHADER_UNIFORM_SHADOW_PARAMS "ybrShadowParams"	// + i : vec4
#define YBR_SHADER_UNIFORM_VIEW_POS "viewPos"				// vec3

// ybrLightParams.x に入れるライトの種類
typedef enum {
	YBR_SHADER_LIGHT_DIRECTIONAL = 0,  // 平行光（減衰なし）
	YBR_SHADER_LIGHT_POINT = 1,		   // 点光源
	YBR_SHADER_LIGHT_SPOT = 2		   // スポット（コーンで絞る）
} YbrShaderLightKind;

// GPU スキニングで送れるボーン数の既定値。
#define YBR_SHADER_MAX_BONES 64
#define YBR_SHADER_MAX_BONES_CAP 128

// GPU スキニングで使う名前 (raylib の既定と同じにしてある)
#define YBR_SHADER_ATTRIB_BONE_IDS "vertexBoneIds"
#define YBR_SHADER_ATTRIB_BONE_WEIGHTS "vertexBoneWeights"
#define YBR_SHADER_UNIFORM_BONES "boneMatrices"

typedef enum {
	YBR_SHADER_OK = 0,
	YBR_SHADER_ERR_NULL_NODE,		  // ノードが NULL
	YBR_SHADER_ERR_UNSUPPORTED_GL,	  // OpenGL 1.1 など GLSL 非対応
	YBR_SHADER_ERR_UNSUPPORTED_NODE,  // 未対応 / 未定義のノードタイプ
	YBR_SHADER_ERR_MISSING_SOCKET,	  // 期待するソケットが無い
	YBR_SHADER_ERR_OUT_OF_MEMORY,
	YBR_SHADER_ERR_TOO_MANY_UNIFORMS  // uniform が多すぎる（上限 192）
} YbrShaderError;

// uniform に流し込む値の型。raylib の SetShaderValue に渡す形式に対応する
typedef enum {
	YBR_UNIFORM_FLOAT = 0,
	YBR_UNIFORM_VEC2,
	YBR_UNIFORM_VEC3,
	YBR_UNIFORM_VEC4,
	YBR_UNIFORM_INT,
	YBR_UNIFORM_SAMPLER2D
} YbrShaderUniformType;

typedef struct YbrShaderUniform {
	char* name;	 // GLSL 内の uniform 名
	YbrShaderUniformType type;
	// raylib の rlShaderLocationIndex に対応するものがあればその値。
	// 対応が無い独自 uniform は -1 で、GetShaderLocation で引く。
	int locIndex;
	// 1 なら raylib が毎フレーム自動で設定するので、こちらから流す必要はない
	// (mvp / matModel / matNormal / texture0 など)。
	int autoSet;
	float value[4];	 // 初期値 (SAMPLER2D では未使用)
	int valueCount;
	char* textureId;  // SAMPLER2D のとき TEXTURE ブロックの id
	// SAMPLER2D のときのラップ / フィルタ。分からなければ -1。
	// 値は YbrTexWrap / YbrTexInterp。
	int texWrap;
	int texFilter;
} YbrShaderUniform;

typedef struct YbrShaderResult {
	char* vertexCode;	 // 失敗時は NULL
	char* fragmentCode;	 // 失敗時は NULL
	int glVersion;		 // rlGlVersion / 失敗時は YBR_GL_UNKNOWN
	YbrShaderError error;

	// エラーメッセージ。解放不要だが、次の変換呼び出しで上書きされる。
	const char* errorMessage;
	int uniformCount;
	YbrShaderUniform* uniforms;
} YbrShaderResult;

// 生成オプション
typedef struct YbrShaderOptions {
	// rlGetVersion() の戻り値
	int glVersion;

	// 方向光の数 (0..YBR_SHADER_MAX_LIGHTS)。既定 1。
	// 0 のときはライティングを一切行わず、色をそのまま出す。
	int lightCount;

	// 影を落とすライトの数 (0..YBR_SHADER_MAX_SHADOWS)。
	int shadowLights;
	int skinning;  // 1 なら GPU スキニング用の属性と uniform を足す
	int maxBones;  // boneMatrices の要素数 (1..YBR_SHADER_MAX_BONES_CAP)

	// ノードグループを解決するためのシーン。NULL なら GROUP ノードは
	// 出力ソケットの既定値になる (展開も関数化もされない)。
	const YbrScene* scene;
} YbrShaderOptions;

YbrShaderOptions YbrShaderOptionsDefaults(int glVersion);

// マテリアルの Material Output から **ノードグラフ全体を辿って** 変換する。
YbrShaderResult YbrShaderFromMaterial(const YbrMaterial* material,
									  int glVersion);

// SIMPLE モードのマテリアル (baseColor / metallic / roughness / 各種マップ)
// から 直接シェーダーを作る。ノードグラフは見ない。
YbrShaderResult YbrShaderFromSimpleMaterial(const YbrMaterial* material,
											int glVersion);

// mode を見て SIMPLE / PRO を自動で選び、オプションも渡せる版。
// PRO でノードの変換に失敗したときは SIMPLE のパラメータで作り直す。
YbrShaderResult YbrShaderFromMaterialEx(const YbrMaterial* material,
										const YbrShaderOptions* options);

// エラーメッセージを返す
const char* YbrShaderErrorString(YbrShaderError error);

// シェーダーノードタイプの名前 (デバッグ用 / 未知なら "UNKNOWN")
const char* YbrShaderNodeTypeName(YbrShaderNodeType type);

void YbrUnloadShaderResult(YbrShaderResult* result);

// SetShaderValue に渡す uniformType (raylib の ShaderUniformDataType 相当)
int YbrShaderUniformFormat(YbrShaderUniformType type);

// そのノードタイプを変換できるか
int YbrShaderIsNodeSupported(YbrShaderNodeType type);

#ifdef __cplusplus
}
#endif

#endif /* YBR_SHADER_H */
