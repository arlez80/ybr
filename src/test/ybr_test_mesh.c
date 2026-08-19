/*
	Yui Blender to Raylib - メッシュの分割と最適化のテスト
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_test.h"

/* ================================================================== */
/* メッシュ分割                                                       */
/* ================================================================== */
void test_split_mesh(void)
{
	group("split mesh");

	float pos[4 * 3] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
	unsigned int idx[6] = {0, 1, 2, 0, 2, 3};
	unsigned int mi[2] = {0, 1};
	char* mats[2] = {(char*)"M0", (char*)"M1"};

	YbrMesh m;
	memset(&m, 0, sizeof(m));
	m.id = (char*)"Quad";
	m.vertexCount = 4;
	m.triangleCount = 2;
	m.positions = pos;
	m.indices = idx;
	m.materialIndices = mi;
	m.materialCount = 2;
	m.materials = mats;

	YbrSubmesh* subs = NULL;
	int n = 0;
	check(YbrSplitMesh(&m, &subs, &n) == 1, "分割できる");
	check(n == 2, "マテリアルごとに 2 つ");
	if (n == 2) {
		check(subs[0].materialIndex == 0 && subs[1].materialIndex == 1,
			  "マテリアル番号");
		check(subs[0].triangleCount == 1 && subs[1].triangleCount == 1,
			  "三角形 1 枚ずつ");
		check(subs[0].vertexCount == 3 && subs[1].vertexCount == 3,
			  "頂点 3 つずつ");
		/* index が付け替わっていること */
		int inRange = 1;
		for (int i = 0; i < 3; i++)
			if (3 <= subs[0].indices[i]) inRange = 0;
		check(inRange, "index が分割後の範囲に収まる");
		/* vertexMap で元の頂点に戻れること */
		check(subs[0].vertexMap[subs[0].indices[0]] == idx[0],
			  "vertexMap が元を指す");
	}
	YbrFreeSubmeshes(subs, n);

	/* マテリアル情報が無いメッシュ */
	m.materialIndices = NULL;
	m.materialCount = 0;
	m.materials = NULL;
	subs = NULL;
	n = 0;
	check(YbrSplitMesh(&m, &subs, &n) == 1, "マテリアル無しでも分割できる");
	check(n == 1 && subs && subs[0].triangleCount == 2, "1 つにまとまる");
	check(n == 1 && subs && subs[0].materialIndex == -1, "マテリアル番号は -1");
	YbrFreeSubmeshes(subs, n);

	/* 空のメッシュ */
	YbrMesh empty;
	memset(&empty, 0, sizeof(empty));
	subs = NULL;
	n = 0;
	check(YbrSplitMesh(&empty, &subs, &n) == 1 && n == 0,
		  "空のメッシュでも落ちない");
	YbrFreeSubmeshes(subs, n);
}

/* ================================================================== */
/* メッシュ最適化                                                     */
/* ================================================================== */
/* 三角形の集合を「頂点座標の三つ組」として取り出す (並べ替えに強い比較用) */
typedef struct TriKey {
	float v[9];
} TriKey;

static int trikey_cmp(const void* a, const void* b)
{
	return memcmp(a, b, sizeof(TriKey));
}

static TriKey* collect_tris(const YbrMesh* m)
{
	TriKey* out =
		(TriKey*)YBR_MALLOC((size_t)m->triangleCount * sizeof(TriKey));
	if (!out) return NULL;
	for (int t = 0; t < m->triangleCount; t++) {
		for (int k = 0; k < 3; k++) {
			unsigned int vi = m->indices[t * 3 + k];
			for (int c = 0; c < 3; c++)
				out[t].v[k * 3 + c] = m->positions[vi * 3 + c];
		}
	}
	qsort(out, (size_t)m->triangleCount, sizeof(TriKey), trikey_cmp);
	return out;
}

/* N x N のグリッドを作る。dup が 1 なら頂点を三角形ごとにばらす。 */
static YbrMesh* make_opt_grid(int N, int dup, int shuffle)
{
	int quads = (N - 1) * (N - 1);
	int nt = quads * 2;
	YbrMesh* m = (YbrMesh*)YBR_CALLOC(1, sizeof(YbrMesh));
	m->id = (char*)"Grid";
	m->triangleCount = nt;
	m->indices =
		(unsigned int*)YBR_MALLOC((size_t)nt * 3 * sizeof(unsigned int));

	if (dup) {
		m->vertexCount = nt * 3;
		m->positions =
			(float*)YBR_MALLOC((size_t)m->vertexCount * 3 * sizeof(float));
	}
	else {
		m->vertexCount = N * N;
		m->positions =
			(float*)YBR_MALLOC((size_t)m->vertexCount * 3 * sizeof(float));
		for (int y = 0; y < N; y++)
			for (int x = 0; x < N; x++) {
				int i = y * N + x;
				m->positions[i * 3 + 0] = (float)x;
				m->positions[i * 3 + 1] = 0.0f;
				m->positions[i * 3 + 2] = (float)y;
			}
	}

	int t = 0, v = 0;
	for (int y = 0; y < N - 1; y++) {
		for (int x = 0; x < N - 1; x++) {
			int q[4] = {y * N + x, y * N + x + 1, (y + 1) * N + x,
						(y + 1) * N + x + 1};
			int tri[6] = {q[0], q[1], q[2], q[1], q[3], q[2]};
			for (int k = 0; k < 6; k++) {
				if (dup) {
					int src = tri[k];
					m->positions[v * 3 + 0] = (float)(src % N);
					m->positions[v * 3 + 1] = 0.0f;
					m->positions[v * 3 + 2] = (float)(src / N);
					m->indices[t * 3 + (k % 3)] = (unsigned int)v;
					v++;
				}
				else {
					m->indices[t * 3 + (k % 3)] = (unsigned int)tri[k];
				}
				if (k % 3 == 2) t++;
			}
		}
	}

	if (shuffle) {
		unsigned int seed = 12345u;
		for (int i = nt - 1; 0 < i; i--) {
			seed = seed * 1103515245u + 12345u;
			int j = (int)((seed >> 16) % (unsigned int)(i + 1));
			for (int k = 0; k < 3; k++) {
				unsigned int tmp = m->indices[i * 3 + k];
				m->indices[i * 3 + k] = m->indices[j * 3 + k];
				m->indices[j * 3 + k] = tmp;
			}
		}
	}
	return m;
}

