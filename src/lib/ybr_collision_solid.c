/*
	Yui Blender to Raylib - 固定物当たり判定
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_collision_solid.h"

#include "ybr_collision_internal.h"

// ----------------------------------------------------------------------------
// データ構造

typedef struct ColNode {
	Vector3 bmin, bmax;	 // 実際に含む三角形すべての AABB
	int firstTri;		 // triIndices 内の開始位置
	int triCount;
	int child[8];  // -1 = 無し
} ColNode;

// 動かない前提の前計算。三角形と同じ並びで持つ。
typedef struct TriEdges {
	Vector3 e1, e2;	 // v[1]-v[0] / v[2]-v[0] (Moller-Trumbore 用)
} TriEdges;

struct YbrSolid {
	YbrTriangle* tris;	// 8 分木のノード順に並べ替えてある
	TriEdges* edges;	// tris と同じ並びの前計算
	int triCount;

	ColNode* nodes;
	int nodeCount, nodeCap;

	int* triIndices;
	int indexCount, indexCap;

	Vector3 bmin, bmax;
	int hasBounds;
	int depthUsed;
};

// ----------------------------------------------------------------------------
// 三角形の収集

typedef struct Gather {
	const YbrScene* scene;
	YbrTriangle* tris;
	int count, cap;
	const YbrSolidBuildOptions* opts;
	int failed;
} Gather;

static void gather_mesh(Gather* g, const YbrNode* node, const YbrMesh* m,
						Matrix world, unsigned int tag)
{
	if (!m || m->triangleCount <= 0 || !m->positions || !m->indices) return;

	// 鏡映 (行列式が負) だと三角形の巻き方向が裏返るので入れ替える
	float det = world.m0 * (world.m5 * world.m10 - world.m9 * world.m6) -
				world.m4 * (world.m1 * world.m10 - world.m9 * world.m2) +
				world.m8 * (world.m1 * world.m6 - world.m5 * world.m2);
	int flip = (det < 0.0f);

	for (int t = 0; t < m->triangleCount; t++) {
		unsigned int i0 = m->indices[t * 3 + 0];
		unsigned int i1 = m->indices[t * 3 + 1];
		unsigned int i2 = m->indices[t * 3 + 2];
		if (m->vertexCount <= (int)i0 || m->vertexCount <= (int)i1 ||
			m->vertexCount <= (int)i2)
			continue;
		if (flip) {
			unsigned int tmp = i1;
			i1 = i2;
			i2 = tmp;
		}

		YbrTriangle tri;
		const unsigned int idx[3] = {i0, i1, i2};
		for (int k = 0; k < 3; k++) {
			const float* p = m->positions + (size_t)idx[k] * 3;
			tri.v[k] = Vector3Transform(v3(p[0], p[1], p[2]), world);
		}

		Vector3 n =
			v3cross(v3sub(tri.v[1], tri.v[0]), v3sub(tri.v[2], tri.v[0]));
		float len = Vector3Length(n);
		if (len <= 1e-12f) continue;  // 面積 0 は捨てる
		tri.normal = v3mul(n, 1.0f / len);

		// YbrSolid は YbrScene から作るのでモデルのパートは無い
		tri.sceneNode = node;
		tri.node = NULL;
		tri.mesh = m;
		tri.meshTriangle = t;
		tri.materialIndex =
			m->materialIndices ? (int)m->materialIndices[t] : -1;
		tri.tag = tag;

		void* nb =
			YbrGrowBuffer(g->tris, &g->cap, g->count + 1, sizeof(YbrTriangle));
		if (!nb) {
			g->failed = 1;
			return;
		}
		g->tris = (YbrTriangle*)nb;
		g->tris[g->count++] = tri;
	}
}

static void gather_node(Gather* g, const YbrNode* n, Matrix parentWorld)
{
	if (g->failed || !n) return;

	Matrix world = MatrixMultiply(n->matrix, parentWorld);

	if (n->type == YBR_NODE_MESH && n->dataId) {
		const YbrMesh* m = YbrFindMesh(g->scene, n->dataId);
		if (m) {
			unsigned int tag = 0;
			int take = 1;
			if (g->opts && g->opts->filter)
				take = g->opts->filter(n, m, &tag, g->opts->filterUserData);
			if (take) gather_mesh(g, n, m, world, tag);
		}
	}

	for (int i = 0; i < n->childCount; i++)
		gather_node(g, &n->children[i], world);
}

// ----------------------------------------------------------------------------
// 8分木の構築

typedef struct Builder {
	YbrSolid* col;
	int maxDepth;
	int maxTri;
	float looseness;
	int failed;
} Builder;

static int new_node(Builder* bd)
{
	YbrSolid* c = bd->col;
	void* nb =
		YbrGrowBuffer(c->nodes, &c->nodeCap, c->nodeCount + 1, sizeof(ColNode));
	if (!nb) {
		bd->failed = 1;
		return -1;
	}
	c->nodes = (ColNode*)nb;
	ColNode* n = &c->nodes[c->nodeCount];
	memset(n, 0, sizeof(*n));
	for (int i = 0; i < 8; i++) n->child[i] = -1;
	n->firstTri = 0;
	n->triCount = 0;
	return c->nodeCount++;
}

static int push_indices(Builder* bd, const int* idx, int n, int* outFirst)
{
	YbrSolid* c = bd->col;
	void* nb = YbrGrowBuffer(c->triIndices, &c->indexCap, c->indexCount + n,
							 sizeof(int));
	if (!nb) {
		bd->failed = 1;
		return 0;
	}
	c->triIndices = (int*)nb;
	*outFirst = c->indexCount;
	memcpy(c->triIndices + c->indexCount, idx, (size_t)n * sizeof(int));
	c->indexCount += n;
	return 1;
}

static Aabb child_box(Aabb box, int i)
{
	Vector3 mid = v3mul(v3add(box.min, box.max), 0.5f);
	Aabb r;
	r.min.x = (i & 1) ? mid.x : box.min.x;
	r.max.x = (i & 1) ? box.max.x : mid.x;
	r.min.y = (i & 2) ? mid.y : box.min.y;
	r.max.y = (i & 2) ? box.max.y : mid.y;
	r.min.z = (i & 4) ? mid.z : box.min.z;
	r.max.z = (i & 4) ? box.max.z : mid.z;
	return r;
}

static Aabb loosen(Aabb b, float k)
{
	Vector3 c = v3mul(v3add(b.min, b.max), 0.5f);
	Vector3 h = v3mul(v3sub(b.max, b.min), 0.5f * k);
	Aabb r;
	r.min = v3sub(c, h);
	r.max = v3add(c, h);
	return r;
}

// 戻り値はノード番号 (-1 = 空 / 失敗)。idx は読み取りのみ
static int build_node(Builder* bd, const int* idx, int n, Aabb box, int depth)
{
	if (bd->failed || n <= 0) return -1;
	if (bd->col->depthUsed < depth) bd->col->depthUsed = depth;

	int leaf = (n <= bd->maxTri) || (bd->maxDepth <= depth);

	int selfIdxFirst = 0, selfCount = 0;
	int* tmp = NULL;
	int counts[9] = {0}, offs[9] = {0};
	unsigned char* owner = NULL;

	if (!leaf) {
		tmp = (int*)YBR_MALLOC((size_t)n * sizeof(int));
		owner = (unsigned char*)YBR_MALLOC((size_t)n);
		if (!tmp || !owner) {
			YBR_FREE(tmp);
			YBR_FREE(owner);
			bd->failed = 1;
			return -1;
		}
		Aabb lb[8];
		for (int i = 0; i < 8; i++)
			lb[i] = loosen(child_box(box, i), bd->looseness);

		for (int i = 0; i < 9; i++) counts[i] = 0;
		for (int i = 0; i < n; i++) {
			Aabb tb = tri_aabb(&bd->col->tris[idx[i]]);
			// 8 = このノードに残す
			int o = 8;
			for (int k = 0; k < 8; k++) {
				if (aabb_contains(lb[k], tb)) {
					o = k;
					break;
				}
			}
			owner[i] = (unsigned char)o;
			counts[o]++;
		}
		// 1 枚も下へ降ろせない
		if (counts[8] == n) {
			YBR_FREE(tmp);
			YBR_FREE(owner);
			tmp = NULL;
			owner = NULL;
			leaf = 1;
		}
		else {
			offs[0] = 0;
			for (int i = 1; i < 9; i++) offs[i] = offs[i - 1] + counts[i - 1];
			int cur[9];
			memcpy(cur, offs, sizeof(cur));
			for (int i = 0; i < n; i++) tmp[cur[owner[i]]++] = idx[i];
			YBR_FREE(owner);
			owner = NULL;
		}
	}

	int ni = new_node(bd);
	if (ni < 0) {
		YBR_FREE(tmp);
		return -1;
	}

	Aabb bounds = aabb_empty();

	if (leaf) {
		if (!push_indices(bd, idx, n, &selfIdxFirst)) {
			YBR_FREE(tmp);
			return -1;
		}
		selfCount = n;
		for (int i = 0; i < n; i++)
			aabb_add_box(&bounds, tri_aabb(&bd->col->tris[idx[i]]));
		bd->col->nodes[ni].firstTri = selfIdxFirst;
		bd->col->nodes[ni].triCount = selfCount;
	}
	else {
		// まず自分に残る分を確定させる (プールは追記しかしないので安全)
		if (0 < counts[8]) {
			if (!push_indices(bd, tmp + offs[8], counts[8], &selfIdxFirst)) {
				YBR_FREE(tmp);
				return -1;
			}
			selfCount = counts[8];
			for (int i = 0; i < counts[8]; i++)
				aabb_add_box(&bounds,
							 tri_aabb(&bd->col->tris[tmp[offs[8] + i]]));
		}
		bd->col->nodes[ni].firstTri = selfIdxFirst;
		bd->col->nodes[ni].triCount = selfCount;

		for (int k = 0; k < 8; k++) {
			if (counts[k] <= 0) continue;
			int ci = build_node(bd, tmp + offs[k], counts[k], child_box(box, k),
								depth + 1);
			if (bd->failed) {
				YBR_FREE(tmp);
				return -1;
			}
			bd->col->nodes[ni].child[k] = ci;
			if (0 <= ci) {
				Aabb cb;
				cb.min = bd->col->nodes[ci].bmin;
				cb.max = bd->col->nodes[ci].bmax;
				aabb_add_box(&bounds, cb);
			}
		}
		YBR_FREE(tmp);
	}

	if (!aabb_valid(bounds)) bounds.min = bounds.max = v3(0, 0, 0);
	bd->col->nodes[ni].bmin = bounds.min;
	bd->col->nodes[ni].bmax = bounds.max;
	return ni;
}

// ----------------------------------------------------------------------------
// 作成 / 破棄

YbrSolidBuildOptions YbrSolidBuildDefaults(void)
{
	YbrSolidBuildOptions o;
	o.maxDepth = 8;
	o.maxTrianglesPerNode = 16;
	o.looseness = 1.2f;
	o.filter = NULL;
	o.filterUserData = NULL;
	return o;
}

static YbrSolid* finish_build(Gather* g, const YbrSolidBuildOptions* opts)
{
	YbrSolidBuildOptions o = YbrSolidBuildDefaults();
	if (opts) o = *opts;
	if (o.maxDepth < 1) o.maxDepth = 1;
	if (16 < o.maxDepth) o.maxDepth = 16;
	if (o.maxTrianglesPerNode < 1) o.maxTrianglesPerNode = 1;
	if (!(1.0f <= o.looseness)) o.looseness = 1.0f;
	if (4.0f < o.looseness) o.looseness = 4.0f;

	if (g->failed) {
		YBR_FREE(g->tris);
		return NULL;
	}

	YbrSolid* c = (YbrSolid*)YBR_CALLOC(1, sizeof(YbrSolid));
	if (!c) {
		YBR_FREE(g->tris);
		return NULL;
	}
	c->tris = g->tris;
	c->triCount = g->count;

	if (c->triCount <= 0) {
		c->bmin = c->bmax = v3(0, 0, 0);
		return c;
	}

	Aabb root = aabb_empty();
	for (int i = 0; i < c->triCount; i++)
		aabb_add_box(&root, tri_aabb(&c->tris[i]));
	// 完全に平らなシーンでも分割できるよう、わずかに広げる
	{
		Vector3 d = v3sub(root.max, root.min);
		float m = d.x;
		if (m < d.y) m = d.y;
		if (m < d.z) m = d.z;
		float pad = (0.0f < m ? m : 1.0f) * 1e-3f;
		root = aabb_expand(root, pad);
	}

	int* idx = (int*)YBR_MALLOC((size_t)c->triCount * sizeof(int));
	if (!idx) {
		YbrSolidUnload(c);
		return NULL;
	}
	for (int i = 0; i < c->triCount; i++) idx[i] = i;

	Builder bd;
	bd.col = c;
	bd.maxDepth = o.maxDepth;
	bd.maxTri = o.maxTrianglesPerNode;
	bd.looseness = o.looseness;
	bd.failed = 0;

	int rootNode = build_node(&bd, idx, c->triCount, root, 0);
	YBR_FREE(idx);
	if (bd.failed || rootNode != 0) {
		YbrSolidUnload(c);
		return NULL;
	}

	// 動かない前提の後処理
	if (0 < c->indexCount) {
		YbrTriangle* sorted = (YbrTriangle*)YBR_MALLOC((size_t)c->indexCount *
													   sizeof(YbrTriangle));
		TriEdges* edges =
			(TriEdges*)YBR_MALLOC((size_t)c->indexCount * sizeof(TriEdges));
		if (!sorted || !edges) {
			YBR_FREE(sorted);
			YBR_FREE(edges);
			YbrSolidUnload(c);
			return NULL;
		}
		for (int i = 0; i < c->indexCount; i++) {
			sorted[i] = c->tris[c->triIndices[i]];
			edges[i].e1 = v3sub(sorted[i].v[1], sorted[i].v[0]);
			edges[i].e2 = v3sub(sorted[i].v[2], sorted[i].v[0]);
		}
		YBR_FREE(c->tris);
		c->tris = sorted;
		c->triCount = c->indexCount;
		c->edges = edges;
		// 並べ替えたので添字表はもう要らない
		YBR_FREE(c->triIndices);
		c->triIndices = NULL;
		c->indexCap = 0;
	}

	c->bmin = c->nodes[0].bmin;
	c->bmax = c->nodes[0].bmax;
	c->hasBounds = 1;
	return c;
}

YbrSolid* YbrSolidBuild(const YbrScene* scene, const YbrSolidBuildOptions* opts)
{
	Gather g;
	memset(&g, 0, sizeof(g));
	g.scene = scene;
	g.opts = opts;

	if (scene) {
		for (int i = 0; i < scene->rootCount; i++)
			gather_node(&g, &scene->roots[i], MatrixIdentity());
	}
	return finish_build(&g, opts);
}

YbrSolid* YbrSolidBuildFromTriangles(const YbrTriangle* tris, int count,
									 const YbrSolidBuildOptions* opts)
{
	Gather g;
	memset(&g, 0, sizeof(g));
	if (tris && 0 < count) {
		g.tris = (YbrTriangle*)YBR_MALLOC((size_t)count * sizeof(YbrTriangle));
		if (!g.tris) return NULL;
		memcpy(g.tris, tris, (size_t)count * sizeof(YbrTriangle));
		g.count = count;
		g.cap = count;
	}
	return finish_build(&g, opts);
}

YbrSolid* YbrSolidBuildFromNode(const YbrScene* scene, const YbrNode* node,
								Matrix parentWorld,
								const YbrSolidBuildOptions* opts)
{
	Gather g;
	memset(&g, 0, sizeof(g));
	g.scene = scene;
	g.opts = opts;
	if (scene && node) gather_node(&g, node, parentWorld);
	return finish_build(&g, opts);
}

void YbrSolidUnload(YbrSolid* col)
{
	if (!col) return;
	YBR_FREE(col->tris);
	YBR_FREE(col->edges);
	YBR_FREE(col->nodes);
	YBR_FREE(col->triIndices);
	YBR_FREE(col);
}

int YbrSolidGetTriangleCount(const YbrSolid* col)
{
	return col ? col->triCount : 0;
}

const YbrTriangle* YbrSolidGetTriangle(const YbrSolid* col, int index)
{
	if (!col || index < 0 || col->triCount <= index) return NULL;
	return &col->tris[index];
}

int YbrSolidGetBounds(const YbrSolid* col, Vector3* outMin, Vector3* outMax)
{
	if (!col || !col->hasBounds) return 0;
	if (outMin) *outMin = col->bmin;
	if (outMax) *outMax = col->bmax;
	return 1;
}

int YbrSolidGetNodeCount(const YbrSolid* col)
{
	return col ? col->nodeCount : 0;
}
int YbrSolidGetDepth(const YbrSolid* col) { return col ? col->depthUsed : 0; }

// 走査の共通部分

static int tag_ok(const YbrTriangle* t, unsigned int mask)
{
	return mask == 0 || (t->tag & mask) != 0;
}

static Aabb node_box(const YbrSolid* c, int ni)
{
	Aabb b;
	b.min = c->nodes[ni].bmin;
	b.max = c->nodes[ni].bmax;
	return b;
}

// ----------------------------------------------------------------------------
// 線分 / レイ

static void fill_ray_hit(YbrRayHit* out, const YbrTriangle* tri, Vector3 a,
						 Vector3 b, float t, float u, float v, int front)
{
	out->hit = 1;
	out->t = t;
	out->point = Vector3Lerp(a, b, t);
	out->normal = tri->normal;
	out->distance = Vector3Distance(a, b) * t;
	out->u = u;
	out->v = v;
	out->frontFace = front;
	out->triangle = *tri;
}

int YbrSolidSegment(const YbrSolid* col, Vector3 a, Vector3 b,
					const YbrQueryOptions* opts, YbrRayHit* out)
{
	YbrRayHit hit;
	memset(&hit, 0, sizeof(hit));
	hit.t = 1.0f;
	if (out) *out = hit;
	if (!col || col->triCount <= 0 || col->nodeCount <= 0) return 0;

	YbrQueryOptions q = opts ? *opts : YbrQueryOptionsDefaults();

	int stack[YBR_COL_STACK];
	int sp = 0;
	stack[sp++] = 0;

	float bestT = 1.0f;
	int found = 0;
	Vector3 cur = b;  // 見つかるたびに終点を手前へ詰める

	while (0 < sp) {
		int ni = stack[--sp];
		if (!aabb_overlap_segment(node_box(col, ni), a, cur)) continue;

		const ColNode* n = &col->nodes[ni];
		for (int i = 0; i < n->triCount; i++) {
			const YbrTriangle* tri = &col->tris[n->firstTri + i];
			if (!tag_ok(tri, q.tagMask)) continue;
			float t, u, v;
			int r = YbrSegmentTriangleHit(a, b, tri->v[0], tri->v[1], tri->v[2],
										  q.cullBackFace, &t, &u, &v);
			if (r && t <= bestT) {
				bestT = t;
				found = 1;
				cur = Vector3Lerp(a, b, t);
				fill_ray_hit(&hit, tri, a, b, t, u, v, 0 < r);
			}
		}
		for (int k = 0; k < 8; k++) {
			if (n->child[k] < 0) continue;
			if (sp < YBR_COL_STACK) stack[sp++] = n->child[k];
		}
	}

	if (out) *out = hit;
	return found;
}

int YbrSolidRay(const YbrSolid* col, Vector3 origin, Vector3 direction,
				float maxDistance, const YbrQueryOptions* opts, YbrRayHit* out)
{
	Vector3 d = Vector3Normalize(direction);
	if (Vector3LengthSqr(d) <= 0.0f) {
		if (out) memset(out, 0, sizeof(*out));
		return 0;
	}
	if (!(0.0f < maxDistance)) {
		// 無限。当たり判定情報の広がりから十分な長さを決める
		float len = 1.0f;
		if (col && col->hasBounds) {
			float e = Vector3Distance(col->bmin, col->bmax);
			Vector3 c = v3mul(v3add(col->bmin, col->bmax), 0.5f);
			len = e + Vector3Distance(origin, c) + 1.0f;
		}
		maxDistance = len;
	}
	return YbrSolidSegment(col, origin, v3add(origin, v3mul(d, maxDistance)),
						   opts, out);
}

// ----------------------------------------------------------------------------
// 球 / カプセル (共通処理)

typedef struct ShapeCtx {
	YbrShapeHit* hit;
	Vector3 resolve;
} ShapeCtx;

// 接触を 1 件足す。normal は「形状を押し出す向き」
static void add_contact(ShapeCtx* cx, const YbrTriangle* tri, Vector3 point,
						Vector3 normal, float depth)
{
	YbrShapeHit* h = cx->hit;
	h->count++;
	if (!h->hit || h->depth < depth) {
		h->hit = 1;
		h->depth = depth;
		h->point = point;
		h->normal = normal;
		h->triangle = *tri;
	}
	else {
		h->hit = 1;
	}

	// すでに解消済みの向きは足さない
	// (同じ壁の連続ポリゴンで押し出し量が二重に乗るのを防ぐ)
	float already = v3dot(normal, cx->resolve);
	if (already < depth)
		cx->resolve = v3add(cx->resolve, v3mul(normal, depth - already));
}

// 球 1 個 vs 三角形
static int sphere_tri(Vector3 center, float radius, const YbrTriangle* tri,
					  Vector3* outPoint, Vector3* outNormal, float* outDepth)
{
	Vector3 q =
		YbrClosestPointOnTriangle(center, tri->v[0], tri->v[1], tri->v[2]);
	Vector3 d = v3sub(center, q);
	float len2 = Vector3LengthSqr(d);
	if (radius * radius <= len2) return 0;

	float len = sqrtf(len2);
	Vector3 n;
	if (1e-6f < len) {
		n = v3mul(d, 1.0f / len);
	}
	else {
		// 中心が面上
		n = tri->normal;
	}
	*outPoint = q;
	*outNormal = n;
	*outDepth = radius - len;
	return 1;
}

static int shape_query(const YbrSolid* col, Vector3 a, Vector3 b, float radius,
					   int isCapsule, const YbrQueryOptions* opts,
					   YbrShapeHit* out, YbrTriangleVisitor visitor, void* ud)
{
	YbrShapeHit h;
	memset(&h, 0, sizeof(h));
	if (out) *out = h;
	if (!col || col->triCount <= 0 || col->nodeCount <= 0 || !(0.0f <= radius))
		return 0;

	YbrQueryOptions q = opts ? *opts : YbrQueryOptionsDefaults();

	ShapeCtx cx;
	cx.hit = &h;
	cx.resolve = v3(0, 0, 0);

	Aabb qb = aabb_empty();
	aabb_add_point(&qb, a);
	if (isCapsule) aabb_add_point(&qb, b);
	qb = aabb_expand(qb, radius);

	int stack[YBR_COL_STACK];
	int sp = 0;
	stack[sp++] = 0;
	int visited = 0;
	int stop = 0;

	while (0 < sp && !stop) {
		int ni = stack[--sp];
		Aabb nb = node_box(col, ni);
		if (!aabb_overlap(nb, qb)) continue;
		if (!isCapsule && radius * radius < aabb_dist2_point(nb, a)) continue;

		const ColNode* n = &col->nodes[ni];
		for (int i = 0; i < n->triCount; i++) {
			const YbrTriangle* tri = &col->tris[n->firstTri + i];
			if (!tag_ok(tri, q.tagMask)) continue;

			Vector3 point, normal;
			float depth;
			int touched = 0;

			if (!isCapsule) {
				touched = sphere_tri(a, radius, tri, &point, &normal, &depth);
			}
			else {
				Vector3 ps, pt;
				float d = YbrSegmentTriangleDistance(a, b, tri->v[0], tri->v[1],
													 tri->v[2], &ps, &pt);
				if (d < radius) {
					point = pt;
					if (1e-6f < d) {
						normal = v3mul(v3sub(ps, pt), 1.0f / d);
						depth = radius - d;
					}
					else {
						// 軸が面を貫いている。面法線で押し戻す
						normal = tri->normal;
						depth = radius;
					}
					touched = 1;
				}
			}

			if (!touched) continue;
			add_contact(&cx, tri, point, normal, depth);
			if (visitor) {
				visited++;
				if (!visitor(tri, ud)) {
					stop = 1;
					break;
				}
			}
		}
		if (stop) break;

		for (int k = 0; k < 8; k++) {
			if (n->child[k] < 0) continue;
			if (sp < YBR_COL_STACK) stack[sp++] = n->child[k];
		}
	}

	h.resolve = cx.resolve;
	if (out) *out = h;
	return visitor ? visited : h.hit;
}

int YbrSolidSphere(const YbrSolid* col, Vector3 center, float radius,
				   const YbrQueryOptions* opts, YbrShapeHit* out)
{
	return shape_query(col, center, center, radius, 0, opts, out, NULL, NULL);
}

int YbrSolidCapsule(const YbrSolid* col, Vector3 a, Vector3 b, float radius,
					const YbrQueryOptions* opts, YbrShapeHit* out)
{
	return shape_query(col, a, b, radius, 1, opts, out, NULL, NULL);
}

int YbrSolidOverlapSphere(const YbrSolid* col, Vector3 center, float radius,
						  const YbrQueryOptions* opts,
						  YbrTriangleVisitor visitor, void* userData)
{
	if (!visitor) return 0;
	return shape_query(col, center, center, radius, 0, opts, NULL, visitor,
					   userData);
}

int YbrSolidOverlapCapsule(const YbrSolid* col, Vector3 a, Vector3 b,
						   float radius, const YbrQueryOptions* opts,
						   YbrTriangleVisitor visitor, void* userData)
{
	if (!visitor) return 0;
	return shape_query(col, a, b, radius, 1, opts, NULL, visitor, userData);
}

// AABB

int YbrSolidOverlapBox(const YbrSolid* col, Vector3 boxMin, Vector3 boxMax,
					   const YbrQueryOptions* opts, YbrTriangleVisitor visitor,
					   void* userData)
{
	if (!col || !visitor || col->nodeCount <= 0) return 0;
	YbrQueryOptions q = opts ? *opts : YbrQueryOptionsDefaults();

	Aabb qb;
	qb.min = Vector3Min(boxMin, boxMax);
	qb.max = Vector3Max(boxMin, boxMax);

	int stack[YBR_COL_STACK];
	int sp = 0, count = 0;
	stack[sp++] = 0;

	while (0 < sp) {
		int ni = stack[--sp];
		if (!aabb_overlap(node_box(col, ni), qb)) continue;

		const ColNode* n = &col->nodes[ni];
		for (int i = 0; i < n->triCount; i++) {
			const YbrTriangle* tri = &col->tris[n->firstTri + i];
			if (!tag_ok(tri, q.tagMask)) continue;
			if (!YbrTriangleBoxOverlap(tri->v[0], tri->v[1], tri->v[2], qb.min,
									   qb.max))
				continue;
			count++;
			if (!visitor(tri, userData)) return count;
		}
		for (int k = 0; k < 8; k++) {
			if (n->child[k] < 0) continue;
			if (sp < YBR_COL_STACK) stack[sp++] = n->child[k];
		}
	}
	return count;
}

// ----------------------------------------------------------------------------
// 三角形

// 2 枚の三角形の交差線分を求める。同一平面上のケースは扱わない。
static int tri_tri(Vector3 a0, Vector3 a1, Vector3 a2, Vector3 b0, Vector3 b1,
				   Vector3 b2, Vector3* outA, Vector3* outB)
{
	Vector3 pts[6];
	int np = 0;

	const Vector3 ea[3][2] = {{a0, a1}, {a1, a2}, {a2, a0}};
	const Vector3 eb[3][2] = {{b0, b1}, {b1, b2}, {b2, b0}};

	for (int i = 0; i < 3 && np < 6; i++) {
		float t;
		if (YbrSegmentTriangleHit(ea[i][0], ea[i][1], b0, b1, b2, 0, &t, NULL,
								  NULL))
			pts[np++] = Vector3Lerp(ea[i][0], ea[i][1], t);
	}
	for (int i = 0; i < 3 && np < 6; i++) {
		float t;
		if (YbrSegmentTriangleHit(eb[i][0], eb[i][1], a0, a1, a2, 0, &t, NULL,
								  NULL))
			pts[np++] = Vector3Lerp(eb[i][0], eb[i][1], t);
	}
	if (np == 0) return 0;

	// もっとも離れた 2 点を交差線分とする
	Vector3 p = pts[0], qq = pts[0];
	float best = 0.0f;
	for (int i = 0; i < np; i++) {
		for (int j = i + 1; j < np; j++) {
			float d = Vector3DistanceSqr(pts[i], pts[j]);
			if (best < d) {
				best = d;
				p = pts[i];
				qq = pts[j];
			}
		}
	}
	*outA = p;
	*outB = qq;
	return 1;
}

int YbrSolidTriangle(const YbrSolid* col, Vector3 v0, Vector3 v1, Vector3 v2,
					 const YbrQueryOptions* opts, YbrTriHit* out)
{
	YbrTriHit h;
	memset(&h, 0, sizeof(h));
	if (out) *out = h;
	if (!col || col->triCount <= 0 || col->nodeCount <= 0) return 0;

	YbrQueryOptions q = opts ? *opts : YbrQueryOptionsDefaults();

	Aabb qb = aabb_empty();
	aabb_add_point(&qb, v0);
	aabb_add_point(&qb, v1);
	aabb_add_point(&qb, v2);

	int stack[YBR_COL_STACK];
	int sp = 0;
	stack[sp++] = 0;

	while (0 < sp) {
		int ni = stack[--sp];
		if (!aabb_overlap(node_box(col, ni), qb)) continue;

		const ColNode* n = &col->nodes[ni];
		for (int i = 0; i < n->triCount; i++) {
			const YbrTriangle* tri = &col->tris[n->firstTri + i];
			if (!tag_ok(tri, q.tagMask)) continue;
			if (!aabb_overlap(tri_aabb(tri), qb)) continue;

			Vector3 pa, pb;
			if (!tri_tri(v0, v1, v2, tri->v[0], tri->v[1], tri->v[2], &pa, &pb))
				continue;

			if (!h.hit) {
				h.hit = 1;
				h.pointA = pa;
				h.pointB = pb;
				h.point = v3mul(v3add(pa, pb), 0.5f);
				h.triangle = *tri;
			}
			h.count++;
		}
		for (int k = 0; k < 8; k++) {
			if (n->child[k] < 0) continue;
			if (sp < YBR_COL_STACK) stack[sp++] = n->child[k];
		}
	}

	if (out) *out = h;
	return h.hit;
}

// ----------------------------------------------------------------------------
// スイープ (Solid)

int YbrSolidSweepSphere(const YbrSolid* col, Vector3 from, Vector3 to,
						float radius, const YbrQueryOptions* opts,
						YbrRayHit* out)
{
	YbrRayHit hit;
	memset(&hit, 0, sizeof(hit));
	hit.t = 1.0f;
	if (out) *out = hit;
	if (!col || col->triCount <= 0 || col->nodeCount <= 0) return 0;

	YbrQueryOptions q = opts ? *opts : YbrQueryOptionsDefaults();

	// 移動経路を半径ぶん膨らませた AABB で枝刈りする
	Aabb qb = aabb_empty();
	aabb_add_point(&qb, from);
	aabb_add_point(&qb, to);
	qb = aabb_expand(qb, 0.0f < radius ? radius : 0.0f);

	int stack[YBR_COL_STACK];
	int sp = 0;
	stack[sp++] = 0;

	float bestT = 1.0f;
	int found = 0;

	while (0 < sp) {
		int ni = stack[--sp];
		Aabb nb = node_box(col, ni);
		if (!aabb_overlap(nb, qb)) continue;
		if (!aabb_overlap_segment(aabb_expand(nb, radius), from, to)) continue;

		const ColNode* n = &col->nodes[ni];
		for (int i = 0; i < n->triCount; i++) {
			const YbrTriangle* tri = &col->tris[n->firstTri + i];
			if (!tag_ok(tri, q.tagMask)) continue;

			float t;
			Vector3 point, normal;
			if (!YbrSweepSphereTriangle(from, to, radius, tri->v[0], tri->v[1],
										tri->v[2], &t, &point, &normal))
				continue;
			if (found && bestT <= t) continue;

			bestT = t;
			found = 1;
			hit.hit = 1;
			hit.t = t;
			hit.point = point;
			hit.normal = normal;
			hit.distance = Vector3Distance(from, to) * t;
			hit.frontFace = (0.0f < v3dot(tri->normal, normal));
			hit.triangle = *tri;
		}
		for (int k = 0; k < 8; k++) {
			if (n->child[k] < 0) continue;
			if (sp < YBR_COL_STACK) stack[sp++] = n->child[k];
		}
	}

	if (out) *out = hit;
	return found;
}
