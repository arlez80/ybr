/*
	Yui Blender to Raylib - 描画環境 (YbrWorld) のテスト
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_test.h"

/* ================================================================== */
/* シーンのライト / カメラ                                            */
/* ================================================================== */
void test_scene_lights_camera(void)
{
	group("scene lights / camera");

	/* --- ライト --- */
	YbrLight sun;
	memset(&sun, 0, sizeof(sun));
	sun.id = (char*)"Sun";
	sun.type = YBR_LIGHT_SUN;
	sun.color = V(1.0f, 0.5f, 0.25f);
	sun.energy = 1.0f;

	YbrNode lightNode;
	memset(&lightNode, 0, sizeof(lightNode));
	lightNode.name = (char*)"SunNode";
	lightNode.type = YBR_NODE_LIGHT;
	lightNode.dataId = (char*)"Sun";
	/* -Z を照らす向き = 単位行列なら (0, 0, -1) */
	lightNode.matrix = MatrixIdentity();

	YbrCamera cam;
	memset(&cam, 0, sizeof(cam));
	cam.id = (char*)"Cam";
	cam.type = YBR_CAMERA_PERSP;
	cam.fovX = 1.0f;
	cam.fovY = 0.7f;
	cam.sensorFit = YBR_SENSOR_FIT_VERTICAL;
	cam.clipStart = 0.1f;
	cam.clipEnd = 100.0f;

	YbrNode camNode;
	memset(&camNode, 0, sizeof(camNode));
	camNode.name = (char*)"CamNode";
	camNode.type = YBR_NODE_CAMERA;
	camNode.dataId = (char*)"Cam";
	camNode.matrix = MatrixTranslate(1.0f, 2.0f, 3.0f);

	YbrNode roots[2] = {lightNode, camNode};

	YbrScene sc;
	memset(&sc, 0, sizeof(sc));
	sc.lightCount = 1;
	sc.lights = &sun;
	sc.cameraCount = 1;
	sc.cameras = &cam;
	sc.rootCount = 2;
	sc.roots = roots;

	/* --- カメラ --- */
	Camera3D out;
	float nearP = 0.0f, farP = 0.0f;
	memset(&out, 0, sizeof(out));
	check(YbrCameraToRaylib(&sc, NULL, 16.0f / 9.0f, &out, &nearP, &farP) == 1,
		  "カメラを変換できる");
	check_near(nearP, 0.1f, 1e-5f, "clip_start が返る");
	check_near(farP, 100.0f, 1e-3f, "clip_end が返る");
	check_vec(out.position, V(1.0f, 2.0f, 3.0f), 1e-4f, "位置がノードから来る");
	check_vec(Vector3Subtract(out.target, out.position), V(0.0f, 0.0f, -1.0f),
			  1e-4f, "-Z を向く");
	check_vec(out.up, V(0.0f, 1.0f, 0.0f), 1e-4f, "+Y が上");
	check(out.projection == CAMERA_PERSPECTIVE, "透視投影");
	check_near(out.fovy, 0.7f * RAD2DEG, 1e-2f,
			   "sensor fit VERTICAL は fovY をそのまま");

	/* 横基準なら aspect で割り戻す */
	cam.sensorFit = YBR_SENSOR_FIT_HORIZONTAL;
	check(YbrCameraToRaylib(&sc, NULL, 2.0f, &out, NULL, NULL) == 1,
		  "横基準でも変換できる");
	check_near(out.fovy, 2.0f * atanf(tanf(0.5f) / 2.0f) * RAD2DEG, 1e-2f,
			   "横の画角から縦を出す");

	cam.type = YBR_CAMERA_ORTHO;
	cam.orthoScale = 8.0f;
	check(YbrCameraToRaylib(&sc, NULL, 1.0f, &out, NULL, NULL) == 1,
		  "平行投影も変換できる");
	check(out.projection == CAMERA_ORTHOGRAPHIC, "平行投影");
	check_near(out.fovy, 8.0f, 1e-4f, "ortho_scale が入る");

	check(YbrCameraToRaylib(&sc, "nope", 1.0f, &out, NULL, NULL) == 0,
		  "無い名前は 0");
	check(YbrCameraToRaylib(NULL, NULL, 1.0f, &out, NULL, NULL) == 0,
		  "NULL は 0");

	/* --- ライトの取り込み (ライトは YbrWorld が持つ) --- */
	YbrWorld* w = YbrWorldCreate();
	if (!w) return;

	check(YbrWorldApplySceneLights(w, &sc) == 1, "ライトを 1 つ取り込む");
	check(YbrWorldGetLightCount(w) == 1, "灯数が取り込んだ数になる");
	const YbrWorldLight* l = YbrWorldGetLight(w, 0);
	check_vec(l->direction, V(0.0f, 0.0f, -1.0f), 1e-4f,
			  "SUN は行列の -Z 方向");
	check_near(l->color.x, 1.0f, 1e-2f, "色が入る");
	check(l->color.y < l->color.x, "色の比が保たれる");

	YbrWorldUnload(w);
}

