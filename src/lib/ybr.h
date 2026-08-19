/*
	Yui Blender to Raylib - Ybrファイル読み書き
		Programed by あるる（きのもと 結衣）
*/

#ifndef YBR_H
#define YBR_H

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// アロケータ
#ifndef YBR_MALLOC
#ifdef RL_MALLOC
#define YBR_MALLOC(sz) RL_MALLOC(sz)
#else
#define YBR_MALLOC(sz) malloc(sz)
#endif
#endif
#ifndef YBR_CALLOC
#ifdef RL_CALLOC
#define YBR_CALLOC(n, sz) RL_CALLOC(n, sz)
#else
#define YBR_CALLOC(n, sz) calloc(n, sz)
#endif
#endif
#ifndef YBR_FREE
#ifdef RL_FREE
#define YBR_FREE(p) RL_FREE(p)
#else
#define YBR_FREE(p) free(p)
#endif
#endif

#include "raylib.h"
#include "raymath.h"

// raylib 5.0 以降のみ対応。
#if defined(RAYLIB_VERSION_MAJOR) && (RAYLIB_VERSION_MAJOR < 5)
#error "ybr requires raylib 5.0 or later"
#endif

#define YBR_FORMAT_NAME "Yui Blender to Raylib"
#define YBR_MAGIC "YUI"
#define YBR_SUPPORTED_VERSION 1

// 共通の型コード
// ファイル中では整数で格納される。未対応の値は null で書き出され、
// ここでは *_UNKNOWN (-1) になる。

typedef enum {
	YBR_NODE_UNKNOWN = 0,
	YBR_NODE_EMPTY,
	YBR_NODE_MESH,
	YBR_NODE_ARMATURE,
	YBR_NODE_CURVE,
	YBR_NODE_LIGHT,
	YBR_NODE_CAMERA,
	YBR_NODE_OBJECT,	 // 個別のコードを持たないオブジェクト
	YBR_NODE_COLLECTION	 // コレクション（行列は単位行列 / dataId は NULL）
} YbrNodeType;

typedef enum {
	YBR_SPLINE_POLY = 0,
	YBR_SPLINE_BEZIER,
	YBR_SPLINE_NURBS
} YbrSplineType;

typedef enum {
	YBR_LIGHT_POINT = 0,
	YBR_LIGHT_SUN,
	YBR_LIGHT_SPOT,
	YBR_LIGHT_AREA
} YbrLightType;

typedef enum {
	YBR_CAMERA_PERSP = 0,
	YBR_CAMERA_ORTHO,
	YBR_CAMERA_PANORAMIC
} YbrCameraType;

typedef enum {
	YBR_SENSOR_FIT_UNKNOWN = -1,
	YBR_SENSOR_FIT_AUTO = 0,
	YBR_SENSOR_FIT_HORIZONTAL,
	YBR_SENSOR_FIT_VERTICAL
} YbrSensorFit;

typedef enum {
	YBR_CUSTOM_BOOL = 0,
	YBR_CUSTOM_INT,
	YBR_CUSTOM_FLOAT,
	YBR_CUSTOM_STRING,
	YBR_CUSTOM_ARRAY
} YbrCustomType;

typedef enum {
	YBR_COLORSPACE_UNKNOWN = -1,
	YBR_COLORSPACE_SRGB = 0,
	YBR_COLORSPACE_LINEAR,
	YBR_COLORSPACE_NON_COLOR
} YbrColorSpace;

typedef enum {
	YBR_TEXWRAP_UNKNOWN = -1,
	YBR_TEXWRAP_REPEAT = 0,
	YBR_TEXWRAP_EXTEND,
	YBR_TEXWRAP_CLIP,
	YBR_TEXWRAP_MIRROR
} YbrTexWrap;

typedef enum {
	YBR_TEXFILTER_UNKNOWN = -1,
	YBR_TEXFILTER_LINEAR = 0,
	YBR_TEXFILTER_CLOSEST,
	YBR_TEXFILTER_CUBIC,
	YBR_TEXFILTER_SMART
} YbrTexFilter;

// ノード

