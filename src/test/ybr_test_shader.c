/*
	Yui Blender to Raylib - シェーダー生成のテスト
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_test.h"

/* 変換して成功したら 1。needle が非 NULL ならその文字列が含まれるかも見る */
static int probe_convert(YbrShaderNodeType type, const char* propName,
						 const char* propValue, const char* needle,
						 const char* what)
{
	ShaderProbe p;
	probe_build(&p, type, propName, propValue);

	YbrShaderResult r = YbrShaderFromMaterial(&p.material, RL_OPENGL_33);
	int ok = (r.error == YBR_SHADER_OK) && r.fragmentCode && r.vertexCode;
	if (!ok) {
		printf("  (%s -> %s: %s)\n", what, YbrShaderErrorString(r.error),
			   r.errorMessage ? r.errorMessage : "");
	}
	else if (needle && !strstr(r.fragmentCode, needle)) {
		printf("  (%s: generated GLSL does not contain '%s')\n", what, needle);
		ok = 0;
	}
	YbrUnloadShaderResult(&r);
	return ok;
}

void test_shader_nodes(void)
{
	group("shader nodes");

	/* --- プロシージャルテクスチャ --- */
	check(
		probe_convert(YBR_SN_TEX_NOISE, NULL, NULL, "ybrNoise01", "TEX_NOISE"),
		"TEX_NOISE");
	check(probe_convert(YBR_SN_TEX_WHITE_NOISE, NULL, NULL, "ybrHash13",
						"TEX_WHITE_NOISE"),
		  "TEX_WHITE_NOISE");
	check(probe_convert(YBR_SN_TEX_CHECKER, NULL, NULL, NULL, "TEX_CHECKER"),
		  "TEX_CHECKER");
	check(probe_convert(YBR_SN_TEX_BRICK, NULL, NULL, NULL, "TEX_BRICK"),
		  "TEX_BRICK");
	check(probe_convert(YBR_SN_TEX_MAGIC, NULL, NULL, "ybrMagic", "TEX_MAGIC"),
		  "TEX_MAGIC");
	check(probe_convert(YBR_SN_TEX_IMAGE, NULL, NULL, "sampler2D", "TEX_IMAGE"),
		  "TEX_IMAGE");
	check(probe_convert(YBR_SN_TEX_ENVIRONMENT, NULL, NULL, "sampler2D",
						"TEX_ENVIRONMENT"),
		  "TEX_ENVIRONMENT");

	/* Gradient : 全種類 */
	const char* grads[] = {"LINEAR",   "QUADRATIC", "EASING",
						   "DIAGONAL", "SPHERICAL", "QUADRATIC_SPHERE",
						   "RADIAL"};
	int gradOk = 1;
	for (int i = 0; i < (int)(sizeof(grads) / sizeof(grads[0])); i++)
		if (!probe_convert(YBR_SN_TEX_GRADIENT, "gradient_type", grads[i], NULL,
						   grads[i]))
			gradOk = 0;
	check(gradOk, "TEX_GRADIENT : 7 種類すべて");

	/* Voronoi : feature x distance */
	const char* feats[] = {"F1", "SMOOTH_F1", "F2", "DISTANCE_TO_EDGE",
						   "N_SPHERE_RADIUS"};
	const char* metrics[] = {"EUCLIDEAN", "MANHATTAN", "CHEBYCHEV",
							 "MINKOWSKI"};
	int voroOk = 1;
	for (int i = 0; i < 5; i++)
		if (!probe_convert(YBR_SN_TEX_VORONOI, "feature", feats[i],
						   "ybrVoronoiEx", feats[i]))
			voroOk = 0;
	for (int i = 0; i < 4; i++)
		if (!probe_convert(YBR_SN_TEX_VORONOI, "distance", metrics[i],
						   "ybrVoroDist", metrics[i]))
			voroOk = 0;
	check(voroOk, "TEX_VORONOI : feature 5 種 + 距離 4 種");

	/* Musgrave : 5 種類 */
	const char* mus[] = {"FBM", "MULTIFRACTAL", "RIDGED_MULTIFRACTAL",
						 "HYBRID_MULTIFRACTAL", "HETERO_TERRAIN"};
	int musOk = 1;
	for (int i = 0; i < 5; i++)
		if (!probe_convert(YBR_SN_TEX_MUSGRAVE, "musgrave_type", mus[i],
						   "ybrMusgrave", mus[i]))
			musOk = 0;
	check(musOk, "TEX_MUSGRAVE : 5 種類すべて");

	/* Wave : bands / rings */
	int waveOk =
		probe_convert(YBR_SN_TEX_WAVE, "wave_type", "BANDS", NULL, "BANDS") &&
		probe_convert(YBR_SN_TEX_WAVE, "wave_type", "RINGS", NULL, "RINGS");
	check(waveOk, "TEX_WAVE : BANDS / RINGS");

	/* Sky : 3 種類 */
	int skyOk = probe_convert(YBR_SN_TEX_SKY, "sky_type", "PREETHAM", "ybrSky",
							  "PREETHAM") &&
				probe_convert(YBR_SN_TEX_SKY, "sky_type", "HOSEK_WILKIE",
							  "ybrSky", "HOSEK") &&
				probe_convert(YBR_SN_TEX_SKY, "sky_type", "NISHITA", "ybrSky",
							  "NISHITA");
	check(skyOk, "TEX_SKY : 3 種類すべて");

	check(probe_convert(YBR_SN_TEX_IES, NULL, NULL, NULL, "TEX_IES"),
		  "TEX_IES");
	check(probe_convert(YBR_SN_TEX_POINTDENSITY, NULL, NULL, "ybrPointDensity",
						"TEX_POINTDENSITY"),
		  "TEX_POINTDENSITY");

	/* --- 以前は変換できなかったノード --- */
	check(probe_convert(YBR_SN_BSDF_HAIR, NULL, NULL, NULL, "BSDF_HAIR"),
		  "BSDF_HAIR");
	check(probe_convert(YBR_SN_BSDF_HAIR_PRINCIPLED, NULL, NULL, NULL,
						"BSDF_HAIR_PRINCIPLED"),
		  "BSDF_HAIR_PRINCIPLED");
	check(probe_convert(YBR_SN_GROUP, NULL, NULL, "ybrOut2_0", "GROUP"),
		  "GROUP (出力の既定値で通す)");
	check(probe_convert(YBR_SN_GROUP_INPUT, NULL, NULL, NULL, "GROUP_INPUT"),
		  "GROUP_INPUT");
	check(probe_convert(YBR_SN_GROUP_OUTPUT, NULL, NULL, NULL, "GROUP_OUTPUT"),
		  "GROUP_OUTPUT");

	/* --- 対応表と実装が食い違っていないこと --- */
	int mismatch = 0, supported = 0;
	for (int t = 0; t <= 130; t++) {
		YbrShaderNodeType type = (YbrShaderNodeType)t;
		if (!YbrShaderIsNodeSupported(type)) continue;
		supported++;
		if (!probe_convert(type, NULL, NULL, NULL, YbrShaderNodeTypeName(type)))
			mismatch++;
	}
	check(60 < supported, "対応ノードが十分ある");
	check(mismatch == 0,
		  "YbrShaderIsNodeSupported が 1 のノードはすべて変換できる");

	/* 未知のノードはきちんと失敗する */
	{
		ShaderProbe p;
		probe_build(&p, YBR_SN_UNKNOWN, NULL, NULL);
		YbrShaderResult r = YbrShaderFromMaterial(&p.material, RL_OPENGL_33);
		check(r.error == YBR_SHADER_ERR_UNSUPPORTED_NODE,
			  "未知のノードはエラーになる");
		check(r.errorMessage != NULL, "エラーメッセージが入る");
		YbrUnloadShaderResult(&r);
	}

	/* OpenGL 1.1 は GLSL が無いので弾かれる */
	{
		ShaderProbe p;
		probe_build(&p, YBR_SN_TEX_NOISE, NULL, NULL);
		YbrShaderResult r = YbrShaderFromMaterial(&p.material, RL_OPENGL_11);
		check(r.error == YBR_SHADER_ERR_UNSUPPORTED_GL, "OpenGL 1.1 は非対応");
		YbrUnloadShaderResult(&r);
	}
}

