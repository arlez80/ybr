/*
	Yui Blender to Raylib - モデルインスタンス
		Programed by あるる（きのもと 結衣）
*/
#include <math.h>
#include <string.h>

#include "ybr_internal.h"
#include "ybr_model.h"

// CPU スキニング

// CPU スキニングの計算だけ行う (GPU への転送は描画時)。
// srcPos / srcNrm は入力の頂点。
static void skin_part_src(const YbrModelPart* p, const YbrPose* pose,
						  const float* srcPos, const float* srcNrm,
						  float* dstPos, float* dstNrm)
{
	const int nv = p->vertexCount;
	for (int i = 0; i < nv; i++) {
		Vector3 pos = {srcPos[i * 3 + 0], srcPos[i * 3 + 1], srcPos[i * 3 + 2]};
		Vector3 acc = {0.0f, 0.0f, 0.0f};
		Vector3 nrm = {0.0f, 0.0f, 0.0f};
		Vector3 srcN = {0.0f, 0.0f, 0.0f};
		if (srcNrm) {
			srcN.x = srcNrm[i * 3 + 0];
			srcN.y = srcNrm[i * 3 + 1];
			srcN.z = srcNrm[i * 3 + 2];
		}
		float total = 0.0f;

		for (int k = 0; k < 4; k++) {
			float w = p->boneWeights[i * 4 + k];
			if (w <= 0.0f) continue;
			int b = (int)p->boneIds[i * 4 + k];
			if (b < 0 || pose->boneCount <= b) continue;
			Matrix sk = pose->bones[b].skin;

			acc = Vector3Add(acc, Vector3Scale(Vector3Transform(pos, sk), w));
			if (dstNrm) {
				// 法線は平行移動を無視する
				Vector3 n;
				n.x = sk.m0 * srcN.x + sk.m4 * srcN.y + sk.m8 * srcN.z;
				n.y = sk.m1 * srcN.x + sk.m5 * srcN.y + sk.m9 * srcN.z;
				n.z = sk.m2 * srcN.x + sk.m6 * srcN.y + sk.m10 * srcN.z;
				nrm = Vector3Add(nrm, Vector3Scale(n, w));
			}
			total += w;
		}

		if (total <= 1e-6f) {
			acc = pos;
			nrm = srcN;
		}
		dstPos[i * 3 + 0] = acc.x;
		dstPos[i * 3 + 1] = acc.y;
		dstPos[i * 3 + 2] = acc.z;
		if (dstNrm) {
			nrm = Vector3Normalize(nrm);
			dstNrm[i * 3 + 0] = nrm.x;
			dstNrm[i * 3 + 1] = nrm.y;
			dstNrm[i * 3 + 2] = nrm.z;
		}
	}
}

void YbrModelSkinPart(const YbrModelPart* p, const YbrPose* pose, float* dstPos,
					  float* dstNrm)
{
	if (!p || !pose || !dstPos) return;
	skin_part_src(p, pose, p->positions, p->normals, dstPos, dstNrm);
}

// ----------------------------------------------------------------------------
// インスタンス

// YbrModelInstanceを作成する
YbrModelInstance* YbrModelInstanceCreate(const YbrModel* model)
{
	if (!model) return NULL;

	YbrModelInstance* inst =
		(YbrModelInstance*)YBR_CALLOC(1, sizeof(YbrModelInstance));
	if (!inst) return NULL;
	inst->model = model;
	inst->transform = MatrixIdentity();

	// アーマチュアがある場合
	if (model->armature) {
		if (!YbrPoseInit(&inst->pose, model->armature)) {
			YbrModelInstanceUnload(inst);
			return NULL;
		}
		inst->hasPose = 1;
	}

	// 表示フラグ
	if (0 < model->partCount) {
		inst->partVisible =
			(unsigned char*)YBR_MALLOC((size_t)model->partCount);
		if (!inst->partVisible) {
			YbrModelInstanceUnload(inst);
			return NULL;
		}
		memset(inst->partVisible, 1, (size_t)model->partCount);
	}
	if (0 < model->nodeCount) {
		inst->nodeVisible =
			(unsigned char*)YBR_MALLOC((size_t)model->nodeCount);
		inst->nodeMaterial = (const YbrModelMaterial**)YBR_CALLOC(
			(size_t)model->nodeCount, sizeof(YbrModelMaterial*));
		if (!inst->nodeVisible || !inst->nodeMaterial) {
			YbrModelInstanceUnload(inst);
			return NULL;
		}
		memset(inst->nodeVisible, 1, (size_t)model->nodeCount);
	}

	// CPUスキニング用のバッファ
	if (0 < model->partCount) {
		inst->animPositions =
			(float**)YBR_CALLOC((size_t)model->partCount, sizeof(float*));
		inst->animNormals =
			(float**)YBR_CALLOC((size_t)model->partCount, sizeof(float*));
		if (!inst->animPositions || !inst->animNormals) {
			YbrModelInstanceUnload(inst);
			return NULL;
		}
		for (int i = 0; i < model->partCount; i++) {
			const YbrModelPart* p = &model->parts[i];
			if (!p->dynamicBuffers || p->vertexCount <= 0) continue;
			inst->animPositions[i] =
				(float*)YBR_MALLOC((size_t)p->vertexCount * 3 * sizeof(float));
			if (!inst->animPositions[i]) {
				YbrModelInstanceUnload(inst);
				return NULL;
			}
			memcpy(inst->animPositions[i], p->positions,
				   (size_t)p->vertexCount * 3 * sizeof(float));
			if (p->normals) {
				inst->animNormals[i] = (float*)YBR_MALLOC(
					(size_t)p->vertexCount * 3 * sizeof(float));
				if (!inst->animNormals[i]) {
					YbrModelInstanceUnload(inst);
					return NULL;
				}
				memcpy(inst->animNormals[i], p->normals,
					   (size_t)p->vertexCount * 3 * sizeof(float));
			}
		}
	}

	YbrModelInstanceApplyPose(inst);
	return inst;
}

