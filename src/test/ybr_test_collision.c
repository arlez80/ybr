/*
	Yui Blender to Raylib - 当たり判定 (YbrSolid) のテスト
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_test.h"

/* この三角形が名前 name のシーンノードから来たものか */
static int tri_from_node(const YbrTriangle* t, const char* name)
{
	return t->sceneNode && t->sceneNode->name &&
		   !strcmp(t->sceneNode->name, name);
}

static int tri_is_mirror(const YbrTriangle* t)
{
	return tri_from_node(t, "Mirror");
}

/* ================================================================== */
/* 幾何ユーティリティのテスト                                         */
/* ================================================================== */
void test_geometry(void)
{
	group("geometry");

	/* 法線が +Y になる巻き方向 : cross(b-a, c-a) = (0,1,0) */
	Vector3 a = V(0, 0, 0), b = V(0, 0, 1), c = V(1, 0, 0);

	check_vec(YbrClosestPointOnTriangle(V(0.2f, 5.0f, 0.2f), a, b, c),
			  V(0.2f, 0.0f, 0.2f), 1e-5f, "closest point : 面の内側へ落ちる");
	check_vec(YbrClosestPointOnTriangle(V(-3.0f, 0.0f, -3.0f), a, b, c), a,
			  1e-5f, "closest point : 頂点へ吸着");
	check_vec(YbrClosestPointOnTriangle(V(0.5f, 0.0f, -2.0f), a, b, c),
			  V(0.5f, 0.0f, 0.0f), 1e-5f, "closest point : 辺へ吸着");

	float t, u, v;
	int r = YbrSegmentTriangleHit(V(0.25f, 1, 0.25f), V(0.25f, -1, 0.25f), a, b,
								  c, 0, &t, &u, &v);
	check(r == 1, "segment-tri : 表から当たる");
	check_near(t, 0.5f, 1e-5f, "segment-tri : t");
	check_near(u, 0.25f, 1e-5f, "segment-tri : u");
	check_near(v, 0.25f, 1e-5f, "segment-tri : v");

	r = YbrSegmentTriangleHit(V(0.25f, -1, 0.25f), V(0.25f, 1, 0.25f), a, b, c,
							  0, &t, NULL, NULL);
	check(r == -1, "segment-tri : 裏から当たると -1");
	r = YbrSegmentTriangleHit(V(0.25f, -1, 0.25f), V(0.25f, 1, 0.25f), a, b, c,
							  1, &t, NULL, NULL);
	check(r == 0, "segment-tri : cullBackFace で裏面は無視");

	r = YbrSegmentTriangleHit(V(5, 1, 5), V(5, -1, 5), a, b, c, 0, &t, NULL,
							  NULL);
	check(r == 0, "segment-tri : 外れる");

	/* 三角形の真上を平行に通る線分 (端点は三角形の外) は、
	 * 端点だけを見ると距離を過大評価してしまうケース */
	Vector3 s0 = V(-1.0f, 0.5f, 0.25f), s1 = V(2.0f, 0.5f, 0.25f);
	check_near(YbrSegmentTriangleDistance(s0, s1, a, b, c, NULL, NULL), 0.5f,
			   1e-5f, "segment-tri distance : 面の上を横断");

	check_near(YbrSegmentTriangleDistance(
				   V(0.25f, 2, 0.25f), V(0.25f, 1, 0.25f), a, b, c, NULL, NULL),
			   1.0f, 1e-5f, "segment-tri distance : 真上");
	check_near(
		YbrSegmentTriangleDistance(V(0.25f, 1, 0.25f), V(0.25f, -1, 0.25f), a,
								   b, c, NULL, NULL),
		0.0f, 1e-5f, "segment-tri distance : 貫通は 0");
}

