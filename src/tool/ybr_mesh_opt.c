/*
	Yui Blender to Raylib - メッシュの最適化
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_mesh_opt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define YBR_VC_MAX_CACHE 64 /* Forsyth の点表の上限 */

YbrMeshOptOptions YbrMeshOptDefaults(void)
{
	YbrMeshOptOptions o;
	o.mergeVertices = 1;
	o.dropUnused = 1;
	o.optimizeCache = 1;
	o.optimizeFetch = 1;
	o.cacheSize = 32;
	o.positionGrid = 0.0f;
	o.normalBits = 0;
	o.uvBits = 0;
	o.maxTextureSize = 0;
	return o;
}

static YbrMeshOptOptions sanitize(const YbrMeshOptOptions* in)
{
	YbrMeshOptOptions o = in ? *in : YbrMeshOptDefaults();
	if (o.cacheSize < 4) o.cacheSize = 4;
	if (YBR_VC_MAX_CACHE < o.cacheSize) o.cacheSize = YBR_VC_MAX_CACHE;
	if (o.normalBits < 0) o.normalBits = 0;
	if (16 < o.normalBits) o.normalBits = 16;
	if (o.uvBits < 0) o.uvBits = 0;
	if (16 < o.uvBits) o.uvBits = 16;
	if (o.positionGrid < 0.0f) o.positionGrid = 0.0f;
	if (o.maxTextureSize < 0) o.maxTextureSize = 0;
	return o;
}

// ACMR (三角形 1 枚あたりのキャッシュミス数)

float YbrMeshComputeACMR(const unsigned int* indices, int triangleCount,
						 int vertexCount, int cacheSize)
{
	if (!indices || triangleCount <= 0) return 0.0f;
	if (cacheSize < 1) cacheSize = 1;
	if (YBR_VC_MAX_CACHE < cacheSize) cacheSize = YBR_VC_MAX_CACHE;

	int cache[YBR_VC_MAX_CACHE];
	for (int i = 0; i < cacheSize; i++) cache[i] = -1;

	int misses = 0;
	int head = 0;
	for (int i = 0; i < triangleCount * 3; i++) {
		int v = (int)indices[i];
		if (v < 0 || vertexCount <= v) continue;
		int hit = 0;
		for (int k = 0; k < cacheSize; k++)
			if (cache[k] == v) {
				hit = 1;
				break;
			}
		if (!hit) {
			misses++;
			cache[head] = v;
			head = (head + 1) % cacheSize;
		}
	}
	return (float)misses / (float)triangleCount;
}

// 量子化

static float quantize(float v, float grid)
{
	if (!(0.0f < grid)) return v;
	return floorf(v * grid + 0.5f) / grid;
}

static void quantize_mesh(YbrMesh* m, const YbrMeshOptOptions* o)
{
	int nv = m->vertexCount;

	if (0.0f < o->positionGrid && m->positions)
		for (int i = 0; i < nv * 3; i++)
			m->positions[i] = quantize(m->positions[i], o->positionGrid);

	if (0 < o->normalBits && m->tangents) {
		float grid = (float)(1 << o->normalBits);
		for (int i = 0; i < nv; i++) {
			float* t = m->tangents + i * 4;
			for (int k = 0; k < 3; k++) t[k] = quantize(t[k], grid);
			float len = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
			if (1e-8f < len) {
				t[0] /= len;
				t[1] /= len;
				t[2] /= len;
			}
		}
	}

	if (0 < o->normalBits && m->normals) {
		float grid = (float)(1 << o->normalBits);
		for (int i = 0; i < nv; i++) {
			float* n = m->normals + i * 3;
			for (int k = 0; k < 3; k++) n[k] = quantize(n[k], grid);
			// 丸めで長さが崩れるので正規化し直す
			float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			if (1e-8f < len) {
				n[0] /= len;
				n[1] /= len;
				n[2] /= len;
			}
		}
	}

	if (0 < o->uvBits && m->texcoords) {
		float grid = (float)(1 << o->uvBits);
		for (int i = 0; i < nv * 2; i++)
			m->texcoords[i] = quantize(m->texcoords[i], grid);
	}
}

// 頂点の入れ替え

