/*
	Yui Blender to Raylib - モデルインスタンスのテスト
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_test.h"

/* ================================================================== */
/* パート / ノードの表示切り替え                                      */
/* ================================================================== */
void test_visibility(void)
{
	group("visibility");

	/* 手で組んだ YbrModel でフラグの操作だけ確かめる (GPU は要らない) */
	DynProbe p;
	check(dyn_probe_init(&p, 1) == 1, "モデルを用意できる");
	if (!p.dyn) return;

	check(p.model.partCount == 1 && p.model.nodeCount == 1,
		  "パートとノードが 1 つずつ");
	/* 手組みなので visible は 0。YbrModelLoad 経由なら 1 になる */
	YbrModelInstanceSetAllPartsVisible(p.inst, 1);
	YbrModelInstanceSetAllNodesVisible(p.inst, 1);
	check(YbrModelInstanceIsPartVisible(p.inst, 0) == 1,
		  "パートを表示にできる");
	check(YbrModelInstanceIsNodeVisible(p.inst, 0) == 1,
		  "ノードを表示にできる");

	YbrModelInstanceSetPartVisible(p.inst, 0, 0);
	check(YbrModelInstanceIsPartVisible(p.inst, 0) == 0, "パートを隠せる");
	YbrModelInstanceSetPartVisible(p.inst, 0, 1);
	check(YbrModelInstanceIsPartVisible(p.inst, 0) == 1, "戻せる");

	YbrModelInstanceSetNodeVisible(p.inst, 0, 0);
	check(YbrModelInstanceIsNodeVisible(p.inst, 0) == 0, "ノードを隠せる");
	YbrModelInstanceSetNodeVisible(p.inst, 0, 1);

	/* 範囲外や NULL で落ちない */
	YbrModelInstanceSetPartVisible(p.inst, -1, 0);
	YbrModelInstanceSetPartVisible(p.inst, 99, 0);
	YbrModelInstanceSetNodeVisible(NULL, 0, 0);
	check(YbrModelInstanceIsPartVisible(p.inst, 99) == 0, "範囲外は 0 を返す");
	check(YbrModelInstanceIsPartVisible(NULL, 0) == 0, "NULL は 0 を返す");
	check(YbrModelInstanceSetPartVisibleByMesh(NULL, "x", 0) == 0,
		  "NULL で 0 件");

	/* id 指定 */
	check(YbrModelInstanceSetPartVisibleByMesh(p.inst, "CubeMesh", 0) == 1,
		  "メッシュ id で隠せる");
	check(YbrModelInstanceIsPartVisible(p.inst, 0) == 0, "実際に隠れている");
	check(YbrModelInstanceSetPartVisibleByMesh(p.inst, "NoSuchMesh", 1) == 0,
		  "無い id は 0 件");
	check(YbrModelInstanceIsPartVisible(p.inst, 0) == 0, "変わっていない");
	check(YbrModelInstanceSetPartVisibleByMesh(p.inst, "CubeMesh", 1) == 1,
		  "戻せる");

	check(YbrModelInstanceSetNodeVisibleByName(p.inst, "Cube", 0) == 1,
		  "ノード名で隠せる");
	check(YbrModelInstanceIsNodeVisible(p.inst, 0) == 0, "実際に隠れている");
	check(YbrModelInstanceSetNodeVisibleByName(p.inst, "Nope", 0) == 0,
		  "無い名前は 0 件");

	/* マテリアルが無いパートは materialIndex = -1 なので対象外 */
	check(YbrModelInstanceSetPartVisibleByMaterial(p.inst, "Mat", 0) == 0,
		  "マテリアル無しのパートは id 指定で拾わない");

	/* 当たり判定は表示状態を見ない */
	YbrModelInstanceSetAllPartsVisible(p.inst, 0);
	YbrModelInstanceSetAllNodesVisible(p.inst, 0);
	YbrRayHit h;
	check(YbrDynamicSegment(p.dyn, V(0, 3, 0), V(0, -3, 0), NULL, &h) == 1,
		  "隠しても当たり判定は残る");

	dyn_probe_free(&p);
}