/* ================================================================== */
/* 当たり判定情報の作成                                               */
/* ================================================================== */
void test_build(const YbrSolid* col)
{
	group("build");

	check(col != NULL, "YbrSolidBuild が成功する");
	if (!col) return;

	int expect = GRID_TRIS + 12 * 2;
	check(YbrSolidGetTriangleCount(col) == expect, "三角形の総数");
	check(1 < YbrSolidGetNodeCount(col), "8 分木が分割されている");
	check(0 < YbrSolidGetDepth(col), "深さが 1 以上");

	Vector3 bmin, bmax;
	check(YbrSolidGetBounds(col, &bmin, &bmax) == 1, "全体 AABB が取れる");
	check(bmin.x <= -4.0f && 2.5f <= bmax.x, "AABB が床と箱を含む");
	check_near(bmin.y, 0.0f, 1e-4f, "AABB の下端は床");
	check_near(bmax.y, 1.0f, 1e-4f, "AABB の上端は箱の天面");

	/* 床の三角形はすべて法線が +Y であること */
	int badNormal = 0, groundCount = 0, mirrorCount = 0, badMesh = 0;
	for (int i = 0; i < YbrSolidGetTriangleCount(col); i++) {
		const YbrTriangle* t = YbrSolidGetTriangle(col, i);
		if (tri_from_node(t, "Ground")) {
			groundCount++;
			if (1e-4f < fabsf(t->normal.y - 1.0f)) badNormal++;
			/* シーンノードと一緒にメッシュへの参照も入っている */
			if (!t->mesh || strcmp(t->mesh->id, "GroundMesh")) badMesh++;
		}
		if (tri_is_mirror(t)) mirrorCount++;
		/* YbrSolid はモデルから作らないので node は常に NULL */
		if (t->node) badMesh++;
	}
	check(groundCount == GRID_TRIS, "床の三角形数");
	check(badNormal == 0, "床の法線はすべて +Y");
	check(badMesh == 0, "sceneNode と mesh が入り、node は NULL");
	check(mirrorCount == 12, "鏡映した立方体も取り込まれる");

	/* 鏡映 (行列式が負) しても法線が外を向いていること。
	 * 立方体の重心から各ポリゴンの重心へのベクトルと法線の向きが
	 * そろっていれば外向き。 */
	Vector3 mirrorCenter = V(-2.0f, 0.5f, 0.0f);
	int inward = 0, checked = 0;
	for (int i = 0; i < YbrSolidGetTriangleCount(col); i++) {
		const YbrTriangle* t = YbrSolidGetTriangle(col, i);
		if (!tri_is_mirror(t)) continue;
		Vector3 g = Vector3Scale(
			Vector3Add(Vector3Add(t->v[0], t->v[1]), t->v[2]), 1.0f / 3.0f);
		checked++;
		if (Vector3DotProduct(t->normal, Vector3Subtract(g, mirrorCenter)) <=
			0.0f)
			inward++;
	}
	check(checked == 12 && inward == 0, "鏡映しても法線は外向きのまま");
}

/* ================================================================== */
/* 線分                                                               */
/* ================================================================== */
void test_segment(const YbrSolid* col)
{
	group("segment");

	YbrRayHit h;

	check(YbrSolidSegment(col, V(0, 3, 0), V(0, -1, 0), NULL, &h) == 1,
		  "真上から床へ");
	check_vec(h.point, V(0, 0, 0), 1e-4f, "衝突点");
	check_vec(h.normal, V(0, 1, 0), 1e-4f, "衝突したポリゴンの法線");
	check_near(h.t, 0.75f, 1e-4f, "線分内の位置 t");
	check_near(h.distance, 3.0f, 1e-3f, "始点からの距離");
	check(h.frontFace == 1, "表から当たっている");
	check(tri_from_node(&h.triangle, "Ground"), "衝突したノード名");
	/* 3 頂点も返ってくる */
	check_near(h.triangle.v[0].y, 0.0f, 1e-4f, "ポリゴンの頂点 0 は床の高さ");
	check_near(h.triangle.v[1].y, 0.0f, 1e-4f, "ポリゴンの頂点 1 は床の高さ");
	check_near(h.triangle.v[2].y, 0.0f, 1e-4f, "ポリゴンの頂点 2 は床の高さ");

	check(YbrSolidSegment(col, V(0, 3, 0), V(0, 1, 0), NULL, &h) == 0,
		  "床まで届かない線分は当たらない");
	check(YbrSolidSegment(col, V(50, 3, 50), V(50, -3, 50), NULL, &h) == 0,
		  "床の外は当たらない");

	/* 下から上へ = 床の裏面 */
	check(YbrSolidSegment(col, V(0, -1, 0), V(0, 3, 0), NULL, &h) == 1,
		  "下から上へ : 当たる");
	check(h.frontFace == 0, "下から上へ : 裏面");
	YbrQueryOptions q = YbrQueryOptionsDefaults();
	q.cullBackFace = 1;
	check(YbrSolidSegment(col, V(0, -1, 0), V(0, 3, 0), &q, &h) == 0,
		  "cullBackFace で裏面は無視される");

	/* 箱の側面。もっとも手前の 1 枚が返ること */
	check(YbrSolidSegment(col, V(-5, 0.5f, 0), V(5, 0.5f, 0), NULL, &h) == 1,
		  "横断すると箱に当たる");
	check(h.point.x < 0.0f, "もっとも手前 (左側の箱) に当たる");
	check(tri_is_mirror(&h.triangle), "手前の箱は Mirror");

	/* 総当たりとの突き合わせ */
	int mismatch = 0;
	for (int i = 0; i < 2000; i++) {
		Vector3 a = V(frnd(-6, 6), frnd(-2, 4), frnd(-6, 6));
		Vector3 b = V(frnd(-6, 6), frnd(-2, 4), frnd(-6, 6));
		float bt;
		int bhit = brute_segment(col, a, b, 0, &bt, NULL);
		int ohit = YbrSolidSegment(col, a, b, NULL, &h);
		if (bhit != ohit) {
			mismatch++;
			continue;
		}
		if (bhit && 1e-4f < fabsf(bt - h.t)) mismatch++;
	}
	check(mismatch == 0, "総当たりと一致 (ランダム 2000 本)");
}