// remap[old] = new (使わない頂点は -1)。newCount 個に詰め直す。
static int apply_vertex_remap(YbrMesh* m, const int* remap, int newCount)
{
	int nv = m->vertexCount;
	if (newCount <= 0) return 0;

#define MOVE(field, comps, type)                                              \
	do {                                                                      \
		if (m->field) {                                                       \
			type* dst =                                                       \
				(type*)YBR_MALLOC((size_t)newCount * (comps) * sizeof(type)); \
			if (!dst) return 0;                                               \
			for (int i = 0; i < nv; i++) {                                    \
				if (remap[i] < 0) continue;                                   \
				memcpy(dst + (size_t)remap[i] * (comps),                      \
					   m->field + (size_t)i * (comps),                        \
					   (comps) * sizeof(type));                               \
			}                                                                 \
			YBR_FREE(m->field);                                               \
			m->field = dst;                                                   \
		}                                                                     \
	} while (0)

	MOVE(positions, 3, float);
	MOVE(normals, 3, float);
	MOVE(tangents, 4, float);
	MOVE(texcoords, 2, float);
	MOVE(colors, 4, unsigned char);
	MOVE(vertexMap, 1, unsigned int);
	if (m->skin) {
		unsigned short* j = m->skin->joints;
		float* w = m->skin->weights;
		m->skin->joints = NULL;
		m->skin->weights = NULL;
		{
			unsigned short* dj = (unsigned short*)YBR_MALLOC(
				(size_t)newCount * 4 * sizeof(unsigned short));
			float* dw =
				(float*)YBR_MALLOC((size_t)newCount * 4 * sizeof(float));
			if (!dj || !dw) {
				YBR_FREE(dj);
				YBR_FREE(dw);
				return 0;
			}
			for (int i = 0; i < nv; i++) {
				if (remap[i] < 0) continue;
				memcpy(dj + (size_t)remap[i] * 4, j + (size_t)i * 4,
					   4 * sizeof(unsigned short));
				memcpy(dw + (size_t)remap[i] * 4, w + (size_t)i * 4,
					   4 * sizeof(float));
			}
			YBR_FREE(j);
			YBR_FREE(w);
			m->skin->joints = dj;
			m->skin->weights = dw;
		}
	}
#undef MOVE

	// 頂点グループも同じ表で付け替える。消えた頂点は落とし、
	// まとめられて同じ頂点になったものは 1 本にする。
	for (int g = 0; g < m->vertexGroupCount; g++) {
		YbrVertexGroup* vg = &m->vertexGroups[g];
		if (!vg->indices || !vg->weights || vg->count <= 0) continue;

		unsigned char* seen = (unsigned char*)YBR_CALLOC((size_t)newCount, 1);
		if (!seen) return 0;

		int out = 0;
		for (int i = 0; i < vg->count; i++) {
			int v = (int)vg->indices[i];
			if (v < 0 || nv <= v || remap[v] < 0) continue;
			int nvi = remap[v];
			if (seen[nvi]) continue;
			seen[nvi] = 1;
			vg->indices[out] = (unsigned int)nvi;
			vg->weights[out] = vg->weights[i];
			out++;
		}
		YBR_FREE(seen);
		vg->count = out;
	}

	for (int i = 0; i < m->triangleCount * 3; i++) {
		int v = (int)m->indices[i];
		m->indices[i] =
			(unsigned int)((0 <= v && v < nv && 0 <= remap[v]) ? remap[v] : 0);
	}
	m->vertexCount = newCount;
	return 1;
}

// 重複頂点のマージ

typedef struct HashEntry {
	unsigned int hash;
	int index; /* 代表となる頂点 */
	int next;  /* 同じバケットの次 / -1 */
} HashEntry;

static unsigned int hash_bytes(const void* p, size_t n, unsigned int h)
{
	const unsigned char* b = (const unsigned char*)p;
	for (size_t i = 0; i < n; i++) {
		h ^= b[i];
		h *= 16777619u;
	}
	return h;
}