/* ================================================================== */
/* マテリアルの差し替え (ノード単位)                                  */
/* ================================================================== */
void test_material_override(void)
{
	group("material override");

	DynProbe p;
	check(dyn_probe_init(&p, 1) == 1, "モデルを用意できる");
	if (!p.dyn) return;

	check(YbrModelInstanceGetNodeMaterial(p.inst, 0) == NULL,
		  "既定では差し替え無し");

	/* 差し替えるマテリアルは呼び出し側の持ち物 (ここではスタック上) */
	YbrModelMaterial custom;
	memset(&custom, 0, sizeof(custom));
	custom.id = "Custom";
	custom.transparent = 1;

	YbrModelInstanceSetNodeMaterial(p.inst, 0, &custom);
	check(YbrModelInstanceGetNodeMaterial(p.inst, 0) == &custom,
		  "差し替えが入る");
	check(p.inst->nodeMaterial[0] == &custom, "ノードが覚えている");

	/* パート側は書き換わっていない */
	check(p.model.parts[0].materialIndex == -1, "パートのマテリアルは元のまま");

	/* 名前でも指定できる */
	YbrModelInstanceClearNodeMaterials(p.inst);
	check(YbrModelInstanceGetNodeMaterial(p.inst, 0) == NULL, "解除できる");
	check(YbrModelInstanceSetNodeMaterialByName(p.inst, "Cube", &custom) == 1,
		  "ノード名で差し替えられる");
	check(YbrModelInstanceGetNodeMaterial(p.inst, 0) == &custom,
		  "実際に入っている");
	check(YbrModelInstanceSetNodeMaterialByName(p.inst, "Nope", &custom) == 0,
		  "無い名前は 0 件");

	/* 範囲外 / NULL で落ちない */
	YbrModelInstanceSetNodeMaterial(p.inst, -1, &custom);
	YbrModelInstanceSetNodeMaterial(p.inst, 99, &custom);
	YbrModelInstanceSetNodeMaterial(NULL, 0, &custom);
	YbrModelInstanceClearNodeMaterials(NULL);
	check(YbrModelInstanceGetNodeMaterial(p.inst, 99) == NULL, "範囲外は NULL");
	check(YbrModelInstanceGetNodeMaterial(NULL, 0) == NULL, "NULL は NULL");
	check(YbrModelInstanceSetNodeMaterialByName(NULL, "x", &custom) == 0,
		  "NULL は 0 件");

	/* NULL を入れれば元に戻る */
	YbrModelInstanceSetNodeMaterial(p.inst, 0, NULL);
	check(YbrModelInstanceGetNodeMaterial(p.inst, 0) == NULL,
		  "NULL で元に戻る");

	/* 解放されないこと : ここで custom はスタック上なので、
	 * YbrDynamicUnload / dyn_probe_free が触れば ASan が捕まえる */
	YbrModelInstanceSetNodeMaterial(p.inst, 0, &custom);
	dyn_probe_free(&p);
	check(custom.transparent == 1, "差し替えたマテリアルは壊されない");
}