/* ================================================================== */
/* 視錐台カリング                                                     */
/* ================================================================== */
void test_frustum(void)
{
	group("frustum");

	Camera3D cam;
	memset(&cam, 0, sizeof(cam));
	cam.position = V(0.0f, 0.0f, 10.0f);
	cam.target = V(0.0f, 0.0f, 0.0f);
	cam.up = V(0.0f, 1.0f, 0.0f);
	cam.fovy = 45.0f;
	cam.projection = CAMERA_PERSPECTIVE;

	/* near / far は明示する。0 を渡したときの既定は raylib の
	 * RL_CULL_DISTANCE_FAR に従うので、値を決め打ちにできない。 */
	const float nearP = 0.01f, farP = 100.0f;
	YbrFrustum f = YbrFrustumFromCamera(cam, 16.0f / 9.0f, nearP, farP);

	check(YbrFrustumContainsPoint(&f, V(0, 0, 0)) == 1, "正面は中");
	check(YbrFrustumContainsPoint(&f, V(0, 0, 20)) == 0, "カメラの後ろは外");
	/* カメラは z=10 にいて -Z を向いているので、z=-200 までの距離は 210 > far
	 */
	check(YbrFrustumContainsPoint(&f, V(0, 0, -200)) == 0, "far より遠くは外");
	check(YbrFrustumContainsPoint(&f, V(100, 0, 0)) == 0, "右に外れる");
	check(YbrFrustumContainsPoint(&f, V(0, 100, 0)) == 0, "上に外れる");
	check(YbrFrustumContainsPoint(&f, V(1, 1, 0)) == 1, "少し右上は中");

	check(YbrFrustumContainsSphere(&f, V(0, 0, 0), 1.0f) == 1, "球 : 中");
	check(YbrFrustumContainsSphere(&f, V(0, 100, 0), 1.0f) == 0, "球 : 外");
	check(YbrFrustumContainsSphere(&f, V(0, 100, 0), 200.0f) == 1,
		  "球 : 大きければかすって中");

	check(YbrFrustumContainsBox(&f, V(-1, -1, -1), V(1, 1, 1)) == 1, "箱 : 中");
	check(YbrFrustumContainsBox(&f, V(-1, -1, 50), V(1, 1, 60)) == 0,
		  "箱 : 後方は外");
	check(YbrFrustumContainsBox(&f, V(-500, -500, -500), V(500, 500, 500)) == 1,
		  "箱 : 視錐台をまたぐ");
	check(YbrFrustumContainsBox(NULL, V(-1, -1, -1), V(1, 1, 1)) == 1,
		  "NULL なら常に中");

	/* near / far に 0 を渡したときは raylib の既定が入る。
	 * 距離そのものは raylib のバージョン次第なので、
	 * 「使える視錐台になっている」ことだけ確かめる。 */
	{
		YbrFrustum d = YbrFrustumFromCamera(cam, 16.0f / 9.0f, 0.0f, 0.0f);
		check(YbrFrustumContainsPoint(&d, V(0, 0, 0)) == 1, "既定 : 正面は中");
		check(YbrFrustumContainsPoint(&d, V(0, 0, 20)) == 0, "既定 : 後ろは外");
		check(YbrFrustumContainsPoint(&d, V(100, 0, 0)) == 0, "既定 : 横は外");
	}

	/* 平行投影 */
	cam.projection = CAMERA_ORTHOGRAPHIC;
	cam.fovy = 10.0f; /* 縦の大きさ */
	YbrFrustum g = YbrFrustumFromCamera(cam, 1.0f, 0.0f, 0.0f);
	check(YbrFrustumContainsPoint(&g, V(0, 0, 0)) == 1, "平行投影 : 中");
	check(YbrFrustumContainsPoint(&g, V(8, 0, 0)) == 0,
		  "平行投影 : 横に外れる");

	/* インスタンスのワールド AABB での判定 */
	DynProbe p;
	if (dyn_probe_init(&p, 1) != 1) return;
	YbrModelInstanceSetTransform(p.inst, MatrixIdentity());
	check(YbrModelInstanceIsVisible(p.inst, &f) == 1, "原点の体は見えている");
	YbrModelInstanceSetTransform(p.inst, MatrixTranslate(0.0f, 0.0f, 500.0f));
	check(YbrModelInstanceIsVisible(p.inst, &f) == 0, "後ろへ動かすと外れる");
	check(YbrModelInstanceIsVisible(p.inst, NULL) == 1, "NULL なら常に見える");
	check(YbrModelInstanceIsVisible(NULL, &f) == 0, "NULL のインスタンスは 0");

	/* シーンのカメラから作ると clip_start / clip_end も .ybr の値になる */
	{
		YbrCamera sceneCam;
		memset(&sceneCam, 0, sizeof(sceneCam));
		sceneCam.id = (char*)"Cam";
		sceneCam.type = YBR_CAMERA_PERSP;
		sceneCam.fovX = 1.0f;
		sceneCam.fovY = 0.7f;
		sceneCam.sensorFit = YBR_SENSOR_FIT_VERTICAL;
		sceneCam.clipStart = 0.1f;
		sceneCam.clipEnd = 20.0f; /* 20 より遠くは切る */

		YbrNode camNode;
		memset(&camNode, 0, sizeof(camNode));
		camNode.name = (char*)"CamNode";
		camNode.type = YBR_NODE_CAMERA;
		camNode.dataId = (char*)"Cam";
		camNode.matrix = MatrixTranslate(0.0f, 0.0f, 10.0f);

		YbrScene sc;
		memset(&sc, 0, sizeof(sc));
		sc.cameraCount = 1;
		sc.cameras = &sceneCam;
		sc.rootCount = 1;
		sc.roots = &camNode;

		YbrFrustum sf = YbrFrustumFromScene(&sc, NULL, 1.0f);
		check(YbrFrustumContainsPoint(&sf, V(0, 0, 0)) == 1,
			  "シーンのカメラ : 中");
		check(YbrFrustumContainsPoint(&sf, V(0, 0, -15)) == 0,
			  "clip_end の外は切られる");

		/* カメラが無ければ何も切らない */
		YbrScene empty;
		memset(&empty, 0, sizeof(empty));
		YbrFrustum nf = YbrFrustumFromScene(&empty, NULL, 1.0f);
		check(YbrFrustumContainsPoint(&nf, V(0, 0, 9999)) == 1,
			  "カメラが無ければ何も切らない");
	}

	dyn_probe_free(&p);
}