static int vertex_equal(const YbrMesh* m, int a, int b)
{
	if (memcmp(m->positions + (size_t)a * 3, m->positions + (size_t)b * 3,
			   3 * sizeof(float)) != 0)
		return 0;
	if (m->normals &&
		memcmp(m->normals + (size_t)a * 3, m->normals + (size_t)b * 3,
			   3 * sizeof(float)) != 0)
		return 0;
	if (m->tangents &&
		memcmp(m->tangents + (size_t)a * 4, m->tangents + (size_t)b * 4,
			   4 * sizeof(float)) != 0)
		return 0;
	if (m->texcoords &&
		memcmp(m->texcoords + (size_t)a * 2, m->texcoords + (size_t)b * 2,
			   2 * sizeof(float)) != 0)
		return 0;
	if (m->colors &&
		memcmp(m->colors + (size_t)a * 4, m->colors + (size_t)b * 4, 4) != 0)
		return 0;
	if (m->skin) {
		if (memcmp(m->skin->joints + (size_t)a * 4,
				   m->skin->joints + (size_t)b * 4,
				   4 * sizeof(unsigned short)) != 0)
			return 0;
		if (memcmp(m->skin->weights + (size_t)a * 4,
				   m->skin->weights + (size_t)b * 4, 4 * sizeof(float)) != 0)
			return 0;
	}
	return 1;
}

static int merge_vertices(YbrMesh* m)
{
	int nv = m->vertexCount;
	if (nv <= 1 || !m->positions) return 1;

	int buckets = 1;
	while (buckets < nv * 2) buckets *= 2;

	int* head = (int*)YBR_MALLOC((size_t)buckets * sizeof(int));
	HashEntry* ent = (HashEntry*)YBR_MALLOC((size_t)nv * sizeof(HashEntry));
	int* remap = (int*)YBR_MALLOC((size_t)nv * sizeof(int));
	if (!head || !ent || !remap) {
		YBR_FREE(head);
		YBR_FREE(ent);
		YBR_FREE(remap);
		return 0;
	}
	for (int i = 0; i < buckets; i++) head[i] = -1;

	int newCount = 0;
	for (int i = 0; i < nv; i++) {
		unsigned int h = 2166136261u;
		h = hash_bytes(m->positions + (size_t)i * 3, 3 * sizeof(float), h);
		if (m->normals)
			h = hash_bytes(m->normals + (size_t)i * 3, 3 * sizeof(float), h);
		if (m->tangents)
			h = hash_bytes(m->tangents + (size_t)i * 4, 4 * sizeof(float), h);
		if (m->texcoords)
			h = hash_bytes(m->texcoords + (size_t)i * 2, 2 * sizeof(float), h);
		if (m->colors) h = hash_bytes(m->colors + (size_t)i * 4, 4, h);
		if (m->skin) {
			h = hash_bytes(m->skin->joints + (size_t)i * 4,
						   4 * sizeof(unsigned short), h);
			h = hash_bytes(m->skin->weights + (size_t)i * 4, 4 * sizeof(float),
						   h);
		}

		int b = (int)(h & (unsigned int)(buckets - 1));
		int found = -1;
		for (int e = head[b]; 0 <= e; e = ent[e].next) {
			if (ent[e].hash != h) continue;
			if (vertex_equal(m, i, ent[e].index)) {
				found = ent[e].index;
				break;
			}
		}
		if (0 <= found) {
			remap[i] = remap[found];
		}
		else {
			remap[i] = newCount++;
			ent[i].hash = h;
			ent[i].index = i;
			ent[i].next = head[b];
			head[b] = i;
		}
	}

	int ok = 1;
	if (newCount < nv) {
		// remap は「新しい番号」だが、詰めるときに同じ番号へ複数の
		// 頂点が来るので、代表 1 つだけをコピーすればよい
		int* first = (int*)YBR_MALLOC((size_t)nv * sizeof(int));
		if (!first) {
			ok = 0;
		}
		else {
			for (int i = 0; i < nv; i++) first[i] = -1;
			for (int i = 0; i < nv; i++) {
				if (first[remap[i]] < 0) first[remap[i]] = i;
			}
			int* copy = (int*)YBR_MALLOC((size_t)nv * sizeof(int));
			if (!copy)
				ok = 0;
			else {
				for (int i = 0; i < nv; i++)
					copy[i] = (first[remap[i]] == i) ? remap[i] : -1;
				// インデックスは remap を使うので先に置き換える
				for (int i = 0; i < m->triangleCount * 3; i++) {
					int v = (int)m->indices[i];
					if (0 <= v && v < nv)
						m->indices[i] = (unsigned int)remap[v];
				}
				int savedTri = m->triangleCount;
				m->triangleCount = 0; /* indices は書き換え済み */
				ok = apply_vertex_remap(m, copy, newCount);
				m->triangleCount = savedTri;
				YBR_FREE(copy);
			}
			YBR_FREE(first);
		}
	}

	YBR_FREE(head);
	YBR_FREE(ent);
	YBR_FREE(remap);
	return ok;
}

