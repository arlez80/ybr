/*
	Yui Blender to Raylib - テストの共通部分
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_test.h"

/* ================================================================== */
/* ちいさなテストフレームワーク                                       */
/* ================================================================== */
int g_pass = 0, g_fail = 0;
static const char* g_group = "";

void group(const char* name)
{
	g_group = name;
	printf("\n--- %s ---\n", name);
}

void check(int cond, const char* what)
{
	if (cond) {
		g_pass++;
	}
	else {
		g_fail++;
		printf("  FAIL [%s] %s\n", g_group, what);
	}
}

void check_near(float a, float b, float eps, const char* what)
{
	if (fabsf(a - b) <= eps) {
		g_pass++;
	}
	else {
		g_fail++;
		printf("  FAIL [%s] %s  (%.6f != %.6f)\n", g_group, what, (double)a,
			   (double)b);
	}
}

void check_vec(Vector3 a, Vector3 b, float eps, const char* what)
{
	if (Vector3Distance(a, b) <= eps) {
		g_pass++;
	}
	else {
		g_fail++;
		printf("  FAIL [%s] %s  ((%.4f,%.4f,%.4f) != (%.4f,%.4f,%.4f))\n",
			   g_group, what, (double)a.x, (double)a.y, (double)a.z,
			   (double)b.x, (double)b.y, (double)b.z);
	}
}

Vector3 V(float x, float y, float z)
{
	Vector3 r;
	r.x = x;
	r.y = y;
	r.z = z;
	return r;
}

/* 決定論的な乱数 (環境で結果が変わらないように自前) */
static unsigned int g_rng = 12345u;
float frnd(float lo, float hi)
{
	g_rng = g_rng * 1664525u + 1013904223u;
	return lo + (hi - lo) * (float)((g_rng >> 8) & 0xFFFFFF) / (float)0x1000000;
}

/* ================================================================== */
/* テスト用シーンの組み立て                                           */
/*   YbrUnload() は使わず、静的な配列を指すだけにしておく             */
/* ================================================================== */

static float gridPos[GRID_VERTS * 3];
static unsigned int gridIdx[GRID_TRIS * 3];

/* [-half, half] の平らな床。法線は +Y */
static void make_grid(YbrMesh* m, float half)
{
	int n = GRID_CELLS + 1;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			int i = z * n + x;
			gridPos[i * 3 + 0] =
				-half + 2.0f * half * (float)x / (float)GRID_CELLS;
			gridPos[i * 3 + 1] = 0.0f;
			gridPos[i * 3 + 2] =
				-half + 2.0f * half * (float)z / (float)GRID_CELLS;
		}
	}
	int t = 0;
	for (int z = 0; z < GRID_CELLS; z++) {
		for (int x = 0; x < GRID_CELLS; x++) {
			unsigned int v00 = (unsigned int)(z * n + x);
			unsigned int v10 = v00 + 1;
			unsigned int v01 = v00 + (unsigned int)n;
			unsigned int v11 = v01 + 1;
			gridIdx[t * 3 + 0] = v00;
			gridIdx[t * 3 + 1] = v01;
			gridIdx[t * 3 + 2] = v10;
			t++;
			gridIdx[t * 3 + 0] = v10;
			gridIdx[t * 3 + 1] = v01;
			gridIdx[t * 3 + 2] = v11;
			t++;
		}
	}
	memset(m, 0, sizeof(*m));
	m->id = (char*)"GroundMesh";
	m->vertexCount = GRID_VERTS;
	m->triangleCount = GRID_TRIS;
	m->positions = gridPos;
	m->indices = gridIdx;
}

/* 一辺 1 の立方体 (原点中心)。法線は外向き */
float cubePos[8 * 3] = {-0.5f, -0.5f, -0.5f, 0.5f,	-0.5f, -0.5f, 0.5f, 0.5f,
						-0.5f, -0.5f, 0.5f,	 -0.5f, -0.5f, -0.5f, 0.5f, 0.5f,
						-0.5f, 0.5f,  0.5f,	 0.5f,	0.5f,  -0.5f, 0.5f, 0.5f};
unsigned int cubeIdx[12 * 3] = {
	4, 5, 6, 4, 6, 7, /* +Z */
	1, 0, 3, 1, 3, 2, /* -Z */
	5, 1, 2, 5, 2, 6, /* +X */
	0, 4, 7, 0, 7, 3, /* -X */
	3, 7, 6, 3, 6, 2, /* +Y */
	0, 1, 5, 0, 5, 4  /* -Y */
};