/* ================================================================== */
/* SIMPLE モードのシェーダー / GPU スキニング                         */
/* ================================================================== */
void test_simple_shader(void)
{
	group("simple shader");

	YbrTexture baseMap, roughMap, normMap;
	memset(&baseMap, 0, sizeof(baseMap));
	baseMap.image = (char*)"BaseTex";
	memset(&roughMap, 0, sizeof(roughMap));
	roughMap.image = (char*)"RoughTex";
	memset(&normMap, 0, sizeof(normMap));
	normMap.image = (char*)"NormTex";

	YbrMaterial mat;
	memset(&mat, 0, sizeof(mat));
	mat.id = (char*)"Simple";
	mat.mode = YBR_MATERIAL_SIMPLE;
	mat.baseColor = (Vector4){0.9f, 0.4f, 0.2f, 1.0f};
	mat.metallic = 0.25f;
	mat.roughness = 0.6f;
	mat.specular = 0.5f;
	mat.alpha = 1.0f;

	/* --- マップ無し --- */
	YbrShaderResult r = YbrShaderFromSimpleMaterial(&mat, RL_OPENGL_33);
	check(r.error == YBR_SHADER_OK, "SIMPLE マテリアルから作れる");
	check(r.vertexCode && r.fragmentCode, "頂点 / フラグメントの両方が出る");
	if (r.fragmentCode) {
		check(strstr(r.fragmentCode, "colDiffuse") != NULL,
			  "colDiffuse を使う");
		check(strstr(r.fragmentCode, "ybrMetallic") != NULL, "metallic を使う");
		check(strstr(r.fragmentCode, "ybrRoughnessMap") == NULL,
			  "マップが無ければ宣言もされない");
	}
	check(0 < r.uniformCount, "uniform が列挙される");
	YbrUnloadShaderResult(&r);

	/* --- マップ付き --- */
	mat.baseColorMap = &baseMap;
	mat.roughnessMap = &roughMap;
	mat.normalMap = &normMap;
	mat.normalStrength = 0.8f;
	r = YbrShaderFromSimpleMaterial(&mat, RL_OPENGL_33);
	check(r.error == YBR_SHADER_OK, "マップ付きでも作れる");
	if (r.fragmentCode) {
		check(strstr(r.fragmentCode, "ybrRoughnessMap") != NULL,
			  "roughness マップが入る");
		check(strstr(r.fragmentCode, "ybrNormalMap") != NULL,
			  "normal マップが入る");
		check(strstr(r.fragmentCode, "ybrMetallicMap") == NULL,
			  "無いマップは入らない");
	}
	/* サンプラの uniform には参照する TEXTURE ブロックの id が付く */
	{
		int found = 0;
		for (int i = 0; i < r.uniformCount; i++)
			if (r.uniforms[i].type == YBR_UNIFORM_SAMPLER2D &&
				r.uniforms[i].textureId &&
				!strcmp(r.uniforms[i].textureId, "RoughTex"))
				found = 1;
		check(found, "サンプラにテクスチャ ID が付く");
	}
	YbrUnloadShaderResult(&r);

	/* --- GLSL ES 2.0 でも作れる --- */
	r = YbrShaderFromSimpleMaterial(&mat, RL_OPENGL_ES_20);
	check(r.error == YBR_SHADER_OK, "GLSL ES 2.0 でも作れる");
	if (r.fragmentCode)
		check(strstr(r.fragmentCode, "texture2D(") != NULL,
			  "ES では texture2D を使う");
	YbrUnloadShaderResult(&r);

	r = YbrShaderFromSimpleMaterial(&mat, RL_OPENGL_11);
	check(r.error == YBR_SHADER_ERR_UNSUPPORTED_GL, "OpenGL 1.1 は非対応");
	YbrUnloadShaderResult(&r);
}

