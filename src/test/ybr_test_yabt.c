/*
	Yui Blender to Raylib - Yabt (合成ツリー) 読み書きのテスト
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_test.h"

static int streq(const char* a, const char* b)
{
	return a && b && strcmp(a, b) == 0;
}

/* Walk / Run の 2 本を持つだけのシーン。SOURCE の名前解決に使う */
typedef struct YabtScene {
	YbrAnimFrame frames[2][2];
	YbrAnimTrack tracks[2];
	YbrAnimation anims[2];
	YbrScene scene;
} YabtScene;

static void yabt_scene_init(YabtScene* s)
{
	memset(s, 0, sizeof(*s));
	make_armature();

	const char* names[2] = {"Walk", "Run"};
	for (int a = 0; a < 2; a++) {
		s->frames[a][0].frame = 0;
		s->frames[a][0].interp = YBR_INTERP_LINEAR;
		s->frames[a][0].transform = MatrixTranslate(0.0f, 1.0f, 0.0f);
		s->frames[a][1].frame = 10;
		s->frames[a][1].interp = YBR_INTERP_LINEAR;
		s->frames[a][1].transform = MatrixTranslate(0.0f, 3.0f, 0.0f);

		s->tracks[a].object = (char*)"Obj";
		s->tracks[a].bone = (char*)"child";
		s->tracks[a].frameCount = 2;
		s->tracks[a].frames = s->frames[a];

		s->anims[a].id = (char*)names[a];
		s->anims[a].object = (char*)"Obj";
		s->anims[a].fps = 10.0f;
		s->anims[a].frameCount = 11;
		s->anims[a].sincA = 3;
		s->anims[a].trackCount = 1;
		s->anims[a].tracks = &s->tracks[a];
	}

	s->scene.armature = &g_arm;
	s->scene.animationCount = 2;
	s->scene.animations = s->anims;
}

static void yabt_source(YbrAnimBlendNode* n, const char* id,
						const char* name,
						YbrAnimLoopMode loop, float speed)
{
	memset(n, 0, sizeof(*n));
	n->uniqueId = id;
	n->type = YBR_ABN_SOURCE;
	n->source.name = name;
	n->source.loopMode = loop;
	n->source.playSpeed = speed;
}

/*   TRANSITION (id 100)
 *     +- LERP (id 10) : Walk (id 1) x Run (id 2)  filtered = {root}
 *     +- SOURCE (id 3) : Run                                        */
typedef struct YabtProbe {
	YbrAnimBlendNode leaves[3];
	YbrAnimBlendNode lerp;
	YbrAnimBlendNode inputs[2];
	YbrAnimBlendNode root;
	const char* filtered[1];
} YabtProbe;

static void yabt_probe_init(YabtProbe* p)
{
	memset(p, 0, sizeof(*p));
	yabt_source(&p->leaves[0], "walk", "Walk", YBR_ALM_LOOP, 1.5f);
	yabt_source(&p->leaves[1], "run", "Run", YBR_ALM_ONE_SHOT, 0.5f);
	yabt_source(&p->leaves[2], "run2", "Run", YBR_ALM_LOOP, 1.0f);

	p->filtered[0] = "root";

	p->lerp.uniqueId = "mix";
	p->lerp.type = YBR_ABN_LERP;
	p->lerp.lerp.input = &p->leaves[0];
	p->lerp.lerp.mixInput = &p->leaves[1];
	p->lerp.lerp.weight = 0.25f;
	p->lerp.lerp.filteredBones = p->filtered;
	p->lerp.lerp.filteredBoneCount = 1;

	p->inputs[0] = p->lerp;
	p->inputs[1] = p->leaves[2];

	p->root.uniqueId = "root";
	p->root.type = YBR_ABN_TRANSITION;
	p->root.transition.inputs = p->inputs;
	p->root.transition.inputCount = 2;
	p->root.transition.index = 1;
	p->root.transition.transitionSeconds = 0.4f;
	p->root.transition.nextIndex = 0;   /* 実行時の状態 */
	p->root.transition.position = 0.2f; /* 実行時の状態 */
}