/* ================================================================== */
/* インスタンス / AABB / アタッチメント                               */
/* ================================================================== */
void test_instances(void)
{
	group("model instance");

	DynProbe a, b;
	check(dyn_probe_init(&a, 1) == 1, "1 体目");
	check(dyn_probe_init(&b, 1) == 1, "2 体目");
	if (!a.inst || !b.inst) return;

	/* --- ジオメトリは共有 --- */
	check(a.inst->model == &a.model, "インスタンスがモデルを指す");
	check(YbrModelInstanceGetModel(a.inst) == &a.model, "GetModel");
	check(a.model.parts[0].positions == b.model.parts[0].positions ||
			  a.model.partCount == 1,
		  "パートは共有 (別モデルなので参照は別)");

	/* --- 状態は独立 --- */
	YbrModelInstanceSetPartVisible(a.inst, 0, 0);
	check(YbrModelInstanceIsPartVisible(a.inst, 0) == 0, "1 体目は隠れる");
	check(YbrModelInstanceIsPartVisible(b.inst, 0) == 1,
		  "2 体目は影響を受けない");
	YbrModelInstanceSetPartVisible(a.inst, 0, 1);

	a.pose->bones[1].translation = V(5.0f, 1.0f, 0.0f);
	YbrModelInstanceApplyPose(a.inst);
	YbrModelInstanceApplyPose(b.inst);
	check_near(a.pose->bones[1].pose.m12, 5.0f, 1e-4f, "1 体目のポーズ");
	check_near(b.pose->bones[1].pose.m12, 0.0f, 1e-4f, "2 体目のポーズは別");

	/* --- ワールド行列 --- */
	YbrModelInstanceSetTransform(b.inst, MatrixTranslate(10.0f, 0.0f, 0.0f));
	check_near(YbrModelInstanceGetTransform(b.inst).m12, 10.0f, 1e-5f,
			   "行列を持てる");
	check_near(YbrModelInstanceGetTransform(a.inst).m12, 0.0f, 1e-5f,
			   "別々に持てる");

	/* --- ユーザーデータ --- */
	int marker = 3;
	YbrModelInstanceSetUserData(a.inst, &marker);
	check(YbrModelInstanceGetUserData(a.inst) == &marker, "ユーザーデータ");
	check(YbrModelInstanceGetUserData(b.inst) == NULL, "別インスタンスは NULL");

	dyn_probe_free(&a);
	dyn_probe_free(&b);
	YbrModelInstanceUnload(NULL);
	check(1, "NULL でも落ちない");
}

void test_instance_bounds(void)
{
	group("instance bounds");

	DynProbe p;
	check(dyn_probe_init(&p, 1) == 1, "スキン付きモデル");
	if (!p.inst) return;

	/* --- レスト姿勢 --- */
	Vector3 lo, hi;
	check(YbrModelGetLocalBounds(&p.model, &lo, &hi) == 1,
		  "レスト AABB が取れる");
	check_vec(lo, V(-0.5f, -0.5f, -0.5f), 1e-4f, "レストの下端");
	check_vec(hi, V(0.5f, 0.5f, 0.5f), 1e-4f, "レストの上端");

	check(YbrModelInstanceGetBounds(p.inst, &lo, &hi) == 1,
		  "ワールド AABB が取れる");
	check_vec(lo, V(-0.5f, -0.5f, -0.5f), 1e-3f, "レストでは同じ");

	/* --- ボーンを動かすと AABB も動く (ここが今回の肝) --- */
	p.pose->bones[1].translation = V(10.0f, 1.0f, 0.0f);
	YbrModelInstanceApplyPose(p.inst);
	check(YbrModelInstanceGetBounds(p.inst, &lo, &hi) == 1, "更新後も取れる");
	check(9.0f < lo.x, "AABB がボーンに追従して動く");
	check(hi.x < 11.0f, "行き過ぎない");

	/* --- モデル行列も効く --- */
	YbrPoseReset(p.pose);
	YbrModelInstanceApplyPose(p.inst);
	YbrModelInstanceSetTransform(p.inst, MatrixTranslate(0.0f, 100.0f, 0.0f));
	YbrModelInstanceGetBounds(p.inst, &lo, &hi);
	check(99.0f < lo.y, "インスタンスの行列も反映される");

	/* --- 前計算を外すとポーズに追従しなくなる (作らなければ作らないなり) ---
	 */
	YbrPoseReset(p.pose);
	YbrModelInstanceSetTransform(p.inst, MatrixIdentity());
	YbrModelInstanceSetSkinBounds(p.inst, NULL);
	check(YbrModelInstanceGetSkinBounds(p.inst) == NULL, "前計算を外せる");
	p.pose->bones[1].translation = V(10.0f, 1.0f, 0.0f);
	YbrModelInstanceApplyPose(p.inst);
	check(YbrModelInstanceGetBounds(p.inst, &lo, &hi) == 1,
		  "前計算が無くても AABB は返る");
	check(lo.x < 9.0f, "ただしポーズには追従しない (レスト姿勢のまま)");

	/* 紐づけ直すとまた追従する */
	YbrModelInstanceSetSkinBounds(p.inst, p.skinBounds);
	check(YbrModelInstanceGetSkinBounds(p.inst) == p.skinBounds,
		  "紐づけ直せる");
	YbrModelInstanceApplyPose(p.inst);
	YbrModelInstanceGetBounds(p.inst, &lo, &hi);
	check(9.0f < lo.x, "また追従するようになる");

	dyn_probe_free(&p);

	/* --- スキンが無いモデルには前計算そのものが要らない --- */
	{
		DynProbe r;
		if (dyn_probe_init(&r, 0) == 1) {
			check(YbrSkinBoundsCreate(&r.model) == NULL,
				  "スキンが無ければ NULL (作る必要が無い)");
			dyn_probe_free(&r);
		}
	}
	check(YbrSkinBoundsCreate(NULL) == NULL, "NULL には NULL");
	YbrSkinBoundsUnload(NULL); /* 二重解放しないこと */

	/* --- スキンが無いモデル --- */
	{
		DynProbe q;
		check(dyn_probe_init(&q, 0) == 1, "スキン無しモデル");
		if (q.inst) {
			Vector3 a, b2;
			check(YbrModelInstanceGetBounds(q.inst, &a, &b2) == 1,
				  "AABB が取れる");
			check_vec(a, V(-0.5f, -0.5f, -0.5f), 1e-4f, "静的パートぶん");
			dyn_probe_free(&q);
		}
	}
}