void test_gpu_skinning_shader(void)
{
	group("gpu skinning shader");

	YbrMaterial mat;
	memset(&mat, 0, sizeof(mat));
	mat.id = (char*)"Skinned";
	mat.mode = YBR_MATERIAL_SIMPLE;
	mat.baseColor = (Vector4){1.0f, 1.0f, 1.0f, 1.0f};
	mat.roughness = 0.5f;

	/* --- スキニング無し --- */
	YbrShaderOptions o = YbrShaderOptionsDefaults(RL_OPENGL_33);
	YbrShaderResult r = YbrShaderFromMaterialEx(&mat, &o);
	check(r.error == YBR_SHADER_OK, "スキニング無しで作れる");
	if (r.vertexCode)
		check(strstr(r.vertexCode, "vertexBoneIds") == NULL,
			  "ボーン属性は出ない");
	YbrUnloadShaderResult(&r);

	/* --- スキニング有り --- */
	o.skinning = 1;
	o.maxBones = 32;
	r = YbrShaderFromMaterialEx(&mat, &o);
	check(r.error == YBR_SHADER_OK, "スキニング有りで作れる");
	if (r.vertexCode) {
		check(strstr(r.vertexCode, "vertexBoneIds") != NULL,
			  "ボーン番号の属性が出る");
		check(strstr(r.vertexCode, "vertexBoneWeights") != NULL,
			  "ウェイトの属性が出る");
		/* mat4 ではなく vec4 の配列で受け取る (rlSetUniform だけで送れる) */
		check(strstr(r.vertexCode, "uniform vec4 boneMatrices[128]") != NULL,
			  "boneMatrices は vec4 x (4*maxBones)");
		check(strstr(r.vertexCode, "ybrBoneMatrix") != NULL,
			  "行列の組み立て関数が出る");
		/* 長い GLSL が途中で切れていないこと */
		check(strstr(r.vertexCode, "mat3(skin)*vertexNormal") != NULL,
			  "法線もスキニングされる (出力が途中で切れていない)");
		check(strstr(r.vertexCode, "gl_Position = mvp*vec4(pos, 1.0);") != NULL,
			  "スキニング後の座標を使う");
	}
	YbrUnloadShaderResult(&r);

	/* --- 上限のクランプ --- */
	o.maxBones = 100000;
	r = YbrShaderFromMaterialEx(&mat, &o);
	check(r.error == YBR_SHADER_OK, "極端な maxBones でも作れる");
	if (r.vertexCode) {
		char expect[64];
		snprintf(expect, sizeof(expect), "uniform vec4 boneMatrices[%d]",
				 YBR_SHADER_MAX_BONES_CAP * 4);
		check(strstr(r.vertexCode, expect) != NULL,
			  "maxBones は上限で丸められる");
	}
	YbrUnloadShaderResult(&r);

	/* --- GLSL ES 2.0 でもスキニングできる --- */
	o.maxBones = 16;
	o.glVersion = RL_OPENGL_ES_20;
	r = YbrShaderFromMaterialEx(&mat, &o);
	check(r.error == YBR_SHADER_OK, "ES 2.0 でもスキニングできる");
	if (r.vertexCode)
		check(strstr(r.vertexCode, "attribute vec4 vertexBoneIds") != NULL,
			  "ES では attribute で宣言される");
	YbrUnloadShaderResult(&r);

	/* --- PRO で変換できないノードがあれば SIMPLE に落ちる --- */
	{
		YbrShaderNode bad;
		memset(&bad, 0, sizeof(bad));
		bad.name = (char*)"Bad";
		bad.type = YBR_SN_UNKNOWN;

		YbrMaterial pro = mat;
		pro.mode = YBR_MATERIAL_PRO;
		pro.nodeCount = 1;
		pro.nodes = &bad;

		YbrShaderOptions po = YbrShaderOptionsDefaults(RL_OPENGL_33);
		YbrShaderResult pr = YbrShaderFromMaterialEx(&pro, &po);
		check(pr.error == YBR_SHADER_OK,
			  "PRO が変換できなければ SIMPLE で作り直す");
		if (pr.fragmentCode)
			check(strstr(pr.fragmentCode, "colDiffuse") != NULL,
				  "SIMPLE の内容になっている");
		YbrUnloadShaderResult(&pr);
	}
}