static void test_yabt_roundtrip(void)
{
	group("yabt : 書いて読み直す");

	YabtProbe p;
	yabt_probe_init(&p);

	size_t size = 0;
	unsigned char* buf = YabtSaveToMemory(&p.root, &size);
	check(buf != NULL, "書き出せる");
	check(0 < size, "中身がある");
	if (!buf) return;

	YabtTree* t = YabtLoadFromMemory(buf, size);
	check(t != NULL, "読み直せる");
	if (!t) {
		printf("  (%s)\n", YabtGetError());
		YBR_FREE(buf);
		return;
	}

	check(t->version == YABT_SUPPORTED_VERSION, "バージョンが入る");
	check(t->root != NULL, "根がある");
	check(t->root->type == YBR_ABN_TRANSITION, "根は遷移");
	check(streq(t->root->uniqueId, "root"), "根の uniqueId");
	check(t->root->transition.inputCount == 2, "入力の数");
	check_near(t->root->transition.transitionSeconds, 0.4f, 1e-6f, "遷移時間");
	check(t->root->transition.index == 1, "今の入力");

	/* 実行時の状態は書かない。読んだら必ず止まった状態になる */
	check(t->root->transition.nextIndex == -1, "遷移先は残らない");
	check_near(t->root->transition.position, 0.0f, 1e-6f, "経過時間は残らない");

	const YbrAnimBlendNode* lerp = &t->root->transition.inputs[0];
	check(lerp->type == YBR_ABN_LERP, "入力 0 は線形補間");
	check(streq(lerp->uniqueId, "mix"), "入力 0 の uniqueId");
	check_near(lerp->lerp.weight, 0.25f, 1e-6f, "重み");
	check(lerp->lerp.filteredBoneCount == 1, "除外ボーンの数");
	check(lerp->lerp.filteredBones != NULL &&
			  streq(lerp->lerp.filteredBones[0], "root"),
		  "除外ボーンの名前");

	const YbrAnimBlendNode* walk = lerp->lerp.input;
	const YbrAnimBlendNode* run = lerp->lerp.mixInput;
	check(walk && walk->type == YBR_ABN_SOURCE, "input はソース");
	check(walk && streq(walk->source.name, "Walk"), "input の名前");
	check(walk && walk->source.loopMode == YBR_ALM_LOOP, "input のループ");
	check(walk && fabsf(walk->source.playSpeed - 1.5f) < 1e-6f,
		  "input の再生速度");
	check(run && streq(run->source.name, "Run"), "mix_input の名前");
	check(run && run->source.loopMode == YBR_ALM_ONE_SHOT,
		  "mix_input のループ");

	/* 実体はまだ結びつけていない (YbrAnimBlendTreeInit がやる) */
	check(walk && walk->source.animation == NULL, "実体はまだ NULL");

	const YbrAnimBlendNode* leaf = &t->root->transition.inputs[1];
	check(leaf->type == YBR_ABN_SOURCE, "入力 1 はソース");
	check(streq(leaf->uniqueId, "run2"), "入力 1 の uniqueId");

	/* もう 1 度書き出すと同じバイト列になる */
	size_t size2 = 0;
	unsigned char* buf2 = YabtSaveToMemory(t->root, &size2);
	check(buf2 != NULL, "読んだツリーを書き出せる");
	check(buf2 && size2 == size && memcmp(buf, buf2, size) == 0,
		  "書いて読んで書くと同じになる");
	YBR_FREE(buf2);

	YabtUnload(t);
	YBR_FREE(buf);
}

/* 読み込んだツリーがそのまま YbrAnimBlendTree で動くか */
static void test_yabt_evaluate(void)
{
	group("yabt : 読んだツリーを評価する");

	YabtProbe p;
	yabt_probe_init(&p);

	size_t size = 0;
	unsigned char* buf = YabtSaveToMemory(&p.root, &size);
	if (!buf) {
		check(0, "書き出せる");
		return;
	}
	YabtTree* t = YabtLoadFromMemory(buf, size);
	YBR_FREE(buf);
	check(t != NULL, "読み直せる");
	if (!t) return;

	YabtScene sc;
	yabt_scene_init(&sc);

	YbrPose pose;
	check(YbrPoseInit(&pose, &g_arm) == 1, "ポーズを作れる");

	YbrAnimBlendTree tree;
	check(YbrAnimBlendTreeInit(&tree, &sc.scene, &pose, t->root) == 1,
		  "合成ツリーを初期化できる");
	check(YbrAnimBlendTreeFind(&tree, "mix") != NULL, "uniqueId で引ける");
	check(YbrAnimBlendTreeFind(&tree, "nope") == NULL, "無い id は NULL");

	/* 初期化でアニメーションの実体が結びつく */
	YbrAnimBlendNode* walk = YbrAnimBlendTreeFind(&tree, "walk");
	check(walk != NULL && walk->source.animation != NULL, "実体が結びつく");

	for (int i = 0; i < 30; i++)
		check(YbrAnimBlendTreeEval(&tree, 1.0f / 60.0f, &pose) == 1 || i != 0,
			  "評価できる");

	int child = YbrPoseFindBone(&pose, "child");
	check(0 <= child, "child ボーンがある");
	if (0 <= child) {
		Matrix m = pose.bones[child].pose;
		check(m.m13 != 0.0f, "アニメーションが効いている");
	}

	YbrAnimBlendTreeUnload(&tree);
	YbrPoseUnload(&pose);
	YabtUnload(t);
}