// シーンツリーはシーンコレクションの階層をそのまま反映する。
// オブジェクトのカスタムプロパティ。
// Blender 側で "ybr_" で始まるものだけが対象で、key
// はプレフィックスを外した名前。
typedef struct YbrCustomProperty {
	char* key;
	YbrCustomType type;
	double number;	// BOOL / INT / FLOAT
	char* text;		// STRING
	float* values;	// ARRAY
	int valueCount;
} YbrCustomProperty;

typedef struct YbrNode {
	char* name;
	YbrNodeType type;
	char* dataId;
	Matrix matrix;	// ルートは world / 以下は親相対
	int customPropertyCount;
	YbrCustomProperty* customProperties;
	int childCount;
	struct YbrNode* children;
} YbrNode;

// メッシュ

typedef struct YbrVertexGroup {
	char* name;
	int index;
	int count;
	unsigned int* indices;	// 出力頂点インデックス
	float* weights;
} YbrVertexGroup;

// ウェイト上位 4 本に正規化済みのスキニングデータ。
typedef struct YbrSkin {
	int influences;			 // 常に4
	unsigned short* joints;	 // 4 * vertexCount / ボーン index
	float* weights;			 // 4 * vertexCount / 合計 1.0
} YbrSkin;

typedef struct YbrMesh {
	char* id;
	int vertexCount;
	int triangleCount;
	float* positions;  // 3 * vertexCount
	float* normals;	   // 3 * vertexCount (NULL可)
	float* tangents;   // 4 * vertexCount (NULL 可) xyz が接線、w が従法線の向き
					   // (+1 / -1)
	float* texcoords;  // 2 * vertexCount (NULL 可)
	unsigned char* colors;			// 4 * vertexCount (NULL 可)
	unsigned int* indices;			// 3 * triangleCount
	unsigned int* materialIndices;	// triangleCount (NULL 可)
	unsigned int* vertexMap;		// 出力頂点 -> 元頂点

	int materialCount;
	char** materials;  // 参照マテリアル ID

	char* armature;		 // アーマチュア「オブジェクト名」
	char* armatureData;	 // アーマチュアのデータ ID

	YbrSkin* skin;	// NULL可

	// 生データを残すオプションが ON のときのみ格納される
	int vertexGroupCount;
	YbrVertexGroup* vertexGroups;
} YbrMesh;

// アーマチュア

typedef struct YbrBone {
	char* name;
	int parent;			// 親 index / ルートは -1
	Matrix rest;		// アーマチュア空間
	Matrix restParent;	// 親ボーン相対
	float length;
} YbrBone;

typedef struct YbrArmature {
	char* id;
	int boneCount;
	YbrBone* bones;	 // 親が必ず子より前
} YbrArmature;

// カーブ（スプライン）

typedef struct YbrSpline {
	YbrSplineType type;
	int cyclic;
	int order;	// NURBS の order_u
	int pointCount;
	float* points;					 // 3 * pointCount
	float* handlesLeft;				 // BEZIER のみ
	float* handlesRight;			 // BEZIER のみ
	unsigned char* handleTypesLeft;	 // 0 FREE / 1 AUTO / 2 VECTOR / 3 ALIGNED
	unsigned char* handleTypesRight;
	float* weights;	 // POLY / NURBS
	float* tilts;
	float* radii;
} YbrSpline;

typedef struct YbrCurve {
	char* id;
	int is3d;
	int splineCount;
	YbrSpline* splines;
} YbrCurve;

// ライト

typedef struct YbrLight {
	char* id;
	YbrLightType type;
	Vector3 color;	// リニア RGB
	float energy;	// Blender の W / SUN は irradiance
	int useShadow;
	float radius;	  // shadow_soft_size
	float angle;	  // SUN  : 見かけの角度 (rad)
	float spotSize;	  // SPOT : コーン全角 (rad)
	float spotBlend;  // SPOT : 0..1
	char* shape;	  // AREA : SQUARE / RECTANGLE..
	float size;
	float sizeY;
	int hasCutoff;
	float cutoffDistance;
} YbrLight;

// マテリアル

typedef enum { YBR_MATERIAL_SIMPLE = 0, YBR_MATERIAL_PRO } YbrMaterialMode;

typedef struct YbrTexture {
	char* image;  // TEXTURE ブロックの id
	char* filepath;
	YbrColorSpace colorspace;
	YbrTexWrap extension;
	YbrTexFilter interpolation;
} YbrTexture;