/* ================================================================== */
/* ノードグループ                                                     */
/* ================================================================== */
/* グループ : 入力 Color を 2 乗して返す (Math の MULTIPLY) */
typedef struct GroupProbe {
	/* --- グループの中身 --- */
	YbrShaderNode gnodes[3];
	YbrShaderLink glinks[3];
	YbrShaderSocket giOut[1], mathIn[3], mathOut[1], goIn[1];
	YbrShaderSocket ifIn[1], ifOut[1];
	YbrProp mathProp;
	YbrNodeGroup group;

	/* --- マテリアル --- */
	YbrShaderNode nodes[3];
	YbrShaderLink links[4];
	YbrShaderSocket outIn[1], bsdfIn[2], bsdfOut[1], grpIn[1], grpOut[1];
	YbrProp grpProp;
	YbrMaterial material;
	YbrScene scene;
} GroupProbe;

static void group_probe_build(GroupProbe* p, const char* groupId,
							  int withOutput)
{
	memset(p, 0, sizeof(*p));

	/* --- グループの中身 --- */
	p->giOut[0] = ssock("Color", YBR_SS_RGBA, 4, 1.0f);
	p->mathIn[0] = ssock("Value", YBR_SS_VALUE, 1, 0.0f);
	p->mathIn[1] = ssock("Value_001", YBR_SS_VALUE, 1, 0.0f);
	p->mathIn[2] = ssock("Value_002", YBR_SS_VALUE, 1, 0.0f);
	p->mathOut[0] = ssock("Value", YBR_SS_VALUE, 1, 0.0f);
	p->goIn[0] = ssock("Color", YBR_SS_RGBA, 4, 0.0f);

	p->mathProp.name = (char*)"operation";
	p->mathProp.type = YBR_PROP_STRING;
	p->mathProp.text = (char*)"MULTIPLY";

	p->gnodes[0].name = (char*)"Group Input";
	p->gnodes[0].type = YBR_SN_GROUP_INPUT;
	p->gnodes[0].outputCount = 1;
	p->gnodes[0].outputs = p->giOut;

	p->gnodes[1].name = (char*)"Math";
	p->gnodes[1].type = YBR_SN_MATH;
	p->gnodes[1].inputCount = 3;
	p->gnodes[1].inputs = p->mathIn;
	p->gnodes[1].outputCount = 1;
	p->gnodes[1].outputs = p->mathOut;
	p->gnodes[1].propCount = 1;
	p->gnodes[1].props = &p->mathProp;

	p->gnodes[2].name = (char*)"Group Output";
	p->gnodes[2].type = YBR_SN_GROUP_OUTPUT;
	p->gnodes[2].inputCount = 1;
	p->gnodes[2].inputs = p->goIn;

	p->glinks[0] = (YbrShaderLink){0, 0, 1, 0};
	p->glinks[1] = (YbrShaderLink){0, 0, 1, 1};
	p->glinks[2] = (YbrShaderLink){1, 0, 2, 0};

	p->ifIn[0] = ssock("Color", YBR_SS_RGBA, 4, 1.0f);
	p->ifOut[0] = ssock("Color", YBR_SS_RGBA, 4, 1.0f);

	p->group.id = (char*)"Square";
	p->group.inputCount = 1;
	p->group.inputs = p->ifIn;
	p->group.outputCount = 1;
	p->group.outputs = p->ifOut;
	p->group.nodes = p->gnodes;
	p->group.nodeCount = withOutput ? 3 : 2; /* GROUP_OUTPUT を外せる */
	p->group.links = p->glinks;
	p->group.linkCount = withOutput ? 3 : 2;

	/* --- マテリアル --- */
	p->outIn[0] = ssock("Surface", YBR_SS_SHADER, 0, 0.0f);
	p->bsdfIn[0] = ssock("Base Color", YBR_SS_RGBA, 4, 0.8f);
	p->bsdfIn[1] = ssock("Roughness", YBR_SS_VALUE, 1, 0.5f);
	p->bsdfOut[0] = ssock("BSDF", YBR_SS_SHADER, 0, 0.0f);
	p->grpIn[0] = ssock("Color", YBR_SS_RGBA, 4, 0.25f);
	p->grpOut[0] = ssock("Color", YBR_SS_RGBA, 4, 0.0f);

	p->grpProp.name = (char*)"node_tree";
	p->grpProp.type = YBR_PROP_STRING;
	p->grpProp.text = (char*)groupId;

	p->nodes[0].name = (char*)"Material Output";
	p->nodes[0].type = YBR_SN_OUTPUT_MATERIAL;
	p->nodes[0].inputCount = 1;
	p->nodes[0].inputs = p->outIn;

	p->nodes[1].name = (char*)"Principled BSDF";
	p->nodes[1].type = YBR_SN_BSDF_PRINCIPLED;
	p->nodes[1].inputCount = 2;
	p->nodes[1].inputs = p->bsdfIn;
	p->nodes[1].outputCount = 1;
	p->nodes[1].outputs = p->bsdfOut;

	p->nodes[2].name = (char*)"Group";
	p->nodes[2].type = YBR_SN_GROUP;
	p->nodes[2].inputCount = 1;
	p->nodes[2].inputs = p->grpIn;
	p->nodes[2].outputCount = 1;
	p->nodes[2].outputs = p->grpOut;
	p->nodes[2].propCount = 1;
	p->nodes[2].props = &p->grpProp;

	p->links[0] = (YbrShaderLink){2, 0, 1, 0}; /* Group -> Base Color */
	p->links[1] = (YbrShaderLink){1, 0, 0, 0}; /* BSDF  -> Surface    */

	p->material.id = (char*)"GroupMat";
	p->material.mode = YBR_MATERIAL_PRO;
	p->material.nodeCount = 3;
	p->material.nodes = p->nodes;
	p->material.linkCount = 2;
	p->material.links = p->links;

	p->scene.materialCount = 1;
	p->scene.materials = &p->material;
	p->scene.nodeGroupCount = 1;
	p->scene.nodeGroups = &p->group;
}

