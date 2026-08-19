/*
	Yui Blender to Raylib - ファイルの読み書きのテスト
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_test.h"

/* 埋め込みテクスチャが読み込みで捨てられないこと。
 * RAW の画素は dataSize を使わない (compression 付きの data 専用) ので、
 * 検証で dataSize を見ると全部消えてしまう。 */
void test_embedded_texture_survives(void)
{
	group("embedded texture");

	static unsigned char px[2 * 2 * 4] = {255, 0, 0,   255, 0,	 255, 0,   255,
										  0,   0, 255, 255, 255, 255, 255, 255};
	YbrTextureData tex;
	memset(&tex, 0, sizeof(tex));
	tex.id = (char*)"Tex";
	tex.width = 2;
	tex.height = 2;
	tex.embedded = 1;
	tex.compression = YBR_TEX_RAW;
	tex.pixels = px;

	YbrScene sc;
	memset(&sc, 0, sizeof(sc));
	sc.textureCount = 1;
	sc.textures = &tex;

	size_t size = 0;
	unsigned char* buf = YbrSaveToMemory(&sc, &size);
	YbrScene* lo = buf ? YbrLoadFromMemory(buf, size) : NULL;
	check(lo != NULL, "読み戻せる");
	if (!lo) {
		free(buf);
		return;
	}

	const YbrTextureData* t = YbrFindTexture(lo, "Tex");
	check(t != NULL, "テクスチャがある");
	if (t) {
		check(t->embedded == 1, "埋め込みのまま残る");
		check(t->pixels != NULL, "画素が捨てられていない");
		check(t->width == 2 && t->height == 2, "大きさ");
		if (t->pixels)
			check(t->pixels[0] == 255 && t->pixels[1] == 0 && t->pixels[2] == 0,
				  "画素の中身が一致する");
	}
	YbrUnload(lo);
	free(buf);
}

/* ================================================================== */
/* 壊れたファイル / バイト順                                          */
/* ================================================================== */
/* MESH ブロック 1 つだけの最小の .ybr を組み立てる */
static unsigned char* make_tiny_ybr(size_t* len, int vertexCount,
									int triangleCount, unsigned int lastIndex,
									int positionFloats)
{
	static const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	unsigned int idx[3];
	idx[0] = 0;
	idx[1] = 1;
	idx[2] = lastIndex;

	CborWriter w;
	CborWriterInit(&w);
	CborWriteArrayHeader(&w, 4);
	CborWriteText(&w, YBR_MAGIC);
	CborWriteInt(&w, YBR_SUPPORTED_VERSION);
	CborWriteArrayHeader(&w, 0); /* シーンツリー */
	CborWriteArrayHeader(&w, 1); /* データブロック */
	CborWriteMapHeader(&w, 6);
	CborWriteKeyText(&w, "kind", "MESH");
	CborWriteKeyText(&w, "id", "M");
	CborWriteKeyInt(&w, "vertex_count", vertexCount);
	CborWriteKeyInt(&w, "triangle_count", triangleCount);
	CborWriteKeyF32Array(&w, "positions", pos, (size_t)positionFloats);
	CborWriteKeyU32Array(&w, "indices", idx, 3);
	return CborWriterTake(&w, len);
}

void test_broken_files(void)
{
	group("broken files");

	size_t n = 0;
	unsigned char* d;

	/* --- まともなファイルは通る --- */
	d = make_tiny_ybr(&n, 3, 1, 2, 9);
	YbrScene* sc = YbrLoadFromMemory(d, n);
	check(sc != NULL, "正しいファイルは読める");
	if (sc) {
		check(sc->meshes[0].vertexCount == 3, "頂点数が入る");
		YbrUnload(sc);
	}
	YBR_FREE(d);

	/* --- index が頂点数の外 --- */
	d = make_tiny_ybr(&n, 3, 1, 99, 9);
	check(YbrLoadFromMemory(d, n) == NULL, "範囲外の index を弾く");
	YBR_FREE(d);

	/* --- 頂点数と配列の長さが食い違う --- */
	d = make_tiny_ybr(&n, 3, 1, 2, 3);
	check(YbrLoadFromMemory(d, n) == NULL, "配列が短いファイルを弾く");
	YBR_FREE(d);

	/* --- 誇張された vertex_count は実データに合わせて切り詰める --- */
	d = make_tiny_ybr(&n, 100000000, 1, 2, 9);
	sc = YbrLoadFromMemory(d, n);
	check(sc && sc->meshes[0].vertexCount == 3,
		  "大きすぎる vertex_count を切り詰める");
	if (sc) YbrUnload(sc);
	YBR_FREE(d);

	/* --- 途中で切れたファイル --- */
	d = make_tiny_ybr(&n, 3, 1, 2, 9);
	check(YbrLoadFromMemory(d, n / 2) == NULL, "途中で切れたファイルを弾く");
	YBR_FREE(d);

	/* --- でたらめなバイト列 --- */
	{
		unsigned char junk[64];
		memset(junk, 0xFF, sizeof(junk));
		check(YbrLoadFromMemory(junk, sizeof(junk)) == NULL,
			  "でたらめなバイト列を弾く");
		check(YbrLoadFromMemory(NULL, 0) == NULL, "NULL を弾く");
	}

	/* --- magic / バージョン違い --- */
	{
		CborWriter w;
		CborWriterInit(&w);
		CborWriteArrayHeader(&w, 4);
		CborWriteText(&w, "XXX");
		CborWriteInt(&w, YBR_SUPPORTED_VERSION);
		CborWriteArrayHeader(&w, 0);
		CborWriteArrayHeader(&w, 0);
		size_t m = 0;
		unsigned char* b = CborWriterTake(&w, &m);
		check(YbrLoadFromMemory(b, m) == NULL, "magic 違いを弾く");
		YBR_FREE(b);
	}

	/* --- 入れ子が深すぎる CBOR --- */
	{
		unsigned char deep[CBOR_MAX_DEPTH * 4];
		memset(deep, 0x81, sizeof(deep)); /* array(1) の入れ子 */
		CborValue v;
		check(CborParse(deep, sizeof(deep), &v) == 0, "深すぎる入れ子を弾く");
		CborFree(&v);
	}

	/* --- 巨大な要素数を宣言しただけのファイル (確保する前に弾く) --- */
	{
		unsigned char bomb[5] = {0x9A, 0xFF, 0xFF, 0xFF,
								 0xFF}; /* array(4294967295) */
		CborValue v;
		check(CborParse(bomb, sizeof(bomb), &v) == 0, "巨大な要素数を弾く");
		CborFree(&v);
	}
}