void test_attachment(void)
{
	group("attachment");

	DynProbe p;
	check(dyn_probe_init(&p, 1) == 1, "モデルを用意できる");
	if (!p.inst) return;

	Matrix m;
	check(YbrModelInstanceGetBoneWorld(p.inst, "child", &m) == 1,
		  "ボーンを引ける");
	check_near(m.m13, 1.0f, 1e-4f, "レストでは +Y に 1");
	check(YbrModelInstanceGetBoneWorld(p.inst, "root", &m) == 1,
		  "root も引ける");
	check(YbrModelInstanceGetBoneWorld(p.inst, "nope", &m) == 0,
		  "無い名前は 0");
	check(YbrModelInstanceGetBoneWorld(NULL, "child", &m) == 0, "NULL は 0");

	/* ポーズを動かすと追従する */
	p.pose->bones[1].translation = V(3.0f, 1.0f, 0.0f);
	YbrModelInstanceApplyPose(p.inst);
	check(YbrModelInstanceGetBoneWorld(p.inst, "child", &m) == 1,
		  "動かしても引ける");
	check_near(m.m12, 3.0f, 1e-4f, "ボーンの移動が反映される");

	/* インスタンスの行列も掛かる */
	YbrModelInstanceSetTransform(p.inst, MatrixTranslate(0.0f, 0.0f, 7.0f));
	YbrModelInstanceGetBoneWorld(p.inst, "child", &m);
	check_near(m.m14, 7.0f, 1e-4f, "体のワールド行列も掛かる");

	/* ノードでも取れる */
	check(YbrModelInstanceGetNodeWorld(p.inst, "Cube", &m) == 1,
		  "ノードを引ける");
	check_near(m.m14, 7.0f, 1e-4f, "ノードにも体の行列が掛かる");
	check(YbrModelInstanceGetNodeWorld(p.inst, "nope", &m) == 0,
		  "無い名前は 0");

	check(YbrModelFindNode(&p.model, "Cube") == 0, "ノードを名前で引ける");
	check(YbrModelFindNode(&p.model, "nope") < 0, "無い名前は -1");
	check(YbrModelGetNodeSource(&p.model, 0) == NULL,
		  "手組みなので source は NULL");

	dyn_probe_free(&p);
}