void test_node_group(void)
{
	group("node group");

	GroupProbe p;
	group_probe_build(&p, "Square", 1);

	/* --- シーンを渡すと関数になる --- */
	YbrShaderOptions o = YbrShaderOptionsDefaults(RL_OPENGL_33);
	o.scene = &p.scene;
	YbrShaderResult r = YbrShaderFromMaterialEx(&p.material, &o);
	check(r.error == YBR_SHADER_OK, "グループ入りのマテリアルを変換できる");
	if (r.fragmentCode) {
		check(strstr(r.fragmentCode, "vec4 ybrGroup0_0(vec4 ybrGArg0)") != NULL,
			  "グループが GLSL の関数になる");
		check(strstr(r.fragmentCode, "node group 'Square'") != NULL,
			  "どのグループか分かるコメントが入る");
		check(strstr(r.fragmentCode, "= ybrGroup0_0(") != NULL,
			  "main から呼ばれる");
		/* 関数定義が main より前にあること */
		const char* fn = strstr(r.fragmentCode, "vec4 ybrGroup0_0(");
		const char* main_ = strstr(r.fragmentCode, "void main()");
		check(fn && main_ && fn < main_, "関数定義は main より前に置かれる");
		/* GROUP_INPUT が引数になっていること */
		check(strstr(r.fragmentCode, "= ybrGArg0;") != NULL,
			  "Group Input が関数の引数になる");
		/* グループ内の uniform 名が親とぶつからないこと */
		check(strstr(r.fragmentCode, "ybrG0In") != NULL,
			  "グループ内の uniform に名前空間が付く");
	}
	YbrUnloadShaderResult(&r);

	/* --- 同じグループを 2 回使っても関数は 1 つ --- */
	{
		GroupProbe q;
		group_probe_build(&q, "Square", 1);
		/* Roughness にも同じグループをつなぐ */
		q.links[1] = (YbrShaderLink){2, 0, 1, 1};
		q.material.linkCount = 3;
		q.links[2] = (YbrShaderLink){1, 0, 0, 0};

		YbrShaderOptions qo = YbrShaderOptionsDefaults(RL_OPENGL_33);
		qo.scene = &q.scene;
		YbrShaderResult qr = YbrShaderFromMaterialEx(&q.material, &qo);
		check(qr.error == YBR_SHADER_OK, "同じグループを 2 か所で使える");
		if (qr.fragmentCode) {
			const char* first =
				strstr(qr.fragmentCode, "vec4 ybrGroup0_0(vec4");
			check(first != NULL, "関数が出る");
			if (first)
				check(strstr(first + 1, "vec4 ybrGroup0_0(vec4") == NULL,
					  "関数定義は 1 つだけ (使い回される)");
		}
		YbrUnloadShaderResult(&qr);
	}

	/* --- シーンを渡さなければ既定値になる (従来の動作) --- */
	o.scene = NULL;
	r = YbrShaderFromMaterialEx(&p.material, &o);
	check(r.error == YBR_SHADER_OK, "シーン無しでも変換は通る");
	if (r.fragmentCode) {
		check(strstr(r.fragmentCode, "ybrGroup") == NULL, "関数化されない");
		check(strstr(r.fragmentCode, "ybrOut2_0") != NULL,
			  "出力ソケットの既定値になる");
	}
	YbrUnloadShaderResult(&r);

	/* --- グループが見つからない --- */
	{
		GroupProbe q;
		group_probe_build(&q, "NoSuchGroup", 1);
		YbrShaderOptions qo = YbrShaderOptionsDefaults(RL_OPENGL_33);
		qo.scene = &q.scene;
		YbrShaderResult qr = YbrShaderFromMaterialEx(&q.material, &qo);
		check(qr.error == YBR_SHADER_OK, "グループが無くても落ちない");
		if (qr.fragmentCode)
			check(strstr(qr.fragmentCode, "ybrGroup") == NULL, "既定値で通る");
		YbrUnloadShaderResult(&qr);
	}

	/* --- GROUP_OUTPUT が無いグループ --- */
	{
		GroupProbe q;
		group_probe_build(&q, "Square", 0);
		YbrShaderOptions qo = YbrShaderOptionsDefaults(RL_OPENGL_33);
		qo.scene = &q.scene;
		YbrShaderResult qr = YbrShaderFromMaterialEx(&q.material, &qo);
		check(qr.error == YBR_SHADER_OK, "出口が無いグループでも落ちない");
		YbrUnloadShaderResult(&qr);
	}

	check(YbrFindNodeGroup(&p.scene, "Square") == &p.group,
		  "YbrFindNodeGroup で引ける");
	check(YbrFindNodeGroup(&p.scene, "Nope") == NULL, "無い名前は NULL");

	/* --- 保存 / 読み込みの往復 --- */
	{
		GroupProbe q;
		group_probe_build(&q, "Square", 1);
		q.scene.version = YBR_SUPPORTED_VERSION;

		size_t size = 0;
		unsigned char* buf = YbrSaveToMemory(&q.scene, &size);
		check(buf != NULL && 0 < size, "ノードグループを書き出せる");
		if (!buf) return;

		YbrScene* ld = YbrLoadFromMemory(buf, size);
		YBR_FREE(buf);
		check(ld != NULL, "書き出したものを読み戻せる");
		if (!ld) return;

		check(ld->nodeGroupCount == 1, "NODEGROUP ブロックが 1 つ");
		const YbrNodeGroup* g = YbrFindNodeGroup(ld, "Square");
		check(g != NULL, "id で引ける");
		if (g) {
			check(g->nodeCount == 3 && g->linkCount == 3, "中身がそのまま残る");
			check(g->inputCount == 1 && g->inputs[0].name &&
					  !strcmp(g->inputs[0].name, "Color"),
				  "インターフェースの入力");
			check(g->outputCount == 1, "インターフェースの出力");
			check(g->nodes[0].type == YBR_SN_GROUP_INPUT, "Group Input が残る");
			check(g->nodes[1].propCount == 1 && g->nodes[1].props[0].text &&
					  !strcmp(g->nodes[1].props[0].text, "MULTIPLY"),
				  "プロパティが残る");
			check(g->links[2].fromNode == 1 && g->links[2].toNode == 2,
				  "リンクが残る");
		}

		/* 読み戻したシーンからでも関数化できる */
		YbrShaderOptions lo = YbrShaderOptionsDefaults(RL_OPENGL_33);
		lo.scene = ld;
		YbrShaderResult lr = YbrShaderFromMaterialEx(&ld->materials[0], &lo);
		check(lr.error == YBR_SHADER_OK, "読み戻したあとも変換できる");
		if (lr.fragmentCode)
			check(strstr(lr.fragmentCode, "ybrGroup0_0") != NULL,
				  "読み戻したあとも関数になる");
		YbrUnloadShaderResult(&lr);
		YbrUnload(ld);
	}
}