/* ================================================================== */
/* 球                                                                 */
/* ================================================================== */
void test_sphere(const YbrSolid* col)
{
	group("sphere");

	YbrShapeHit h;

	check(YbrSolidSphere(col, V(0, 0.5f, 0), 1.0f, NULL, &h) == 1,
		  "床にめり込む");
	check_near(h.depth, 0.5f, 1e-4f, "めり込み量");
	check_vec(h.normal, V(0, 1, 0), 1e-4f, "押し出す向きは上");
	check_near(h.point.y, 0.0f, 1e-4f, "接触点は床の上");
	check_vec(h.resolve, V(0, 0.5f, 0), 1e-3f, "resolve = 上へ 0.5");
	check(1 < h.count, "複数のポリゴンに接触している");

	check(YbrSolidSphere(col, V(0, 2.0f, 0), 1.0f, NULL, &h) == 0,
		  "浮いていれば当たらない");
	check(h.count == 0, "当たらないとき count は 0");

	/* 床の上に半径ぶん浮かせた球は、ちょうど接するので当たらない */
	check(YbrSolidSphere(col, V(0, 1.0f, 0), 1.0f, NULL, &h) == 0,
		  "ちょうど接する場合");

	/* 隅 : 床と箱の両方に触る */
	check(YbrSolidSphere(col, V(-1.6f, 0.3f, 0.0f), 0.5f, NULL, &h) == 1,
		  "床と箱の角");
	check(0.0f < h.depth, "めり込み量が正");

	int mismatch = 0;
	for (int i = 0; i < 2000; i++) {
		Vector3 c = V(frnd(-6, 6), frnd(-1, 3), frnd(-6, 6));
		float r = frnd(0.05f, 1.5f);
		float bd;
		int bhit = brute_sphere(col, c, r, &bd);
		int ohit = YbrSolidSphere(col, c, r, NULL, &h);
		if (bhit != ohit) {
			mismatch++;
			continue;
		}
		if (bhit && 1e-4f < fabsf(bd - h.depth)) mismatch++;
	}
	check(mismatch == 0, "総当たりと一致 (ランダム 2000 個)");
}