static void test_yabt_broken(void)
{
	group("yabt : 壊れたファイル");

	YabtProbe p;
	yabt_probe_init(&p);
	size_t size = 0;
	unsigned char* good = YabtSaveToMemory(&p.root, &size);
	if (!good) {
		check(0, "元になるファイルを作れる");
		return;
	}

	check(YabtLoadFromMemory(NULL, 0) == NULL, "NULL は弾く");
	check(YabtLoadFromMemory(good, 0) == NULL, "長さ 0 は弾く");
	check(YabtGetError()[0] != '\0', "エラーが入る");

	/* 途中で切る */
	int survived = 0;
	for (size_t n = 1; n < size; n += (size / 64) + 1) {
		YabtTree* t = YabtLoadFromMemory(good, n);
		if (t) {
			survived++;
			YabtUnload(t);
		}
	}
	check(survived == 0, "途中で切れたものは読めない");

	/* 1 バイトずつ壊す (落ちないこと) */
	unsigned char* work = (unsigned char*)YBR_MALLOC(size);
	if (work) {
		for (size_t i = 0; i < size; i++) {
			memcpy(work, good, size);
			work[i] ^= 0xFF;
			YabtTree* t = YabtLoadFromMemory(work, size);
			if (t) YabtUnload(t);
		}
		check(1, "壊しても落ちない");
		YBR_FREE(work);
	}

	/* マジックとバージョン */
	{
		CborWriter w;
		CborWriterInit(&w);
		CborWriteArrayHeader(&w, 3);
		CborWriteText(&w, "YBRX");
		CborWriteInt(&w, YABT_SUPPORTED_VERSION);
		CborWriteMapHeader(&w, 0);
		size_t n;
		unsigned char* b = CborWriterTake(&w, &n);
		check(YabtLoadFromMemory(b, n) == NULL, "マジックが違うと弾く");
		YBR_FREE(b);
	}
	{
		CborWriter w;
		CborWriterInit(&w);
		CborWriteArrayHeader(&w, 3);
		CborWriteText(&w, YABT_MAGIC);
		CborWriteInt(&w, YABT_SUPPORTED_VERSION + 1);
		CborWriteMapHeader(&w, 0);
		size_t n;
		unsigned char* b = CborWriterTake(&w, &n);
		check(YabtLoadFromMemory(b, n) == NULL, "バージョンが違うと弾く");
		YBR_FREE(b);
	}
	/* 知らないノード種別 */
	{
		CborWriter w;
		CborWriterInit(&w);
		CborWriteArrayHeader(&w, 3);
		CborWriteText(&w, YABT_MAGIC);
		CborWriteInt(&w, YABT_SUPPORTED_VERSION);
		CborWriteMapHeader(&w, 2);
		CborWriteKeyInt(&w, "type", 99);
		CborWriteKeyText(&w, "unique_id", "x");
		size_t n;
		unsigned char* b = CborWriterTake(&w, &n);
		check(YabtLoadFromMemory(b, n) == NULL, "知らない種別は弾く");
		YBR_FREE(b);
	}
	/* 名前の無いソース */
	{
		CborWriter w;
		CborWriterInit(&w);
		CborWriteArrayHeader(&w, 3);
		CborWriteText(&w, YABT_MAGIC);
		CborWriteInt(&w, YABT_SUPPORTED_VERSION);
		CborWriteMapHeader(&w, 2);
		CborWriteKeyInt(&w, "type", YBR_ABN_SOURCE);
		CborWriteKeyNull(&w, "name");
		size_t n;
		unsigned char* b = CborWriterTake(&w, &n);
		check(YabtLoadFromMemory(b, n) == NULL, "名前の無いソースは弾く");
		YBR_FREE(b);
	}
	/* 入力の無い遷移 */
	{
		CborWriter w;
		CborWriterInit(&w);
		CborWriteArrayHeader(&w, 3);
		CborWriteText(&w, YABT_MAGIC);
		CborWriteInt(&w, YABT_SUPPORTED_VERSION);
		CborWriteMapHeader(&w, 2);
		CborWriteKeyInt(&w, "type", YBR_ABN_TRANSITION);
		CborWriteKeyArrayHeader(&w, "inputs", 0);
		size_t n;
		unsigned char* b = CborWriterTake(&w, &n);
		check(YabtLoadFromMemory(b, n) == NULL, "入力の無い遷移は弾く");
		YBR_FREE(b);
	}
	/* 片方しか入力の無い線形補間 */
	{
		CborWriter w;
		CborWriterInit(&w);
		CborWriteArrayHeader(&w, 3);
		CborWriteText(&w, YABT_MAGIC);
		CborWriteInt(&w, YABT_SUPPORTED_VERSION);
		CborWriteMapHeader(&w, 2);
		CborWriteKeyInt(&w, "type", YBR_ABN_LERP);
		CborWriteText(&w, "input");
		CborWriteMapHeader(&w, 2);
		CborWriteKeyInt(&w, "type", YBR_ABN_SOURCE);
		CborWriteKeyText(&w, "name", "Walk");
		size_t n;
		unsigned char* b = CborWriterTake(&w, &n);
		check(YabtLoadFromMemory(b, n) == NULL, "入力が足りないと弾く");
		YBR_FREE(b);
	}

	YBR_FREE(good);
}