// PRO モード : シェーダーノードグラフ
// シェーダーノードのタイプ（Blender の node.type に対応）
typedef enum {
	YBR_SN_UNKNOWN = 0,
	YBR_SN_OUTPUT_MATERIAL = 1,
	YBR_SN_OUTPUT_WORLD = 2,
	YBR_SN_OUTPUT_LIGHT = 3,
	YBR_SN_OUTPUT_AOV = 4,
	YBR_SN_OUTPUT_LINESTYLE = 5,
	YBR_SN_BSDF_PRINCIPLED = 10,
	YBR_SN_BSDF_DIFFUSE = 11,
	YBR_SN_BSDF_GLOSSY = 12,
	YBR_SN_BSDF_GLASS = 13,
	YBR_SN_BSDF_REFRACTION = 14,
	YBR_SN_BSDF_TRANSLUCENT = 15,
	YBR_SN_BSDF_TRANSPARENT = 16,
	YBR_SN_BSDF_VELVET = 17,
	YBR_SN_BSDF_SHEEN_BL400 = 17,  // Blender 4.0 以降の名前
	YBR_SN_BSDF_TOON = 18,
	YBR_SN_BSDF_HAIR = 19,
	YBR_SN_BSDF_HAIR_PRINCIPLED = 20,
	YBR_SN_BSDF_ANISOTROPIC = 21,
	YBR_SN_SUBSURFACE_SCATTERING = 22,
	YBR_SN_EMISSION = 23,
	YBR_SN_BACKGROUND = 24,
	YBR_SN_HOLDOUT = 25,
	YBR_SN_ADD_SHADER = 26,
	YBR_SN_MIX_SHADER = 27,
	YBR_SN_VOLUME_ABSORPTION = 28,
	YBR_SN_VOLUME_SCATTER = 29,
	YBR_SN_PRINCIPLED_VOLUME = 30,
	YBR_SN_SHADERTORGB = 31,
	YBR_SN_LIGHT_FALLOFF = 32,
	YBR_SN_TEX_IMAGE = 40,
	YBR_SN_TEX_ENVIRONMENT = 41,
	YBR_SN_TEX_NOISE = 42,
	YBR_SN_TEX_CHECKER = 43,
	YBR_SN_TEX_BRICK = 44,
	YBR_SN_TEX_GRADIENT = 45,
	YBR_SN_TEX_MAGIC = 46,
	YBR_SN_TEX_VORONOI = 47,
	YBR_SN_TEX_WAVE = 48,
	YBR_SN_TEX_WHITE_NOISE = 49,
	YBR_SN_TEX_SKY = 50,
	YBR_SN_TEX_IES = 51,
	YBR_SN_TEX_POINTDENSITY = 52,
	YBR_SN_TEX_MUSGRAVE = 53,
	YBR_SN_TEX_COORD = 54,
	YBR_SN_UVMAP = 60,
	YBR_SN_ATTRIBUTE = 61,
	YBR_SN_NEW_GEOMETRY = 62,
	YBR_SN_OBJECT_INFO = 63,
	YBR_SN_PARTICLE_INFO = 64,
	YBR_SN_HAIR_INFO = 65,
	YBR_SN_POINT_INFO = 66,
	YBR_SN_VOLUME_INFO = 67,
	YBR_SN_CAMERA = 68,
	YBR_SN_LIGHT_PATH = 69,
	YBR_SN_FRESNEL = 70,
	YBR_SN_LAYER_WEIGHT = 71,
	YBR_SN_WIREFRAME = 72,
	YBR_SN_BEVEL = 73,
	YBR_SN_AMBIENT_OCCLUSION = 74,
	YBR_SN_TANGENT = 75,
	YBR_SN_RGB = 80,
	YBR_SN_VALUE = 81,
	YBR_SN_MIX_RGB = 82,
	YBR_SN_MIX = 83,
	YBR_SN_VALTORGB = 84,
	YBR_SN_RGBTOBW = 85,
	YBR_SN_MATH = 86,
	YBR_SN_VECT_MATH = 87,
	YBR_SN_VECT_TRANSFORM = 88,
	YBR_SN_SEPRGB = 89,
	YBR_SN_SEPARATE_COLOR_BL303 = 89,  // Blender 3.3 以降の名前
	YBR_SN_COMBRGB = 90,
	YBR_SN_COMBINE_COLOR_BL303 = 90,  // Blender 3.3 以降の名前
	YBR_SN_SEPHSV = 91,
	YBR_SN_COMBHSV = 92,
	YBR_SN_SEPXYZ = 93,
	YBR_SN_COMBXYZ = 94,
	YBR_SN_HUE_SAT = 95,
	YBR_SN_BRIGHTCONTRAST = 96,
	YBR_SN_GAMMA = 97,
	YBR_SN_INVERT = 98,
	YBR_SN_CURVE_RGB = 99,
	YBR_SN_CURVE_VEC = 100,
	YBR_SN_CURVE_FLOAT = 101,
	YBR_SN_CLAMP = 102,
	YBR_SN_MAP_RANGE = 103,
	YBR_SN_FLOAT_CURVE = 104,
	YBR_SN_BLACKBODY = 105,
	YBR_SN_WAVELENGTH = 106,
	YBR_SN_NORMAL = 110,
	YBR_SN_NORMAL_MAP = 111,
	YBR_SN_BUMP = 112,
	YBR_SN_DISPLACEMENT = 113,
	YBR_SN_VECTOR_DISPLACEMENT = 114,
	YBR_SN_MAPPING = 115,
	YBR_SN_VECTOR_ROTATE = 116,
	YBR_SN_GROUP = 120,
	YBR_SN_GROUP_INPUT = 121,
	YBR_SN_GROUP_OUTPUT = 122,
	YBR_SN_REROUTE = 123,
} YbrShaderNodeType;