void test_byte_order(void)
{
	group("byte order");

	check(CborByteOrderOk() == 1, "この処理系で数値を読み書きできる");

	/* blob はホストのバイト順に関わらずリトルエンディアンで書かれる */
	CborWriter w;
	CborWriterInit(&w);
	CborWriteMapHeader(&w, 1);
	float one = 1.0f;
	CborWriteKeyF32Array(&w, "f", &one, 1);
	size_t len = 0;
	unsigned char* d = CborWriterTake(&w, &len);
	check(d != NULL, "書き出せる");
	if (d) {
		/* 1.0f = 0x3F800000 -> 00 00 80 3F */
		int found = 0;
		for (size_t i = 0; i + 4 <= len; i++)
			if (d[i] == 0x00 && d[i + 1] == 0x00 && d[i + 2] == 0x80 &&
				d[i + 3] == 0x3F) {
				found = 1;
				break;
			}
		check(found, "float はリトルエンディアンで並ぶ");

		CborValue v;
		check(CborParse(d, len, &v) == 1, "読み戻せる");
		int count = 0;
		float* back = CborGetF32(&v, "f", &count);
		check(count == 1 && back && back[0] == 1.0f, "値が一致する");
		YBR_FREE(back);
		CborFree(&v);
		YBR_FREE(d);
	}

	/* CBOR の整数 / float 本体はビッグエンディアン (RFC 8949) */
	CborWriterInit(&w);
	CborWriteUInt(&w, 0x1234u);
	d = CborWriterTake(&w, &len);
	check(d && len == 3 && d[0] == 0x19 && d[1] == 0x12 && d[2] == 0x34,
		  "CBOR の整数は RFC どおりの並び");
	YBR_FREE(d);
}

/* ================================================================== */
/* 読み込んだシーンでのテスト (任意)                                  */
/* ================================================================== */
void test_loaded(const char* path)
{
	group("loaded scene");

	YbrScene* sc = YbrLoad(path);
	if (!sc) {
		printf("  SKIP : %s を読めなかった (%s)\n", path, YbrGetError());
		return;
	}
	printf("  %s : meshes=%d roots=%d animations=%d\n", path, sc->meshCount,
		   sc->rootCount, sc->animationCount);

	YbrSolid* col = YbrSolidBuild(sc, NULL);
	check(col != NULL, "読み込んだシーンから作れる");
	if (col) {
		int n = YbrSolidGetTriangleCount(col);
		printf("  triangles=%d nodes=%d depth=%d\n", n,
			   YbrSolidGetNodeCount(col), YbrSolidGetDepth(col));

		Vector3 bmin, bmax;
		if (0 < n && YbrSolidGetBounds(col, &bmin, &bmax)) {
			Vector3 c = Vector3Scale(Vector3Add(bmin, bmax), 0.5f);
			float span = Vector3Distance(bmin, bmax) + 1.0f;

			/* 中心から外へ向かうレイと総当たりを突き合わせる */
			int mismatch = 0, hits = 0;
			for (int i = 0; i < 500; i++) {
				Vector3 a = V(c.x + frnd(-span, span), c.y + frnd(-span, span),
							  c.z + frnd(-span, span));
				Vector3 b = V(c.x + frnd(-span, span), c.y + frnd(-span, span),
							  c.z + frnd(-span, span));
				YbrRayHit h;
				float bt;
				int bhit = brute_segment(col, a, b, 0, &bt, NULL);
				int ohit = YbrSolidSegment(col, a, b, NULL, &h);
				if (bhit != ohit) {
					mismatch++;
					continue;
				}
				if (bhit) {
					hits++;
					if (1e-4f < fabsf(bt - h.t)) mismatch++;
				}
			}
			check(mismatch == 0, "総当たりと一致 (ランダム 500 本)");
			printf("  hit rate : %d / 500\n", hits);
		}
		YbrSolidUnload(col);
	}

	/* アニメーションのサンプラも一応動かしておく */
	for (int i = 0; i < sc->animationCount && i < 1; i++) {
		const YbrAnimation* a = &sc->animations[i];
		for (int t = 0; t < a->trackCount && t < 1; t++) {
			YbrAnimSampler s;
			check(YbrAnimSamplerInitFromAnimation(&s, a, &a->tracks[t]) == 1,
				  "サンプラを初期化できる");
			Matrix m = YbrAnimSamplerMatrix(&s, 0.5f);
			check(m.m15 == 1.0f || m.m15 != 0.0f, "行列が返る");
			YbrAnimSamplerUnload(&s);
		}
	}

	YbrUnload(sc);
}