/* ================================================================== */
/* ライトの数                                                         */
/* ================================================================== */
void test_light_count(void)
{
	group("light count");

	YbrMaterial mat;
	memset(&mat, 0, sizeof(mat));
	mat.id = (char*)"Lit";
	mat.mode = YBR_MATERIAL_SIMPLE;
	mat.baseColor = (Vector4){0.8f, 0.7f, 0.6f, 1.0f};
	mat.roughness = 0.4f;
	mat.specular = 0.5f;

	/* --- 0 灯 : ライトの uniform を一切作らない --- */
	YbrShaderOptions o = YbrShaderOptionsDefaults(RL_OPENGL_33);
	o.lightCount = 0;
	YbrShaderResult r = YbrShaderFromMaterialEx(&mat, &o);
	check(r.error == YBR_SHADER_OK, "0 灯で作れる");
	if (r.fragmentCode) {
		check(strstr(r.fragmentCode, "ybrLightDir") == NULL,
			  "ライトの uniform が出ない");
		check(strstr(r.fragmentCode, "uniform vec4 ambient") == NULL,
			  "ambient も出ない");
		check(strstr(r.fragmentCode, "uniform vec3 viewPos") == NULL,
			  "viewPos も出ない");
		check(strstr(r.fragmentCode, "return albedo;") != NULL,
			  "色をそのまま返す");
		check(strstr(r.fragmentCode, "ybrShadeN") != NULL, "関数自体はある");
	}
	{
		int lights = 0;
		for (int i = 0; i < r.uniformCount; i++)
			if (strstr(r.uniforms[i].name, "ybrLight")) lights++;
		check(lights == 0, "uniform 一覧にもライトが出ない");
	}
	YbrUnloadShaderResult(&r);

	/* --- 1..4 灯 --- */
	for (int n = 1; n <= YBR_SHADER_MAX_LIGHTS; n++) {
		o.lightCount = n;
		r = YbrShaderFromMaterialEx(&mat, &o);
		int ok = (r.error == YBR_SHADER_OK) && r.fragmentCode;
		if (ok) {
			for (int i = 0; i < n; i++) {
				char nd[32], nc[32];
				snprintf(nd, sizeof(nd), "uniform vec3 ybrLightDir%d;", i);
				snprintf(nc, sizeof(nc), "uniform vec4 ybrLightColor%d;", i);
				if (!strstr(r.fragmentCode, nd) || !strstr(r.fragmentCode, nc))
					ok = 0;
			}
			/* n 個より多くは出ない */
			char extra[32];
			snprintf(extra, sizeof(extra), "ybrLightDir%d", n);
			if (strstr(r.fragmentCode, extra)) ok = 0;
			if (!strstr(r.fragmentCode, "uniform vec4 ambient;")) ok = 0;
			if (!strstr(r.fragmentCode, "uniform vec3 viewPos;")) ok = 0;
		}
		check(ok, "灯数ぶんだけ uniform が出る");
		YbrUnloadShaderResult(&r);
	}

	/* --- 範囲外はクランプされる --- */
	o.lightCount = 99;
	r = YbrShaderFromMaterialEx(&mat, &o);
	check(r.error == YBR_SHADER_OK, "極端な灯数でも作れる");
	if (r.fragmentCode) {
		char over[32];
		snprintf(over, sizeof(over), "ybrLightDir%d", YBR_SHADER_MAX_LIGHTS);
		check(strstr(r.fragmentCode, over) == NULL, "上限で丸められる");
	}
	YbrUnloadShaderResult(&r);

	o.lightCount = -3;
	r = YbrShaderFromMaterialEx(&mat, &o);
	check(r.error == YBR_SHADER_OK && r.fragmentCode &&
			  strstr(r.fragmentCode, "ybrLightDir") == NULL,
		  "負の値は 0 として扱う");
	YbrUnloadShaderResult(&r);

	/* --- PRO のノードグラフでも同じ --- */
	{
		ShaderProbe p;
		probe_build(&p, YBR_SN_BSDF_PRINCIPLED, NULL, NULL);
		YbrShaderOptions po = YbrShaderOptionsDefaults(RL_OPENGL_33);
		po.lightCount = 0;
		YbrShaderResult pr = YbrShaderFromMaterialEx(&p.material, &po);
		check(pr.error == YBR_SHADER_OK, "PRO も 0 灯で作れる");
		if (pr.fragmentCode)
			check(strstr(pr.fragmentCode, "ybrLightDir") == NULL,
				  "PRO でもライトの uniform が出ない");
		YbrUnloadShaderResult(&pr);

		po.lightCount = 2;
		pr = YbrShaderFromMaterialEx(&p.material, &po);
		check(pr.error == YBR_SHADER_OK && pr.fragmentCode &&
				  strstr(pr.fragmentCode, "ybrLightDir1") != NULL &&
				  strstr(pr.fragmentCode, "ybrLightDir2") == NULL,
			  "PRO で 2 灯");
		YbrUnloadShaderResult(&pr);
	}

	/* --- viewPos を使うノードは 0 灯でも宣言される --- */
	{
		ShaderProbe p;
		probe_build(&p, YBR_SN_FRESNEL, NULL, NULL);
		YbrShaderOptions po = YbrShaderOptionsDefaults(RL_OPENGL_33);
		po.lightCount = 0;
		YbrShaderResult pr = YbrShaderFromMaterialEx(&p.material, &po);
		check(pr.error == YBR_SHADER_OK, "Fresnel を 0 灯で変換できる");
		if (pr.fragmentCode)
			check(strstr(pr.fragmentCode, "uniform vec3 viewPos;") != NULL,
				  "viewPos はライト無しでも宣言される");
		YbrUnloadShaderResult(&pr);
	}
}