// シェーダーソケットのタイプ
typedef enum {
	YBR_SS_UNKNOWN = 0,
	YBR_SS_VALUE = 1,
	YBR_SS_INT = 2,
	YBR_SS_BOOLEAN = 3,
	YBR_SS_VECTOR = 4,
	YBR_SS_ROTATION = 5,
	YBR_SS_MATRIX = 6,
	YBR_SS_STRING = 7,
	YBR_SS_RGBA = 8,
	YBR_SS_SHADER = 9,
	YBR_SS_OBJECT = 10,
	YBR_SS_IMAGE = 11,
	YBR_SS_GEOMETRY = 12,
	YBR_SS_COLLECTION = 13,
	YBR_SS_TEXTURE = 14,
	YBR_SS_MATERIAL = 15,
	YBR_SS_MENU = 16,
	YBR_SS_CUSTOM = 17,
} YbrShaderSocketType;

typedef enum {
	YBR_PROP_FLOAT = 0,
	YBR_PROP_INT,
	YBR_PROP_BOOL,
	YBR_PROP_STRING,
	YBR_PROP_ARRAY
} YbrPropType;

typedef struct YbrProp {
	char* name;
	YbrPropType type;
	double number;	// FLOAT / INT / BOOL
	char* text;		// STRING
	float* values;	// ARRAY
	int valueCount;
} YbrProp;

typedef struct YbrShaderSocket {
	char* name;
	YbrShaderSocketType type;
	int valueCount;	 // 0 = デフォルト値なし / 1 なら x のみ有効
	Vector4 value;
} YbrShaderSocket;

typedef struct YbrShaderNode {
	char* name;
	YbrShaderNodeType type;
	char* label;
	int inputCount;
	YbrShaderSocket* inputs;
	int outputCount;
	YbrShaderSocket* outputs;
	int propCount;
	YbrProp* props;
} YbrShaderNode;

typedef struct YbrShaderLink {
	int fromNode, fromSocket;
	int toNode, toSocket;
} YbrShaderLink;

typedef struct YbrMaterial {
	char* id;
	YbrMaterialMode mode;
	char* renderMethod;
	int backfaceCulling;
	// 半透明を持つマテリアルか (書き出し側で判定して入れる)。
	// 1 なら不透明を描いたあとに、奥から順に描く必要がある。
	int transparent;

	// SIMPLE
	Vector4 baseColor;	// RGBA
	float specular;
	float metallic;
	float roughness;
	float alpha;
	float normalStrength;
	YbrTexture* baseColorMap;  // NULL可
	YbrTexture* specularMap;   // NULL可
	YbrTexture* metallicMap;   // NULL可
	YbrTexture* roughnessMap;  // NULL可
	YbrTexture* alphaMap;	   // NULL可
	YbrTexture* normalMap;	   // NULL可

	// PRO
	int nodeCount;
	YbrShaderNode* nodes;
	int linkCount;
	YbrShaderLink* links;
} YbrMaterial;