/* ================================================================== */
/* カプセル                                                           */
/* ================================================================== */
void test_capsule(const YbrSolid* col)
{
	group("capsule");

	YbrShapeHit h;

	check(
		YbrSolidCapsule(col, V(0, 1.5f, 0), V(0, 3.0f, 0), 0.5f, NULL, &h) == 0,
		"浮いているカプセル");
	check(
		YbrSolidCapsule(col, V(0, 0.3f, 0), V(0, 2.0f, 0), 0.5f, NULL, &h) == 1,
		"床にめり込むカプセル");
	check_near(h.depth, 0.2f, 1e-4f, "めり込み量");
	check_vec(h.normal, V(0, 1, 0), 1e-4f, "押し出す向きは上");

	/* 横倒しのカプセルが箱に当たる */
	check(YbrSolidCapsule(col, V(-5, 0.5f, 0), V(5, 0.5f, 0), 0.2f, NULL, &h) ==
			  1,
		  "横倒しのカプセルが箱に当たる");

	/* 軸が床を貫いている場合 */
	check(YbrSolidCapsule(col, V(0, -1, 0), V(0, 1, 0), 0.3f, NULL, &h) == 1,
		  "軸が床を貫いている");
	check(0.0f < h.depth, "貫通時もめり込み量が正");

	int mismatch = 0;
	for (int i = 0; i < 1500; i++) {
		Vector3 a = V(frnd(-6, 6), frnd(-1, 3), frnd(-6, 6));
		Vector3 b = V(a.x + frnd(-2, 2), a.y + frnd(-2, 2), a.z + frnd(-2, 2));
		float r = frnd(0.05f, 1.0f);
		float bd;
		int bhit = brute_capsule(col, a, b, r, &bd);
		int ohit = YbrSolidCapsule(col, a, b, r, NULL, &h);
		if (bhit != ohit) {
			mismatch++;
			continue;
		}
		if (bhit && 1e-4f < fabsf(bd - h.depth)) mismatch++;
	}
	check(mismatch == 0, "総当たりと一致 (ランダム 1500 本)");
}

/* ================================================================== */
/* 三角形                                                             */
/* ================================================================== */
void test_triangle(const YbrSolid* col)
{
	group("triangle");

	YbrTriHit h;

	/* 床をまたぐ縦向きの三角形 */
	check(YbrSolidTriangle(col, V(0, -1, 0), V(1, -1, 0), V(0.5f, 1, 0), NULL,
						   &h) == 1,
		  "床をまたぐ三角形");
	check_near(h.pointA.y, 0.0f, 1e-4f, "交差線分の端点 A は床の高さ");
	check_near(h.pointB.y, 0.0f, 1e-4f, "交差線分の端点 B は床の高さ");
	check_near(h.point.y, 0.0f, 1e-4f, "中点も床の高さ");
	check(0 < h.count, "交差したポリゴン数");
	check(tri_from_node(&h.triangle, "Ground"), "交差したポリゴンの情報");

	check(YbrSolidTriangle(col, V(0, 2, 0), V(1, 2, 0), V(0.5f, 3, 0), NULL,
						   &h) == 0,
		  "床の上に浮いた三角形は当たらない");
	check(YbrSolidTriangle(col, V(20, -1, 20), V(21, -1, 20), V(20, 1, 20),
						   NULL, &h) == 0,
		  "床の外は当たらない");

	/* 箱を貫く三角形 */
	check(YbrSolidTriangle(col, V(1.0f, 0.5f, 0), V(3.0f, 0.5f, 0),
						   V(2.0f, 1.5f, 0), NULL, &h) == 1,
		  "箱を貫く三角形");
}

/* ================================================================== */
/* 列挙 / タグ                                                        */
/* ================================================================== */
typedef struct CountCtx {
	int n;
	int stopAt;
} CountCtx;

static int count_visitor(const YbrTriangle* tri, void* ud)
{
	CountCtx* c = (CountCtx*)ud;
	(void)tri;
	c->n++;
	return (c->stopAt <= 0 || c->n < c->stopAt);
}

static int filter_ground_only(const YbrNode* node, const YbrMesh* mesh,
							  unsigned int* outTag, void* ud)
{
	(void)mesh;
	(void)ud;
	if (node->name && !strcmp(node->name, "Ground")) {
		*outTag = 1u;
		return 1;
	}
	*outTag = 2u;
	return 1;
}