static void make_cube(YbrMesh* m)
{
	memset(m, 0, sizeof(*m));
	m->id = (char*)"CubeMesh";
	m->vertexCount = 8;
	m->triangleCount = 12;
	m->positions = cubePos;
	m->indices = cubeIdx;
}

/* ルート 3 つ:
 *   Ground  : 床 (単位行列)
 *   Box     : 立方体を (2, 0.5, 0) へ移動、Y 軸まわりに 30 度回転
 *   Mirror  : 立方体を X 方向に -1 倍 (鏡映) して (-2, 0.5, 0) へ */
void make_scene(TestScene* ts)
{
	memset(ts, 0, sizeof(*ts));
	make_grid(&ts->meshes[0], 4.0f);
	make_cube(&ts->meshes[1]);

	ts->roots[0].name = (char*)"Ground";
	ts->roots[0].type = YBR_NODE_MESH;
	ts->roots[0].dataId = (char*)"GroundMesh";
	ts->roots[0].matrix = MatrixIdentity();

	ts->roots[1].name = (char*)"Box";
	ts->roots[1].type = YBR_NODE_MESH;
	ts->roots[1].dataId = (char*)"CubeMesh";
	ts->roots[1].matrix = MatrixMultiply(MatrixRotateY(0.5236f),
										 MatrixTranslate(2.0f, 0.5f, 0.0f));

	ts->roots[2].name = (char*)"Mirror";
	ts->roots[2].type = YBR_NODE_MESH;
	ts->roots[2].dataId = (char*)"CubeMesh";
	ts->roots[2].matrix = MatrixMultiply(MatrixScale(-1.0f, 1.0f, 1.0f),
										 MatrixTranslate(-2.0f, 0.5f, 0.0f));

	ts->scene.version = YBR_SUPPORTED_VERSION;
	ts->scene.meshCount = 2;
	ts->scene.meshes = ts->meshes;
	ts->scene.rootCount = 3;
	ts->scene.roots = ts->roots;
}

/* ================================================================== */
/* 総当たり版 (8 分木の答え合わせ用)                                  */
/* ================================================================== */
int brute_segment(const YbrSolid* col, Vector3 a, Vector3 b, int cull,
				  float* outT, int* outTri)
{
	int best = -1;
	float bestT = 2.0f;
	int n = YbrSolidGetTriangleCount(col);
	for (int i = 0; i < n; i++) {
		const YbrTriangle* t = YbrSolidGetTriangle(col, i);
		float tt;
		if (YbrSegmentTriangleHit(a, b, t->v[0], t->v[1], t->v[2], cull, &tt,
								  NULL, NULL)) {
			if (tt < bestT) {
				bestT = tt;
				best = i;
			}
		}
	}
	if (outT) *outT = bestT;
	if (outTri) *outTri = best;
	return 0 <= best;
}

int brute_sphere(const YbrSolid* col, Vector3 c, float r, float* outDepth)
{
	float best = 0.0f;
	int hit = 0;
	int n = YbrSolidGetTriangleCount(col);
	for (int i = 0; i < n; i++) {
		const YbrTriangle* t = YbrSolidGetTriangle(col, i);
		Vector3 q = YbrClosestPointOnTriangle(c, t->v[0], t->v[1], t->v[2]);
		float d = Vector3Distance(c, q);
		if (d < r) {
			hit = 1;
			if (best < r - d) best = r - d;
		}
	}
	if (outDepth) *outDepth = best;
	return hit;
}

int brute_capsule(const YbrSolid* col, Vector3 a, Vector3 b, float r,
				  float* outDepth)
{
	float best = 0.0f;
	int hit = 0;
	int n = YbrSolidGetTriangleCount(col);
	for (int i = 0; i < n; i++) {
		const YbrTriangle* t = YbrSolidGetTriangle(col, i);
		float d = YbrSegmentTriangleDistance(a, b, t->v[0], t->v[1], t->v[2],
											 NULL, NULL);
		if (d < r) {
			hit = 1;
			float depth = (1e-6f < d) ? r - d : r;
			if (best < depth) best = depth;
		}
	}
	if (outDepth) *outDepth = best;
	return hit;
}