// 未使用頂点の削除

static int drop_unused(YbrMesh* m)
{
	int nv = m->vertexCount;
	if (nv <= 0) return 1;

	int* remap = (int*)YBR_MALLOC((size_t)nv * sizeof(int));
	if (!remap) return 0;
	for (int i = 0; i < nv; i++) remap[i] = -1;

	int used = 0;
	for (int i = 0; i < m->triangleCount * 3; i++) {
		int v = (int)m->indices[i];
		if (v < 0 || nv <= v) continue;
		if (remap[v] < 0) remap[v] = used++;
	}
	int ok = 1;
	if (used < nv && 0 < used) ok = apply_vertex_remap(m, remap, used);
	YBR_FREE(remap);
	return ok;
}

// 頂点キャッシュ最適化 (Forsyth)

static float cache_score(int cachePos, int cacheSize, int remaining)
{
	const float kLast = 0.75f; /* 直前の 3 頂点は同じ点にする */
	const float kDecay = 1.5f;
	const float kBoost = 2.0f;
	const float kScale = 0.5f;

	float score = 0.0f;
	if (0 <= cachePos) {
		if (cachePos < 3)
			score = kLast;
		else {
			float t = (float)(cacheSize - cachePos) / (float)(cacheSize - 3);
			score = powf(t, kDecay);
		}
	}
	if (0 < remaining) score += kBoost * powf((float)remaining, -kScale);
	return score;
}

