/*
	Yui Blender to Raylib - GL コンテキストが要るテスト
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_test.h"

/* 生成した GLSL を実際にドライバでコンパイルし、YbrModel の GPU 部分
 * (VAO / VBO のアップロードと描画) までを見る。 */
static int compile_result(const YbrShaderResult* r, const char* what)
{
	if (r->error != YBR_SHADER_OK) {
		printf("  FAIL %s : %s (%s)\n", what, YbrShaderErrorString(r->error),
			   r->errorMessage ? r->errorMessage : "");
		return 0;
	}
	Shader sh = LoadShaderFromMemory(r->vertexCode, r->fragmentCode);
	int ok = (sh.id != 0);
	if (!ok) printf("  FAIL %s : GLSL did not compile / link\n", what);
	if (sh.id) UnloadShader(sh);
	return ok;
}

static YbrMaterial gl_simple_material(void)
{
	YbrMaterial m;
	memset(&m, 0, sizeof(m));
	m.id = (char*)"T";
	m.mode = YBR_MATERIAL_SIMPLE;
	m.baseColor = (Vector4){0.8f, 0.7f, 0.6f, 1.0f};
	m.metallic = 0.1f;
	m.roughness = 0.4f;
	m.specular = 0.5f;
	m.alpha = 1.0f;
	return m;
}

void test_gl_simple_shaders(void)
{
	group("compile: SIMPLE x lights");
	YbrMaterial m = gl_simple_material();

	for (int n = 0; n <= YBR_SHADER_MAX_LIGHTS; n++) {
		YbrShaderOptions o = YbrShaderOptionsDefaults(rlGetVersion());
		o.lightCount = n;
		YbrShaderResult r = YbrShaderFromMaterialEx(&m, &o);
		char what[64];
		snprintf(what, sizeof(what), "SIMPLE lights=%d", n);
		check(compile_result(&r, what), what);
		YbrUnloadShaderResult(&r);
	}

	group("compile: GPU skinning");
	for (int bones = 1; bones <= 64; bones *= 8) {
		YbrShaderOptions o = YbrShaderOptionsDefaults(rlGetVersion());
		o.skinning = 1;
		o.maxBones = bones;
		YbrShaderResult r = YbrShaderFromMaterialEx(&m, &o);
		char what[64];
		snprintf(what, sizeof(what), "skinning maxBones=%d", bones);
		check(compile_result(&r, what), what);
		YbrUnloadShaderResult(&r);
	}
}

/* 対応ノードを 1 つずつ挿した PRO グラフを全部コンパイルする */
void test_gl_all_nodes(void)
{
	group("compile: every supported node (PRO)");

	int tried = 0, bad = 0;
	for (int t = 0; t <= 130; t++) {
		YbrShaderNodeType type = (YbrShaderNodeType)t;
		if (!YbrShaderIsNodeSupported(type)) continue;

		ShaderProbe p;
		probe_build(&p, type, NULL, NULL);
		YbrShaderOptions o = YbrShaderOptionsDefaults(rlGetVersion());
		YbrShaderResult r = YbrShaderFromMaterialEx(&p.material, &o);
		tried++;
		if (!compile_result(&r, YbrShaderNodeTypeName(type))) bad++;
		YbrUnloadShaderResult(&r);
	}
	printf("  %d node types compiled\n", tried);
	check(60 < tried, "対応ノードが十分ある");
	check(bad == 0, "全ノードの GLSL がドライバで通る");
}

/* 点光源 / スポットを実際に使うシェーダーが通るか。
 * ybrLightSample() を挟むので、GLSL ES 1.00 の out パラメータや
 * pow() の扱いを含めてドライバに通しておきたい。 */
