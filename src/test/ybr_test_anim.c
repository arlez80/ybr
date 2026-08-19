/*
	Yui Blender to Raylib - ポーズ / アニメーションのテスト
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_test.h"

static Vector3 origin_of(Matrix m) { return V(m.m12, m.m13, m.m14); }

void test_pose(void)
{
	group("pose");
	make_armature();

	YbrPose pose;
	check(YbrPoseInit(&pose, &g_arm) == 1, "YbrPoseInit");
	check(pose.boneCount == 2, "ボーン数");
	check(YbrPoseFindBone(&pose, "child") == 1, "名前でボーンを引ける");
	check(YbrPoseFindBone(&pose, "nope") == -1, "無い名前は -1");

	check_vec(origin_of(pose.bones[1].pose), V(0, 1, 0), 1e-5f,
			  "レストポーズの位置");
	check_vec(Vector3Transform(V(1, 2, 3), pose.bones[1].skin), V(1, 2, 3),
			  1e-5f, "レストでは skin 行列が恒等");

	/* 子ボーンを親相対で動かす */
	pose.bones[1].translation = V(0, 3, 0);
	YbrPoseUpdate(&pose);
	check_vec(origin_of(pose.bones[1].pose), V(0, 3, 0), 1e-5f, "子を動かす");
	check_vec(Vector3Transform(V(0, 1, 0), pose.bones[1].skin), V(0, 3, 0),
			  1e-5f, "skin 行列が頂点を運ぶ");

	/* 親を回すと子もついてくる */
	YbrPoseReset(&pose);
	float h = 3.14159265f / 4.0f;
	Quaternion q = {0.0f, sinf(h), 0.0f, cosf(h)}; /* Y 軸まわり 90 度 */
	pose.bones[0].rotation = q;
	pose.bones[1].translation = V(1, 0, 0);
	YbrPoseUpdate(&pose);
	check_vec(origin_of(pose.bones[1].pose), V(0, 0, -1), 1e-4f,
			  "親の回転が子に伝わる");

	YbrPoseReset(&pose);
	check_vec(origin_of(pose.bones[1].pose), V(0, 1, 0), 1e-5f,
			  "Reset でレストに戻る");

	YbrPoseUnload(&pose);
	check(1, "YbrPoseUnload が落ちない");
}

/* ================================================================== */
/* アニメーション合成ツリー                                           */
/* ================================================================== */
static YbrAnimFrame g_frames[2];
static YbrAnimTrack g_track;
static YbrAnimation g_anim;
static YbrScene g_animScene;

/* child が 10 フレームかけて +Y に 1 -> 3 まで動くアニメーション */
static void make_animation(void)
{
	memset(g_frames, 0, sizeof(g_frames));
	g_frames[0].frame = 0;
	g_frames[0].interp = YBR_INTERP_STEP;
	g_frames[0].transform = MatrixTranslate(0.0f, 1.0f, 0.0f);
	g_frames[1].frame = 10;
	g_frames[1].interp = YBR_INTERP_LINEAR;
	g_frames[1].transform = MatrixTranslate(0.0f, 3.0f, 0.0f);

	memset(&g_track, 0, sizeof(g_track));
	g_track.object = (char*)"Obj";
	g_track.bone = (char*)"child";
	g_track.frameCount = 2;
	g_track.frames = g_frames;

	memset(&g_anim, 0, sizeof(g_anim));
	g_anim.id = (char*)"Walk";
	g_anim.object = (char*)"Obj";
	g_anim.fps = 10.0f;
	g_anim.frameCount = 11;
	g_anim.sincA = 3;
	g_anim.trackCount = 1;
	g_anim.tracks = &g_track;

	memset(&g_animScene, 0, sizeof(g_animScene));
	g_animScene.armature = &g_arm;
	g_animScene.animationCount = 1;
	g_animScene.animations = &g_anim;
}

static void source_node(YbrAnimBlendNode* n, const char* id,
						YbrAnimLoopMode loop)
{
	memset(n, 0, sizeof(*n));
	n->uniqueId = id;
	n->type = YBR_ABN_SOURCE;
	n->source.name = "Walk";
	n->source.loopMode = loop;
	n->source.playSpeed = 1.0f;
}