// ノードグループ (PRO モード)。
typedef struct YbrNodeGroup {
	char* id;
	int inputCount;
	YbrShaderSocket* inputs;
	int outputCount;
	YbrShaderSocket* outputs;
	int nodeCount;
	YbrShaderNode* nodes;
	int linkCount;
	YbrShaderLink* links;
} YbrNodeGroup;

// カメラ

typedef struct YbrCamera {
	char* id;
	YbrCameraType type;
	float lens;			 // mm
	float sensorWidth;	 // mm
	float sensorHeight;	 // mm
	YbrSensorFit sensorFit;
	float fovX;	 // ラジアン
	float fovY;	 // ラジアン
	float clipStart;
	float clipEnd;
	float orthoScale;
	float shiftX;
	float shiftY;
} YbrCamera;

// アニメーション
// すべてベイク済み。1 フレーム = 1 行列。

typedef enum {
	YBR_FRAME_UNKNOWN = -1,	 // 知らないフレームタイプ (読み飛ばす)
	YBR_FRAME_TRANSFORM = 0	 // 今はこれだけ。将来の拡張用
} YbrFrameType;

// キー i の interp は「キー i-1 から キー i へ向かう区間」の再構成方法。
typedef enum {
	YBR_INTERP_STEP = 0,  // キー i-1 の値のままホールド
	YBR_INTERP_LINEAR,	  // キー i-1 と i の線形補間
	YBR_INTERP_CUBIC,	  // 非一様 Catmull-Rom (i-2 .. i+1)
	YBR_INTERP_SINC,	  // Lanczos-a 再構成 (i-a .. i+a-1)
	YBR_INTERP_HERMITE	  // 3 次エルミート (キーに接線を持つ)
} YbrInterp;

#define YBR_INTERP_COUNT 5

// SINC (Lanczos) の a。YbrAnimation.sincA に入り、ファイルにも保存される。
// 古いファイルなど値が無い場合はこの既定値を使う。
#define YBR_SINC_A_DEFAULT 3
#define YBR_SINC_A_MAX 16

typedef struct YbrAnimFrame {
	int frame;	// 先頭からの相対フレーム番号
	YbrFrameType type;
	YbrInterp interp;
	Matrix transform;  // 親空間における自分のローカル行列
} YbrAnimFrame;

// HERMITE 用の接線。値は「1 フレームあたりの変化量」(微分)。
typedef struct YbrAnimTangent {
	Vector3 translation;
	Quaternion rotation;
	Vector3 scale;
} YbrAnimTangent;

#define YBR_TANGENT_FLOATS 10  // 3 + 4 + 3

typedef struct YbrAnimTrack {
	char* object;  // 対象オブジェクト名
	char* bone;	   // ボーン名 / オブジェクトなら NULL
	int frameCount;
	YbrAnimFrame* frames;  // 値が変わったフレームだけが入る

	// HERMITE を使うキーがあるときだけ入る (無ければ NULL)。
	YbrAnimTangent* tangents;
} YbrAnimTrack;

// ポーズマーカー (Blender のアクションに付いている名前付きフレーム)。
typedef struct YbrAnimMarker {
	char* name;
	int frame;
} YbrAnimMarker;

// NLA トラックの action strip 1 本ぶん。
typedef struct YbrAnimation {
	char* id;	   // アクション名 (参照 ID)
	char* object;  // 対象オブジェクト名
	float fps;
	int frameCount;	 // 全フレーム数
	int sincA;		 // SINC (Lanczos) の a。1..16 / 既定 3
	int trackCount;
	YbrAnimTrack* tracks;

	// ポーズマーカー。frame の昇順に並んでいる (無ければ 0 / NULL)
	int markerCount;
	YbrAnimMarker* markers;
} YbrAnimation;

// テクスチャ本体

typedef enum {
	YBR_TEX_RAW = 0,  // pixels に RGBA8 が入る
	YBR_TEX_PNG,	  // data に PNG ファイルが入る
	YBR_TEX_JPEG	  // data に JPEG ファイルが入る
} YbrTextureCompression;