static int optimize_cache(YbrMesh* m, int cacheSize)
{
	int nt = m->triangleCount;
	int nv = m->vertexCount;
	if (nt <= 1 || nv <= 0) return 1;

	int* remaining = (int*)YBR_CALLOC((size_t)nv, sizeof(int));
	int* offset = (int*)YBR_CALLOC((size_t)nv + 1, sizeof(int));
	int* cachePos = (int*)YBR_MALLOC((size_t)nv * sizeof(int));
	float* vscore = (float*)YBR_MALLOC((size_t)nv * sizeof(float));
	float* tscore = (float*)YBR_MALLOC((size_t)nt * sizeof(float));
	char* emitted = (char*)YBR_CALLOC((size_t)nt, 1);
	unsigned int* newIdx =
		(unsigned int*)YBR_MALLOC((size_t)nt * 3 * sizeof(unsigned int));
	unsigned int* newMat =
		m->materialIndices
			? (unsigned int*)YBR_MALLOC((size_t)nt * sizeof(unsigned int))
			: NULL;

	if (!remaining || !offset || !cachePos || !vscore || !tscore || !emitted ||
		!newIdx || (m->materialIndices && !newMat)) {
		YBR_FREE(remaining);
		YBR_FREE(offset);
		YBR_FREE(cachePos);
		YBR_FREE(vscore);
		YBR_FREE(tscore);
		YBR_FREE(emitted);
		YBR_FREE(newIdx);
		YBR_FREE(newMat);
		return 0;
	}

	// 頂点 -> 三角形の表
	for (int i = 0; i < nt * 3; i++) {
		int v = (int)m->indices[i];
		if (0 <= v && v < nv) remaining[v]++;
	}
	int total = 0;
	for (int v = 0; v < nv; v++) {
		offset[v] = total;
		total += remaining[v];
	}
	offset[nv] = total;

	int* tris = (int*)YBR_MALLOC((size_t)(total ? total : 1) * sizeof(int));
	int* fill = (int*)YBR_CALLOC((size_t)nv, sizeof(int));
	if (!tris || !fill) {
		YBR_FREE(remaining);
		YBR_FREE(offset);
		YBR_FREE(cachePos);
		YBR_FREE(vscore);
		YBR_FREE(tscore);
		YBR_FREE(emitted);
		YBR_FREE(newIdx);
		YBR_FREE(newMat);
		YBR_FREE(tris);
		YBR_FREE(fill);
		return 0;
	}
	for (int t = 0; t < nt; t++) {
		for (int k = 0; k < 3; k++) {
			int v = (int)m->indices[t * 3 + k];
			if (0 <= v && v < nv) tris[offset[v] + fill[v]++] = t;
		}
	}

	for (int v = 0; v < nv; v++) {
		cachePos[v] = -1;
		vscore[v] = cache_score(-1, cacheSize, remaining[v]);
	}
	for (int t = 0; t < nt; t++) {
		tscore[t] = 0.0f;
		for (int k = 0; k < 3; k++) {
			int v = (int)m->indices[t * 3 + k];
			if (0 <= v && v < nv) tscore[t] += vscore[v];
		}
	}

	int cache[YBR_VC_MAX_CACHE + 3];
	int cacheCount = 0;
	int out = 0;

	while (out < nt) {
		// いちばん点の高い三角形を選ぶ
		int best = -1;
		float bestScore = -1.0f;
		// キャッシュに載っている頂点まわりだけ見れば足りる
		for (int c = 0; c < cacheCount; c++) {
			int v = cache[c];
			for (int e = offset[v]; e < offset[v + 1]; e++) {
				int t = tris[e];
				if (emitted[t]) continue;
				if (bestScore < tscore[t]) {
					bestScore = tscore[t];
					best = t;
				}
			}
		}
		if (best < 0) {	 // キャッシュが空 / 行き止まり : 全体から探す
			for (int t = 0; t < nt; t++) {
				if (emitted[t]) continue;
				if (bestScore < tscore[t]) {
					bestScore = tscore[t];
					best = t;
				}
			}
		}
		if (best < 0) break;

		// 出力
		emitted[best] = 1;
		for (int k = 0; k < 3; k++)
			newIdx[out * 3 + k] = m->indices[best * 3 + k];
		if (newMat) newMat[out] = m->materialIndices[best];
		out++;

		// キャッシュを更新
		for (int k = 0; k < 3; k++) {
			int v = (int)m->indices[best * 3 + k];
			if (v < 0 || nv <= v) continue;
			if (0 < remaining[v]) remaining[v]--;

			int at = -1;
			for (int c = 0; c < cacheCount; c++)
				if (cache[c] == v) {
					at = c;
					break;
				}
			if (0 <= at) {
				for (int c = at; 0 < c; c--) cache[c] = cache[c - 1];
			}
			else {
				if (cacheCount < cacheSize + 3) cacheCount++;
				for (int c = cacheCount - 1; 0 < c; c--)
					cache[c] = cache[c - 1];
			}
			cache[0] = v;
		}
		if (cacheSize < cacheCount) {
			for (int c = cacheSize; c < cacheCount; c++)
				cachePos[cache[c]] = -1;
			cacheCount = cacheSize;
		}

		// 点を計算し直す (キャッシュ内の頂点とその三角形だけ)
		for (int c = 0; c < cacheCount; c++) cachePos[cache[c]] = c;
		for (int c = 0; c < cacheCount; c++) {
			int v = cache[c];
			float ns = cache_score(cachePos[v], cacheSize, remaining[v]);
			float diff = ns - vscore[v];
			vscore[v] = ns;
			if (diff == 0.0f) continue;
			for (int e = offset[v]; e < offset[v + 1]; e++) {
				int t = tris[e];
				if (!emitted[t]) tscore[t] += diff;
			}
		}
	}

	int ok = (out == nt);
	if (ok) {
		memcpy(m->indices, newIdx, (size_t)nt * 3 * sizeof(unsigned int));
		if (newMat)
			memcpy(m->materialIndices, newMat,
				   (size_t)nt * sizeof(unsigned int));
	}

	YBR_FREE(remaining);
	YBR_FREE(offset);
	YBR_FREE(cachePos);
	YBR_FREE(vscore);
	YBR_FREE(tscore);
	YBR_FREE(emitted);
	YBR_FREE(newIdx);
	YBR_FREE(newMat);
	YBR_FREE(tris);
	YBR_FREE(fill);
	return ok;
}

// 頂点フェッチ最適化 (使う順に頂点を並べ直す)