/* ------------------------------------------------------------------ */
/* 生成した GLSL のスコープ検査                                        */
/*   「ブロックの中で宣言した変数を、ブロックを抜けてから使う」        */
/*   類のミスは GL が無いと気づけない。中カッコの深さを数えるだけで    */
/*   拾えるので、ここで見ておく。                                      */
/* ------------------------------------------------------------------ */
/* code の中で decl (例 "mat4 skin") が宣言されたあと、
 * その変数がスコープを抜けてから使われていないかを見る。
 * 問題なければ 1。 */
static int glsl_decl_stays_in_scope(const char* code, const char* decl,
									const char* name)
{
	if (!code || !decl || !name) return 1;
	const char* at = strstr(code, decl);
	if (!at) return 1; /* 宣言が無ければ見るものが無い */

	int declDepth = 0;
	for (const char* p = code; p < at; p++) {
		if (*p == '{')
			declDepth++;
		else if (*p == '}')
			declDepth--;
	}

	size_t nameLen = strlen(name);
	int depth = declDepth;
	for (const char* p = at + strlen(decl); *p; p++) {
		if (*p == '{')
			depth++;
		else if (*p == '}')
			depth--;
		if (depth < declDepth) {
			/* ここでスコープが閉じた。以降にこの名前が出てきたら未定義 */
			const char* q = strstr(p, name);
			while (q) {
				char before = q[-1];
				char after = q[nameLen];
				int wordStart =
					!(isalnum((unsigned char)before) || before == '_');
				int wordEnd = !(isalnum((unsigned char)after) || after == '_');
				if (wordStart && wordEnd) return 0; /* スコープ外で使っている */
				q = strstr(q + 1, name);
			}
			return 1;
		}
	}
	return 1;
}