typedef struct YbrTextureData {
	char* id;  // = 画像名 (参照 ID)
	char* name;
	char* filepath;	 // 埋め込みでないときはこれを読む
	YbrColorSpace colorspace;
	int width;
	int height;
	int embedded;  // 1 なら pixels か data が有効
	YbrTextureCompression compression;
	int quality;			// JPEG 品質 / PNG 圧縮レベル
	unsigned char* pixels;	// RAW のみ / RGBA8 / 上原点 / NULL 可
	unsigned char* data;	// PNG / JPEG のファイルバイト列
	int dataSize;
} YbrTextureData;

// エンプティ / シーン

typedef struct YbrEmpty {
	char* id;
	char* displayType;
	float displaySize;
} YbrEmpty;

typedef struct YbrScene {
	int version;

	int rootCount;
	YbrNode* roots;

	int meshCount;
	YbrMesh* meshes;

	// アーマチュアは 0 個か 1 個。書き出し側 (Blender アドオン) が複数の
	// アーマチュアを 1 つに合成してから書くので、読み込み側も 1 つ前提。
	// 万一 2 つ以上の ARMATURE ブロックがあった場合は先頭だけを採用する。
	YbrArmature* armature;	// 無ければ NULL

	int curveCount;
	YbrCurve* curves;

	int lightCount;
	YbrLight* lights;

	int emptyCount;
	YbrEmpty* empties;

	int materialCount;
	YbrMaterial* materials;
	int nodeGroupCount;
	YbrNodeGroup* nodeGroups;

	int animationCount;
	YbrAnimation* animations;

	int textureCount;
	YbrTextureData* textures;

	int cameraCount;
	YbrCamera* cameras;
} YbrScene;

// ----------------------------------------------------------------------------
// API

YbrScene* YbrLoad(const char* fileName);
YbrScene* YbrLoadFromMemory(const unsigned char* data, size_t size);
void YbrUnload(YbrScene* scene);
int YbrSave(const YbrScene* scene, const char* fileName);
unsigned char* YbrSaveToMemory(const YbrScene* scene, size_t* outSize);
const char* YbrGetError(void);

// シーンノード
const YbrMesh* YbrFindMesh(const YbrScene* scene, const char* id);
const YbrArmature* YbrGetArmature(const YbrScene* scene);
const YbrCurve* YbrFindCurve(const YbrScene* scene, const char* id);
const YbrLight* YbrFindLight(const YbrScene* scene, const char* id);
const YbrEmpty* YbrFindEmpty(const YbrScene* scene, const char* id);
const YbrMaterial* YbrFindMaterial(const YbrScene* scene, const char* id);
const YbrAnimation* YbrFindAnimation(const YbrScene* scene, const char* id);
const YbrNodeGroup* YbrFindNodeGroup(const YbrScene* scene, const char* id);
const YbrTextureData* YbrFindTexture(const YbrScene* scene, const char* id);
const YbrCamera* YbrFindCamera(const YbrScene* scene, const char* id);
const YbrNode* YbrFindNode(const YbrScene* scene, const char* name);

// アニメーション関連
int YbrAnimTrackTangentsSymmetric(const YbrAnimTrack* tr);
const YbrAnimMarker* YbrFindAnimMarker(const YbrAnimation* anim,
									   const char* name);

// カスタムプロパティ
const YbrCustomProperty* YbrFindCustomProperty(const YbrNode* node,
											   const char* key);
double YbrGetCustomNumber(const YbrNode* node, const char* key,
						  double fallback);
const char* YbrGetCustomText(const YbrNode* node, const char* key,
							 const char* fallback);

// テクスチャ
const char* YbrTextureFileExt(const YbrTextureData* tex);

// シーンツリーからノードを探してワールド行列を返す
int YbrSceneFindNodeWorld(const YbrScene* scene, YbrNodeType type,
						  const char* dataId, const char* nodeName,
						  Matrix* out);

// 深さ優先でノードを走査する
typedef int (*YbrNodeVisitor)(const YbrNode* node, const YbrNode* parent,
							  int depth, void* userData);
void YbrWalkNodes(const YbrScene* scene, YbrNodeVisitor visitor,
				  void* userData);

#ifdef __cplusplus
}
#endif

#endif	// YBR_H