static int optimize_fetch(YbrMesh* m)
{
	int nv = m->vertexCount;
	if (nv <= 0 || m->triangleCount <= 0) return 1;

	int* remap = (int*)YBR_MALLOC((size_t)nv * sizeof(int));
	if (!remap) return 0;
	for (int i = 0; i < nv; i++) remap[i] = -1;

	int next = 0;
	for (int i = 0; i < m->triangleCount * 3; i++) {
		int v = (int)m->indices[i];
		if (v < 0 || nv <= v) continue;
		if (remap[v] < 0) remap[v] = next++;
	}
	// 使われていない頂点は末尾へ (dropUnused が OFF のときのため)
	for (int i = 0; i < nv; i++)
		if (remap[i] < 0) remap[i] = next++;

	int ok = apply_vertex_remap(m, remap, next);
	YBR_FREE(remap);
	return ok;
}

int YbrOptimizeMesh(YbrMesh* m, const YbrMeshOptOptions* opts,
					YbrMeshOptStats* st)
{
	if (!m || m->vertexCount <= 0 || !m->positions) return 1;
	YbrMeshOptOptions o = sanitize(opts);

	if (st) {
		st->meshCount++;
		st->verticesBefore += m->vertexCount;
		st->trianglesBefore += m->triangleCount;
		st->acmrBefore += YbrMeshComputeACMR(m->indices, m->triangleCount,
											 m->vertexCount, o.cacheSize);
	}

	quantize_mesh(m, &o);

	int ok = 1;
	if (o.mergeVertices && !merge_vertices(m)) ok = 0;
	if (ok && o.dropUnused && !drop_unused(m)) ok = 0;
	if (ok && o.optimizeCache && !optimize_cache(m, o.cacheSize)) ok = 0;
	if (ok && o.optimizeFetch && !optimize_fetch(m)) ok = 0;

	if (st) {
		st->verticesAfter += m->vertexCount;
		st->trianglesAfter += m->triangleCount;
		st->acmrAfter += YbrMeshComputeACMR(m->indices, m->triangleCount,
											m->vertexCount, o.cacheSize);
	}
	return ok;
}

// テクスチャの縮小 (RAW のみ)

static int shrink_texture(YbrTextureData* t, int maxSize)
{
	if (!t || !t->embedded || t->compression != YBR_TEX_RAW) return 0;
	if (!t->pixels || t->width <= 0 || t->height <= 0) return 0;

	int longSide = (t->height < t->width) ? t->width : t->height;
	if (longSide <= maxSize) return 0;

	// 2 のべき乗ぶんずつ、ボックスフィルタで半分にしていく
	int changed = 0;
	while (maxSize < ((t->height < t->width) ? t->width : t->height) &&
		   1 < t->width && 1 < t->height) {
		int nw = t->width / 2, nh = t->height / 2;
		if (nw < 1) nw = 1;
		if (nh < 1) nh = 1;

		unsigned char* dst = (unsigned char*)YBR_MALLOC((size_t)nw * nh * 4);
		if (!dst) return changed;
		for (int y = 0; y < nh; y++) {
			for (int x = 0; x < nw; x++) {
				for (int c = 0; c < 4; c++) {
					int sum = 0;
					for (int dy = 0; dy < 2; dy++) {
						for (int dx = 0; dx < 2; dx++) {
							int sx = x * 2 + dx, sy = y * 2 + dy;
							if (t->width <= sx) sx = t->width - 1;
							if (t->height <= sy) sy = t->height - 1;
							sum +=
								t->pixels[((size_t)sy * t->width + sx) * 4 + c];
						}
					}
					dst[((size_t)y * nw + x) * 4 + c] =
						(unsigned char)((sum + 2) / 4);
				}
			}
		}
		YBR_FREE(t->pixels);
		t->pixels = dst;
		t->width = nw;
		t->height = nh;
		changed = 1;
	}
	return changed;
}

int YbrOptimizeScene(YbrScene* scene, const YbrMeshOptOptions* opts,
					 YbrMeshOptStats* st)
{
	if (!scene) return 1;
	YbrMeshOptOptions o = sanitize(opts);

	int ok = 1;
	for (int i = 0; i < scene->meshCount; i++)
		if (!YbrOptimizeMesh(&scene->meshes[i], &o, st)) ok = 0;

	if (0 < o.maxTextureSize) {
		for (int i = 0; i < scene->textureCount; i++) {
			if (st) st->textureCount++;
			if (shrink_texture(&scene->textures[i], o.maxTextureSize) && st)
				st->texturesResized++;
		}
	}
	else if (st) {
		st->textureCount += scene->textureCount;
	}
	return ok;
}