void YbrModelInstanceUnload(YbrModelInstance* inst)
{
	if (!inst) return;
	if (inst->hasPose) YbrPoseUnload(&inst->pose);
	YBR_FREE(inst->partVisible);
	YBR_FREE(inst->nodeVisible);
	YBR_FREE(inst->nodeMaterial);
	if (inst->animPositions && inst->model) {
		for (int i = 0; i < inst->model->partCount; i++)
			YBR_FREE(inst->animPositions[i]);
	}
	if (inst->animNormals && inst->model) {
		for (int i = 0; i < inst->model->partCount; i++)
			YBR_FREE(inst->animNormals[i]);
	}
	YBR_FREE(inst->animPositions);
	YBR_FREE(inst->animNormals);
	YBR_FREE(inst->drawOrder);
	YBR_FREE(inst->boneUpload);
	YBR_FREE(inst);
}

const YbrModel* YbrModelInstanceGetModel(const YbrModelInstance* inst)
{
	return inst ? inst->model : NULL;
}

void YbrModelInstanceSetTransform(YbrModelInstance* inst, Matrix transform)
{
	if (!inst) return;
	inst->transform = transform;
	YbrModelInstanceUpdateBounds(inst);
}

Matrix YbrModelInstanceGetTransform(const YbrModelInstance* inst)
{
	return inst ? inst->transform : MatrixIdentity();
}

YbrPose* YbrModelInstanceGetPose(YbrModelInstance* inst, const char* armatureId)
{
	if (!inst || !inst->hasPose) return NULL;
	// アーマチュアは 1 つだけ。id を渡されたときだけ念のため照合する。
	if (armatureId && !YbrStrEq(inst->model->armature->id, armatureId))
		return NULL;
	return &inst->pose;
}

int YbrModelInstanceHasPose(const YbrModelInstance* inst)
{
	return (inst && inst->hasPose) ? 1 : 0;
}

void YbrModelInstanceApplyPose(YbrModelInstance* inst)
{
	if (!inst || !inst->model) return;
	const YbrModel* m = inst->model;

	if (inst->hasPose) YbrPoseUpdate(&inst->pose);

	for (int i = 0; i < m->partCount; i++) {
		const YbrModelPart* p = &m->parts[i];
		if (!p->dynamicBuffers) continue; // GPU スキニング / 静的
		if (inst->partVisible && !inst->partVisible[i]) continue;
		if (!p->skinned || !inst->hasPose) continue;
		if (!inst->animPositions || !inst->animPositions[i]) continue;
		YbrModelSkinPart(p, &inst->pose, inst->animPositions[i],
						 inst->animNormals ? inst->animNormals[i] : NULL);
	}

	YbrModelInstanceUpdateBounds(inst);
}