/* ================================================================== */
/* ボーン / ポーズ                                                    */
/* ================================================================== */
/* root (原点) - child (親から +Y に 1) の 2 本だけのアーマチュア */
static YbrBone g_bones[2];
YbrArmature g_arm;

void make_armature(void)
{
	memset(g_bones, 0, sizeof(g_bones));
	g_bones[0].name = (char*)"root";
	g_bones[0].parent = -1;
	g_bones[0].rest = MatrixIdentity();
	g_bones[0].restParent = MatrixIdentity();
	g_bones[1].name = (char*)"child";
	g_bones[1].parent = 0;
	g_bones[1].rest = MatrixTranslate(0.0f, 1.0f, 0.0f);
	g_bones[1].restParent = MatrixTranslate(0.0f, 1.0f, 0.0f);

	memset(&g_arm, 0, sizeof(g_arm));
	g_arm.id = (char*)"Arm";
	g_arm.boneCount = 2;
	g_arm.bones = g_bones;
}

/* ================================================================== */
/* シェーダーノード -> GLSL 変換                                      */
/* ================================================================== */
/* [対象ノード] -> Principled BSDF.Base Color -> Material Output.Surface */

YbrShaderSocket ssock(const char* name, YbrShaderSocketType t, int count,
					  float v)
{
	YbrShaderSocket s;
	memset(&s, 0, sizeof(s));
	s.name = (char*)name;
	s.type = t;
	s.valueCount = count;
	s.value = (Vector4){v, v, v, 1.0f};
	return s;
}

void probe_build(ShaderProbe* p, YbrShaderNodeType type, const char* propName,
				 const char* propValue)
{
	memset(p, 0, sizeof(*p));

	p->out_in[0] = ssock("Surface", YBR_SS_SHADER, 0, 0.0f);
	p->nodes[0].name = (char*)"Material Output";
	p->nodes[0].type = YBR_SN_OUTPUT_MATERIAL;
	p->nodes[0].inputCount = 1;
	p->nodes[0].inputs = p->out_in;

	p->bsdf_in[0] = ssock("Base Color", YBR_SS_RGBA, 4, 0.8f);
	p->bsdf_in[1] = ssock("Metallic", YBR_SS_VALUE, 1, 0.0f);
	p->bsdf_in[2] = ssock("Roughness", YBR_SS_VALUE, 1, 0.5f);
	p->bsdf_out[0] = ssock("BSDF", YBR_SS_SHADER, 0, 0.0f);
	p->nodes[1].name = (char*)"Principled BSDF";
	p->nodes[1].type = YBR_SN_BSDF_PRINCIPLED;
	p->nodes[1].inputCount = 3;
	p->nodes[1].inputs = p->bsdf_in;
	p->nodes[1].outputCount = 1;
	p->nodes[1].outputs = p->bsdf_out;

	/* 名前で引かれる入力はここに無ければ既定値になるので、
	 * よく使われるものだけ用意しておけばよい */
	p->target_in[0] = ssock("Vector", YBR_SS_VECTOR, 3, 0.0f);
	p->target_in[1] = ssock("Color", YBR_SS_RGBA, 4, 0.5f);
	p->target_in[2] = ssock("Fac", YBR_SS_VALUE, 1, 0.5f);
	p->target_in[3] = ssock("Scale", YBR_SS_VALUE, 1, 5.0f);
	p->target_in[4] = ssock("Factor", YBR_SS_VALUE, 1, 0.5f);
	p->target_in[5] = ssock("A", YBR_SS_RGBA, 4, 0.2f);
	p->target_in[6] = ssock("B", YBR_SS_RGBA, 4, 0.8f);
	p->target_in[7] = ssock("Value", YBR_SS_VALUE, 1, 0.5f);
	p->target_in[8] = ssock("From Min", YBR_SS_VALUE, 1, 0.0f);
	p->target_in[9] = ssock("From Max", YBR_SS_VALUE, 1, 1.0f);
	p->target_in[10] = ssock("To Min", YBR_SS_VALUE, 1, 0.0f);
	p->target_in[11] = ssock("To Max", YBR_SS_VALUE, 1, 1.0f);
	p->target_out[0] = ssock("Fac", YBR_SS_VALUE, 1, 0.0f);
	p->target_out[1] = ssock("Color", YBR_SS_RGBA, 4, 0.0f);
	p->target_out[2] = ssock("Position", YBR_SS_VECTOR, 3, 0.0f);
	p->target_out[3] = ssock("Alpha", YBR_SS_VALUE, 1, 1.0f);
	p->nodes[2].name = (char*)"Target";
	p->nodes[2].type = type;
	p->nodes[2].inputCount = 12;
	p->nodes[2].inputs = p->target_in;
	p->nodes[2].outputCount = 4;
	p->nodes[2].outputs = p->target_out;

	if (propName) {
		p->props[0].name = (char*)propName;
		p->props[0].type = YBR_PROP_STRING;
		p->props[0].text = (char*)propValue;
		p->nodes[2].propCount = 1;
		p->nodes[2].props = p->props;
	}

	p->links[0].fromNode = 2;
	p->links[0].fromSocket = 0;
	p->links[0].toNode = 1;
	p->links[0].toSocket = 0;
	p->links[1].fromNode = 1;
	p->links[1].fromSocket = 0;
	p->links[1].toNode = 0;
	p->links[1].toSocket = 0;

	p->material.id = (char*)"Probe";
	p->material.mode = YBR_MATERIAL_PRO;
	p->material.nodeCount = 3;
	p->material.nodes = p->nodes;
	p->material.linkCount = 2;
	p->material.links = p->links;
}

