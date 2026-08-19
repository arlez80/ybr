/*
	Yui Blender to Raylib - Yabtファイル読み書き
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_yabt.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ybr_cbor.h"
#include "ybr_internal.h"

// エラー

static char yabtError[256] = {0};

static void yabt_err(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(yabtError, sizeof(yabtError), fmt, ap);
	va_end(ap);
}

const char* YabtGetError(void) { return yabtError; }

// 確保したメモリの管理
// ノードは種類ごとに形が違う (遷移だけは連続した配列でないといけない) ので、
// 確保した塊をそのまま覚えておいてまとめて捨てる。

static void* yabt_keep(YabtTree* t, void* p)
{
	if (!p) return NULL;
	void** grown =
		(void**)YbrGrowBuffer(t->blocks, &t->blockCap, t->blockCount + 1,
							  sizeof(void*));
	if (!grown) {
		YBR_FREE(p);
		return NULL;
	}
	t->blocks = grown;
	t->blocks[t->blockCount++] = p;
	return p;
}

static void* yabt_alloc(YabtTree* t, size_t count, size_t size)
{
	return yabt_keep(t, YBR_CALLOC(count, size));
}

// 読み込み

static int load_node(YabtTree* t, const CborValue* v, YbrAnimBlendNode* out,
					 int depth);

static int load_filtered_bones(YabtTree* t, const CborValue* v,
							   YbrAnimBlendInput2* out)
{
	const CborValue* arr = CborGet(v, "filtered_bones");
	if (!arr || arr->type == CBOR_NULL) return 1;
	if (arr->type != CBOR_ARRAY) {
		yabt_err("YABT: 'filtered_bones' must be an array or null");
		return 0;
	}
	if (arr->count == 0) return 1;

	const char** names =
		(const char**)yabt_alloc(t, arr->count, sizeof(const char*));
	if (!names) {
		yabt_err("YABT: out of memory");
		return 0;
	}
	for (size_t i = 0; i < arr->count; i++) {
		if (arr->items[i].type != CBOR_TEXT) {
			yabt_err("YABT: 'filtered_bones[%d]' must be text", (int)i);
			return 0;
		}
		char* s = (char*)yabt_keep(t, CborDup(&arr->items[i]));
		if (!s) {
			yabt_err("YABT: out of memory");
			return 0;
		}
		names[i] = s;
	}
	out->filteredBones = names;
	out->filteredBoneCount = (int)arr->count;
	return 1;
}

static int load_source(YabtTree* t, const CborValue* v, YbrAnimBlendNode* out)
{
	char* name = (char*)yabt_keep(t, CborGetStr(v, "name"));
	if (!name) {
		yabt_err("YABT: source node needs 'name'");
		return 0;
	}
	out->source.name = name;

	int loop = CborGetCode(v, "loop_mode", YBR_ALM_ONE_SHOT);
	if (loop != YBR_ALM_ONE_SHOT && loop != YBR_ALM_LOOP) {
		yabt_err("YABT: unknown loop_mode %d", loop);
		return 0;
	}
	out->source.loopMode = (YbrAnimLoopMode)loop;

	float speed = CborGetFlt(v, "play_speed", 1.0f);
	if (!(0.0f < speed)) speed = 1.0f;
	out->source.playSpeed = speed;
	return 1;
}

static int load_input2(YabtTree* t, const CborValue* v, YbrAnimBlendNode* out,
					   int depth)
{
	float weight = CborGetFlt(v, "weight", 0.0f);
	out->lerp.weight = Clamp(weight, 0.0f, 1.0f);

	if (!load_filtered_bones(t, v, &out->lerp)) return 0;

	const CborValue* in = CborGet(v, "input");
	const CborValue* mix = CborGet(v, "mix_input");
	if (!in || !mix) {
		yabt_err("YABT: node needs both 'input' and 'mix_input'");
		return 0;
	}

	out->lerp.input =
		(YbrAnimBlendNode*)yabt_alloc(t, 1, sizeof(YbrAnimBlendNode));
	out->lerp.mixInput =
		(YbrAnimBlendNode*)yabt_alloc(t, 1, sizeof(YbrAnimBlendNode));
	if (!out->lerp.input || !out->lerp.mixInput) {
		yabt_err("YABT: out of memory");
		return 0;
	}
	if (!load_node(t, in, out->lerp.input, depth + 1)) return 0;
	if (!load_node(t, mix, out->lerp.mixInput, depth + 1)) return 0;
	return 1;
}

static int load_transition(YabtTree* t, const CborValue* v,
						   YbrAnimBlendNode* out, int depth)
{
	const CborValue* arr = CborGet(v, "inputs");
	if (!arr || arr->type != CBOR_ARRAY || arr->count == 0) {
		yabt_err("YABT: transition node needs a non-empty 'inputs' array");
		return 0;
	}

	// 遷移の入力は連続した配列でないといけない (&inputs[i] で参照される)
	YbrAnimBlendNode* nodes = (YbrAnimBlendNode*)yabt_alloc(
		t, arr->count, sizeof(YbrAnimBlendNode));
	if (!nodes) {
		yabt_err("YABT: out of memory");
		return 0;
	}
	for (size_t i = 0; i < arr->count; i++)
		if (!load_node(t, &arr->items[i], &nodes[i], depth + 1)) return 0;

	out->transition.inputs = nodes;
	out->transition.inputCount = (int)arr->count;

	int index = CborGetInt(v, "index", 0);
	if (index < 0 || out->transition.inputCount <= index) index = 0;
	out->transition.index = index;

	float sec = CborGetFlt(v, "transition_seconds", 0.0f);
	if (!(0.0f <= sec)) sec = 0.0f;
	out->transition.transitionSeconds = sec;

	// 遷移中かどうかは実行時の状態なので、読み込み時は必ず止まった状態にする
	out->transition.nextIndex = -1;
	out->transition.position = 0.0f;
	return 1;
}

static int load_node(YabtTree* t, const CborValue* v, YbrAnimBlendNode* out,
					 int depth)
{
	if (YABT_MAX_DEPTH < depth) {
		yabt_err("YABT: tree is deeper than %d", YABT_MAX_DEPTH);
		return 0;
	}
	if (!v || v->type != CBOR_MAP) {
		yabt_err("YABT: node must be a map");
		return 0;
	}

	// 付けていないときは空文字。文字列以外が来たら空文字として扱う。
	const CborValue* uid = CborGet(v, "unique_id");
	if (uid && uid->type == CBOR_TEXT && 0 < uid->len) {
		char* s = (char*)yabt_keep(t, CborDup(uid));
		if (!s) {
			yabt_err("YABT: out of memory");
			return 0;
		}
		out->uniqueId = s;
	}
	else {
		out->uniqueId = "";
	}

	int type = CborGetCode(v, "type", -1);
	switch (type) {
		case YBR_ABN_SOURCE:
			out->type = YBR_ABN_SOURCE;
			return load_source(t, v, out);
		case YBR_ABN_LERP:
		case YBR_ABN_ADD:
			out->type = (YbrAnimBlendNodeType)type;
			return load_input2(t, v, out, depth);
		case YBR_ABN_TRANSITION:
			out->type = YBR_ABN_TRANSITION;
			return load_transition(t, v, out, depth);
		default:
			yabt_err("YABT: unknown node type %d", type);
			return 0;
	}
}

YabtTree* YabtLoadFromMemory(const unsigned char* data, size_t size)
{
	yabtError[0] = '\0';

	if (!data || size < 4) {
		yabt_err("YABT: empty data");
		return NULL;
	}

	CborValue root;
	if (!CborParse(data, size, &root)) {
		CborFree(&root);
		yabt_err("YABT: CBOR parse failed");
		return NULL;
	}
	if (root.type != CBOR_ARRAY || root.count < 3) {
		CborFree(&root);
		yabt_err("YABT: root must be an array of 3 elements");
		return NULL;
	}

	const CborValue* magic = &root.items[0];
	const CborValue* verval = &root.items[1];
	const CborValue* tree = &root.items[2];

	if (magic->type != CBOR_TEXT || magic->len != 4 ||
		memcmp(magic->bytes, YABT_MAGIC, 4) != 0) {
		CborFree(&root);
		yabt_err("YABT: bad magic (expected \"%s\")", YABT_MAGIC);
		return NULL;
	}

	int version = (int)CborGetNumV(verval, 0.0);
	if (version != YABT_SUPPORTED_VERSION) {
		CborFree(&root);
		yabt_err("YABT: unsupported version %d (expected %d)", version,
				 YABT_SUPPORTED_VERSION);
		return NULL;
	}

	YabtTree* t = (YabtTree*)YBR_CALLOC(1, sizeof(YabtTree));
	if (!t) {
		CborFree(&root);
		yabt_err("YABT: out of memory");
		return NULL;
	}
	t->version = version;

	t->root = (YbrAnimBlendNode*)yabt_alloc(t, 1, sizeof(YbrAnimBlendNode));
	if (!t->root) {
		yabt_err("YABT: out of memory");
		goto fail;
	}
	if (!load_node(t, tree, t->root, 0)) goto fail;

	CborFree(&root);
	return t;

fail:
	if (yabtError[0] == '\0') yabt_err("YABT: load failed");
	CborFree(&root);
	YabtUnload(t);
	return NULL;
}

YabtTree* YabtLoad(const char* fileName)
{
	yabtError[0] = '\0';

	FILE* f = fopen(fileName, "rb");
	if (!f) {
		yabt_err("YABT: cannot open '%s'", fileName);
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		yabt_err("YABT: cannot seek '%s'", fileName);
		return NULL;
	}
	long size = ftell(f);
	if (size <= 0) {
		fclose(f);
		yabt_err("YABT: '%s' is empty", fileName);
		return NULL;
	}
	rewind(f);

	unsigned char* buf = (unsigned char*)YBR_MALLOC((size_t)size);
	if (!buf) {
		fclose(f);
		yabt_err("YABT: out of memory");
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);

	if (got != (size_t)size) {
		YBR_FREE(buf);
		yabt_err("YABT: read error on '%s'", fileName);
		return NULL;
	}

	YabtTree* t = YabtLoadFromMemory(buf, got);
	YBR_FREE(buf);
	return t;
}

void YabtUnload(YabtTree* tree)
{
	if (!tree) return;
	for (int i = 0; i < tree->blockCount; i++) YBR_FREE(tree->blocks[i]);
	YBR_FREE(tree->blocks);
	YBR_FREE(tree);
}

// 書き出し

// 書ける形になっているか。YbrAnimBlendTreeInit() の検査から
// 「シーンにそのアニメーションがあるか」を抜いたもの。
static int check_node(const YbrAnimBlendNode* n, int depth)
{
	if (YABT_MAX_DEPTH < depth) {
		yabt_err("YABT: tree is deeper than %d", YABT_MAX_DEPTH);
		return 0;
	}
	if (!n) {
		yabt_err("YABT: node is NULL");
		return 0;
	}
	switch (n->type) {
		case YBR_ABN_SOURCE:
			if (!n->source.name) {
				yabt_err("YABT: source node has no name");
				return 0;
			}
			return 1;
		case YBR_ABN_ADD:
		case YBR_ABN_LERP:
			if (!n->lerp.input || !n->lerp.mixInput) {
				yabt_err("YABT: node is missing an input");
				return 0;
			}
			if (!check_node(n->lerp.input, depth + 1)) return 0;
			return check_node(n->lerp.mixInput, depth + 1);
		case YBR_ABN_TRANSITION:
			if (n->transition.inputCount <= 0 || !n->transition.inputs) {
				yabt_err("YABT: transition node has no inputs");
				return 0;
			}
			for (int i = 0; i < n->transition.inputCount; i++)
				if (!check_node(&n->transition.inputs[i], depth + 1)) return 0;
			return 1;
		default:
			yabt_err("YABT: unknown node type %d", (int)n->type);
			return 0;
	}
}

static void write_node(CborWriter* w, const YbrAnimBlendNode* n);

static void write_input2(CborWriter* w, const YbrAnimBlendInput2* in)
{
	CborWriteKeyFloat(w, "weight", in->weight);

	if (in->filteredBones && 0 < in->filteredBoneCount) {
		CborWriteKeyArrayHeader(w, "filtered_bones",
								(size_t)in->filteredBoneCount);
		for (int i = 0; i < in->filteredBoneCount; i++)
			CborWriteTextOrNull(w, in->filteredBones[i]);
	}
	else {
		CborWriteKeyNull(w, "filtered_bones");
	}

	CborWriteText(w, "input");
	write_node(w, in->input);
	CborWriteText(w, "mix_input");
	write_node(w, in->mixInput);
}

static void write_node(CborWriter* w, const YbrAnimBlendNode* n)
{
	switch (n->type) {
		case YBR_ABN_SOURCE:
			// type, unique_id, name, loop_mode, play_speed
			CborWriteMapHeader(w, 5);
			CborWriteKeyInt(w, "type", n->type);
			CborWriteKeyText(w, "unique_id",
							 n->uniqueId ? n->uniqueId : "");
			CborWriteKeyText(w, "name", n->source.name);
			CborWriteKeyInt(w, "loop_mode", n->source.loopMode);
			CborWriteKeyFloat(w, "play_speed", n->source.playSpeed);
			break;

		case YBR_ABN_ADD:
		case YBR_ABN_LERP:
			// type, unique_id, weight, filtered_bones, input, mix_input
			CborWriteMapHeader(w, 6);
			CborWriteKeyInt(w, "type", n->type);
			CborWriteKeyText(w, "unique_id",
							 n->uniqueId ? n->uniqueId : "");
			write_input2(w, &n->lerp);
			break;

		case YBR_ABN_TRANSITION:
			// type, unique_id, index, transition_seconds, inputs
			CborWriteMapHeader(w, 5);
			CborWriteKeyInt(w, "type", n->type);
			CborWriteKeyText(w, "unique_id",
							 n->uniqueId ? n->uniqueId : "");
			CborWriteKeyInt(w, "index", n->transition.index);
			CborWriteKeyFloat(w, "transition_seconds",
							  n->transition.transitionSeconds);
			CborWriteKeyArrayHeader(w, "inputs",
									(size_t)n->transition.inputCount);
			for (int i = 0; i < n->transition.inputCount; i++)
				write_node(w, &n->transition.inputs[i]);
			break;

		default:
			break;
	}
}

unsigned char* YabtSaveToMemory(const YbrAnimBlendNode* root, size_t* outSize)
{
	yabtError[0] = '\0';
	if (outSize) *outSize = 0;

	if (!root) {
		yabt_err("YABT: root is NULL");
		return NULL;
	}
	if (!check_node(root, 0)) return NULL;

	CborWriter w;
	CborWriterInit(&w);

	// ["YABT", version, tree]
	CborWriteArrayHeader(&w, 3);
	CborWriteText(&w, YABT_MAGIC);
	CborWriteInt(&w, YABT_SUPPORTED_VERSION);
	write_node(&w, root);

	if (w.failed) {
		CborWriterFree(&w);
		yabt_err("YABT: out of memory while writing");
		return NULL;
	}

	size_t len;
	unsigned char* buf = CborWriterTake(&w, &len);
	if (!buf) {
		yabt_err("YABT: out of memory while writing");
		return NULL;
	}
	if (outSize) *outSize = len;
	return buf;
}

int YabtSave(const YbrAnimBlendNode* root, const char* fileName)
{
	size_t size;
	unsigned char* buf = YabtSaveToMemory(root, &size);
	if (!buf) return 0;

	FILE* f = fopen(fileName, "wb");
	if (!f) {
		yabt_err("YABT: cannot open '%s' for writing", fileName);
		YBR_FREE(buf);
		return 0;
	}
	size_t written = fwrite(buf, 1, size, f);
	fclose(f);
	YBR_FREE(buf);

	if (written != size) {
		yabt_err("YABT: write error on '%s'", fileName);
		return 0;
	}
	return 1;
}
