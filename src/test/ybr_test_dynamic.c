/*
	Yui Blender to Raylib - ボーン追従の当たり判定 (YbrDynamic) のテスト
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_test.h"

void test_dynamic(void)
{
	group("dynamic (bone)");

	DynProbe p;
	check(dyn_probe_init(&p, 1) == 1, "YbrModel から作れる");
	if (!p.dyn) return;

	check(YbrDynamicGetInstance(p.dyn) == p.inst,
		  "元のインスタンスを覚えている");
	check(YbrDynamicGetTriangleCount(p.dyn) == 12, "三角形の数");
	check(YbrDynamicGetPartCount(p.dyn) == 1, "ボーン 1 本ぶんのパートになる");
	check(YbrDynamicGetPartBone(p.dyn, 0) == 1, "child ボーンに割り当てられる");
	check(YbrDynamicIsEnabled(p.dyn) == 1, "既定で有効");

	/* --- レストポーズ : child は原点から +Y に 1 --- */
	Vector3 lo, hi;
	check(YbrDynamicGetBounds(p.dyn, &lo, &hi) == 1, "ワールド AABB が取れる");
	check_vec(lo, V(-0.5f, -0.5f, -0.5f), 1e-4f, "レストでは元の位置");

	YbrRayHit h;
	check(YbrDynamicSegment(p.dyn, V(0, 3, 0), V(0, -3, 0), NULL, &h) == 1,
		  "上から当たる");
	check_near(h.point.y, 0.5f, 1e-4f, "天面に当たる");
	check_vec(h.normal, V(0, 1, 0), 1e-4f, "法線は上");
	check(YbrDynamicGetLastBone(p.dyn) == 1, "当たったボーンが分かる");
	check(YbrDynamicGetLastBoneName(p.dyn) &&
			  !strcmp(YbrDynamicGetLastBoneName(p.dyn), "child"),
		  "ボーン名も引ける");

	/* 三角形の出どころ : YbrModel から作るので node と mesh が入り、
	 * シーンツリーは通っていないので sceneNode は NULL */
	check(h.triangle.node == &p.part, "当たった三角形がモデルパートを指す");
	check(h.triangle.mesh == &p.mesh, "元のメッシュも指す");
	check(h.triangle.sceneNode == NULL, "YbrDynamic では sceneNode は NULL");

	/* --- ボーンを動かすと当たり判定も追従する --- */
	p.pose->bones[1].translation = V(5.0f, 1.0f, 0.0f); /* 親相対 */
	YbrPoseUpdate(p.pose);
	YbrDynamicUpdate(p.dyn);

	check(YbrDynamicSegment(p.dyn, V(0, 3, 0), V(0, -3, 0), NULL, &h) == 0,
		  "元の場所にはもう無い");
	check(YbrDynamicSegment(p.dyn, V(5, 3, 0), V(5, -3, 0), NULL, &h) == 1,
		  "移動先で当たる");
	check_vec(h.point, V(5.0f, 0.5f, 0.0f), 1e-3f, "衝突点もワールド空間");
	check(YbrDynamicGetTriangleCount(p.dyn) == 12, "作り直していない");

	/* --- ボーンの回転にも追従する ---
	 * Y 軸まわりに 90 度。立方体は原点に残るので、真上からの線分は
	 * 引き続き天面に当たるはず。 */
	YbrPoseReset(p.pose);
	float hf = 3.14159265f * 0.25f; /* 90 度の半分 */
	p.pose->bones[1].rotation = (Quaternion){0, sinf(hf), 0, cosf(hf)};
	YbrPoseUpdate(p.pose);
	YbrDynamicUpdate(p.dyn);
	check(YbrDynamicSegment(p.dyn, V(0, 3, 0), V(0, -3, 0), NULL, &h) == 1,
		  "回転しても当たる");
	check_near(h.point.y, 0.5f, 1e-3f, "天面の高さは変わらない");
	check_vec(h.normal, V(0, 1, 0), 1e-3f, "回した立方体でも上面の法線は上");

	/* --- モデル全体の行列 --- */
	YbrPoseReset(p.pose);
	YbrDynamicSetTransform(p.dyn, MatrixTranslate(0.0f, 0.0f, 10.0f));
	check(YbrDynamicSegment(p.dyn, V(0, 3, 0), V(0, -3, 0), NULL, &h) == 0,
		  "モデルごと動かすと外れる");
	check(YbrDynamicSegment(p.dyn, V(0, 3, 10), V(0, -3, 10), NULL, &h) == 1,
		  "動かした先で当たる");
	YbrDynamicSetTransform(p.dyn, MatrixIdentity());

	/* --- 球 / カプセル / 三角形 / スイープ --- */
	YbrShapeHit sh;
	check(YbrDynamicSphere(p.dyn, V(0, 0.9f, 0), 0.5f, NULL, &sh) == 1,
		  "球が当たる");
	check_near(sh.depth, 0.1f, 1e-3f, "めり込み量");
	check_vec(sh.normal, V(0, 1, 0), 1e-3f, "押し出す向き");

	check(YbrDynamicCapsule(p.dyn, V(0, 0.7f, 0), V(0, 3.0f, 0), 0.3f, NULL,
							&sh) == 1,
		  "カプセルが当たる");
	check(YbrDynamicCapsule(p.dyn, V(0, 1.5f, 0), V(0, 3.0f, 0), 0.3f, NULL,
							&sh) == 0,
		  "離れていれば当たらない");

	YbrTriHit th;
	check(YbrDynamicTriangle(p.dyn, V(-1, 0.5f, 0), V(1, 0.5f, 0), V(0, 2, 0),
							 NULL, &th) == 1,
		  "三角形が当たる");

	check(YbrDynamicSweepSphere(p.dyn, V(0, 60, 0), V(0, -60, 0), 0.1f, NULL,
								&h) == 1,
		  "高速でもすり抜けない");
	check_near(h.point.y, 0.5f, 1e-2f, "天面に当たる");

	/* --- 無効化 --- */
	YbrDynamicSetEnabled(p.dyn, 0);
	check(YbrDynamicSegment(p.dyn, V(0, 3, 0), V(0, -3, 0), NULL, &h) == 0,
		  "無効なら当たらない");
	YbrDynamicSetEnabled(p.dyn, 1);

	int marker = 7;
	YbrDynamicSetUserData(p.dyn, &marker);
	check(YbrDynamicGetUserData(p.dyn) == &marker, "ユーザーデータ");

	dyn_probe_free(&p);
	YbrDynamicUnload(NULL);
	check(1, "NULL でも落ちない");

	/* --- スキンの無いメッシュは静的パートになる --- */
	{
		DynProbe q;
		check(dyn_probe_init(&q, 0) == 1, "スキン無しでも作れる");
		if (q.dyn) {
			check(YbrDynamicGetPartCount(q.dyn) == 1, "パートは 1 つ");
			check(YbrDynamicGetPartBone(q.dyn, 0) == -1, "静的パートになる");
			YbrRayHit hh;
			check(YbrDynamicSegment(q.dyn, V(0, 3, 0), V(0, -3, 0), NULL,
									&hh) == 1,
				  "それでも当たる");
			check(YbrDynamicGetLastBone(q.dyn) == -1, "ボーンは -1");
			dyn_probe_free(&q);
		}
	}
}