void test_overlap_and_tags(const YbrScene* scene, const YbrSolid* col)
{
	group("overlap / tag");

	CountCtx c;
	c.n = 0;
	c.stopAt = 0;
	int n = YbrSolidOverlapSphere(col, V(0, 0.1f, 0), 1.0f, NULL, count_visitor,
								  &c);
	check(0 < n && n == c.n, "OverlapSphere が呼ばれた回数と一致");

	c.n = 0;
	c.stopAt = 3;
	n = YbrSolidOverlapSphere(col, V(0, 0.1f, 0), 1.0f, NULL, count_visitor,
							  &c);
	check(n == 3, "visitor が 0 を返すと打ち切られる");

	c.n = 0;
	c.stopAt = 0;
	n = YbrSolidOverlapBox(col, V(-0.6f, -0.1f, -0.6f), V(0.6f, 0.1f, 0.6f),
						   NULL, count_visitor, &c);
	check(0 < n, "OverlapBox で床の一部が取れる");

	/* 同じ三角形が 2 度返らないこと (8 分木に重複登録していない) */
	int total = YbrSolidGetTriangleCount(col);
	c.n = 0;
	c.stopAt = 0;
	n = YbrSolidOverlapBox(col, V(-100, -100, -100), V(100, 100, 100), NULL,
						   count_visitor, &c);
	check(n == total, "全体を覆う箱でちょうど全三角形 (重複なし)");

	/* タグで絞り込む */
	YbrSolidBuildOptions bo = YbrSolidBuildDefaults();
	bo.filter = filter_ground_only;
	YbrSolid* tagged = YbrSolidBuild(scene, &bo);
	check(tagged != NULL, "filter 付きで作れる");
	if (!tagged) return;

	YbrQueryOptions q = YbrQueryOptionsDefaults();
	q.tagMask = 1u; /* 床だけ */
	YbrRayHit h;
	check(YbrSolidSegment(tagged, V(-5, 0.5f, 0), V(5, 0.5f, 0), &q, &h) == 0,
		  "tagMask=床 だと箱には当たらない");
	q.tagMask = 2u; /* 箱だけ */
	check(YbrSolidSegment(tagged, V(0, 3, 0), V(0, -1, 0), &q, &h) == 0,
		  "tagMask=箱 だと床には当たらない");
	q.tagMask = 0u; /* 全部 */
	check(YbrSolidSegment(tagged, V(0, 3, 0), V(0, -1, 0), &q, &h) == 1,
		  "tagMask=0 なら全部見る");

	YbrSolidUnload(tagged);
}

/* ================================================================== */
/* 構築オプション / 縮退ケース                                        */
/* ================================================================== */
void test_options(const YbrScene* scene)
{
	group("options");

	YbrSolidBuildOptions bo = YbrSolidBuildDefaults();
	bo.maxDepth = 1;
	bo.maxTrianglesPerNode = 1;
	YbrSolid* shallow = YbrSolidBuild(scene, &bo);
	check(shallow != NULL, "maxDepth=1 でも作れる");
	if (shallow) {
		YbrRayHit h;
		check(YbrSolidSegment(shallow, V(0, 3, 0), V(0, -1, 0), NULL, &h) == 1,
			  "maxDepth=1 でも当たる");
		check(YbrSolidGetDepth(shallow) <= 1, "深さが制限されている");
		YbrSolidUnload(shallow);
	}

	bo = YbrSolidBuildDefaults();
	bo.maxDepth = 999; /* 範囲外は丸められる */
	bo.looseness = 0.1f;
	YbrSolid* deep = YbrSolidBuild(scene, &bo);
	check(deep != NULL, "範囲外の値でも作れる");
	if (deep) {
		YbrRayHit h;
		check(YbrSolidSegment(deep, V(0, 3, 0), V(0, -1, 0), NULL, &h) == 1,
			  "範囲外の値でも当たる");
		check(YbrSolidGetDepth(deep) <= 16, "深さは 16 まで");
		YbrSolidUnload(deep);
	}

	/* 空のシーン */
	YbrScene empty;
	memset(&empty, 0, sizeof(empty));
	YbrSolid* none = YbrSolidBuild(&empty, NULL);
	check(none != NULL, "空のシーンでも NULL にならない");
	if (none) {
		YbrRayHit h;
		YbrShapeHit s;
		YbrTriHit t;
		check(YbrSolidGetTriangleCount(none) == 0, "三角形 0");
		check(YbrSolidGetBounds(none, NULL, NULL) == 0, "AABB は無い");
		check(YbrSolidSegment(none, V(0, 1, 0), V(0, -1, 0), NULL, &h) == 0,
			  "空でも線分判定が落ちない");
		check(YbrSolidSphere(none, V(0, 0, 0), 1.0f, NULL, &s) == 0,
			  "空でも球判定が落ちない");
		check(YbrSolidTriangle(none, V(0, 0, 0), V(1, 0, 0), V(0, 0, 1), NULL,
							   &t) == 0,
			  "空でも三角形判定が落ちない");
		YbrSolidUnload(none);
	}

	/* NULL 安全 */
	YbrRayHit h;
	check(YbrSolidSegment(NULL, V(0, 1, 0), V(0, -1, 0), NULL, &h) == 0,
		  "col が NULL でも落ちない");
	YbrSolidUnload(NULL);
	check(1, "YbrSolidUnload(NULL) が落ちない");

	/* 一部のノードだけから作る */
	YbrSolid* part =
		YbrSolidBuildFromNode(scene, &scene->roots[1], MatrixIdentity(), NULL);
	check(part != NULL, "ノード指定で作れる");
	if (part) {
		check(YbrSolidGetTriangleCount(part) == 12, "立方体 1 個ぶんだけ");
		YbrSolidUnload(part);
	}
}