/* ================================================================== */
/* 点光源 / スポット                                                  */
/* ================================================================== */
void test_point_spot_lights(void)
{
	group("point / spot lights");

	/* --- 生成される GLSL --- */
	YbrMaterial mat;
	memset(&mat, 0, sizeof(mat));
	mat.id = (char*)"Lit";
	mat.mode = YBR_MATERIAL_SIMPLE;
	mat.baseColor = (Vector4){0.8f, 0.8f, 0.8f, 1.0f};
	mat.roughness = 0.5f;
	mat.specular = 0.5f;

	YbrShaderOptions o = YbrShaderOptionsDefaults(RL_OPENGL_33);
	o.lightCount = 2;
	YbrShaderResult r = YbrShaderFromMaterialEx(&mat, &o);
	check(r.error == YBR_SHADER_OK, "2 灯で作れる");
	if (r.fragmentCode) {
		check(strstr(r.fragmentCode, "uniform vec3 ybrLightPos0;") != NULL,
			  "ライトの位置 uniform が出る");
		check(strstr(r.fragmentCode, "uniform vec4 ybrLightParams1;") != NULL,
			  "ライトのパラメータ uniform が出る");
		check(strstr(r.fragmentCode, "ybrLightPos2") == NULL,
			  "灯数ぶんだけ出る");
		check(strstr(r.fragmentCode, "float ybrLightSample(") != NULL,
			  "減衰の関数が出る");
	}
	{ /* uniform 一覧にも並ぶ (既定値は平行光) */
		int pos = 0, par = 0;
		for (int i = 0; i < r.uniformCount; i++) {
			if (!strcmp(r.uniforms[i].name, "ybrLightPos0")) pos = 1;
			if (!strcmp(r.uniforms[i].name, "ybrLightParams0")) {
				par = 1;
				check_near(r.uniforms[i].value[0], 0.0f, 1e-6f,
						   "既定は平行光 (種類 0)");
			}
		}
		check(pos && par, "uniform 一覧に位置とパラメータがある");
	}
	YbrUnloadShaderResult(&r);

	/* 0 灯なら減衰の関数も出ない */
	o.lightCount = 0;
	r = YbrShaderFromMaterialEx(&mat, &o);
	check(r.error == YBR_SHADER_OK && r.fragmentCode &&
			  strstr(r.fragmentCode, "ybrLightSample") == NULL,
		  "0 灯なら減衰の関数も出ない");
	YbrUnloadShaderResult(&r);

	/* --- 設定した値が入るか (ライトは YbrWorld が持つ) --- */
	YbrWorld* w = YbrWorldCreate();
	check(w != NULL, "YbrWorld を作れる");
	if (!w) return;
	check(YbrWorldGetLightCount(w) == 0, "作った直後は 0 灯");

	YbrWorldSetPointLight(w, 0, V(1, 2, 3), (Color){255, 0, 0, 255}, 25.0f,
						  3.0f);
	const YbrWorldLight* l = YbrWorldGetLight(w, 0);
	check(l && l->kind == YBR_LIGHTKIND_POINT, "点光源として入る");
	if (l) {
		check_vec(l->position, V(1, 2, 3), 1e-5f, "位置が入る");
		check_near(l->range, 25.0f, 1e-5f, "届く距離が入る");
		check_near(l->intensity, 3.0f, 1e-5f, "強さが入る");
	}

	YbrWorldSetSpotLight(w, 1, V(0, 5, 0), V(0, -1, 0),
						 (Color){255, 255, 255, 255}, 0.0f, 1.0f, 0.3f, 0.6f);
	l = YbrWorldGetLight(w, 1);
	check(l && l->kind == YBR_LIGHTKIND_SPOT, "スポットとして入る");
	check(l && l->spotInner < l->spotOuter, "内側 < 外側");

	/* 外側 <= 内側 で渡しても縁がぼける形に直る */
	YbrWorldSetSpotLight(w, 2, V(0, 5, 0), V(0, -1, 0),
						 (Color){255, 255, 255, 255}, 0.0f, 1.0f, 0.9f, 0.5f);
	l = YbrWorldGetLight(w, 2);
	check(l && l->spotInner < l->spotOuter, "逆に渡しても直る");

	/* 従来の API は平行光のまま (既存の使い方が壊れない) */
	YbrWorldSetLight(w, 3, V(0, -1, 0), (Color){255, 255, 255, 255});
	l = YbrWorldGetLight(w, 3);
	check(l && l->kind == YBR_LIGHTKIND_DIRECTIONAL, "従来の API は平行光");
	check(l && l->intensity == 1.0f, "既定の強さは 1");
	check(YbrWorldGetLight(w, 99) == NULL, "範囲外は NULL");
	check(YbrWorldGetLightCount(w) == 4, "設定すると灯数が伸びる");
	YbrWorldSetLightCount(w, 2);
	check(YbrWorldGetLightCount(w) == 2, "減らせる");
	YbrWorldSetLightCount(w, 99);
	check(YbrWorldGetLightCount(w) == YBR_WORLD_MAX_LIGHTS, "上限で止まる");

	/* --- シーンからの取り込み --- */
	YbrLight lights[2];
	memset(lights, 0, sizeof(lights));
	lights[0].id = (char*)"Pt";
	lights[0].type = YBR_LIGHT_POINT;
	lights[0].color = V(1.0f, 1.0f, 1.0f);
	lights[0].energy = 1000.0f;
	lights[1].id = (char*)"Sp";
	lights[1].type = YBR_LIGHT_SPOT;
	lights[1].color = V(1.0f, 1.0f, 1.0f);
	lights[1].energy = 500.0f;
	lights[1].spotSize = 1.0f; /* 全角 1 rad */
	lights[1].spotBlend = 0.5f;

	YbrNode nodes[2];
	memset(nodes, 0, sizeof(nodes));
	nodes[0].name = (char*)"PtNode";
	nodes[0].type = YBR_NODE_LIGHT;
	nodes[0].dataId = (char*)"Pt";
	nodes[0].matrix = MatrixTranslate(2.0f, 3.0f, 4.0f);
	nodes[1].name = (char*)"SpNode";
	nodes[1].type = YBR_NODE_LIGHT;
	nodes[1].dataId = (char*)"Sp";
	nodes[1].matrix = MatrixTranslate(0.0f, 8.0f, 0.0f);

	YbrScene sc;
	memset(&sc, 0, sizeof(sc));
	sc.lightCount = 2;
	sc.lights = lights;
	sc.rootCount = 2;
	sc.roots = nodes;

	check(YbrWorldApplySceneLights(w, &sc) == 2, "2 灯を取り込む");
	l = YbrWorldGetLight(w, 0);
	check(l && l->kind == YBR_LIGHTKIND_POINT, "POINT は点光源になる");
	check_vec(l->position, V(2, 3, 4), 1e-4f, "位置がノードから来る");
	check_near(l->intensity, 1000.0f / (4.0f * PI), 1e-2f, "W から強さを出す");

	l = YbrWorldGetLight(w, 1);
	check(l && l->kind == YBR_LIGHTKIND_SPOT, "SPOT はスポットになる");
	check_vec(l->position, V(0, 8, 0), 1e-4f, "スポットの位置");
	check_near(l->spotOuter, 0.5f, 1e-4f, "全角の半分が外側コーン");
	check_near(l->spotInner, 0.25f, 1e-4f, "blend から内側コーン");
	check_vec(l->direction, V(0, 0, -1), 1e-4f, "ライトは -Z を照らす");

	/* 面光源はランバート放射体なので、点光源とは係数が違う (P/pi) */
	lights[0].type = YBR_LIGHT_AREA;
	lights[0].shape = (char*)"SQUARE";
	lights[0].size = 1.0f;
	check(YbrWorldApplySceneLights(w, &sc) == 2, "面光源も取り込む");
	l = YbrWorldGetLight(w, 0);
	check(l && l->kind == YBR_LIGHTKIND_POINT, "AREA は点光源で近似する");
	check_near(l->intensity, 1000.0f / PI, 1e-1f,
			   "AREA は P/pi (点光源の 4 倍)");

	/* --- 灯数の上限を超えて置いたときの選び方 --- */
	{
		YbrWorld* big = YbrWorldCreate();
		/* 遠い点光源をたくさん + 近い点光源を 1 つ + 平行光 1 つ */
		for (int i = 0; i < 10; i++)
			YbrWorldSetPointLight(big, i, V(1000.0f + i, 0, 0),
								  (Color){255, 255, 255, 255}, 0.0f, 1.0f);
		YbrWorldSetPointLight(big, 10, V(0.2f, 0.2f, 0.2f),
							  (Color){255, 255, 255, 255}, 0.0f, 1.0f);
		YbrWorldSetLight(big, 11, V(0, -1, 0), (Color){255, 255, 255, 255});
		check(YbrWorldGetLightCount(big) == 12,
			  "12 灯置ける (シェーダーは 4 灯)");

		DynProbe q;
		if (dyn_probe_init(&q, 1) == 1) {
			int pick[YBR_WORLD_MAX_ACTIVE_LIGHTS];
			int n = YbrWorldPickLights(big, q.inst, pick,
									   YBR_WORLD_MAX_ACTIVE_LIGHTS);
			check(n == YBR_WORLD_MAX_ACTIVE_LIGHTS, "上限ぶんだけ選ぶ");

			int hasSun = 0, hasNear = 0;
			for (int i = 0; i < n; i++) {
				if (pick[i] == 11) hasSun = 1;
				if (pick[i] == 10) hasNear = 1;
			}
			check(hasSun, "平行光は必ず選ばれる");
			check(hasNear, "近い点光源が選ばれる");
			check(pick[0] == 11, "平行光が先頭 (距離に関係なく効くため)");

			/* 届く距離の外にある灯は落とす */
			YbrWorld* lim = YbrWorldCreate();
			YbrWorldSetPointLight(lim, 0, V(100, 0, 0),
								  (Color){255, 255, 255, 255}, 1.0f, 1.0f);
			check(YbrWorldPickLights(lim, q.inst, pick,
									 YBR_WORLD_MAX_ACTIVE_LIGHTS) == 0,
				  "届かない灯は選ばれない");
			YbrWorldUnload(lim);
			dyn_probe_free(&q);
		}
		YbrWorldUnload(big);
	}

	/* --- カメラと視錐台 --- */
	{
		YbrWorld* cw = YbrWorldCreate();
		check(YbrWorldGetFrustum(cw) == NULL, "カメラを入れるまで視錐台は無い");

		Camera3D cam;
		memset(&cam, 0, sizeof(cam));
		cam.position = V(0, 0, 10);
		cam.target = V(0, 0, 0);
		cam.up = V(0, 1, 0);
		cam.fovy = 45.0f;
		cam.projection = CAMERA_PERSPECTIVE;
		YbrWorldSetCamera(cw, cam, 16.0f / 9.0f, 0.0f, 0.0f);

		const YbrFrustum* f = YbrWorldGetFrustum(cw);
		check(f != NULL, "カメラを入れると視錐台ができる");
		check(YbrFrustumContainsPoint(f, V(0, 0, 0)) == 1, "正面は中");
		check(YbrFrustumContainsPoint(f, V(0, 0, 50)) == 0, "後ろは外");

		Camera3D out;
		check(YbrWorldGetCamera(cw, &out) == 1, "カメラを読み戻せる");
		check_vec(out.position, V(0, 0, 10), 1e-5f, "位置が一致する");

		YbrWorldSetCulling(cw, 0);
		check(YbrWorldGetFrustum(cw) == NULL,
			  "カリングを切ると視錐台を返さない");
		YbrWorldSetCulling(cw, 1);
		check(YbrWorldGetFrustum(cw) != NULL, "戻せる");
		YbrWorldUnload(cw);
	}

	/* --- 影の設定 (GPU に触らない部分だけ) --- */
	{
		YbrWorldOptions def = YbrWorldOptionsDefaults();
		check(def.shadows == 0, "既定では影を使わない");
		check(def.shadowResolution == 2048, "既定の解像度");
		check(def.shadowLights == 1, "既定の影の灯数");

		YbrWorldOptions wo = def;
		wo.shadows = 1;
		wo.shadowResolution = 0; /* 変な値は直される */
		wo.shadowLights = 99;
		wo.shadowBias = -1.0f;
		YbrWorld* sw = YbrWorldCreateEx(&wo);
		check(sw != NULL, "影ありの world を作れる");
		if (sw) {
			check(sw->options.shadowResolution == 2048, "0 は既定へ直る");
			check(sw->options.shadowLights == YBR_WORLD_MAX_SHADOWS,
				  "上限で止まる");
			check(0.0f < sw->options.shadowBias, "負のバイアスは直る");

			/* 影を落とせるのは平行光とスポットだけ */
			YbrWorldSetLight(sw, 0, V(0, -1, 0), (Color){255, 255, 255, 255});
			YbrWorldSetPointLight(sw, 1, V(3, 3, 3),
								  (Color){255, 255, 255, 255}, 0, 1.0f);
			YbrWorldSetSpotLight(sw, 2, V(0, 5, 0), V(0, -1, 0),
								 (Color){255, 255, 255, 255}, 10.0f, 1.0f, 0.3f,
								 0.5f);
			check(YbrWorldLightCastsShadow(sw, 0) == 1, "平行光は影を落とせる");
			check(YbrWorldLightCastsShadow(sw, 1) == 0, "点光源は落とせない");
			check(YbrWorldLightCastsShadow(sw, 2) == 1, "スポットは落とせる");

			int pick2[YBR_WORLD_MAX_SHADOWS];
			int n =
				YbrWorldResolveShadowLights(sw, pick2, YBR_WORLD_MAX_SHADOWS);
			check(n == 2, "落とせる灯を先頭から拾う");
			check(pick2[0] == 0 && pick2[1] == 2, "点光源は飛ばされる");

			/* 明示的に指定できる */
			YbrWorldSetShadowLight(sw, 0, 2);
			n = YbrWorldResolveShadowLights(sw, pick2, YBR_WORLD_MAX_SHADOWS);
			check(0 < n && pick2[0] == 2, "指定した灯が使われる");
			YbrWorldSetShadowLight(sw, 0, -1);

			/* ライト視点の行列 : 平行光の正射影は光の向きを向いている */
			Matrix vp = YbrWorldLightMatrix(sw, 0);
			Vector3 above = Vector3Transform(V(0.0f, 3.0f, 0.0f), vp);
			Vector3 below = Vector3Transform(V(0.0f, -3.0f, 0.0f), vp);
			check(above.z < below.z, "光に近いほど手前 (深度が小さい)");

			/* スポットは透視なので、コーンの外は clip の外へ出る */
			Matrix svp = YbrWorldLightMatrix(sw, 2);
			Vector3 center2 = Vector3Transform(V(0, 0, 0), svp);
			check(fabsf(center2.x) < 1.0f && fabsf(center2.y) < 1.0f,
				  "スポットの正面は視錐台の中");

			check(YbrWorldGetShadowMap(sw, 0).id == 0,
				  "焼く前は深度テクスチャ無し");
			check(YbrWorldGetShadowMap(sw, 99).id == 0, "範囲外は空");
			YbrWorldUnload(sw);
		}
	}

	check(YbrWorldApplySceneLights(w, NULL) == 0, "NULL のシーンは 0");
	check(YbrWorldApplySceneLights(NULL, &sc) == 0, "NULL の world は 0");
	YbrWorldUnload(w);
	YbrWorldUnload(NULL); /* 二重解放しないこと */
}