// ポーズに追従するワールドAABB
void YbrModelInstanceUpdateBounds(YbrModelInstance* inst)
{
	if (!inst || !inst->model) return;
	const YbrModel* m = inst->model;

	Vector3 lo = {1e30f, 1e30f, 1e30f};
	Vector3 hi = {-1e30f, -1e30f, -1e30f};
	int any = 0;

	// スキンを持たない場合
	if (m->hasStatic) {
		for (int c = 0; c < 8; c++) {
			Vector3 q;
			q.x = (c & 1) ? m->staticMax.x : m->staticMin.x;
			q.y = (c & 2) ? m->staticMax.y : m->staticMin.y;
			q.z = (c & 4) ? m->staticMax.z : m->staticMin.z;
			q = Vector3Transform(q, inst->transform);
			lo = Vector3Min(lo, q);
			hi = Vector3Max(hi, q);
		}
		any = 1;
	}

	// ボーンごと計算
	const YbrSkinBounds* sb = inst->skinBounds;
	if (sb && sb->boneValid && inst->hasPose) {
		const YbrPose* pose = &inst->pose;
		for (int b = 0; b < pose->boneCount && b < sb->boneCount; b++) {
			if (!sb->boneValid[b]) continue;
			Matrix mat = MatrixMultiply(pose->bones[b].pose, inst->transform);
			for (int c = 0; c < 8; c++) {
				Vector3 q;
				q.x = (c & 1) ? sb->boneMax[b].x : sb->boneMin[b].x;
				q.y = (c & 2) ? sb->boneMax[b].y : sb->boneMin[b].y;
				q.z = (c & 4) ? sb->boneMax[b].z : sb->boneMin[b].z;
				q = Vector3Transform(q, mat);
				lo = Vector3Min(lo, q);
				hi = Vector3Max(hi, q);
			}
			any = 1;
		}
	}

	// ボーンごとの前計算が無いか作れなかった場合
	if (!any && m->hasBounds) {
		for (int c = 0; c < 8; c++) {
			Vector3 q;
			q.x = (c & 1) ? m->localMax.x : m->localMin.x;
			q.y = (c & 2) ? m->localMax.y : m->localMin.y;
			q.z = (c & 4) ? m->localMax.z : m->localMin.z;
			q = Vector3Transform(q, inst->transform);
			lo = Vector3Min(lo, q);
			hi = Vector3Max(hi, q);
		}
		any = 1;
	}

	inst->hasBounds = any;
	if (any) {
		inst->worldMin = lo;
		inst->worldMax = hi;
	}
}

void YbrModelInstanceSetSkinBounds(YbrModelInstance* inst,
								   const YbrSkinBounds* bounds)
{
	if (!inst) return;
	// 別のモデルの前計算を紐づけても意味が無いので弾く
	if (bounds && bounds->model != inst->model) return;
	inst->skinBounds = bounds;
	YbrModelInstanceUpdateBounds(inst);
}

const YbrSkinBounds* YbrModelInstanceGetSkinBounds(const YbrModelInstance* inst)
{
	return inst ? inst->skinBounds : NULL;
}

int YbrModelInstanceGetBounds(const YbrModelInstance* inst, Vector3* outMin,
							  Vector3* outMax)
{
	if (!inst || !inst->hasBounds) return 0;
	if (outMin) *outMin = inst->worldMin;
	if (outMax) *outMax = inst->worldMax;
	return 1;
}

// アタッチメント用
int YbrModelInstanceGetBoneWorld(const YbrModelInstance* inst,
								 const char* boneName, Matrix* out)
{
	if (!inst || !boneName || !inst->hasPose) return 0;
	int b = YbrPoseFindBone(&inst->pose, boneName);
	if (b < 0) return 0;
	// ボーンの姿勢はアーマチュア空間なので、体のワールド行列を掛ける
	if (out) *out = MatrixMultiply(inst->pose.bones[b].pose, inst->transform);
	return 1;
}

int YbrModelInstanceGetNodeWorld(const YbrModelInstance* inst,
								 const char* nodeName, Matrix* out)
{
	if (!inst || !inst->model || !nodeName) return 0;
	int i = YbrModelFindNode(inst->model, nodeName);
	if (i < 0) return 0;
	if (out)
		*out = MatrixMultiply(inst->model->nodes[i].transform, inst->transform);
	return 1;
}

void YbrModelInstanceSetUserData(YbrModelInstance* inst, void* userData)
{
	if (inst) inst->userData = userData;
}
void* YbrModelInstanceGetUserData(const YbrModelInstance* inst)
{
	return inst ? inst->userData : NULL;
}

// 表示 / 非表示
void YbrModelInstanceSetPartVisible(YbrModelInstance* inst, int part,
									int visible)
{
	if (!inst || !inst->partVisible || part < 0 ||
		inst->model->partCount <= part)
		return;
	inst->partVisible[part] = visible ? 1 : 0;
}

int YbrModelInstanceIsPartVisible(const YbrModelInstance* inst, int part)
{
	if (!inst || !inst->partVisible || part < 0 ||
		inst->model->partCount <= part)
		return 0;
	return inst->partVisible[part];
}