void test_blend_tree(void)
{
	group("anim blend tree");
	make_armature();
	make_animation();

	YbrPose pose;
	if (!YbrPoseInit(&pose, &g_arm)) {
		check(0, "pose init");
		return;
	}

	/* --- 合成を通さない素の再生 --- */
	YbrPoseApplyAnimation(&pose, &g_anim, 5.0f);
	check_vec(origin_of(pose.bones[1].pose), V(0, 2, 0), 1e-4f,
			  "YbrPoseApplyAnimation : フレーム 5 で中間");

	/* --- SOURCE 1 つだけ --- */
	YbrAnimBlendNode src;
	source_node(&src, "walk", YBR_ALM_ONE_SHOT);

	YbrAnimBlendTree tree;
	check(YbrAnimBlendTreeInit(&tree, &g_animScene, &pose, &src) == 1,
		  "ツリーの初期化");
	check(YbrAnimBlendTreeFind(&tree, "walk") == &src, "uniqueId で引ける");
	check(YbrAnimBlendTreeFind(&tree, "nope") == NULL, "無い id は NULL");
	check(YbrAnimBlendTreeFind(&tree, "") == NULL, "空文字では引けない");
	check(YbrAnimBlendTreeFind(&tree, NULL) == NULL, "NULL では引けない");

	check(YbrAnimBlendTreeEval(&tree, 0.0f, &pose) == 1, "Eval");
	check_vec(origin_of(pose.bones[1].pose), V(0, 1, 0), 1e-4f, "先頭フレーム");

	/* 0.5 秒進める = 10fps なのでフレーム 5 */
	YbrAnimBlendTreeEval(&tree, 0.5f, &pose);
	YbrAnimBlendTreeEval(&tree, 0.0f, &pose);
	check_vec(origin_of(pose.bones[1].pose), V(0, 2, 0), 1e-4f, "0.5 秒後");

	/* ONE_SHOT は末尾で止まる */
	for (int i = 0; i < 20; i++) YbrAnimBlendTreeEval(&tree, 0.5f, &pose);
	check_vec(origin_of(pose.bones[1].pose), V(0, 3, 0), 1e-4f,
			  "ONE_SHOT は末尾で止まる");
	YbrAnimBlendTreeUnload(&tree);

	/* --- LERP : 同じソース同士なら結果は変わらない --- */
	YbrAnimBlendNode a, b, lerp;
	source_node(&a, "a", YBR_ALM_LOOP);
	source_node(&b, "b", YBR_ALM_LOOP);
	memset(&lerp, 0, sizeof(lerp));
	lerp.uniqueId = "lerp";
	lerp.type = YBR_ABN_LERP;
	lerp.lerp.input = &a;
	lerp.lerp.mixInput = &b;
	lerp.lerp.weight = 0.5f;

	check(YbrAnimBlendTreeInit(&tree, &g_animScene, &pose, &lerp) == 1,
		  "LERP ツリー");
	YbrAnimBlendTreeEval(&tree, 0.5f, &pose);
	YbrAnimBlendTreeEval(&tree, 0.0f, &pose);
	check_vec(origin_of(pose.bones[1].pose), V(0, 2, 0), 1e-4f,
			  "LERP : 同じソース同士は素通し");
	YbrAnimBlendTreeUnload(&tree);

	/* --- ADD : ウェイト 0 なら入力そのまま --- */
	YbrAnimBlendNode add;
	source_node(&a, "a", YBR_ALM_LOOP);
	source_node(&b, "b", YBR_ALM_LOOP);
	memset(&add, 0, sizeof(add));
	add.uniqueId = "add";
	add.type = YBR_ABN_ADD;
	add.add.input = &a;
	add.add.mixInput = &b;
	add.add.weight = 0.0f;

	check(YbrAnimBlendTreeInit(&tree, &g_animScene, &pose, &add) == 1,
		  "ADD ツリー");
	YbrAnimBlendTreeEval(&tree, 0.5f, &pose);
	YbrAnimBlendTreeEval(&tree, 0.0f, &pose);
	check_vec(origin_of(pose.bones[1].pose), V(0, 2, 0), 1e-4f,
			  "ADD : ウェイト 0 は素通し");

	/* ウェイト 1 なら差分が 2 回ぶん乗る (レスト +1 に対して +1 +1) */
	add.add.weight = 1.0f;
	YbrAnimBlendTreeEval(&tree, 0.0f, &pose);
	check_vec(origin_of(pose.bones[1].pose), V(0, 3, 0), 1e-4f,
			  "ADD : ウェイト 1 で差分が加算される");
	YbrAnimBlendTreeUnload(&tree);

	/* --- 壊れたツリーは弾く --- */
	YbrAnimBlendNode bad;
	memset(&bad, 0, sizeof(bad));
	bad.uniqueId = "bad";
	bad.type = YBR_ABN_SOURCE;
	bad.source.name = "NoSuchAnimation";
	check(YbrAnimBlendTreeInit(&tree, &g_animScene, &pose, &bad) == 0,
		  "存在しないアニメーション名は弾く");
	YbrAnimBlendTreeUnload(&tree);

	YbrPoseUnload(&pose);
}