void test_gl_light_kinds(void)
{
	group("compile: point / spot lights");
	YbrMaterial m = gl_simple_material();

	for (int n = 1; n <= YBR_SHADER_MAX_LIGHTS; n++) {
		YbrShaderOptions o = YbrShaderOptionsDefaults(rlGetVersion());
		o.lightCount = n;
		YbrShaderResult r = YbrShaderFromMaterialEx(&m, &o);

		char what[64];
		snprintf(what, sizeof(what), "lights=%d with attenuation", n);
		int ok = compile_result(&r, what);

		/* 種類ごとの uniform がちゃんと引けるか (最適化で消えていないか) */
		if (ok && r.vertexCode && r.fragmentCode) {
			Shader sh = LoadShaderFromMemory(r.vertexCode, r.fragmentCode);
			if (sh.id) {
				for (int i = 0; i < n && ok; i++) {
					char nd[32], np[32], na[32];
					snprintf(nd, sizeof(nd), "%s%d",
							 YBR_SHADER_UNIFORM_LIGHT_DIR, i);
					snprintf(np, sizeof(np), "%s%d",
							 YBR_SHADER_UNIFORM_LIGHT_POS, i);
					snprintf(na, sizeof(na), "%s%d",
							 YBR_SHADER_UNIFORM_LIGHT_PARAMS, i);
					if (GetShaderLocation(sh, nd) < 0) ok = 0;
					if (GetShaderLocation(sh, np) < 0) ok = 0;
					if (GetShaderLocation(sh, na) < 0) ok = 0;
				}
				UnloadShader(sh);
			}
		}
		check(ok, what);
		YbrUnloadShaderResult(&r);
	}

	/* 頂点シェーダーは骨の行列で位置 / 法線 / 接線を動かす。
	 * ここを GL に通しておかないと、スコープ違反のような
	 * 「生成はできるがコンパイルできない」ミスに気づけない。 */
	group("compile: skinning");
	for (int n = 0; n <= YBR_SHADER_MAX_LIGHTS; n++) {
		YbrShaderOptions o = YbrShaderOptionsDefaults(rlGetVersion());
		o.skinning = 1;
		o.maxBones = 64;
		o.lightCount = n;
		YbrShaderResult r = YbrShaderFromMaterialEx(&m, &o);

		char what[64];
		snprintf(what, sizeof(what), "skinning x lights=%d", n);
		check(compile_result(&r, what), what);
		YbrUnloadShaderResult(&r);
	}

	group("compile: shadows");
	for (int sh = 1; sh <= YBR_SHADER_MAX_SHADOWS; sh++) {
		YbrShaderOptions o = YbrShaderOptionsDefaults(rlGetVersion());
		o.lightCount = YBR_SHADER_MAX_LIGHTS;
		o.shadowLights = sh;
		YbrShaderResult r = YbrShaderFromMaterialEx(&m, &o);

		char what[64];
		snprintf(what, sizeof(what), "shadows=%d", sh);
		int ok = compile_result(&r, what);
		if (ok && r.fragmentCode)
			ok = (strstr(r.fragmentCode, "float ybrShadowAt(") != NULL);
		check(ok, what);
		YbrUnloadShaderResult(&r);
	}
	{ /* 影の数は灯数を超えない */
		YbrShaderOptions o = YbrShaderOptionsDefaults(rlGetVersion());
		o.lightCount = 1;
		o.shadowLights = YBR_SHADER_MAX_SHADOWS;
		YbrShaderResult r = YbrShaderFromMaterialEx(&m, &o);
		check(r.fragmentCode && strstr(r.fragmentCode, "ybrShadowMap1") == NULL,
			  "灯数を超える影は出ない");
		YbrUnloadShaderResult(&r);
	}

	/* Toon と Hair も減衰の関数を使うので、まとめて通しておく */
	group("compile: toon / hair with attenuation");
	{
		const YbrShaderNodeType types[] = {YBR_SN_BSDF_TOON, YBR_SN_BSDF_HAIR};
		for (int i = 0; i < 2; i++) {
			ShaderProbe p;
			probe_build(&p, types[i], NULL, NULL);
			YbrShaderOptions o = YbrShaderOptionsDefaults(rlGetVersion());
			o.lightCount = 2;
			YbrShaderResult r = YbrShaderFromMaterialEx(&p.material, &o);
			const char* what = (i == 0) ? "toon x 2 lights" : "hair x 2 lights";
			check(compile_result(&r, what), what);
			YbrUnloadShaderResult(&r);
		}
	}
}