void YbrModelInstanceSetAllPartsVisible(YbrModelInstance* inst, int visible)
{
	if (!inst || !inst->partVisible) return;
	memset(inst->partVisible, visible ? 1 : 0, (size_t)inst->model->partCount);
}

int YbrModelInstanceSetPartVisibleByMesh(YbrModelInstance* inst,
										 const char* meshId, int visible)
{
	if (!inst || !inst->partVisible || !meshId) return 0;
	const YbrModel* m = inst->model;
	int n = 0;
	for (int i = 0; i < m->partCount; i++) {
		int mi = m->parts[i].meshIndex;
		if (mi < 0 || m->meshCount <= mi) continue;
		if (!YbrStrEq(m->meshes[mi]->id, meshId)) continue;
		inst->partVisible[i] = visible ? 1 : 0;
		n++;
	}
	return n;
}

int YbrModelInstanceSetPartVisibleByMaterial(YbrModelInstance* inst,
											 const char* materialId,
											 int visible)
{
	if (!inst || !inst->partVisible || !materialId) return 0;
	const YbrModel* m = inst->model;
	int n = 0;
	for (int i = 0; i < m->partCount; i++) {
		int mi = m->parts[i].materialIndex;
		if (mi < 0 || m->materialCount <= mi) continue;
		if (!YbrStrEq(m->materials[mi].id, materialId)) continue;
		inst->partVisible[i] = visible ? 1 : 0;
		n++;
	}
	return n;
}

void YbrModelInstanceSetNodeVisible(YbrModelInstance* inst, int node,
									int visible)
{
	if (!inst || !inst->nodeVisible || node < 0 ||
		inst->model->nodeCount <= node)
		return;
	inst->nodeVisible[node] = visible ? 1 : 0;
}

int YbrModelInstanceIsNodeVisible(const YbrModelInstance* inst, int node)
{
	if (!inst || !inst->nodeVisible || node < 0 ||
		inst->model->nodeCount <= node)
		return 0;
	return inst->nodeVisible[node];
}

void YbrModelInstanceSetAllNodesVisible(YbrModelInstance* inst, int visible)
{
	if (!inst || !inst->nodeVisible) return;
	memset(inst->nodeVisible, visible ? 1 : 0, (size_t)inst->model->nodeCount);
}

int YbrModelInstanceSetNodeVisibleByName(YbrModelInstance* inst,
										 const char* name, int visible)
{
	if (!inst || !inst->nodeVisible || !name) return 0;
	int n = 0;
	for (int i = 0; i < inst->model->nodeCount; i++) {
		if (!YbrStrEq(inst->model->nodes[i].name, name)) continue;
		inst->nodeVisible[i] = visible ? 1 : 0;
		n++;
	}
	return n;
}

// マテリアルの差し替え
void YbrModelInstanceSetNodeMaterial(YbrModelInstance* inst, int node,
									 const YbrModelMaterial* material)
{
	if (!inst || !inst->nodeMaterial || node < 0 ||
		inst->model->nodeCount <= node)
		return;
	inst->nodeMaterial[node] = material;
}

const YbrModelMaterial* YbrModelInstanceGetNodeMaterial(
	const YbrModelInstance* inst, int node)
{
	if (!inst || !inst->nodeMaterial || node < 0 ||
		inst->model->nodeCount <= node)
		return NULL;
	return inst->nodeMaterial[node];
}

int YbrModelInstanceSetNodeMaterialByName(YbrModelInstance* inst,
										  const char* name,
										  const YbrModelMaterial* material)
{
	if (!inst || !inst->nodeMaterial || !name) return 0;
	int n = 0;
	for (int i = 0; i < inst->model->nodeCount; i++) {
		if (!YbrStrEq(inst->model->nodes[i].name, name)) continue;
		inst->nodeMaterial[i] = material;
		n++;
	}
	return n;
}

void YbrModelInstanceClearNodeMaterials(YbrModelInstance* inst)
{
	if (!inst || !inst->nodeMaterial) return;
	for (int i = 0; i < inst->model->nodeCount; i++)
		inst->nodeMaterial[i] = NULL;
}

// 視錐台カリング

int YbrModelInstanceIsVisible(const YbrModelInstance* inst,
							  const YbrFrustum* frustum)
{
	if (!inst) return 0;
	if (!frustum) return 1;

	Vector3 lo, hi;
	if (!YbrModelInstanceGetBounds(inst, &lo, &hi))
		return 1; /* 分からなければ描く */
	return YbrFrustumContainsBox(frustum, lo, hi);
}