static void free_opt_grid(YbrMesh* m)
{
	YBR_FREE(m->positions);
	YBR_FREE(m->indices);
	YBR_FREE(m);
}

void test_mesh_opt(void)
{
	group("mesh optimize");

	/* --- 形が変わらないこと --- */
	{
		YbrMesh* m = make_opt_grid(24, 1, 1); /* 頂点を全部ばらした状態 */
		int triBefore = m->triangleCount;
		int vtxBefore = m->vertexCount;
		TriKey* before = collect_tris(m);

		YbrMeshOptOptions o = YbrMeshOptDefaults();
		check(YbrOptimizeMesh(m, &o, NULL) == 1, "最適化が成功する");
		check(m->triangleCount == triBefore, "三角形の数は変わらない");
		check(m->vertexCount < vtxBefore, "重複頂点がまとまる");
		check(m->vertexCount == 24 * 24, "格子の頂点数まで減る");

		TriKey* after = collect_tris(m);
		check(
			before && after &&
				memcmp(before, after, (size_t)triBefore * sizeof(TriKey)) == 0,
			"三角形の集合 (座標) が完全に一致する");
		YBR_FREE(before);
		YBR_FREE(after);
		free_opt_grid(m);
	}

	/* --- 頂点キャッシュのヒット率が上がること --- */
	{
		YbrMesh* m = make_opt_grid(48, 0, 1); /* 三角形の並びをシャッフル */
		YbrMeshOptOptions o = YbrMeshOptDefaults();
		float acmrBefore = YbrMeshComputeACMR(m->indices, m->triangleCount,
											  m->vertexCount, o.cacheSize);
		YbrOptimizeMesh(m, &o, NULL);
		float acmrAfter = YbrMeshComputeACMR(m->indices, m->triangleCount,
											 m->vertexCount, o.cacheSize);
		check(2.5f < acmrBefore, "シャッフル前提で ACMR は悪い");
		check(acmrAfter < 1.0f, "最適化後は ACMR が 1.0 未満になる");
		check(acmrAfter < acmrBefore * 0.5f, "ACMR が半分以下になる");

		/* 頂点は使う順に並んでいるはず (前から順に登場する) */
		int maxSeen = -1, monotone = 1;
		for (int i = 0; i < m->triangleCount * 3; i++) {
			int v = (int)m->indices[i];
			if (maxSeen + 1 < v) monotone = 0;
			if (maxSeen < v) maxSeen = v;
		}
		check(monotone, "頂点が使う順に並んでいる");
		free_opt_grid(m);
	}

	/* --- 未使用頂点が落ちること --- */
	{
		YbrMesh* m = make_opt_grid(8, 0, 0);
		int nv = m->vertexCount;
		/* 最後の頂点をどこからも参照しないようにする */
		for (int i = 0; i < m->triangleCount * 3; i++)
			if ((int)m->indices[i] == nv - 1) m->indices[i] = 0;

		YbrMeshOptOptions o = YbrMeshOptDefaults();
		o.mergeVertices = 0;
		o.optimizeCache = 0;
		o.optimizeFetch = 0;
		YbrOptimizeMesh(m, &o, NULL);
		check(m->vertexCount < nv, "未使用頂点が落ちる");
		free_opt_grid(m);
	}

	/* --- 量子化 --- */
	{
		YbrMesh* m = make_opt_grid(4, 0, 0);
		m->positions[0] = 0.123456f;
		YbrMeshOptOptions o = YbrMeshOptDefaults();
		o.mergeVertices = 0;
		o.optimizeCache = 0;
		o.optimizeFetch = 0;
		o.dropUnused = 0;
		o.positionGrid = 16.0f; /* 1/16 単位 */
		YbrOptimizeMesh(m, &o, NULL);
		check_near(m->positions[0], 0.125f, 1e-6f, "位置が 1/16 に丸まる");
		free_opt_grid(m);
	}

	/* --- 統計 --- */
	{
		YbrMesh* m = make_opt_grid(16, 1, 1);
		YbrMeshOptStats st;
		memset(&st, 0, sizeof(st));
		YbrMeshOptOptions o = YbrMeshOptDefaults();
		YbrOptimizeMesh(m, &o, &st);
		check(st.meshCount == 1, "メッシュ数");
		check(st.verticesAfter < st.verticesBefore, "頂点が減った記録");
		check(st.trianglesAfter == st.trianglesBefore, "三角形は減らない");
		check(st.acmrAfter < st.acmrBefore, "ACMR が良くなった記録");
		free_opt_grid(m);
	}

	/* --- 空 / NULL でも落ちない --- */
	{
		YbrMesh empty;
		memset(&empty, 0, sizeof(empty));
		check(YbrOptimizeMesh(&empty, NULL, NULL) == 1, "空メッシュ");
		check(YbrOptimizeMesh(NULL, NULL, NULL) == 1, "NULL");
		check(YbrOptimizeScene(NULL, NULL, NULL) == 1, "NULL シーン");
		check(YbrMeshComputeACMR(NULL, 0, 0, 32) == 0.0f, "ACMR の NULL");
	}
}