void test_gl_model(void)
{
	group("YbrModel : upload and draw");

	YbrMesh mesh;
	memset(&mesh, 0, sizeof(mesh));
	mesh.id = (char*)"CubeMesh";
	mesh.vertexCount = 8;
	mesh.triangleCount = 12;
	mesh.positions = cubePos;
	mesh.indices = cubeIdx;

	YbrMaterial mat = gl_simple_material();
	mat.id = (char*)"Mat";
	char* matNames[1] = {(char*)"Mat"};
	mesh.materialCount = 1;
	mesh.materials = matNames;

	YbrNode root;
	memset(&root, 0, sizeof(root));
	root.name = (char*)"Cube";
	root.type = YBR_NODE_MESH;
	root.dataId = (char*)"CubeMesh";
	root.matrix = MatrixIdentity();

	YbrScene sc;
	memset(&sc, 0, sizeof(sc));
	sc.meshCount = 1;
	sc.meshes = &mesh;
	sc.materialCount = 1;
	sc.materials = &mat;
	sc.rootCount = 1;
	sc.roots = &root;

	/* ライトの数はモデルの生成設定ではなく YbrWorld の話なので、
	 * ここではモデルを 1 度だけ作って灯数を world 側で振る。 */
	for (int lights = 0; lights <= 2; lights++) {
		YbrModelOptions o = YbrModelOptionsDefaults();
		YbrModel* m = YbrModelLoad(&sc, &o);
		char what[64];

		snprintf(what, sizeof(what), "YbrModelLoad (world lights=%d)", lights);
		check(m != NULL, what);
		if (!m) continue;

		YbrModelInstance* inst = YbrModelInstanceCreate(m);
		check(inst != NULL, "インスタンスを作れる");
		if (!inst) {
			YbrModelUnload(m);
			continue;
		}

		check(m->partCount == 1, "パートが 1 つ");
		check(m->parts[0].vaoId != 0, "VAO ができている");
		check(m->materials[0].shader.id != 0, "シェーダーが作られている");
		check(m->materials[0].locLightDir[0] >= 0,
			  "ライトの uniform は常に焼き込まれている");

		/* 灯数は world だけで決まる */
		YbrWorld* lw = YbrWorldCreate();
		YbrWorldSetLightCount(lw, lights);
		check(YbrWorldGetLightCount(lw) == lights, "world に灯数が入る");

		Vector3 lo, hi;
		check(YbrModelInstanceGetBounds(inst, &lo, &hi) == 1, "AABB が取れる");

		/* 実際に 1 フレーム描く */
		Camera cam = {0};
		cam.position = (Vector3){3, 3, 3};
		cam.target = (Vector3){0, 0, 0};
		cam.up = (Vector3){0, 1, 0};
		cam.fovy = 45.0f;
		cam.projection = CAMERA_PERSPECTIVE;

		BeginDrawing();
		ClearBackground(BLACK);
		BeginMode3D(cam);
		YbrModelInstanceDraw(inst, NULL, WHITE);
		YbrModelInstanceDrawWires(inst, NULL, GRAY);
		YbrModelInstanceSetPartVisible(inst, 0, 0);
		YbrModelInstanceDraw(inst, NULL, WHITE); /* 非表示なので何も出ない */
		YbrModelInstanceSetPartVisible(inst, 0, 1);
		EndMode3D();
		EndDrawing();
		check(1, "描画で落ちない");

		YbrWorldUnload(lw);
		YbrModelInstanceUnload(inst);
		YbrModelUnload(m);
	}

	/* --- 半透明の 2 パス描画 --- */
	mat.transparent = 1;
	YbrModel* m = YbrModelLoad(&sc, NULL);
	check(m != NULL, "半透明マテリアルでも読める");
	if (m) {
		YbrModelInstance* inst = YbrModelInstanceCreate(m);
		check(inst != NULL, "インスタンスを作れる");
		check(YbrModelHasTransparent(m) == 1, "半透明ありと分かる");
		Camera cam = {0};
		cam.position = (Vector3){3, 3, 3};
		cam.up = (Vector3){0, 1, 0};
		cam.fovy = 45.0f;
		cam.projection = CAMERA_PERSPECTIVE;
		check(YbrModelIsPartTransparent(m, 0) == 1, "パートが半透明と分かる");

		BeginDrawing();
		BeginMode3D(cam);
		YbrModelInstanceDraw(inst, NULL, WHITE);
		/* パスを分けて描く (複数体を並べるときの使い方) */
		YbrModelInstanceDrawPass(inst, NULL, WHITE, YBR_DRAW_OPAQUE);
		YbrModelInstanceDrawPass(inst, NULL, WHITE, YBR_DRAW_TRANSPARENT);
		YbrModelInstanceDrawWiresPass(inst, NULL, GRAY, YBR_DRAW_ALL);
		YbrModelInstanceDrawPass(inst, NULL, WHITE, 0); /* 何も描かない */
		EndMode3D();
		EndDrawing();
		check(1, "半透明パスで落ちない");
		YbrModelInstanceUnload(inst);
		YbrModelUnload(m);
	}
}