/* ================================================================== */
/* HERMITE 補間 / 接線                                                */
/* ================================================================== */
void test_hermite(void)
{
	group("hermite");

	/* --- 接線ユーティリティ --- */
	{
		YbrTransform a = YbrTransformIdentity();
		YbrTransform b = YbrTransformIdentity();
		b.translation = V(0, 4, 0);
		YbrAnimTangent t = YbrAnimTangentFromDelta(a, b, 2.0f);
		check_vec(t.translation, V(0, 2, 0), 1e-5f,
				  "接線 = 変化量 / フレーム差");

		YbrAnimTangent z = YbrAnimTangentFromDelta(a, b, 0.0f);
		check_vec(z.translation, V(0, 0, 0), 1e-6f, "dt=0 なら 0");

		/* ベジェは m = 3(c - p)/dt */
		YbrAnimTangent bz = YbrAnimTangentFromBezier(a, b, 2.0f);
		check_vec(bz.translation, V(0, 6, 0), 1e-5f, "ベジェハンドルから接線");
	}

	/* --- 手で作った 2 キーのトラックを評価する --- */
	{
		/* frame 0 -> 10 で y が 0 -> 10、両端で接線 0 (イーズ) */
		YbrAnimFrame fr[2];
		memset(fr, 0, sizeof(fr));
		fr[0].frame = 0;
		fr[0].interp = YBR_INTERP_STEP;
		fr[0].transform = MatrixTranslate(0.0f, 0.0f, 0.0f);
		fr[1].frame = 10;
		fr[1].interp = YBR_INTERP_HERMITE;
		fr[1].transform = MatrixTranslate(0.0f, 10.0f, 0.0f);

		YbrAnimTangent tan[4];
		for (int i = 0; i < 4; i++) tan[i] = YbrAnimTangentZero();

		YbrAnimTrack tr;
		memset(&tr, 0, sizeof(tr));
		tr.object = (char*)"Obj";
		tr.frameCount = 2;
		tr.frames = fr;
		tr.tangents = tan;

		YbrInterpParams p = YbrInterpParamsDefault();
		/* 接線 0 の 3 次エルミートは smoothstep になる */
		YbrTransform m = YbrAnimTrackEvaluate(&tr, 5.0f, &p);
		check_near(m.translation.y, 5.0f, 1e-4f, "中央は 5.0");
		YbrTransform q = YbrAnimTrackEvaluate(&tr, 2.5f, &p);
		check_near(q.translation.y, 10.0f * (3.0f * 0.0625f - 2.0f * 0.015625f),
				   1e-3f, "1/4 地点は smoothstep(0.25)");
		check(q.translation.y < 2.5f, "両端の接線 0 なので立ち上がりが遅い");

		/* 端点はキーの値をそのまま通る */
		check_near(YbrAnimTrackEvaluate(&tr, 0.0f, &p).translation.y, 0.0f,
				   1e-5f, "始点を通る");
		check_near(YbrAnimTrackEvaluate(&tr, 10.0f, &p).translation.y, 10.0f,
				   1e-5f, "終点を通る");

		/* 接線が線形の傾きなら LINEAR と一致する */
		YbrAnimTangent lin = YbrAnimTangentZero();
		lin.translation = V(0, 1, 0); /* 10 / 10 フレーム */
		for (int i = 0; i < 4; i++) tan[i] = lin;
		check_near(YbrAnimTrackEvaluate(&tr, 2.5f, &p).translation.y, 2.5f,
				   1e-4f, "接線が一定なら直線になる");

		/* 接線が無ければ LINEAR にフォールバックする */
		tr.tangents = NULL;
		check_near(YbrAnimTrackEvaluate(&tr, 2.5f, &p).translation.y, 2.5f,
				   1e-4f, "接線が無ければ LINEAR 扱い");
		tr.tangents = tan;

		/* サンプラ経由でも同じ */
		YbrAnimSampler smp;
		check(YbrAnimSamplerInit(&smp, &tr, &p) == 1, "サンプラを作れる");
		check(smp.tangents != NULL, "サンプラが接線を持つ");
		Matrix sm = YbrAnimSamplerMatrix(&smp, 2.5f);
		check_near(sm.m13, 2.5f, 1e-4f, "サンプラでも同じ値");
		YbrAnimSamplerUnload(&smp);
	}

	/* --- 名前 / パース --- */
	{
		YbrInterp mode;
		check(YbrInterpParse("hermite", &mode) && mode == YBR_INTERP_HERMITE,
			  "\"hermite\" を解釈できる");
		check(YbrInterpParse("bezier", &mode) && mode == YBR_INTERP_HERMITE,
			  "\"bezier\" も HERMITE になる");
		check(!strcmp(YbrInterpName(YBR_INTERP_HERMITE), "HERMITE"),
			  "名前が引ける");
		check(YbrInterpRadius(YBR_INTERP_HERMITE, NULL) == 0,
			  "区間の両端しか見ない");
	}

	/* --- 保存 / 読み込みの往復 --- */
	{
		YbrAnimFrame fr[3];
		memset(fr, 0, sizeof(fr));
		for (int i = 0; i < 3; i++) {
			fr[i].frame = i * 5;
			fr[i].interp = (i == 0) ? YBR_INTERP_STEP : YBR_INTERP_HERMITE;
			fr[i].transform = MatrixTranslate(0.0f, (float)i, 0.0f);
		}
		YbrAnimTangent tan[6];
		for (int i = 0; i < 6; i++) {
			tan[i] = YbrAnimTangentZero();
			tan[i].translation = V(0.0f, 0.1f * (float)i, 0.0f);
		}

		YbrAnimTrack tr;
		memset(&tr, 0, sizeof(tr));
		tr.object = (char*)"Obj";
		tr.frameCount = 3;
		tr.frames = fr;
		tr.tangents = tan;

		YbrAnimation an;
		memset(&an, 0, sizeof(an));
		an.id = (char*)"H";
		an.object = (char*)"Obj";
		an.fps = 24.0f;
		an.frameCount = 11;
		an.sincA = 3;
		an.trackCount = 1;
		an.tracks = &tr;

		YbrScene sc;
		memset(&sc, 0, sizeof(sc));
		sc.version = YBR_SUPPORTED_VERSION;
		sc.animationCount = 1;
		sc.animations = &an;

		check(YbrAnimTrackTangentsSymmetric(&tr) == 0,
			  "in != out なら非対称と判定される");

		size_t size = 0;
		unsigned char* buf = YbrSaveToMemory(&sc, &size);
		check(buf != NULL, "接線付きで書き出せる");
		if (!buf) return;
		YbrScene* ld = YbrLoadFromMemory(buf, size);
		YBR_FREE(buf);
		check(ld != NULL, "読み戻せる");
		if (!ld) return;

		const YbrAnimTrack* lt = &ld->animations[0].tracks[0];
		check(lt->tangents != NULL, "接線が残る");
		if (lt->tangents) {
			int same = 1;
			for (int i = 0; i < 6; i++)
				if (1e-6f <
					fabsf(lt->tangents[i].translation.y - tan[i].translation.y))
					same = 0;
			check(same, "in / out それぞれの値が残る");
		}
		YbrUnload(ld);

		/* in == out なら半分のサイズで書かれる */
		for (int i = 0; i < 3; i++) {
			tan[i * 2 + 0] = tan[i * 2 + 1] = YbrAnimTangentZero();
			tan[i * 2 + 0].translation = V(0.0f, 0.25f * (float)i, 0.0f);
			tan[i * 2 + 1] = tan[i * 2 + 0];
		}
		check(YbrAnimTrackTangentsSymmetric(&tr) == 1,
			  "in == out なら対称と判定");

		size_t sizeSym = 0;
		unsigned char* b2 = YbrSaveToMemory(&sc, &sizeSym);
		check(b2 != NULL && sizeSym < size, "対称なら小さく書ける");
		if (b2) {
			YbrScene* l2 = YbrLoadFromMemory(b2, sizeSym);
			YBR_FREE(b2);
			check(l2 != NULL, "対称でも読み戻せる");
			if (l2) {
				const YbrAnimTrack* t2 = &l2->animations[0].tracks[0];
				check(t2->tangents != NULL, "接線が展開される");
				if (t2->tangents) {
					check_near(t2->tangents[2].translation.y, 0.25f, 1e-6f,
							   "in が復元される");
					check_near(t2->tangents[3].translation.y, 0.25f, 1e-6f,
							   "out も同じ値");
				}
				YbrUnload(l2);
			}
		}
	}
}

