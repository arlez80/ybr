/*
	Yui Blender to Raylib - ボーンの姿勢とアニメーション合成
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_pose.h"

#include <math.h>
#include <string.h>

#include "ybr_internal.h"

// ボーンの状態

int YbrPoseInit(YbrPose* pose, const YbrArmature* armature)
{
	if (!pose) return 0;
	memset(pose, 0, sizeof(*pose));
	if (!armature || armature->boneCount <= 0) return 1;

	int n = armature->boneCount;
	pose->armature = armature;
	pose->boneCount = n;
	pose->bones = (YbrPoseBone*)YBR_CALLOC((size_t)n, sizeof(YbrPoseBone));
	pose->invRest = (Matrix*)YBR_MALLOC((size_t)n * sizeof(Matrix));
	pose->restLocal =
		(YbrTransform*)YBR_MALLOC((size_t)n * sizeof(YbrTransform));
	if (!pose->bones || !pose->invRest || !pose->restLocal) {
		YbrPoseUnload(pose);
		return 0;
	}

	for (int i = 0; i < n; i++) {
		pose->invRest[i] = MatrixInvert(armature->bones[i].rest);
		pose->restLocal[i] =
			YbrTransformFromMatrix(armature->bones[i].restParent);
		pose->bones[i].parent = armature->bones[i].parent;
	}
	YbrPoseReset(pose);
	return 1;
}

void YbrPoseUnload(YbrPose* pose)
{
	if (!pose) return;
	YBR_FREE(pose->bones);
	YBR_FREE(pose->invRest);
	YBR_FREE(pose->restLocal);
	memset(pose, 0, sizeof(*pose));
}

void YbrPoseReset(YbrPose* pose)
{
	if (!pose || !pose->bones) return;
	for (int i = 0; i < pose->boneCount; i++) {
		pose->bones[i].translation = pose->restLocal[i].translation;
		pose->bones[i].rotation = pose->restLocal[i].rotation;
		pose->bones[i].scale = pose->restLocal[i].scale;
	}
	YbrPoseUpdate(pose);
}

void YbrPoseUpdate(YbrPose* pose)
{
	if (!pose || !pose->bones) return;
	for (int i = 0; i < pose->boneCount; i++) {
		YbrPoseBone* b = &pose->bones[i];
		YbrTransform t;
		t.translation = b->translation;
		t.rotation = b->rotation;
		t.scale = b->scale;

		Matrix local = YbrTransformToMatrix(t);
		// raylib の MatrixMultiply(A, B) は「A を適用してから B」
		b->pose = (0 <= b->parent && b->parent < i)
					  ? MatrixMultiply(local, pose->bones[b->parent].pose)
					  : local;
		b->skin = MatrixMultiply(pose->invRest[i], b->pose);
	}
}

int YbrPoseFindBone(const YbrPose* pose, const char* name)
{
	if (!pose || !pose->armature || !name) return -1;
	for (int i = 0; i < pose->boneCount; i++)
		if (YbrStrEq(pose->armature->bones[i].name, name)) return i;
	return -1;
}

void YbrPoseApplyAnimation(YbrPose* pose, const YbrAnimation* anim, float frame)
{
	if (!pose || !pose->bones) return;
	YbrPoseReset(pose);
	if (!anim) return;

	YbrInterpParams params = YbrInterpParamsFromAnimation(anim);
	for (int t = 0; t < anim->trackCount; t++) {
		const YbrAnimTrack* tr = &anim->tracks[t];
		if (!tr->bone) continue;
		int b = YbrPoseFindBone(pose, tr->bone);
		if (b < 0) continue;
		YbrTransform v = YbrAnimTrackEvaluate(tr, frame, &params);
		pose->bones[b].translation = v.translation;
		pose->bones[b].rotation = v.rotation;
		pose->bones[b].scale = v.scale;
	}
	YbrPoseUpdate(pose);
}

// アニメーション合成ツリー

static const YbrAnimation* find_animation(const YbrScene* sc, const char* name)
{
	if (!sc || !name) return NULL;
	for (int i = 0; i < sc->animationCount; i++)
		if (YbrStrEq(sc->animations[i].id, name)) return &sc->animations[i];
	return NULL;
}

static int animation_index(const YbrScene* sc, const YbrAnimation* a)
{
	if (!sc || !a) return -1;
	for (int i = 0; i < sc->animationCount; i++)
		if (&sc->animations[i] == a) return i;
	return -1;
}

static YbrAnimBlendNode* find_node(YbrAnimBlendNode* node,
								   const char* uniqueId)
{
	if (!node) return NULL;
	if (YbrStrEq(node->uniqueId, uniqueId)) return node;

	switch (node->type) {
		case YBR_ABN_ADD:
		case YBR_ABN_LERP: {
			YbrAnimBlendNode* r = find_node(node->lerp.input, uniqueId);
			if (r) return r;
			return find_node(node->lerp.mixInput, uniqueId);
		}
		case YBR_ABN_TRANSITION:
			for (int i = 0; i < node->transition.inputCount; i++) {
				YbrAnimBlendNode* r =
					find_node(&node->transition.inputs[i], uniqueId);
				if (r) return r;
			}
			return NULL;
		case YBR_ABN_SOURCE:
		default:
			return NULL;
	}
}

YbrAnimBlendNode* YbrAnimBlendTreeFind(const YbrAnimBlendTree* tree,
									   const char* uniqueId)
{
	// 空文字は「付けていない」印なので引っかけない
	if (!tree || !uniqueId || uniqueId[0] == '\0') return NULL;
	return find_node(tree->root, uniqueId);
}

static int validate(const YbrAnimBlendTree* tree, YbrAnimBlendNode* node)
{
	if (!node) return 0;
	switch (node->type) {
		case YBR_ABN_ADD:
		case YBR_ABN_LERP:
			if (!node->lerp.input || !node->lerp.mixInput) return 0;
			if (!validate(tree, node->lerp.input)) return 0;
			if (!validate(tree, node->lerp.mixInput)) return 0;
			return 1;
		case YBR_ABN_SOURCE:
			if (!node->source.name) return 0;
			return find_animation(tree->scene, node->source.name) != NULL;
		case YBR_ABN_TRANSITION:
			if (node->transition.inputCount <= 0 || !node->transition.inputs)
				return 0;
			for (int i = 0; i < node->transition.inputCount; i++)
				if (!validate(tree, &node->transition.inputs[i])) return 0;
			return 1;
		default:
			return 0;
	}
}

// 評価に要るスタックの段数。input 側は同じ段で評価するので伸びない。
static int stack_need(const YbrAnimBlendNode* node)
{
	if (!node) return 1;
	switch (node->type) {
		case YBR_ABN_ADD:
		case YBR_ABN_LERP: {
			int a = stack_need(node->lerp.input);
			int b = 1 + stack_need(node->lerp.mixInput);
			return (a < b) ? b : a;
		}
		case YBR_ABN_TRANSITION: {
			int m = 1;
			for (int i = 0; i < node->transition.inputCount; i++) {
				// 遷移中は今の入力と遷移先を同時に評価する
				int d = 1 + stack_need(&node->transition.inputs[i]);
				if (m < d) m = d;
			}
			return m;
		}
		case YBR_ABN_SOURCE:
		default:
			return 1;
	}
}

static void preprocess(YbrAnimBlendTree* tree, YbrAnimBlendNode* node)
{
	switch (node->type) {
		case YBR_ABN_ADD:
		case YBR_ABN_LERP:
			preprocess(tree, node->lerp.input);
			preprocess(tree, node->lerp.mixInput);
			break;
		case YBR_ABN_SOURCE: {
			const YbrAnimation* a =
				find_animation(tree->scene, node->source.name);
			node->source.animation = a;
			node->source.position = 0.0f;
			if (!(0.0f < node->source.playSpeed)) node->source.playSpeed = 1.0f;
			float fps = (a && 0.0f < a->fps) ? a->fps : 24.0f;
			int n = a ? a->frameCount : 1;
			if (n < 1) n = 1;
			node->source.length = (float)n / fps;
		} break;
		case YBR_ABN_TRANSITION:
			if (node->transition.index < 0 ||
				node->transition.inputCount <= node->transition.index)
				node->transition.index = 0;
			node->transition.nextIndex = -1;
			node->transition.position = 0.0f;
			if (!(0.0f < node->transition.transitionSeconds))
				node->transition.transitionSeconds = 0.05f;
			for (int i = 0; i < node->transition.inputCount; i++)
				preprocess(tree, &node->transition.inputs[i]);
			break;
		default:
			break;
	}
	node->playedPreviousEvaluate = 0;
}

int YbrAnimBlendTreeInit(YbrAnimBlendTree* tree, const YbrScene* scene,
						 const YbrPose* pose, YbrAnimBlendNode* root)
{
	if (!tree) return 0;
	memset(tree, 0, sizeof(*tree));
	if (!scene || !pose || pose->boneCount <= 0 || !root) return 0;

	tree->scene = scene;
	tree->pose = pose;
	tree->boneCount = pose->boneCount;
	tree->root = root;

	// アニメーション x ボーン のトラック索引を作っておく
	tree->animationCount = scene->animationCount;
	if (0 < tree->animationCount) {
		tree->trackIndex =
			(int**)YBR_CALLOC((size_t)tree->animationCount, sizeof(int*));
		if (!tree->trackIndex) {
			YbrAnimBlendTreeUnload(tree);
			return 0;
		}
		for (int a = 0; a < tree->animationCount; a++) {
			tree->trackIndex[a] =
				(int*)YBR_MALLOC((size_t)tree->boneCount * sizeof(int));
			if (!tree->trackIndex[a]) {
				YbrAnimBlendTreeUnload(tree);
				return 0;
			}
			for (int b = 0; b < tree->boneCount; b++)
				tree->trackIndex[a][b] = -1;
			const YbrAnimation* an = &scene->animations[a];
			for (int t = 0; t < an->trackCount; t++) {
				if (!an->tracks[t].bone) continue;
				int b = YbrPoseFindBone(pose, an->tracks[t].bone);
				if (0 <= b) tree->trackIndex[a][b] = t;
			}
		}
	}

	for (int i = 0; i < YBR_ABT_MAX_STACK; i++) {
		tree->stack[i] = (YbrAnimDelta*)YBR_MALLOC((size_t)tree->boneCount *
												   sizeof(YbrAnimDelta));
		if (!tree->stack[i]) {
			YbrAnimBlendTreeUnload(tree);
			return 0;
		}
	}

	tree->validated =
		validate(tree, root) && stack_need(root) <= YBR_ABT_MAX_STACK;
	if (tree->validated) preprocess(tree, root);
	return tree->validated;
}

void YbrAnimBlendTreeUnload(YbrAnimBlendTree* tree)
{
	if (!tree) return;
	if (tree->trackIndex) {
		for (int i = 0; i < tree->animationCount; i++)
			YBR_FREE(tree->trackIndex[i]);
		YBR_FREE(tree->trackIndex);
	}
	for (int i = 0; i < YBR_ABT_MAX_STACK; i++) YBR_FREE(tree->stack[i]);
	memset(tree, 0, sizeof(*tree));
}

static void pre_evaluate(YbrAnimBlendNode* node)
{
	switch (node->type) {
		case YBR_ABN_ADD:
		case YBR_ABN_LERP:
			pre_evaluate(node->lerp.input);
			pre_evaluate(node->lerp.mixInput);
			break;
		case YBR_ABN_SOURCE:
			// 前フレームで再生されていなければ頭出しに戻す
			if (!node->playedPreviousEvaluate) node->source.position = 0.0f;
			break;
		case YBR_ABN_TRANSITION:
			for (int i = 0; i < node->transition.inputCount; i++)
				pre_evaluate(&node->transition.inputs[i]);
			break;
		default:
			break;
	}
	node->playedPreviousEvaluate = 0;
}

static int is_filtered(const YbrAnimBlendTree* tree,
					   const YbrAnimBlendInput2* in, int bone)
{
	if (!in->filteredBones || in->filteredBoneCount <= 0) return 0;
	const char* name = tree->pose->armature->bones[bone].name;
	for (int k = 0; k < in->filteredBoneCount; k++)
		if (YbrStrEq(in->filteredBones[k], name)) return 1;
	return 0;
}

static YbrAnimDelta delta_identity(void)
{
	YbrAnimDelta d;
	d.translation = Vector3Zero();
	d.rotation = QuaternionIdentity();
	d.scale = Vector3One();
	return d;
}

static void evaluate(YbrAnimBlendTree* tree, YbrAnimBlendNode* node,
					 float delta)
{
	const int n = tree->boneCount;
	const int sp = tree->stackPointer;

	switch (node->type) {
		case YBR_ABN_SOURCE: {
			YbrAnimDelta* out = tree->stack[sp];
			const YbrAnimation* a = node->source.animation;
			int ai = animation_index(tree->scene, a);

			float fps = (a && 0.0f < a->fps) ? a->fps : 24.0f;
			float lastFrame =
				(a && 1 < a->frameCount) ? (float)(a->frameCount - 1) : 0.0f;
			float frame = Clamp(node->source.position * fps, 0.0f, lastFrame);

			YbrInterpParams params = YbrInterpParamsFromAnimation(a);

			for (int b = 0; b < n; b++) {
				int ti = (0 <= ai) ? tree->trackIndex[ai][b] : -1;
				if (ti < 0) {
					out[b] = delta_identity();
					continue;
				}

				YbrTransform v =
					YbrAnimTrackEvaluate(&a->tracks[ti], frame, &params);
				const YbrTransform* rest = &tree->pose->restLocal[b];

				// レストからの差分にしておくと ADD (加算) が意味を持つ
				out[b].translation =
					Vector3Subtract(v.translation, rest->translation);
				out[b].rotation = QuaternionMultiply(
					QuaternionInvert(rest->rotation), v.rotation);
				out[b].scale = v.scale;
			}

			float step = delta * node->source.playSpeed;
			if (node->source.loopMode == YBR_ALM_LOOP) {
				float len =
					0.0f < node->source.length ? node->source.length : 1.0f;
				node->source.position =
					fmodf(node->source.position + step, len);
				if (node->source.position < 0.0f) node->source.position += len;
			}
			else {
				node->source.position += step;
				if (node->source.length < node->source.position)
					node->source.position = node->source.length;
				if (node->source.position < 0.0f) node->source.position = 0.0f;
			}
		} break;

		case YBR_ABN_LERP: {
			evaluate(tree, node->lerp.input, delta);
			tree->stackPointer = sp + 1;
			evaluate(tree, node->lerp.mixInput, delta);
			tree->stackPointer = sp;

			YbrAnimDelta* out = tree->stack[sp];
			YbrAnimDelta* mix = tree->stack[sp + 1];
			float t = Clamp(node->lerp.weight, 0.0f, 1.0f);

			for (int b = 0; b < n; b++) {
				if (is_filtered(tree, &node->lerp, b)) continue;
				out[b].translation =
					Vector3Lerp(out[b].translation, mix[b].translation, t);
				out[b].rotation =
					QuaternionSlerp(out[b].rotation, mix[b].rotation, t);
				out[b].scale = Vector3Lerp(out[b].scale, mix[b].scale, t);
			}
		} break;

		case YBR_ABN_ADD: {
			evaluate(tree, node->add.input, delta);
			tree->stackPointer = sp + 1;
			evaluate(tree, node->add.mixInput, delta);
			tree->stackPointer = sp;

			YbrAnimDelta* out = tree->stack[sp];
			YbrAnimDelta* mix = tree->stack[sp + 1];
			float t = Clamp(node->add.weight, 0.0f, 1.0f);

			for (int b = 0; b < n; b++) {
				if (is_filtered(tree, &node->add, b)) continue;
				// 差分どうしなので足し込める。回転は「単位 -> mix」を t で
				// 補間したものを元の回転に掛ける。
				out[b].translation = Vector3Add(
					out[b].translation, Vector3Scale(mix[b].translation, t));
				out[b].rotation = QuaternionMultiply(
					QuaternionSlerp(QuaternionIdentity(), mix[b].rotation, t),
					out[b].rotation);
				// スケールは加算の意味がはっきりしないので触らない (rl-abt
				// と同じ)
			}
		} break;

		case YBR_ABN_TRANSITION: {
			YbrAnimBlendTransition* tr = &node->transition;
			if (tr->index < 0 || tr->inputCount <= tr->index) tr->index = 0;

			evaluate(tree, &tr->inputs[tr->index], delta);

			if (0 <= tr->nextIndex && tr->nextIndex < tr->inputCount) {
				tree->stackPointer = sp + 1;
				evaluate(tree, &tr->inputs[tr->nextIndex], delta);
				tree->stackPointer = sp;

				YbrAnimDelta* out = tree->stack[sp];
				YbrAnimDelta* mix = tree->stack[sp + 1];
				float total = 1e-6f < tr->transitionSeconds
								  ? tr->transitionSeconds
								  : 1e-6f;
				float t = Clamp(tr->position / total, 0.0f, 1.0f);

				for (int b = 0; b < n; b++) {
					out[b].translation =
						Vector3Lerp(out[b].translation, mix[b].translation, t);
					out[b].rotation =
						QuaternionSlerp(out[b].rotation, mix[b].rotation, t);
					out[b].scale = Vector3Lerp(out[b].scale, mix[b].scale, t);
				}

				tr->position += delta;
				if (tr->transitionSeconds <= tr->position) {
					tr->position = 0.0f;
					tr->index = tr->nextIndex;
					tr->nextIndex = -1;
				}
			}
		} break;

		default:
			break;
	}

	node->playedPreviousEvaluate = 1;
}

int YbrAnimBlendTreeEval(YbrAnimBlendTree* tree, float delta, YbrPose* outPose)
{
	if (!tree || !tree->validated || !outPose || !outPose->bones) return 0;
	if (outPose->boneCount != tree->boneCount) return 0;
	if (tree->stackPointer != 0) tree->stackPointer = 0;

	pre_evaluate(tree->root);
	tree->stackPointer = 0;
	evaluate(tree, tree->root, delta);

	const YbrAnimDelta* res = tree->stack[0];
	for (int b = 0; b < tree->boneCount; b++) {
		const YbrTransform* rest = &tree->pose->restLocal[b];
		outPose->bones[b].translation =
			Vector3Add(rest->translation, res[b].translation);
		outPose->bones[b].rotation = QuaternionNormalize(
			QuaternionMultiply(rest->rotation, res[b].rotation));
		outPose->bones[b].scale = res[b].scale;
	}
	YbrPoseUpdate(outPose);
	return 1;
}