/* 影 : 深度テクスチャを作って焼き、キューをそのまま描く */
void test_gl_shadows(void)
{
	group("YbrWorld : shadows");

	YbrMesh mesh;
	memset(&mesh, 0, sizeof(mesh));
	mesh.id = (char*)"CubeMesh";
	mesh.vertexCount = 8;
	mesh.triangleCount = 12;
	mesh.positions = cubePos;
	mesh.indices = cubeIdx;

	YbrMaterial mat = gl_simple_material();
	mat.id = (char*)"Mat";
	char* matNames[1] = {(char*)"Mat"};
	mesh.materialCount = 1;
	mesh.materials = matNames;

	YbrNode root;
	memset(&root, 0, sizeof(root));
	root.name = (char*)"Cube";
	root.type = YBR_NODE_MESH;
	root.dataId = (char*)"CubeMesh";
	root.matrix = MatrixIdentity();

	YbrScene sc;
	memset(&sc, 0, sizeof(sc));
	sc.meshCount = 1;
	sc.meshes = &mesh;
	sc.materialCount = 1;
	sc.materials = &mat;
	sc.rootCount = 1;
	sc.roots = &root;

	YbrModel* m = YbrModelLoad(&sc, NULL);
	check(m != NULL, "モデルを読める");
	if (!m) return;
	YbrModelInstance* inst = YbrModelInstanceCreate(m);
	check(inst != NULL, "インスタンスを作れる");
	if (!inst) {
		YbrModelUnload(m);
		return;
	}

	Camera cam = {0};
	cam.position = (Vector3){4, 4, 4};
	cam.target = (Vector3){0, 0, 0};
	cam.up = (Vector3){0, 1, 0};
	cam.fovy = 45.0f;
	cam.projection = CAMERA_PERSPECTIVE;

	YbrWorldOptions wo = YbrWorldOptionsDefaults();
	wo.shadows = 1;
	wo.shadowResolution = 512;
	wo.shadowLights = 1;
	YbrWorld* sw = YbrWorldCreateEx(&wo);
	check(sw != NULL, "影つきの world を作れる");
	if (!sw) {
		YbrModelInstanceUnload(inst);
		YbrModelUnload(m);
		return;
	}
	YbrWorldSetLight(sw, 0, (Vector3){-0.5f, -1.0f, -0.3f}, WHITE);
	YbrWorldSetCamera(sw, cam, (float)GetScreenWidth() / GetScreenHeight(),
					  0.0f, 0.0f);

	YbrWorldBeginFrame(sw);
	check(0 < YbrWorldSubmit(sw, inst, WHITE), "キューに積める");
	check(0 < YbrWorldGetQueueCount(sw), "キューの数を読める");

	int baked = YbrWorldRenderShadows(sw);
	check(baked == 1, "深度テクスチャを 1 枚焼ける");
	Texture2D map = YbrWorldGetShadowMap(sw, 0);
	check(map.id != 0, "深度テクスチャができている");
	check(map.width == 512, "指定した解像度になる");

	BeginDrawing();
	ClearBackground(BLACK);
	BeginMode3D(cam);
	YbrWorldDrawQueue(sw);
	EndMode3D();
	EndDrawing();
	check(1, "影つきで描いて落ちない");

	/* 影を焼いた world をそのまま捨てられる (深度テクスチャもここで消える) */
	YbrWorldUnload(sw);
	check(1, "影つきの world を解放できる");

	YbrModelInstanceUnload(inst);
	YbrModelUnload(m);
}