void test_generated_glsl_scope(void)
{
	group("generated GLSL scope");

	YbrMaterial mat;
	memset(&mat, 0, sizeof(mat));
	mat.id = (char*)"Lit";
	mat.mode = YBR_MATERIAL_SIMPLE;
	mat.baseColor = (Vector4){0.8f, 0.8f, 0.8f, 1.0f};
	mat.roughness = 0.5f;
	mat.specular = 0.5f;

	/* スキニングの有無 x 灯数を一通り作って、どれもスコープが壊れていないこと
	 */
	for (int skin = 0; skin <= 1; skin++) {
		for (int lights = 0; lights <= YBR_SHADER_MAX_LIGHTS; lights++) {
			YbrShaderOptions o = YbrShaderOptionsDefaults(RL_OPENGL_33);
			o.skinning = skin;
			o.maxBones = 64;
			o.lightCount = lights;

			YbrShaderResult r = YbrShaderFromMaterialEx(&mat, &o);
			char what[96];
			snprintf(what, sizeof(what), "skinning=%d lights=%d で生成できる",
					 skin, lights);
			check(r.error == YBR_SHADER_OK && r.vertexCode && r.fragmentCode,
				  what);

			if (r.vertexCode) {
				snprintf(what, sizeof(what),
						 "skinning=%d : skin がスコープ内で使われている", skin);
				check(
					glsl_decl_stays_in_scope(r.vertexCode, "mat4 skin", "skin"),
					what);
				if (skin)
					check(strstr(r.vertexCode, "tan3") != NULL,
						  "接線も頂点シェーダーで変形する");
			}
			if (r.fragmentCode && 0 < lights)
				check(strstr(r.fragmentCode, "float ybrLightSample(") != NULL,
					  "灯があれば減衰の関数が出る");
			if (r.fragmentCode && lights == 0)
				check(strstr(r.fragmentCode, "ybrLightSample") == NULL,
					  "0 灯ならライト関係は一切出ない");

			/* 影を頼んでも、灯数を超えては出ない */
			YbrShaderOptions so = o;
			so.shadowLights = YBR_SHADER_MAX_SHADOWS;
			YbrShaderResult sr = YbrShaderFromMaterialEx(&mat, &so);
			if (sr.fragmentCode) {
				int want = (YBR_SHADER_MAX_SHADOWS < lights)
							   ? YBR_SHADER_MAX_SHADOWS
							   : lights;
				char nm[40];
				snprintf(nm, sizeof(nm), "%s%d", YBR_SHADER_UNIFORM_SHADOW_MAP,
						 want);
				snprintf(what, sizeof(what), "lights=%d : 影は %d 枚まで",
						 lights, want);
				check(strstr(sr.fragmentCode, nm) == NULL, what);
				if (0 < want)
					check(strstr(sr.fragmentCode, "ybrShadowAt") != NULL,
						  "影ありなら判定関数が出る");
				check(glsl_decl_stays_in_scope(sr.vertexCode, "mat4 skin",
											   "skin"),
					  "影ありでもスコープは壊れない");
			}
			YbrUnloadShaderResult(&sr);
			YbrUnloadShaderResult(&r);
		}
	}

	/* 検査そのものが効いているか (わざと壊した例を弾けること) */
	check(glsl_decl_stays_in_scope(
			  "void main(){ if (a) { mat4 skin = b; } c = skin; }", "mat4 skin",
			  "skin") == 0,
		  "スコープ外の使用を検出できる");
	check(glsl_decl_stays_in_scope(
			  "void main(){ mat4 skin = b; if (a) { skin = c; } d = skin; }",
			  "mat4 skin", "skin") == 1,
		  "正しい書き方は通す");
}