/* 書けない形のツリーは書き出しの時点で止める */
static void test_yabt_reject(void)
{
	group("yabt : 書けないツリー");

	check(YabtSaveToMemory(NULL, NULL) == NULL, "NULL は書けない");

	YbrAnimBlendNode n;
	memset(&n, 0, sizeof(n));
	n.type = YBR_ABN_SOURCE;
	n.source.name = NULL;
	check(YabtSaveToMemory(&n, NULL) == NULL, "名前の無いソースは書けない");

	memset(&n, 0, sizeof(n));
	n.type = YBR_ABN_LERP;
	check(YabtSaveToMemory(&n, NULL) == NULL, "入力の無い補間は書けない");

	memset(&n, 0, sizeof(n));
	n.type = YBR_ABN_TRANSITION;
	check(YabtSaveToMemory(&n, NULL) == NULL, "入力の無い遷移は書けない");

	memset(&n, 0, sizeof(n));
	n.type = (YbrAnimBlendNodeType)77;
	check(YabtSaveToMemory(&n, NULL) == NULL, "知らない種別は書けない");

	/* 深すぎるツリー */
	{
		const int depth = YABT_MAX_DEPTH + 8;
		YbrAnimBlendNode* nodes = (YbrAnimBlendNode*)YBR_CALLOC(
			(size_t)depth + 1, sizeof(YbrAnimBlendNode));
		if (nodes) {
			yabt_source(&nodes[depth], "", "Walk", YBR_ALM_LOOP, 1.0f);
			for (int i = depth - 1; 0 <= i; i--) {
				nodes[i].uniqueId = "";
				nodes[i].type = YBR_ABN_LERP;
				nodes[i].lerp.input =
					(i == depth - 1) ? &nodes[depth] : &nodes[i + 1];
				nodes[i].lerp.mixInput = &nodes[depth];
				nodes[i].lerp.weight = 0.5f;
			}
			check(YabtSaveToMemory(&nodes[0], NULL) == NULL,
				  "深すぎるツリーは書けない");
			YBR_FREE(nodes);
		}
	}
}

/* ファイル経由の往復。書けない場所なら飛ばす */
static void test_yabt_file(void)
{
	group("yabt : ファイル");

	const char* path = "ybr_test_tmp.yabt";
	YabtProbe p;
	yabt_probe_init(&p);

	if (!YabtSave(&p.root, path)) {
		printf("  (書き込めないので飛ばす: %s)\n", YabtGetError());
		return;
	}
	check(1, "保存できる");

	YabtTree* t = YabtLoad(path);
	check(t != NULL, "読み込める");
	if (t) {
		check(t->root->type == YBR_ABN_TRANSITION, "根が同じ");
		check(t->root->transition.inputCount == 2, "入力の数が同じ");
		YabtUnload(t);
	}
	remove(path);

	check(YabtLoad("no_such_file_here.yabt") == NULL, "無いファイルは NULL");
}

void test_yabt(void)
{
	test_yabt_roundtrip();
	test_yabt_evaluate();
	test_yabt_broken();
	test_yabt_reject();
	test_yabt_file();
}