/* 与えられた .ybr を実際に GPU へ載せてみる */
void test_gl_scene_file(const char* path)
{
	group("scene file (GPU)");

	YbrScene* sc = YbrLoad(path);
	check(sc != NULL, "読み込める");
	if (!sc) return;

	YbrModel* m = YbrModelLoad(sc, NULL);
	check(m != NULL, "YbrModelLoad");
	if (m) {
		YbrModelInstance* inst = YbrModelInstanceCreate(m);
		check(inst != NULL, "インスタンスを作れる");
		printf("  parts=%d nodes=%d materials=%d skinning=%s transparent=%s\n",
			   m->partCount, m->nodeCount, m->materialCount,
			   m->gpuSkinning ? "GPU" : "CPU",
			   YbrModelHasTransparent(m) ? "yes" : "no");
		printf("  textures loaded: %d (shared)\n", m->texCacheCount);

		int shaders = 0;
		for (int i = 0; i < m->materialCount; i++)
			if (m->materials[i].shader.id != 0) shaders++;
		check(shaders == m->materialCount, "全マテリアルのシェーダーが通る");

		/* ポーズ追従 AABB の前計算 (要るときだけ作るもの) */
		YbrSkinBounds* sb = YbrSkinBoundsCreate(m);
		if (sb) {
			YbrModelInstanceSetSkinBounds(inst, sb);
			check(YbrModelInstanceGetSkinBounds(inst) == sb,
				  "前計算を紐づけられる");
			check(0 < sb->boneCount, "ボーンぶんの箱がある");
		}
		else {
			printf("  (skinned parts not found; skin bounds not needed)\n");
		}

		Camera cam = {0};
		Vector3 lo, hi;
		YbrModelInstanceGetBounds(inst, &lo, &hi);
		Vector3 c = Vector3Scale(Vector3Add(lo, hi), 0.5f);
		float span = Vector3Distance(lo, hi);
		if (!(0.0f < span)) span = 4.0f;
		cam.position = (Vector3){c.x + span, c.y + span, c.z + span};
		cam.target = c;
		cam.up = (Vector3){0, 1, 0};
		cam.fovy = 45.0f;
		cam.projection = CAMERA_PERSPECTIVE;

		BeginDrawing();
		ClearBackground(BLACK);
		BeginMode3D(cam);
		YbrModelInstanceDraw(inst, NULL, WHITE);
		EndMode3D();
		EndDrawing();
		check(1, "描画で落ちない");

		YbrModelInstanceUnload(inst);
		YbrSkinBoundsUnload(sb);
		YbrModelUnload(m);
	}
	YbrUnload(sc);
}