static int count_dyn(YbrDynamic* dyn, void* ud)
{
	(void)dyn;
	(*(int*)ud)++;
	return 1;
}

void test_dynamic_world(void)
{
	group("dynamic world");

	enum { N = 4 };
	DynProbe probes[N];
	int ok = 1;
	for (int i = 0; i < N; i++) {
		if (!dyn_probe_init(&probes[i], 1)) {
			ok = 0;
			break;
		}
		YbrDynamicSetTransform(probes[i].dyn,
							   MatrixTranslate((float)i * 3.0f, 0, 0));
	}
	check(ok, "4 体作れる");
	if (!ok) return;

	YbrDynamicWorld* w = YbrDynamicWorldCreate();
	check(w != NULL, "ワールドを作れる");
	if (!w) return;

	for (int i = 0; i < N; i++)
		check(YbrDynamicWorldAdd(w, probes[i].dyn) == 1, "追加");
	check(YbrDynamicWorldGetCount(w) == N, "個数");
	check(YbrDynamicWorldAdd(w, probes[0].dyn) == 1 &&
			  YbrDynamicWorldGetCount(w) == N,
		  "同じものは二重に入らない");
	YbrDynamicWorldUpdate(w);

	int hits = 0;
	YbrDynamicWorldOverlapBox(w, V(2.5f, -1, -1), V(3.5f, 1, 1), count_dyn,
							  &hits);
	check(hits == 1, "AABB が重なるのは 1 つ");

	hits = 0;
	YbrDynamicWorldOverlapBox(w, V(-100, -100, -100), V(100, 100, 100),
							  count_dyn, &hits);
	check(hits == N, "全部を覆えば全部拾う");

	YbrDynamicRayHit rh;
	check(YbrDynamicWorldSegment(w, V(-5, 0, 0), V(30, 0, 0), NULL, &rh) == 1,
		  "横断");
	check(rh.dynamic == probes[0].dyn, "もっとも手前");
	check(rh.bone == 1, "当たったボーンも返る");

	/* ボーンを動かすと追従する */
	probes[0].pose->bones[1].translation = V(0.0f, 20.0f, 0.0f);
	YbrPoseUpdate(probes[0].pose);
	YbrDynamicWorldUpdate(w); /* 各 Dynamic の Update も呼ばれる */
	check(YbrDynamicWorldSegment(w, V(-5, 0, 0), V(30, 0, 0), NULL, &rh) == 1,
		  "動かしたあとも当たる");
	check(rh.dynamic == probes[1].dyn, "次の体が手前になる");

	YbrDynamicShapeHit sh;
	check(YbrDynamicWorldSphere(w, V(3.0f, 0, 0), 0.6f, NULL, &sh) == 1,
		  "球が当たる");
	check(sh.dynamic == probes[1].dyn, "当たった体が分かる");

	check(YbrDynamicWorldSweepSphere(w, V(-40, 0, 0), V(40, 0, 0), 0.1f, NULL,
									 &rh) == 1,
		  "スイープ");

	check(YbrDynamicWorldRemove(w, probes[1].dyn) == 1, "外せる");
	check(YbrDynamicWorldGetCount(w) == N - 1, "個数が減る");
	check(YbrDynamicWorldRemove(w, probes[1].dyn) == 0, "無いものは 0");

	YbrDynamicWorldUnload(w);
	for (int i = 0; i < N; i++) dyn_probe_free(&probes[i]);
	check(1, "ワールドを解放しても中身は自分で解放する");
}
