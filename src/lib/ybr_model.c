/*
	Yui Blender to Raylib - モデル描画
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rlgl.h"
#include "ybr_internal.h"
#include "ybr_model_internal.h"

// ----------------------------------------------------------------------------
// メッシュ分割

#define YBR_MAX_SUBMESH_VERTICES 65535

static void free_submesh(YbrSubmesh* s)
{
	YBR_FREE(s->vertexMap);
	YBR_FREE(s->indices);
	s->vertexMap = NULL;
	s->indices = NULL;
}

void YbrFreeSubmeshes(YbrSubmesh* subs, int count)
{
	if (!subs) return;
	for (int i = 0; i < count; i++) free_submesh(&subs[i]);
	YBR_FREE(subs);
}

int YbrSplitMesh(const YbrMesh* mesh, YbrSubmesh** out, int* outCount)
{
	if (out) *out = NULL;
	if (outCount) *outCount = 0;
	if (!mesh || !out || !outCount) return 0;
	if (mesh->triangleCount <= 0 || !mesh->indices || mesh->vertexCount <= 0)
		return 1;

	int groupCount = 0 < mesh->materialCount ? mesh->materialCount : 1;
	int noMaterial = (mesh->materialCount <= 0);

	int* remap = (int*)YBR_MALLOC((size_t)mesh->vertexCount * sizeof(int));
	int* tris = (int*)YBR_MALLOC((size_t)mesh->triangleCount * sizeof(int));
	if (!remap || !tris) {
		YBR_FREE(remap);
		YBR_FREE(tris);
		return 0;
	}
	for (int i = 0; i < mesh->vertexCount; i++) remap[i] = -1;

	YbrSubmesh* list = NULL;
	int listCount = 0, listCap = 0;
	int ok = 1;

	for (int g = 0; g < groupCount && ok; g++) {
		// このマテリアルを使う三角形を集める
		int n = 0;
		for (int t = 0; t < mesh->triangleCount; t++) {
			int mi = mesh->materialIndices ? (int)mesh->materialIndices[t] : 0;
			if (mi < 0 || groupCount <= mi) mi = 0;
			if (mi == g) tris[n++] = t;
		}
		if (n == 0) continue;

		int pos = 0;
		while (pos < n && ok) {
			YbrSubmesh sm;
			memset(&sm, 0, sizeof(sm));
			sm.materialIndex = noMaterial ? -1 : g;

			int maxTri = n - pos;
			sm.vertexMap = (unsigned int*)YBR_MALLOC((size_t)maxTri * 3 *
													 sizeof(unsigned int));
			sm.indices = (unsigned short*)YBR_MALLOC((size_t)maxTri * 3 *
													 sizeof(unsigned short));
			if (!sm.vertexMap || !sm.indices) {
				free_submesh(&sm);
				ok = 0;
				break;
			}

			int nv = 0, nt = 0;
			while (pos < n && nv + 3 <= YBR_MAX_SUBMESH_VERTICES) {
				int t = tris[pos];
				for (int k = 0; k < 3; k++) {
					unsigned int v = mesh->indices[(size_t)t * 3 + k];
					if (mesh->vertexCount <= (int)v) v = 0;
					if (remap[v] < 0) {
						remap[v] = nv;
						sm.vertexMap[nv++] = v;
					}
					sm.indices[(size_t)nt * 3 + k] = (unsigned short)remap[v];
				}
				nt++;
				pos++;
			}
			// 次のチャンク用に remap を戻す
			for (int i = 0; i < nv; i++) remap[sm.vertexMap[i]] = -1;

			sm.vertexCount = nv;
			sm.triangleCount = nt;

			void* nb = YbrGrowBuffer(list, &listCap, listCount + 1,
									 sizeof(YbrSubmesh));
			if (!nb) {
				free_submesh(&sm);
				ok = 0;
				break;
			}
			list = (YbrSubmesh*)nb;
			list[listCount++] = sm;
		}
	}

	YBR_FREE(remap);
	YBR_FREE(tris);

	if (!ok) {
		YbrFreeSubmeshes(list, listCount);
		return 0;
	}
	*out = list;
	*outCount = listCount;
	return 1;
}

// ----------------------------------------------------------------------------
// 描画用モデル

YbrModelOptions YbrModelOptionsDefaults(void)
{
	YbrModelOptions o;
	o.loadTextures = 1;
	o.uploadNormals = 1;
	o.uploadTexcoords = 1;
	o.uploadColors = 1;
	o.materialShaders = 1;
	o.gpuSkinning = 1;
	o.maxBones = YBR_SHADER_MAX_BONES;
	o.glVersion = 0;
	return o;
}

static int mesh_index_of(const YbrModel* m, const char* id)
{
	for (int i = 0; i < m->meshCount; i++)
		if (YbrStrEq(m->meshes[i]->id, id)) return i;
	return -1;
}

// このメッシュが唯一のアーマチュアに従うか。
// アーマチュアは 1 つしかないので、データ ID が一致するかだけを見る。
static int mesh_uses_armature(const YbrModel* m, const YbrMesh* mesh)
{
	if (!m->armature || !mesh || !mesh->armatureData) return 0;
	return YbrStrEq(m->armature->id, mesh->armatureData);
}

static int material_index_of(const YbrModel* m, const char* id)
{
	if (!id) return -1;
	for (int i = 0; i < m->materialCount; i++)
		if (YbrStrEq(m->materials[i].id, id)) return i;
	return -1;
}

// GPUへ載せる
static void upload_part(YbrModelPart* p, const YbrModelMaterial* mat)
{
	p->vaoId = rlLoadVertexArray();
	rlEnableVertexArray(p->vaoId);

	// GPU スキニングなら位置 / 法線は書き換えないので静的バッファでよい
	int dynamic = p->dynamicBuffers;

	p->vboId[0] = rlLoadVertexBuffer(
		p->positions, p->vertexCount * 3 * (int)sizeof(float), dynamic);
	rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION, 3,
						 RL_FLOAT, 0, 0, 0);
	rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION);

	if (p->texcoords) {
		p->vboId[1] = rlLoadVertexBuffer(
			p->texcoords, p->vertexCount * 2 * (int)sizeof(float), 0);
		rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD, 2,
							 RL_FLOAT, 0, 0, 0);
		rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD);
	}
	if (p->normals) {
		p->vboId[2] = rlLoadVertexBuffer(
			p->normals, p->vertexCount * 3 * (int)sizeof(float), dynamic);
		rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_NORMAL, 3,
							 RL_FLOAT, 0, 0, 0);
		rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_NORMAL);
	}
	if (p->colors) {
		p->vboId[3] = rlLoadVertexBuffer(p->colors, p->vertexCount * 4, 0);
		rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR, 4,
							 RL_UNSIGNED_BYTE, 1, 0, 0);
		rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR);
	}
	if (p->tangents) {
		p->vboId[6] = rlLoadVertexBuffer(
			p->tangents, p->vertexCount * 4 * (int)sizeof(float), 0);
		rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TANGENT, 4,
							 RL_FLOAT, 0, 0, 0);
		rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TANGENT);
	}

	if (p->gpuSkin && mat && p->boneIds && p->boneWeights) {
		// ボーン番号 / ウェイトはどちらも vec4 で送る
		float* ids =
			(float*)YBR_MALLOC((size_t)p->vertexCount * 4 * sizeof(float));
		if (ids) {
			for (int i = 0; i < p->vertexCount * 4; i++)
				ids[i] = (float)p->boneIds[i];
			p->vboId[4] = rlLoadVertexBuffer(
				ids, p->vertexCount * 4 * (int)sizeof(float), 0);
			YBR_FREE(ids);
			rlSetVertexAttribute((unsigned int)mat->attribBoneIds, 4, RL_FLOAT,
								 0, 0, 0);
			rlEnableVertexAttribute((unsigned int)mat->attribBoneIds);
		}
		p->vboId[5] = rlLoadVertexBuffer(
			p->boneWeights, p->vertexCount * 4 * (int)sizeof(float), 0);
		rlSetVertexAttribute((unsigned int)mat->attribBoneWeights, 4, RL_FLOAT,
							 0, 0, 0);
		rlEnableVertexAttribute((unsigned int)mat->attribBoneWeights);
	}

	p->vboId[7] = rlLoadVertexBufferElement(
		p->indices, p->triangleCount * 3 * (int)sizeof(unsigned short), 0);
	rlDisableVertexArray();
}

static void unload_part(YbrModelPart* p)
{
	for (int i = 0; i < 8; i++)
		if (p->vboId[i]) rlUnloadVertexBuffer(p->vboId[i]);
	if (p->vaoId) rlUnloadVertexArray(p->vaoId);

	YBR_FREE(p->positions);
	YBR_FREE(p->normals);
	YBR_FREE(p->tangents);
	YBR_FREE(p->texcoords);
	YBR_FREE(p->colors);
	YBR_FREE(p->indices);
	YBR_FREE(p->boneIds);
	YBR_FREE(p->boneWeights);
	memset(p, 0, sizeof(*p));
}

// サブメッシュ1つをYbrModelPartに展開する
static int build_part(YbrModelPart* p, const YbrMesh* m, const YbrSubmesh* sm,
					  int meshIndex, int materialIndex, int useArmature,
					  const YbrModelOptions* o, const YbrModelMaterial* mat,
					  int gpuSkin)
{
	memset(p, 0, sizeof(*p));
	p->gpuSkin = gpuSkin;

	p->meshIndex = meshIndex;
	p->materialIndex = materialIndex;
	p->vertexCount = sm->vertexCount;
	p->triangleCount = sm->triangleCount;

	int nv = sm->vertexCount;
	p->positions = (float*)YBR_MALLOC((size_t)nv * 3 * sizeof(float));
	p->indices = (unsigned short*)YBR_MALLOC((size_t)sm->triangleCount * 3 *
											 sizeof(unsigned short));
	if (!p->positions || !p->indices) return 0;
	memcpy(p->indices, sm->indices,
		   (size_t)sm->triangleCount * 3 * sizeof(unsigned short));

	for (int i = 0; i < nv; i++) {
		unsigned int v = sm->vertexMap[i];
		p->positions[i * 3 + 0] = m->positions[v * 3 + 0];
		p->positions[i * 3 + 1] = m->positions[v * 3 + 1];
		p->positions[i * 3 + 2] = m->positions[v * 3 + 2];
	}

	if (o->uploadNormals && m->normals) {
		p->normals = (float*)YBR_MALLOC((size_t)nv * 3 * sizeof(float));
		if (!p->normals) return 0;
		for (int i = 0; i < nv; i++) {
			unsigned int v = sm->vertexMap[i];
			p->normals[i * 3 + 0] = m->normals[v * 3 + 0];
			p->normals[i * 3 + 1] = m->normals[v * 3 + 1];
			p->normals[i * 3 + 2] = m->normals[v * 3 + 2];
		}
	}
	// 接線 (法線マップ用)。無ければシェーダー側で近似に落ちる。
	if (o->uploadNormals && m->tangents) {
		p->tangents = (float*)YBR_MALLOC((size_t)nv * 4 * sizeof(float));
		if (!p->tangents) return 0;
		for (int i = 0; i < nv; i++) {
			unsigned int v = sm->vertexMap[i];
			for (int k = 0; k < 4; k++)
				p->tangents[i * 4 + k] = m->tangents[v * 4 + k];
		}
	}
	if (o->uploadTexcoords && m->texcoords) {
		p->texcoords = (float*)YBR_MALLOC((size_t)nv * 2 * sizeof(float));
		if (!p->texcoords) return 0;
		for (int i = 0; i < nv; i++) {
			unsigned int v = sm->vertexMap[i];
			p->texcoords[i * 2 + 0] = m->texcoords[v * 2 + 0];
			p->texcoords[i * 2 + 1] = m->texcoords[v * 2 + 1];
		}
	}
	if (o->uploadColors && m->colors) {
		p->colors = (unsigned char*)YBR_MALLOC((size_t)nv * 4);
		if (!p->colors) return 0;
		for (int i = 0; i < nv; i++) {
			unsigned int v = sm->vertexMap[i];
			memcpy(p->colors + (size_t)i * 4, m->colors + (size_t)v * 4, 4);
		}
	}

	if (m->skin && m->skin->joints && m->skin->weights && useArmature) {
		p->boneIds = (unsigned short*)YBR_MALLOC((size_t)nv * 4 *
												 sizeof(unsigned short));
		p->boneWeights = (float*)YBR_MALLOC((size_t)nv * 4 * sizeof(float));
		if (!p->boneIds || !p->boneWeights) return 0;
		p->skinned = 1;
		// CPU スキニングは頂点バッファを書き換えるので動的にする
		p->dynamicBuffers = !p->gpuSkin;
		for (int i = 0; i < nv; i++) {
			unsigned int v = sm->vertexMap[i];
			for (int k = 0; k < 4; k++) {
				p->boneIds[i * 4 + k] = m->skin->joints[v * 4 + k];
				p->boneWeights[i * 4 + k] = m->skin->weights[v * 4 + k];
			}
		}
	}

	// ローカル AABB (半透明の並べ替えとレスト AABB に使う)
	{
		Vector3 lo = {1e30f, 1e30f, 1e30f};
		Vector3 hi = {-1e30f, -1e30f, -1e30f};
		for (int i = 0; i < nv; i++) {
			Vector3 v = {p->positions[i * 3 + 0], p->positions[i * 3 + 1],
						 p->positions[i * 3 + 2]};
			lo = Vector3Min(lo, v);
			hi = Vector3Max(hi, v);
		}
		if (nv <= 0) {
			lo = (Vector3){0, 0, 0};
			hi = lo;
		}
		p->localMin = lo;
		p->localMax = hi;
	}

	upload_part(p, mat);
	return 1;
}

// ----------------------------------------------------------------------------
// マテリアルシェーダー

static void material_reset_shader(YbrModelMaterial* dst)
{
	memset(&dst->shader, 0, sizeof(dst->shader));
	dst->textureCount = 0;
	dst->ownsShader = 0;
	dst->skinning = 0;
	dst->maxBones = 0;
	dst->locBoneMatrices = -1;
	dst->attribBoneIds = -1;
	dst->attribBoneWeights = -1;
	for (int i = 0; i < YBR_WORLD_MAX_ACTIVE_LIGHTS; i++) {
		dst->locLightDir[i] = -1;
		dst->locLightColor[i] = -1;
		dst->locLightPos[i] = -1;
		dst->locLightParams[i] = -1;
	}
	for (int i = 0; i < YBR_WORLD_MAX_SHADOWS; i++) {
		dst->locShadowMap[i] = -1;
		dst->locLightVP[i] = -1;
		dst->locShadowParams[i] = -1;
	}
	dst->locAmbient = -1;
	dst->locViewPos = -1;
}

// 生成した GLSL をコンパイルして、uniform の初期値まで流し込む。
// 失敗したら 0 を返して既定シェーダーのままにする。
static int material_build_shader(YbrModel* model, YbrModelMaterial* dst,
								 const YbrMaterial* src,
								 const YbrModelOptions* o, int wantSkinning,
								 const YbrScene* scene)
{
	YbrShaderOptions so =
		YbrShaderOptionsDefaults(o->glVersion ? o->glVersion : rlGetVersion());
	so.skinning = wantSkinning;
	so.maxBones = (0 < o->maxBones) ? o->maxBones : YBR_SHADER_MAX_BONES;
	// ライトの数は YbrWorld が実行時に決めるので、シェーダー側は常に
	// 上限ぶんの uniform を持っておく。使われない灯は色 0 で送られ、
	// 使われない影は ybrShadowParams.x = 0 で切られる。
	so.lightCount = YBR_WORLD_MAX_ACTIVE_LIGHTS;
	so.shadowLights = YBR_WORLD_MAX_SHADOWS;
	so.scene = scene; // ノードグループを関数化するのに必要

	YbrShaderResult r = YbrShaderFromMaterialEx(src, &so);
	if (r.error != YBR_SHADER_OK || !r.vertexCode || !r.fragmentCode) {
		// 黙って既定のマテリアル (陰影もテクスチャも無い) に落ちると
		// 原因が分からないので、必ず理由を残す。
		TraceLog(LOG_WARNING,
				 "YBR: material '%s': could not generate a shader (%s: %s). "
				 "Falling back to the default material.",
				 src->id ? src->id : "?", YbrShaderErrorString(r.error),
				 r.errorMessage ? r.errorMessage : "");
		YbrUnloadShaderResult(&r);
		return 0;
	}

	Shader sh = LoadShaderFromMemory(r.vertexCode, r.fragmentCode);
	if (sh.id == 0) {
		TraceLog(LOG_WARNING,
				 "YBR: material '%s': the generated GLSL did not compile. "
				 "Falling back to the default material.",
				 src->id ? src->id : "?");
		YbrUnloadShaderResult(&r);
		return 0;
	}

	// uniform の初期値。autoSet のものは raylib / 描画側が毎フレーム入れる
	for (int i = 0; i < r.uniformCount; i++) {
		const YbrShaderUniform* u = &r.uniforms[i];
		int loc = GetShaderLocation(sh, u->name);
		if (0 <= u->locIndex && sh.locs) sh.locs[u->locIndex] = loc;
		if (loc < 0) continue;
		int handled = 0;
		for (int k = 0; k < YBR_WORLD_MAX_ACTIVE_LIGHTS; k++) {
			char nd[32], nc[32], np[32], na[32];
			snprintf(nd, sizeof(nd), "%s%d", YBR_SHADER_UNIFORM_LIGHT_DIR, k);
			snprintf(nc, sizeof(nc), "%s%d", YBR_SHADER_UNIFORM_LIGHT_COLOR, k);
			snprintf(np, sizeof(np), "%s%d", YBR_SHADER_UNIFORM_LIGHT_POS, k);
			snprintf(na, sizeof(na), "%s%d", YBR_SHADER_UNIFORM_LIGHT_PARAMS,
					 k);
			if (!strcmp(u->name, nd)) {
				dst->locLightDir[k] = loc;
				handled = 1;
				break;
			}
			if (!strcmp(u->name, nc)) {
				dst->locLightColor[k] = loc;
				handled = 1;
				break;
			}
			if (!strcmp(u->name, np)) {
				dst->locLightPos[k] = loc;
				handled = 1;
				break;
			}
			if (!strcmp(u->name, na)) {
				dst->locLightParams[k] = loc;
				handled = 1;
				break;
			}
		}
		if (handled) {
			// 描画時に YbrWorld を渡さない場合はここの値がそのまま効くので、
			// 生成時の既定値 (白い平行光) を入れておく。
			if (0 < u->valueCount)
				SetShaderValue(sh, loc, u->value,
							   YbrShaderUniformFormat(u->type));
			continue;
		}

		// sampler は全部ここで割り当てる
		// 読み込めなかった場合も枠だけは取っておく。sampler を割り当てずに
		// 放っておくと GLSL の既定である 0 番ユニットを読みに行き、そこに
		// 別のテクスチャ (あるいは何も) が居ると意図しない絵が出る。
		if (u->type == YBR_UNIFORM_SAMPLER2D) {
			if (YBR_MODEL_MAX_TEXTURES <= dst->textureCount) {
				TraceLog(LOG_WARNING,
						 "YBR: material '%s': more than %d textures are used, "
						 "'%s' was dropped",
						 src->id ? src->id : "?", YBR_MODEL_MAX_TEXTURES,
						 u->name);
				continue;
			}
			Texture2D tex;
			memset(&tex, 0, sizeof(tex));
			if (o->loadTextures && u->textureId)
				tex = YbrModelGetTexture(model, scene, u->textureId,
										 u->texWrap, u->texFilter);

			YbrModelTextureSlot* ts = &dst->textures[dst->textureCount];
			ts->texture = tex; // id == 0 なら描画時に既定テクスチャ
			ts->loc = loc;
			ts->slot = dst->textureCount;
			// ベースカラーは従来どおり mat->texture でも触れるようにする
			if (u->locIndex == RL_SHADER_LOC_MAP_ALBEDO) dst->texture = tex;
			dst->textureCount++;
			continue;
		}

		if (!strcmp(u->name, YBR_SHADER_UNIFORM_AMBIENT)) {
			dst->locAmbient = loc;
			if (0 < u->valueCount)
				SetShaderValue(sh, loc, u->value,
							   YbrShaderUniformFormat(u->type));
		}
		else if (!strcmp(u->name, YBR_SHADER_UNIFORM_VIEW_POS))
			dst->locViewPos = loc;
		else if (!u->autoSet && 0 < u->valueCount)
			SetShaderValue(sh, loc, u->value, YbrShaderUniformFormat(u->type));
	}
	YbrUnloadShaderResult(&r);

	// 影の uniform。生成側は sampler として登録しないので、
	// 名前で直接引く (束ねるのは描画時)。
	for (int i = 0; i < YBR_WORLD_MAX_SHADOWS; i++) {
		char nm[40], nv[40], np[40];
		snprintf(nm, sizeof(nm), "%s%d", YBR_SHADER_UNIFORM_SHADOW_MAP, i);
		snprintf(nv, sizeof(nv), "%s%d", YBR_SHADER_UNIFORM_LIGHT_VP, i);
		snprintf(np, sizeof(np), "%s%d", YBR_SHADER_UNIFORM_SHADOW_PARAMS, i);
		dst->locShadowMap[i] = GetShaderLocation(sh, nm);
		dst->locLightVP[i] = GetShaderLocation(sh, nv);
		dst->locShadowParams[i] = GetShaderLocation(sh, np);
	}

	dst->shader = sh;
	dst->ownsShader = 1;
	dst->maxBones = so.maxBones;

	if (wantSkinning) {
		dst->locBoneMatrices = GetShaderLocation(sh, YBR_SHADER_UNIFORM_BONES);
		dst->attribBoneIds =
			GetShaderLocationAttrib(sh, YBR_SHADER_ATTRIB_BONE_IDS);
		dst->attribBoneWeights =
			GetShaderLocationAttrib(sh, YBR_SHADER_ATTRIB_BONE_WEIGHTS);
		dst->skinning = (0 <= dst->locBoneMatrices && 0 <= dst->attribBoneIds &&
						 0 <= dst->attribBoneWeights);
	}
	return 1;
}

// シーンツリーの走査
typedef struct NodeGather {
	YbrModel* model;
	int cap;
	int failed;
} NodeGather;

static void gather_nodes(NodeGather* g, const YbrNode* n, Matrix parentWorld)
{
	if (g->failed || !n) return;
	Matrix world = MatrixMultiply(n->matrix, parentWorld);

	if (n->type == YBR_NODE_MESH && n->dataId) {
		int mi = mesh_index_of(g->model, n->dataId);
		if (0 <= mi) {
			void* nb =
				YbrGrowBuffer(g->model->nodes, &g->cap, g->model->nodeCount + 1,
							  sizeof(YbrModelNode));
			if (!nb) {
				g->failed = 1;
				return;
			}
			g->model->nodes = (YbrModelNode*)nb;

			YbrModelNode* mn = &g->model->nodes[g->model->nodeCount++];
			mn->name = n->name;
			mn->meshId = n->dataId;
			mn->transform = world;
			mn->meshIndex = mi;
			mn->source = n; // 元の YbrNode
		}
	}
	for (int i = 0; i < n->childCount; i++)
		gather_nodes(g, &n->children[i], world);
}

YbrModel* YbrModelLoad(const YbrScene* scene, const YbrModelOptions* opts)
{
	if (!scene) return NULL;
	YbrModelOptions o = YbrModelOptionsDefaults();
	if (opts) o = *opts;

	YbrModel* m = (YbrModel*)YBR_CALLOC(1, sizeof(YbrModel));
	if (!m) return NULL;
	m->scene = scene;

	// メッシュの参照表
	if (0 < scene->meshCount) {
		m->meshes = (const YbrMesh**)YBR_MALLOC((size_t)scene->meshCount *
												sizeof(YbrMesh*));
		if (!m->meshes) {
			YbrModelUnload(m);
			return NULL;
		}
		for (int i = 0; i < scene->meshCount; i++)
			m->meshes[i] = &scene->meshes[i];
		m->meshCount = scene->meshCount;
	}

	// アーマチュアは 0 個か 1 個（ポーズはインスタンスが持つ）
	m->armature = YbrGetArmature(scene);

	// マテリアル
	if (0 < scene->materialCount) {
		m->materials = (YbrModelMaterial*)YBR_CALLOC(
			(size_t)scene->materialCount, sizeof(YbrModelMaterial));
		if (!m->materials) {
			YbrModelUnload(m);
			return NULL;
		}
		m->materialCount = scene->materialCount;
		for (int i = 0; i < scene->materialCount; i++) {
			const YbrMaterial* src = &scene->materials[i];
			YbrModelMaterial* dst = &m->materials[i];
			material_reset_shader(dst);
			dst->id = src->id;
			dst->baseColor = src->baseColor;
			dst->backfaceCulling = src->backfaceCulling;
			dst->transparent = src->transparent;
			if (src->transparent) m->hasTransparent = 1;
			memset(&dst->texture, 0, sizeof(dst->texture));
			// テクスチャはシェーダーを作るときに、宣言されている sampler を
			// 見ながらまとめて割り当てる (material_build_shader)
		}
	}

	// マテリアルごとのシェーダ
	if (o.materialShaders && 0 < m->materialCount) {
		unsigned char* needSkin =
			(unsigned char*)YBR_CALLOC((size_t)m->materialCount, 1);
		if (!needSkin) {
			YbrModelUnload(m);
			return NULL;
		}

		if (o.gpuSkinning) {
			for (int i = 0; i < m->meshCount; i++) {
				const YbrMesh* mesh = m->meshes[i];
				if (!mesh->skin || !mesh->skin->joints) continue;
				if (!mesh_uses_armature(m, mesh)) continue;
				for (int k = 0; k < mesh->materialCount; k++) {
					int mi = material_index_of(m, mesh->materials[k]);
					if (0 <= mi) needSkin[mi] = 1;
				}
			}
		}

		for (int i = 0; i < m->materialCount; i++) {
			if (!material_build_shader(m, &m->materials[i],
									   &scene->materials[i], &o, needSkin[i],
									   scene))
				material_reset_shader(&m->materials[i]);
			if (m->materials[i].skinning) m->gpuSkinning = 1;
		}
		YBR_FREE(needSkin);
	}

	// 描画パート
	{
		int cap = 0;
		for (int i = 0; i < m->meshCount; i++) {
			const YbrMesh* mesh = m->meshes[i];
			YbrSubmesh* subs = NULL;
			int subCount = 0;
			if (!YbrSplitMesh(mesh, &subs, &subCount)) {
				YbrModelUnload(m);
				return NULL;
			}

			int useArmature = mesh_uses_armature(m, mesh);
			for (int s = 0; s < subCount; s++) {
				int matIndex = -1;
				if (0 <= subs[s].materialIndex &&
					subs[s].materialIndex < mesh->materialCount)
					matIndex = material_index_of(
						m, mesh->materials[subs[s].materialIndex]);

				void* nb = YbrGrowBuffer(m->parts, &cap, m->partCount + 1,
										 sizeof(YbrModelPart));
				if (!nb) {
					YbrFreeSubmeshes(subs, subCount);
					YbrModelUnload(m);
					return NULL;
				}
				m->parts = (YbrModelPart*)nb;

				const YbrModelMaterial* mat =
					(0 <= matIndex) ? &m->materials[matIndex] : NULL;
				// ボーンが多すぎるとシェーダーの配列に入らないので CPU に落とす
				int boneCount = useArmature ? m->armature->boneCount : 0;
				int gpuSkin =
					(mat && mat->skinning && useArmature && mesh->skin &&
					 mesh->skin->joints && boneCount <= mat->maxBones);

				if (!build_part(&m->parts[m->partCount], mesh, &subs[s], i,
								matIndex, useArmature, &o, mat, gpuSkin)) {
					unload_part(&m->parts[m->partCount]);
					YbrFreeSubmeshes(subs, subCount);
					YbrModelUnload(m);
					return NULL;
				}
				m->partCount++;
			}
			YbrFreeSubmeshes(subs, subCount);
		}
	}

	// ノード
	{
		NodeGather g;
		g.model = m;
		g.cap = 0;
		g.failed = 0;
		for (int i = 0; i < scene->rootCount; i++)
			gather_nodes(&g, &scene->roots[i], MatrixIdentity());
		if (g.failed) {
			YbrModelUnload(m);
			return NULL;
		}
	}

	// 　 AABB の前計算（ここで 1 回だけ）
	if (!YbrModelBuildRestBounds(m)) {
		YbrModelUnload(m);
		return NULL;
	}

	return m;
}

void YbrModelUnload(YbrModel* model)
{
	if (!model) return;
	for (int i = 0; i < model->partCount; i++) unload_part(&model->parts[i]);
	YBR_FREE(model->parts);

	if (model->materials) {
		for (int i = 0; i < model->materialCount; i++)
			if (model->materials[i].ownsShader && model->materials[i].shader.id)
				UnloadShader(model->materials[i].shader);
	}
	YbrModelUnloadTextures(model);
	YBR_FREE(model->materials);

	YBR_FREE(model->nodes);
	YBR_FREE(model->meshes);
	YBR_FREE(model);
}

// ----------------------------------------------------------------------------
// モデルの情報

int YbrModelFindNode(const YbrModel* model, const char* name)
{
	if (!model || !name) return -1;
	for (int i = 0; i < model->nodeCount; i++)
		if (YbrStrEq(model->nodes[i].name, name)) return i;
	return -1;
}

const YbrNode* YbrModelGetNodeSource(const YbrModel* model, int node)
{
	if (!model || node < 0 || model->nodeCount <= node) return NULL;
	return model->nodes[node].source;
}

int YbrModelGetLocalBounds(const YbrModel* model, Vector3* outMin,
						   Vector3* outMax)
{
	if (!model || !model->hasBounds) return 0;
	if (outMin) *outMin = model->localMin;
	if (outMax) *outMax = model->localMax;
	return 1;
}

int YbrModelHasTransparent(const YbrModel* model)
{
	return model ? model->hasTransparent : 0;
}

int YbrModelIsPartTransparent(const YbrModel* model, int part)
{
	if (!model || part < 0 || model->partCount <= part) return 0;
	const YbrModelPart* p = &model->parts[part];
	if (p->materialIndex < 0 || model->materialCount <= p->materialIndex)
		return 0;
	return model->materials[p->materialIndex].transparent;
}

const YbrArmature* YbrModelGetArmature(const YbrModel* model)
{
	return model ? model->armature : NULL;
}

// レスト姿勢の AABB を求める。
int YbrModelBuildRestBounds(YbrModel* m)
{
	if (!m) return 0;

	Vector3 lo = {1e30f, 1e30f, 1e30f};
	Vector3 hi = {-1e30f, -1e30f, -1e30f};
	Vector3 slo = lo, shi = hi;

	for (int i = 0; i < m->nodeCount; i++) {
		const YbrModelNode* node = &m->nodes[i];
		for (int k = 0; k < m->partCount; k++) {
			const YbrModelPart* p = &m->parts[k];
			if (p->meshIndex != node->meshIndex || p->vertexCount <= 0)
				continue;
			for (int c = 0; c < 8; c++) {
				Vector3 q;
				q.x = (c & 1) ? p->localMax.x : p->localMin.x;
				q.y = (c & 2) ? p->localMax.y : p->localMin.y;
				q.z = (c & 4) ? p->localMax.z : p->localMin.z;
				q = Vector3Transform(q, node->transform);
				lo = Vector3Min(lo, q);
				hi = Vector3Max(hi, q);
				if (!p->skinned) {
					slo = Vector3Min(slo, q);
					shi = Vector3Max(shi, q);
					m->hasStatic = 1;
				}
			}
			m->hasBounds = 1;
		}
	}
	m->localMin = lo;
	m->localMax = hi;
	m->staticMin = slo;
	m->staticMax = shi;
	return 1;
}

// YbrSkinBounds : ポーズ追従 AABB のための前計算
YbrSkinBounds* YbrSkinBoundsCreate(const YbrModel* m)
{
	if (!m || !m->armature || m->armature->boneCount <= 0) return NULL;

	// スキンを持つパートが無ければ、前計算しても意味が無い
	int anySkinned = 0;
	for (int k = 0; k < m->partCount && !anySkinned; k++)
		if (m->parts[k].skinned) anySkinned = 1;
	if (!anySkinned) return NULL;

	YbrSkinBounds* b = (YbrSkinBounds*)YBR_CALLOC(1, sizeof(YbrSkinBounds));
	if (!b) return NULL;
	b->model = m;

	const YbrArmature* arm = m->armature;
	int total = arm->boneCount;
	b->boneCount = total;

	b->boneMin = (Vector3*)YBR_MALLOC((size_t)total * sizeof(Vector3));
	b->boneMax = (Vector3*)YBR_MALLOC((size_t)total * sizeof(Vector3));
	b->boneValid = (unsigned char*)YBR_CALLOC((size_t)total, 1);
	if (!b->boneMin || !b->boneMax || !b->boneValid) {
		YbrSkinBoundsUnload(b);
		return NULL;
	}

	// 頂点を 1 度だけなめて、各ボーンの rest 空間での AABB を作る。
	// rest の逆行列は YbrPose を作らずにここで組み立てる。
	Matrix* invRest = (Matrix*)YBR_MALLOC((size_t)total * sizeof(Matrix));
	if (!invRest) {
		YbrSkinBoundsUnload(b);
		return NULL;
	}
	for (int i = 0; i < total; i++)
		invRest[i] = MatrixInvert(arm->bones[i].rest);

	for (int k = 0; k < m->partCount; k++) {
		const YbrModelPart* p = &m->parts[k];
		if (!p->skinned) continue;

		for (int v = 0; v < p->vertexCount; v++) {
			Vector3 pos = {p->positions[v * 3 + 0], p->positions[v * 3 + 1],
						   p->positions[v * 3 + 2]};
			for (int w = 0; w < 4; w++) {
				if (p->boneWeights[v * 4 + w] <= 0.0f) continue;
				int bi = (int)p->boneIds[v * 4 + w];
				if (bi < 0 || arm->boneCount <= bi) continue;

				// ボーンの rest 空間へ持っていく
				Vector3 q = Vector3Transform(pos, invRest[bi]);
				if (!b->boneValid[bi]) {
					b->boneMin[bi] = q;
					b->boneMax[bi] = q;
					b->boneValid[bi] = 1;
				}
				else {
					b->boneMin[bi] = Vector3Min(b->boneMin[bi], q);
					b->boneMax[bi] = Vector3Max(b->boneMax[bi], q);
				}
			}
		}
	}
	YBR_FREE(invRest);
	return b;
}

void YbrSkinBoundsUnload(YbrSkinBounds* b)
{
	if (!b) return;
	YBR_FREE(b->boneMin);
	YBR_FREE(b->boneMax);
	YBR_FREE(b->boneValid);
	YBR_FREE(b);
}

// ----------------------------------------------------------------------------
// 描画

static void bind_part_buffers(const YbrModelPart* p, const int* locs)
{
	// VAO が使えない環境 (GL 1.1 / VAO 非対応の ES2) 向けの手動バインド
	if (locs[RL_SHADER_LOC_VERTEX_POSITION] != -1) {
		rlEnableVertexBuffer(p->vboId[0]);
		rlSetVertexAttribute(locs[RL_SHADER_LOC_VERTEX_POSITION], 3, RL_FLOAT,
							 0, 0, 0);
		rlEnableVertexAttribute(locs[RL_SHADER_LOC_VERTEX_POSITION]);
	}
	if (p->texcoords && p->vboId[1] &&
		locs[RL_SHADER_LOC_VERTEX_TEXCOORD01] != -1) {
		rlEnableVertexBuffer(p->vboId[1]);
		rlSetVertexAttribute(locs[RL_SHADER_LOC_VERTEX_TEXCOORD01], 2, RL_FLOAT,
							 0, 0, 0);
		rlEnableVertexAttribute(locs[RL_SHADER_LOC_VERTEX_TEXCOORD01]);
	}
	if (p->normals && p->vboId[2] && locs[RL_SHADER_LOC_VERTEX_NORMAL] != -1) {
		rlEnableVertexBuffer(p->vboId[2]);
		rlSetVertexAttribute(locs[RL_SHADER_LOC_VERTEX_NORMAL], 3, RL_FLOAT, 0,
							 0, 0);
		rlEnableVertexAttribute(locs[RL_SHADER_LOC_VERTEX_NORMAL]);
	}
	if (p->colors && p->vboId[3] && locs[RL_SHADER_LOC_VERTEX_COLOR] != -1) {
		rlEnableVertexBuffer(p->vboId[3]);
		rlSetVertexAttribute(locs[RL_SHADER_LOC_VERTEX_COLOR], 4,
							 RL_UNSIGNED_BYTE, 1, 0, 0);
		rlEnableVertexAttribute(locs[RL_SHADER_LOC_VERTEX_COLOR]);
	}
	if (p->tangents && p->vboId[6] && locs[RL_SHADER_LOC_VERTEX_TANGENT] != -1) {
		rlEnableVertexBuffer(p->vboId[6]);
		rlSetVertexAttribute(locs[RL_SHADER_LOC_VERTEX_TANGENT], 4, RL_FLOAT, 0,
							 0, 0);
		rlEnableVertexAttribute(locs[RL_SHADER_LOC_VERTEX_TANGENT]);
	}
	rlEnableVertexBufferElement(p->vboId[7]);
}

// GPU スキニング用にボーン行列を vec4 の並びで送る
static void upload_bones(YbrModelInstance* inst, const YbrModelMaterial* mat,
						 const YbrPose* pose)
{
	if (mat->locBoneMatrices < 0) return;
	int n = pose->boneCount;
	if (mat->maxBones < n) n = mat->maxBones;
	if (n <= 0) return;

	void* nb = YbrGrowBuffer(inst->boneUpload, &inst->boneUploadCap, n * 16,
							 sizeof(float));
	if (!nb) return;
	inst->boneUpload = (float*)nb;

	for (int i = 0; i < n; i++) {
		const Matrix* s = &pose->bones[i].skin;
		float* d = inst->boneUpload + (size_t)i * 16;
		// raylib の Matrix は列基底なので、そのまま列ごとの vec4 になる
		d[0] = s->m0;
		d[1] = s->m1;
		d[2] = s->m2;
		d[3] = s->m3;
		d[4] = s->m4;
		d[5] = s->m5;
		d[6] = s->m6;
		d[7] = s->m7;
		d[8] = s->m8;
		d[9] = s->m9;
		d[10] = s->m10;
		d[11] = s->m11;
		d[12] = s->m12;
		d[13] = s->m13;
		d[14] = s->m14;
		d[15] = s->m15;
	}
	rlSetUniform(mat->locBoneMatrices, inst->boneUpload, RL_SHADER_UNIFORM_VEC4,
				 n * 4);
}

// mat は「このパートを描くのに使うマテリアル」。
// ノードのマテリアル差し替えは呼び出し側で解決済み。
static void draw_part(YbrModelInstance* inst, const YbrModelMaterial* mat,
					  const YbrModelPart* p, int partIndex, Matrix transform,
					  const YbrWorld* world, const float tint[4])
{
	const YbrModel* model = inst->model;

	unsigned int shaderId =
		(mat && mat->shader.id) ? mat->shader.id : rlGetShaderIdDefault();
	int* locs = (mat && mat->shader.id && mat->shader.locs)
					? mat->shader.locs
					: rlGetShaderLocsDefault();
	rlEnableShader(shaderId);

	// 色
	float color[4] = {tint[0], tint[1], tint[2], tint[3]};
	if (mat) {
		color[0] *= mat->baseColor.x;
		color[1] *= mat->baseColor.y;
		color[2] *= mat->baseColor.z;
		color[3] *= mat->baseColor.w;
	}
	if (locs[RL_SHADER_LOC_COLOR_DIFFUSE] != -1)
		rlSetUniform(locs[RL_SHADER_LOC_COLOR_DIFFUSE], color,
					 RL_SHADER_UNIFORM_VEC4, 1);

	// 行列
	Matrix matModel = MatrixMultiply(transform, rlGetMatrixTransform());
	Matrix matView = rlGetMatrixModelview();
	Matrix matProjection = rlGetMatrixProjection();

	if (locs[RL_SHADER_LOC_MATRIX_MODEL] != -1)
		rlSetUniformMatrix(locs[RL_SHADER_LOC_MATRIX_MODEL], matModel);
	if (locs[RL_SHADER_LOC_MATRIX_NORMAL] != -1)
		rlSetUniformMatrix(locs[RL_SHADER_LOC_MATRIX_NORMAL],
						   MatrixTranspose(MatrixInvert(matModel)));
	if (locs[RL_SHADER_LOC_MATRIX_VIEW] != -1)
		rlSetUniformMatrix(locs[RL_SHADER_LOC_MATRIX_VIEW], matView);
	if (locs[RL_SHADER_LOC_MATRIX_PROJECTION] != -1)
		rlSetUniformMatrix(locs[RL_SHADER_LOC_MATRIX_PROJECTION],
						   matProjection);

	Matrix matMVP =
		MatrixMultiply(MatrixMultiply(matModel, matView), matProjection);
	if (locs[RL_SHADER_LOC_MATRIX_MVP] != -1)
		rlSetUniformMatrix(locs[RL_SHADER_LOC_MATRIX_MVP], matMVP);

	// ライト / 視点
	if (mat && mat->shader.id && world) {
		// world には灯をいくつ置いてもよいので、この体に効く順で選ぶ
		int pick[YBR_WORLD_MAX_ACTIVE_LIGHTS];
		int picked =
			YbrWorldPickLights(world, inst, pick, YBR_WORLD_MAX_ACTIVE_LIGHTS);

		// 影を焼いた灯は、深度テクスチャと同じ枠 (先頭) に来ていないと
		// シェーダー側で対応が取れないので、前へ寄せる。
		for (int s = 0; s < world->shadowCount && s < picked; s++) {
			int li = world->shadows[s].lightIndex;
			for (int k = s; k < picked; k++) {
				if (pick[k] != li) continue;
				int t = pick[s];
				pick[s] = pick[k];
				pick[k] = t;
				break;
			}
		}

		for (int i = 0; i < YBR_WORLD_MAX_ACTIVE_LIGHTS; i++) {
			// 選ばれなかった枠は消えている扱い (色 0 で送る)
			YbrWorldLight off;
			const YbrWorldLight* li;
			if (i < picked) {
				li = &world->lights[pick[i]];
			}
			else {
				memset(&off, 0, sizeof(off));
				off.direction = (Vector3){0.0f, -1.0f, 0.0f};
				off.intensity = 1.0f;
				li = &off;
			}
			if (0 <= mat->locLightDir[i]) {
				float v[3] = {li->direction.x, li->direction.y,
							  li->direction.z};
				rlSetUniform(mat->locLightDir[i], v, RL_SHADER_UNIFORM_VEC3, 1);
			}
			if (0 <= mat->locLightColor[i]) {
				// 強さは色に畳んで送る (点光源は 1 を超えることがある)
				float e = (0.0f < li->intensity) ? li->intensity : 1.0f;
				float c[4] = {li->color.x * e, li->color.y * e, li->color.z * e,
							  li->color.w};
				rlSetUniform(mat->locLightColor[i], c, RL_SHADER_UNIFORM_VEC4,
							 1);
			}
			if (0 <= mat->locLightPos[i]) {
				float v[3] = {li->position.x, li->position.y, li->position.z};
				rlSetUniform(mat->locLightPos[i], v, RL_SHADER_UNIFORM_VEC3, 1);
			}
			if (0 <= mat->locLightParams[i]) {
				// (種類, 届く距離, 内側コーンの cos, 外側コーンの cos)
				float par[4] = {(float)li->kind, li->range, 1.0f, 0.0f};
				if (li->kind == YBR_LIGHTKIND_SPOT) {
					par[2] = cosf(li->spotInner);
					par[3] = cosf(li->spotOuter);
				}
				rlSetUniform(mat->locLightParams[i], par,
							 RL_SHADER_UNIFORM_VEC4, 1);
			}
		}
		// シャドウマップ
		for (int s = 0; s < YBR_WORLD_MAX_SHADOWS; s++) {
			const YbrShadowMap* sm = &world->shadows[s];
			// 焼いてあって、かつその灯がこの枠に入っているときだけ効かせる
			int on = (s < world->shadowCount && sm->ready && s < picked &&
					  pick[s] == sm->lightIndex);

			if (0 <= mat->locShadowParams[s]) {
				float par[4] = {on ? 1.0f : 0.0f, world->options.shadowBias,
								on ? 1.0f / (float)sm->resolution : 0.0f, 0.0f};
				rlSetUniform(mat->locShadowParams[s], par,
							 RL_SHADER_UNIFORM_VEC4, 1);
			}
			if (!on) continue;

			if (0 <= mat->locLightVP[s])
				rlSetUniformMatrix(mat->locLightVP[s], sm->lightVP);

			if (0 <= mat->locShadowMap[s]) {
				// マテリアルのテクスチャの後ろの枠を使う (取り合いを避ける)
				int slot = mat->textureCount + s;
				rlActiveTextureSlot(slot);
				rlEnableTexture(sm->depth.id);
				rlSetUniform(mat->locShadowMap[s], &slot, RL_SHADER_UNIFORM_INT,
							 1);
			}
		}

		if (0 <= mat->locAmbient)
			rlSetUniform(mat->locAmbient, &world->ambientColor,
						 RL_SHADER_UNIFORM_VEC4, 1);
	}

	// 視点はライトと関係なく要る (フレネルなどが使う)
	if (mat && mat->shader.id) {
		if (0 <= mat->locViewPos) {
			// world にカメラが入っていればそれを使う。
			// 無ければビュー行列の逆行列から取り出す。
			Vector3 eye;
			if (world && world->hasCamera) {
				eye = world->camera.position;
			}
			else {
				Matrix inv = MatrixInvert(matView);
				eye = (Vector3){inv.m12, inv.m13, inv.m14};
			}
			float v[3] = {eye.x, eye.y, eye.z};
			rlSetUniform(mat->locViewPos, v, RL_SHADER_UNIFORM_VEC3, 1);
		}

		// ボーン
		if (mat->skinning) {
			if (p->gpuSkin && inst->hasPose) {
				upload_bones(inst, mat, &inst->pose);
			}
			else if (0 <= mat->attribBoneWeights) {
				// スキンを持たないパートは、ウェイト 0 で素通しさせる
				float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
				rlSetVertexAttributeDefault(mat->attribBoneWeights, zero,
											RL_SHADER_ATTRIB_VEC4, 4);
				rlDisableVertexAttribute((unsigned int)mat->attribBoneWeights);
			}
		}
	}

	// CPU スキニングの結果を流し込む
	if (p->dynamicBuffers && inst->animPositions &&
		inst->animPositions[partIndex]) {
		rlUpdateVertexBuffer(p->vboId[0], inst->animPositions[partIndex],
							 p->vertexCount * 3 * (int)sizeof(float), 0);
		if (inst->animNormals && inst->animNormals[partIndex] && p->vboId[2])
			rlUpdateVertexBuffer(p->vboId[2], inst->animNormals[partIndex],
								 p->vertexCount * 3 * (int)sizeof(float), 0);
	}

	// テクスチャ
	// 読み込めなかった枠には raylib の既定テクスチャ (白 1x1) を結ぶ。
	// ここを飛ばすと sampler が 0 番ユニットを読み、直前に別のものが
	// 束ねられていたときにそれが出てしまう。
	int bound = 0;
	if (mat) {
		for (int i = 0; i < mat->textureCount; i++) {
			const YbrModelTextureSlot* ts = &mat->textures[i];
			if (ts->loc < 0) continue;
			unsigned int id =
				ts->texture.id ? ts->texture.id : rlGetTextureIdDefault();
			rlActiveTextureSlot(ts->slot);
			rlEnableTexture(id);
			rlSetUniform(ts->loc, &ts->slot, RL_SHADER_UNIFORM_INT, 1);
			bound++;
		}
	}
	if (bound == 0) {
		rlActiveTextureSlot(0);
		rlEnableTexture(rlGetTextureIdDefault());
		if (locs[RL_SHADER_LOC_MAP_ALBEDO] != -1) {
			int slot = 0;
			rlSetUniform(locs[RL_SHADER_LOC_MAP_ALBEDO], &slot,
						 RL_SHADER_UNIFORM_INT, 1);
		}
	}

	// 頂点属性
	if (!p->tangents && locs[RL_SHADER_LOC_VERTEX_TANGENT] != -1) {
		// 接線が無いメッシュは 0 を入れて、シェーダー側で近似に落とす
		float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		rlSetVertexAttributeDefault(locs[RL_SHADER_LOC_VERTEX_TANGENT], zero,
									RL_SHADER_ATTRIB_VEC4, 4);
		rlDisableVertexAttribute(locs[RL_SHADER_LOC_VERTEX_TANGENT]);
	}
	if (!p->colors && locs[RL_SHADER_LOC_VERTEX_COLOR] != -1) {
		float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		rlSetVertexAttributeDefault(locs[RL_SHADER_LOC_VERTEX_COLOR], white,
									RL_SHADER_ATTRIB_VEC4, 4);
		rlDisableVertexAttribute(locs[RL_SHADER_LOC_VERTEX_COLOR]);
	}

	if (!rlEnableVertexArray(p->vaoId)) bind_part_buffers(p, locs);

	rlDrawVertexArrayElements(0, p->triangleCount * 3, 0);

	rlDisableVertexArray();
	rlDisableVertexBuffer();
	rlDisableVertexBufferElement();

	// 影の枠も外す（マテリアルのテクスチャの後ろに束ねている）
	if (mat && world && 0 < world->shadowCount) {
		for (int s = world->shadowCount - 1; 0 <= s; s--) {
			rlActiveTextureSlot(mat->textureCount + s);
			rlDisableTexture();
		}
	}
	if (mat && 0 < bound) {
		for (int i = mat->textureCount - 1; 0 <= i; i--) {
			if (mat->textures[i].loc < 0) continue;
			rlActiveTextureSlot(mat->textures[i].slot);
			rlDisableTexture();
		}
	}
	rlActiveTextureSlot(0);
	rlDisableTexture();
	rlDisableShader();
	(void)model;
}

// 半透明を奥から描くための1件ぶん
struct YbrDrawItem {
	const YbrModelPart* part;
	int partIndex;
	const YbrModelMaterial* material;
	Matrix world;
	float depth;  // カメラからの距離の2乗
};

const YbrModelMaterial* YbrModelResolveMaterial(const YbrModelInstance* inst,
												int nodeIndex,
												const YbrModelPart* p)
{
	const YbrModel* m = inst->model;
	if (inst->nodeMaterial && 0 <= nodeIndex && inst->nodeMaterial[nodeIndex])
		return inst->nodeMaterial[nodeIndex];
	if (p->materialIndex < 0 || m->materialCount <= p->materialIndex)
		return NULL;
	return &m->materials[p->materialIndex];
}

void YbrModelDrawPart(YbrModelInstance* inst, const YbrModelMaterial* mat,
					  const YbrModelPart* p, int partIndex, Matrix transform,
					  const YbrWorld* world, const float tint[4])
{
	if (mat && !mat->backfaceCulling) rlDisableBackfaceCulling();
	draw_part(inst, mat, p, partIndex, transform, world, tint);
	if (mat && !mat->backfaceCulling) rlEnableBackfaceCulling();
}

// 遠い順（depth の降順）
static int cmp_draw_item(const void* a, const void* b)
{
	float da = ((const struct YbrDrawItem*)a)->depth;
	float db = ((const struct YbrDrawItem*)b)->depth;
	if (da < db) return 1;
	if (db < da) return -1;
	return 0;
}

// スキンを持つパートはポーズで動くので、この箱では判定できない。
void YbrModelPartWorldBox(const YbrModelPart* p, Matrix world, Vector3* outMin,
						  Vector3* outMax)
{
	Vector3 lo = {1e30f, 1e30f, 1e30f};
	Vector3 hi = {-1e30f, -1e30f, -1e30f};
	for (int c = 0; c < 8; c++) {
		Vector3 q;
		q.x = (c & 1) ? p->localMax.x : p->localMin.x;
		q.y = (c & 2) ? p->localMax.y : p->localMin.y;
		q.z = (c & 4) ? p->localMax.z : p->localMin.z;
		q = Vector3Transform(q, world);
		lo = Vector3Min(lo, q);
		hi = Vector3Max(hi, q);
	}
	*outMin = lo;
	*outMax = hi;
}

static void draw_instance(YbrModelInstance* inst, const YbrWorld* worldEnv,
						  const float tint[4], unsigned int pass,
						  const YbrFrustum* frustum)
{
	if (!inst || !inst->model || (pass & YBR_DRAW_ALL) == 0) return;
	const YbrModel* model = inst->model;

	int wantOpaque = (pass & YBR_DRAW_OPAQUE) != 0;
	int wantAlpha = (pass & YBR_DRAW_TRANSPARENT) != 0;

	// カメラ位置はビュー行列の逆行列から取る
	Matrix inv = MatrixInvert(rlGetMatrixModelview());
	Vector3 eye = {inv.m12, inv.m13, inv.m14};

	int count = 0; // あとで奥から描く半透明の件数

	for (int i = 0; i < model->nodeCount; i++) {
		if (inst->nodeVisible && !inst->nodeVisible[i]) continue;
		const YbrModelNode* node = &model->nodes[i];
		Matrix world = MatrixMultiply(node->transform, inst->transform);

		for (int k = 0; k < model->partCount; k++) {
			const YbrModelPart* p = &model->parts[k];
			if (p->meshIndex != node->meshIndex) continue;
			if (inst->partVisible && !inst->partVisible[k]) continue;

			// パート単位のカリングは「ポーズで動かないもの」だけ。
			if (frustum && !p->skinned) {
				Vector3 lo, hi;
				YbrModelPartWorldBox(p, world, &lo, &hi);
				if (!YbrFrustumContainsBox(frustum, lo, hi)) continue;
			}

			const YbrModelMaterial* mat = YbrModelResolveMaterial(inst, i, p);
			int alpha = model->hasTransparent && mat && mat->transparent;
			if (alpha ? !wantAlpha : !wantOpaque) continue;

			if (alpha) {
				void* nb = YbrGrowBuffer(inst->drawOrder, &inst->drawOrderCap,
										 count + 1, sizeof(struct YbrDrawItem));
				if (nb) {
					inst->drawOrder = (struct YbrDrawItem*)nb;
					Vector3 c = Vector3Transform(
						Vector3Scale(Vector3Add(p->localMin, p->localMax),
									 0.5f),
						world);
					inst->drawOrder[count].part = p;
					inst->drawOrder[count].partIndex = k;
					inst->drawOrder[count].material = mat;
					inst->drawOrder[count].world = world;
					inst->drawOrder[count].depth = Vector3DistanceSqr(c, eye);
					count++;
					continue;
				}
				// 確保できなければ並べ替えを諦めてその場で描く
			}
			YbrModelDrawPart(inst, mat, p, k, world, worldEnv, tint);
		}
	}

	if (count <= 0) return;

	if (1 < count)
		qsort(inst->drawOrder, (size_t)count, sizeof(struct YbrDrawItem),
			  cmp_draw_item);

	// 半透明どうしが深度で消し合わないよう、深度書き込みだけ止める
	rlDrawRenderBatchActive();
	rlDisableDepthMask();
	for (int i = 0; i < count; i++)
		YbrModelDrawPart(inst, inst->drawOrder[i].material,
						 inst->drawOrder[i].part, inst->drawOrder[i].partIndex,
						 inst->drawOrder[i].world, worldEnv, tint);
	rlDrawRenderBatchActive();
	rlEnableDepthMask();
}

void YbrModelInstanceDrawPass(const YbrModelInstance* inst,
							  const YbrWorld* world, Color tint,
							  unsigned int pass)
{
	float c[4] = {tint.r / 255.0f, tint.g / 255.0f, tint.b / 255.0f,
				  tint.a / 255.0f};
	draw_instance((YbrModelInstance*)inst, world, c, pass, NULL);
}

void YbrModelInstanceDrawWiresPass(const YbrModelInstance* inst,
								   const YbrWorld* world, Color tint,
								   unsigned int pass)
{
	float c[4] = {tint.r / 255.0f, tint.g / 255.0f, tint.b / 255.0f,
				  tint.a / 255.0f};
	rlEnableWireMode();
	draw_instance((YbrModelInstance*)inst, world, c, pass, NULL);
	rlDisableWireMode();
}

int YbrModelInstanceDrawPassCulled(const YbrModelInstance* inst,
								   const YbrWorld* world, Color tint,
								   unsigned int pass)
{
	if (!inst) return 0;
	const YbrFrustum* frustum = YbrWorldGetFrustum(world);
	// まず体ごと判定する。外れていれば 1 パートも触らない
	if (!YbrModelInstanceIsVisible(inst, frustum)) return 0;

	float c[4] = {tint.r / 255.0f, tint.g / 255.0f, tint.b / 255.0f,
				  tint.a / 255.0f};
	draw_instance((YbrModelInstance*)inst, world, c, pass, frustum);
	return 1;
}

int YbrModelInstanceDrawCulled(const YbrModelInstance* inst,
							   const YbrWorld* world, Color tint)
{
	return YbrModelInstanceDrawPassCulled(inst, world, tint, YBR_DRAW_ALL);
}

void YbrModelInstanceDraw(const YbrModelInstance* inst, const YbrWorld* world,
						  Color tint)
{
	YbrModelInstanceDrawPass(inst, world, tint, YBR_DRAW_ALL);
}

void YbrModelInstanceDrawWires(const YbrModelInstance* inst,
							   const YbrWorld* world, Color tint)
{
	YbrModelInstanceDrawWiresPass(inst, world, tint, YBR_DRAW_ALL);
}

YbrModelMaterial YbrModelMakeMaterial(Shader shader, Texture2D texture,
									  Color baseColor, int transparent)
{
	YbrModelMaterial m;
	memset(&m, 0, sizeof(m));
	m.id = NULL;
	m.baseColor = (Vector4){baseColor.r / 255.0f, baseColor.g / 255.0f,
							baseColor.b / 255.0f, baseColor.a / 255.0f};
	m.texture = texture;
	m.backfaceCulling = 1;
	m.transparent = transparent ? 1 : 0;
	m.shader = shader;
	m.ownsShader = 0; // 呼び出し側の持ち物
	m.maxBones = YBR_SHADER_MAX_BONES;

	m.locBoneMatrices = -1;
	m.attribBoneIds = -1;
	m.attribBoneWeights = -1;
	m.locAmbient = -1;
	m.locViewPos = -1;
	for (int i = 0; i < YBR_WORLD_MAX_ACTIVE_LIGHTS; i++) {
		m.locLightDir[i] = -1;
		m.locLightColor[i] = -1;
		m.locLightPos[i] = -1;
		m.locLightParams[i] = -1;
	}
	for (int i = 0; i < YBR_WORLD_MAX_SHADOWS; i++) {
		m.locShadowMap[i] = -1;
		m.locLightVP[i] = -1;
		m.locShadowParams[i] = -1;
	}
	if (shader.id == 0) return m;

	if (texture.id != 0) {
		m.textures[0].texture = texture;
		m.textures[0].loc = GetShaderLocation(shader, "texture0");
		m.textures[0].slot = 0;
		m.textureCount = 1;
	}

	for (int i = 0; i < YBR_WORLD_MAX_ACTIVE_LIGHTS; i++) {
		char nd[32], nc[32], np[32], na[32];
		snprintf(nd, sizeof(nd), "%s%d", YBR_SHADER_UNIFORM_LIGHT_DIR, i);
		snprintf(nc, sizeof(nc), "%s%d", YBR_SHADER_UNIFORM_LIGHT_COLOR, i);
		snprintf(np, sizeof(np), "%s%d", YBR_SHADER_UNIFORM_LIGHT_POS, i);
		snprintf(na, sizeof(na), "%s%d", YBR_SHADER_UNIFORM_LIGHT_PARAMS, i);
		m.locLightDir[i] = GetShaderLocation(shader, nd);
		m.locLightColor[i] = GetShaderLocation(shader, nc);
		m.locLightPos[i] = GetShaderLocation(shader, np);
		m.locLightParams[i] = GetShaderLocation(shader, na);
	}
	m.locAmbient = GetShaderLocation(shader, YBR_SHADER_UNIFORM_AMBIENT);
	m.locViewPos = GetShaderLocation(shader, YBR_SHADER_UNIFORM_VIEW_POS);
	m.locBoneMatrices = GetShaderLocation(shader, YBR_SHADER_UNIFORM_BONES);

	m.attribBoneIds =
		GetShaderLocationAttrib(shader, YBR_SHADER_ATTRIB_BONE_IDS);
	m.attribBoneWeights =
		GetShaderLocationAttrib(shader, YBR_SHADER_ATTRIB_BONE_WEIGHTS);
	m.skinning = (0 <= m.locBoneMatrices && 0 <= m.attribBoneIds &&
				  0 <= m.attribBoneWeights);
	return m;
}

void YbrModelSetShader(YbrModel* model, int materialIndex, Shader shader)
{
	if (!model || !model->materials) return;
	int from = (materialIndex < 0) ? 0 : materialIndex;
	int to = (materialIndex < 0) ? model->materialCount : materialIndex + 1;
	if (from < 0 || model->materialCount < to) return;

	for (int i = from; i < to; i++) {
		YbrModelMaterial* mat = &model->materials[i];
		if (mat->ownsShader && mat->shader.id) UnloadShader(mat->shader);
		material_reset_shader(mat);
		mat->shader = shader;
		// 差し替え後のシェーダーは呼び出し側の持ち物
		mat->ownsShader = 0;
	}
}