/* ================================================================== */
/* ボーン追従の当たり判定 (YbrDynamic)                                */
/* ================================================================== */

int dyn_probe_init(DynProbe* p, int skinned)
{
	memset(p, 0, sizeof(*p));
	make_armature();

	memcpy(p->positions, cubePos, sizeof(p->positions));
	for (int i = 0; i < 12 * 3; i++) p->indices[i] = (unsigned short)cubeIdx[i];
	for (int i = 0; i < 8; i++) {
		p->joints[i * 4 + 0] = 1; /* child ボーン */
		p->weights[i * 4 + 0] = 1.0f;
		p->boneIds[i * 4 + 0] = 1;
		p->boneWeights[i * 4 + 0] = 1.0f;
	}
	p->skin.influences = 4;
	p->skin.joints = p->joints;
	p->skin.weights = p->weights;

	p->mesh.id = (char*)"CubeMesh";
	p->mesh.vertexCount = 8;
	p->mesh.triangleCount = 12;
	p->mesh.positions = cubePos;
	p->mesh.indices = cubeIdx;
	if (skinned) {
		p->mesh.armatureData = (char*)"Arm";
		p->mesh.skin = &p->skin;
	}
	p->meshPtr = &p->mesh;

	p->armPtr = &g_arm;

	p->part.meshIndex = 0;
	p->part.materialIndex = -1;
	p->part.vertexCount = 8;
	p->part.triangleCount = 12;
	p->part.positions = p->positions;
	p->part.indices = p->indices;
	p->part.localMin = V(-0.5f, -0.5f, -0.5f);
	p->part.localMax = V(0.5f, 0.5f, 0.5f);
	if (skinned) {
		p->part.boneIds = p->boneIds;
		p->part.boneWeights = p->boneWeights;
		p->part.skinned = 1;
		p->part.dynamicBuffers = 1;
	}

	p->node.name = "Cube";
	p->node.meshId = "CubeMesh";
	p->node.transform = MatrixIdentity();
	p->node.meshIndex = 0;

	p->model.meshCount = 1;
	p->model.meshes = &p->meshPtr;
	p->model.partCount = 1;
	p->model.parts = &p->part;
	p->model.nodeCount = 1;
	p->model.nodes = &p->node;
	p->model.armature = p->armPtr;

	YbrModelBuildRestBounds(&p->model);
	p->inst = YbrModelInstanceCreate(&p->model);
	if (!p->inst) return 0;

	/* ポーズ追従の AABB が要るテスト向けに、前計算を作って紐づけておく */
	p->skinBounds = YbrSkinBoundsCreate(&p->model);
	YbrModelInstanceSetSkinBounds(p->inst, p->skinBounds);

	p->pose = YbrModelInstanceGetPose(p->inst, NULL);

	p->dyn = YbrDynamicFromInstance(p->inst, NULL);
	return p->dyn != NULL;
}

void dyn_probe_free(DynProbe* p)
{
	YbrDynamicUnload(p->dyn);
	YbrModelInstanceUnload(p->inst);
	YbrSkinBoundsUnload(p->skinBounds);
	memset(&p->model, 0, sizeof(p->model));
}
