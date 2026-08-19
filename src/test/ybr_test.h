/*
	Yui Blender to Raylib - テストの共通部分
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_TEST_H
#define YBR_TEST_H

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "rlgl.h"
#include "ybr.h"
#include "ybr_anim.h"
#include "ybr_cbor.h"
#include "ybr_collision.h"
#include "ybr_collision_dynamic.h"
#include "ybr_collision_solid.h"
#include "ybr_curve.h"
#include "ybr_mesh_opt.h"
#include "ybr_model.h"
#include "ybr_shader.h"
#include "ybr_yabt.h"
#include "ybr_world.h"

/* ================================================================== */
/* ちいさなテストフレームワーク                                       */
/* ================================================================== */
extern int g_pass, g_fail;

void group(const char* name);
void check(int cond, const char* what);
void check_near(float a, float b, float eps, const char* what);
void check_vec(Vector3 a, Vector3 b, float eps, const char* what);

Vector3 V(float x, float y, float z);
/* 決定論的な乱数 (環境で結果が変わらないように自前) */
float frnd(float lo, float hi);

/* ================================================================== */
/* 共通のテスト用データ                                               */
/* ================================================================== */
#define GRID_CELLS 16
#define GRID_VERTS ((GRID_CELLS + 1) * (GRID_CELLS + 1))
#define GRID_TRIS (GRID_CELLS * GRID_CELLS * 2)

/* 一辺 1 の立方体 (原点中心)。法線は外向き */
extern float cubePos[8 * 3];
extern unsigned int cubeIdx[12 * 3];

typedef struct TestScene {
	YbrScene scene;
	YbrMesh meshes[2];
	YbrNode roots[3];
} TestScene;

/* ルート 3 つ:
 *   Ground  : 床 (単位行列)
 *   Box     : 立方体を (2, 0.5, 0) へ移動、Y 軸まわりに 30 度回転
 *   Mirror  : 立方体を X 方向に -1 倍 (鏡映) して (-2, 0.5, 0) へ */
void make_scene(TestScene* ts);

/* 総当たり版 (8 分木の答え合わせ用) */
int brute_segment(const YbrSolid* col, Vector3 a, Vector3 b, int cull,
				  float* outT, int* outTri);
int brute_sphere(const YbrSolid* col, Vector3 c, float r, float* outDepth);
int brute_capsule(const YbrSolid* col, Vector3 a, Vector3 b, float r,
				  float* outDepth);

/* root (原点) - child (親から +Y に 1) の 2 本だけのアーマチュア */
extern YbrArmature g_arm;
void make_armature(void);

/* ================================================================== */
/* シェーダーノードのテスト用グラフ                                   */
/* ================================================================== */
typedef struct ShaderProbe {
	YbrShaderNode nodes[3];
	YbrShaderLink links[4];
	YbrShaderSocket target_in[12], target_out[4];
	YbrShaderSocket bsdf_in[3], bsdf_out[1];
	YbrShaderSocket out_in[1];
	YbrProp props[2];
	YbrMaterial material;
} ShaderProbe;

YbrShaderSocket ssock(const char* name, YbrShaderSocketType t, int count,
					  float v);
void probe_build(ShaderProbe* p, YbrShaderNodeType type, const char* propName,
				 const char* propValue);

/* ================================================================== */
/* ボーン追従の当たり判定 (YbrDynamic) のテスト用モデル               */
/* ================================================================== */
/* 立方体を 1 つ持ち、頂点全部がボーン 1 (child) に属するモデルを作る。
 * child を動かすと当たり判定も追従するはず。 */
/* YbrModel は GPU へ載せる部分 (ybr_model.c) を通さずに手で組み立てる。
 * YbrDynamic が読むのは parts / nodes / meshes / poses だけなので、
 * これでウィンドウも GL コンテキストも無しにテストできる。 */
typedef struct DynProbe {
	YbrMesh mesh;
	const YbrMesh* meshPtr;
	YbrSkin skin;
	unsigned short joints[8 * 4];
	float weights[8 * 4];

	float positions[8 * 3];
	unsigned short indices[12 * 3];
	unsigned short boneIds[8 * 4];
	float boneWeights[8 * 4];

	YbrModelPart part;
	YbrModelNode node;
	const YbrArmature* armPtr;
	YbrModel model;

	YbrModelInstance* inst;
	YbrPose* pose;
	YbrDynamic* dyn;
	YbrSkinBounds* skinBounds;
} DynProbe;

int dyn_probe_init(DynProbe* p, int skinned);
void dyn_probe_free(DynProbe* p);

/* ================================================================== */
/* 個々のテスト                                                       */
/* ================================================================== */
void test_geometry(void);
void test_build(const YbrSolid* col);
void test_segment(const YbrSolid* col);
void test_sphere(const YbrSolid* col);
void test_capsule(const YbrSolid* col);
void test_triangle(const YbrSolid* col);
void test_overlap_and_tags(const YbrScene* scene, const YbrSolid* col);
void test_options(const YbrScene* scene);
void test_sweep(void);
void test_pose(void);
void test_blend_tree(void);
void test_hermite(void);
void test_pose_markers(void);
void test_yabt(void);
void test_split_mesh(void);
void test_mesh_opt(void);
void test_shader_nodes(void);
void test_simple_shader(void);
void test_gpu_skinning_shader(void);
void test_node_group(void);
void test_light_count(void);
void test_generated_glsl_scope(void);
void test_dynamic(void);
void test_dynamic_world(void);
void test_visibility(void);
void test_material_override(void);
void test_instances(void);
void test_instance_bounds(void);
void test_attachment(void);
void test_scene_lights_camera(void);
void test_frustum(void);
void test_point_spot_lights(void);
void test_embedded_texture_survives(void);
void test_broken_files(void);
void test_byte_order(void);
void test_loaded(const char* path);
void test_gl_simple_shaders(void);
void test_gl_all_nodes(void);
void test_gl_light_kinds(void);
void test_gl_model(void);
void test_gl_shadows(void);
void test_gl_scene_file(const char* path);

#endif /* YBR_TEST_H */