/* ================================================================== */
/* シェイプキー (モーフ) / ポーズマーカー / 重みトラック               */
/* ================================================================== */
/* 三角形 1 枚 + シェイプキー 1 本 + マーカー付きアニメーションを組む。
 * 手で組んだ配列を指すだけなので、YbrUnload には渡さないこと。 */
/* マーカー付きアニメーションを持つ最小のシーン。
 * 手で組んだ配列を指すだけなので、YbrUnload には渡さないこと。 */
typedef struct MarkerScene {
	YbrScene scene;
	YbrMesh mesh;
	YbrNode root;
	YbrAnimation anim;
	YbrAnimMarker markers[2];

	float positions[9];
	float normals[9];
	unsigned int indices[3];
} MarkerScene;

static void marker_scene_init(MarkerScene* s)
{
	memset(s, 0, sizeof(*s));

	static const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	static const float nrm[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
	memcpy(s->positions, pos, sizeof(pos));
	memcpy(s->normals, nrm, sizeof(nrm));
	s->indices[0] = 0;
	s->indices[1] = 1;
	s->indices[2] = 2;

	s->mesh.id = (char*)"Tri";
	s->mesh.vertexCount = 3;
	s->mesh.triangleCount = 1;
	s->mesh.positions = s->positions;
	s->mesh.normals = s->normals;
	s->mesh.indices = s->indices;

	s->root.name = (char*)"TriObj";
	s->root.type = YBR_NODE_MESH;
	s->root.dataId = (char*)"Tri";
	s->root.matrix = MatrixIdentity();

	s->markers[0].name = (char*)"step_l";
	s->markers[0].frame = 3;
	s->markers[1].name = (char*)"step_r";
	s->markers[1].frame = 7;

	s->anim.id = (char*)"Act";
	s->anim.object = (char*)"TriObj";
	s->anim.fps = 24.0f;
	s->anim.frameCount = 10;
	s->anim.sincA = YBR_SINC_A_DEFAULT;
	s->anim.markerCount = 2;
	s->anim.markers = s->markers;

	s->scene.meshCount = 1;
	s->scene.meshes = &s->mesh;
	s->scene.rootCount = 1;
	s->scene.roots = &s->root;
	s->scene.animationCount = 1;
	s->scene.animations = &s->anim;
}

void test_pose_markers(void)
{
	group("pose markers");

	MarkerScene ms;
	marker_scene_init(&ms);

	size_t size = 0;
	unsigned char* buf = YbrSaveToMemory(&ms.scene, &size);
	YbrScene* sc = buf ? YbrLoadFromMemory(buf, size) : NULL;
	check(sc != NULL, "読み戻せる");
	if (!sc) {
		free(buf);
		return;
	}

	const YbrAnimation* a = YbrFindAnimation(sc, "Act");
	check(a && a->markerCount == 2, "マーカーが 2 個");
	check(YbrFindAnimMarker(a, "step_r") != NULL, "名前で引ける");
	check(YbrFindAnimMarker(a, "step_r")->frame == 7, "フレーム番号");
	check(YbrFindAnimMarker(a, "nope") == NULL, "無い名前は NULL");
	check(a->markers[0].frame <= a->markers[1].frame, "フレーム順に並ぶ");

	const YbrAnimMarker* hit[4];
	check(YbrAnimMarkersInRange(a, 0.0f, 5.0f, hit, 4) == 1, "(0,5] に 1 個");
	check(hit[0] && strcmp(hit[0]->name, "step_l") == 0, "拾えたのは step_l");
	check(YbrAnimMarkersInRange(a, 2.9f, 3.1f, hit, 4) == 1,
		  "3 をまたぐと拾う");
	check(YbrAnimMarkersInRange(a, 3.0f, 3.0f, hit, 4) == 0,
		  "進んでいなければ 0");
	check(YbrAnimMarkersInRange(a, 0.0f, 10.0f, hit, 4) == 2, "全部で 2 個");
	check(YbrAnimMarkersInRange(a, 3.0f, 7.0f, hit, 4) == 1,
		  "区間の始点にあるマーカーは含めない");
	check(YbrAnimMarkersInRange(a, 8.0f, 4.0f, hit, 4) == 1,
		  "ループで巻き戻ると先頭側を拾う");
	check(YbrAnimMarkersInRange(a, 0.0f, 10.0f, NULL, 0) == 2,
		  "out が NULL でも数える");

	YbrUnload(sc);
	free(buf);
}