/* ================================================================== */
/* スイープ (動く球)                                                  */
/* ================================================================== */
void test_sweep(void)
{
	group("sweep");

	/* 原点まわりの三角形 (法線 +Y) */
	Vector3 t0 = V(-1, 0, -1), t1 = V(-1, 0, 1), t2 = V(1, 0, -1);

	float t;
	Vector3 pt, n;
	/* 真上から落とす : 半径 0.5 なので y=0.5 で接触 */
	check(YbrSweepSphereTriangle(V(-0.5f, 4, -0.5f), V(-0.5f, -4, -0.5f), 0.5f,
								 t0, t1, t2, &t, &pt, &n) == 1,
		  "面に当たる");
	check_near(4.0f - t * 8.0f, 0.5f, 1e-3f, "接触するのは中心が y=0.5 のとき");
	check_vec(n, V(0, 1, 0), 1e-3f, "押し出す向きは上");
	check_near(pt.y, 0.0f, 1e-3f, "接触点は面の上");

	/* 外れる */
	check(YbrSweepSphereTriangle(V(9, 4, 9), V(9, -4, 9), 0.5f, t0, t1, t2, &t,
								 NULL, NULL) == 0,
		  "遠くを通れば当たらない");

	/* 辺をかすめる : 三角形の外側だが半径ぶん届く */
	check(YbrSweepSphereTriangle(V(-1.3f, 4, 0.0f), V(-1.3f, -4, 0.0f), 0.5f,
								 t0, t1, t2, &t, &pt, &n) == 1,
		  "辺をかすめる");

	/* 最初から接触している */
	check(YbrSweepSphereTriangle(V(0, 0.1f, -0.5f), V(0, 3, -0.5f), 0.5f, t0,
								 t1, t2, &t, NULL, NULL) == 1,
		  "出発時点で接触");
	check_near(t, 0.0f, 1e-6f, "その場合 t = 0");

	/* 高速移動でもすり抜けない (離散判定なら抜ける速度) */
	{
		YbrRayHit h;
		TestScene ts;
		make_scene(&ts);
		YbrSolid* col = YbrSolidBuild(&ts.scene, NULL);
		check(col != NULL, "ステージを作れる");
		if (!col) return;

		check(YbrSolidSweepSphere(col, V(0, 50, 0), V(0, -50, 0), 0.1f, NULL,
								  &h) == 1,
			  "高速で落ちてもすり抜けない");
		check_near(h.point.y, 0.0f, 1e-3f, "床に当たる");
		check_vec(h.normal, V(0, 1, 0), 1e-3f, "法線は上");
		check(tri_from_node(&h.triangle, "Ground"), "当たったのは床");

		/* 離散判定だとこの位置では当たらないが、スイープなら当たる */
		YbrShapeHit s;
		check(YbrSolidSphere(col, V(0, 50, 0), 0.1f, NULL, &s) == 0,
			  "離散判定では当たらない位置");

		check(YbrSolidSweepSphere(col, V(50, 50, 50), V(60, 50, 60), 0.1f, NULL,
								  &h) == 0,
			  "何も無いところは当たらない");
		YbrSolidUnload(col);
	}
}
