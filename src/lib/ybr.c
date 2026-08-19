/*
	Yui Blender to Raylib - Ybrファイル読み書き
		Programed by あるる（きのもと 結衣）
*/
#include "ybr.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ybr_cbor.h"

// 両方が非 NULL で中身が同じか。ID の照合はすべてこれを通す。
static int id_eq(const char* a, const char* b)
{
	return a && b && strcmp(a, b) == 0;
}

// エラー

static char ybrError[256] = {0};

static void ybr_err(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(ybrError, sizeof(ybrError), fmt, ap);
	va_end(ap);
}

const char* YbrGetError(void) { return ybrError; }

// raymath 型への変換グルー (ybr_cbor は Matrix / Vector を知らないので、
// こちら側で CborGetFloats を組み合わせて読む)

// ファイル内の 16 個の float (column-major) を raylib の Matrix へ。
// v[0]=m0, v[1]=m1, ... v[15]=m15 の対応なので memcpy はできない。
static Matrix mat_from_floats(const float* v)
{
	Matrix m;
	m.m0 = v[0];
	m.m1 = v[1];
	m.m2 = v[2];
	m.m3 = v[3];
	m.m4 = v[4];
	m.m5 = v[5];
	m.m6 = v[6];
	m.m7 = v[7];
	m.m8 = v[8];
	m.m9 = v[9];
	m.m10 = v[10];
	m.m11 = v[11];
	m.m12 = v[12];
	m.m13 = v[13];
	m.m14 = v[14];
	m.m15 = v[15];
	return m;
}

// raylib の Matrix -> ファイル用の 16 個の float (column-major)
static void mat_to_floats(Matrix m, float* out)
{
	out[0] = m.m0;
	out[1] = m.m1;
	out[2] = m.m2;
	out[3] = m.m3;
	out[4] = m.m4;
	out[5] = m.m5;
	out[6] = m.m6;
	out[7] = m.m7;
	out[8] = m.m8;
	out[9] = m.m9;
	out[10] = m.m10;
	out[11] = m.m11;
	out[12] = m.m12;
	out[13] = m.m13;
	out[14] = m.m14;
	out[15] = m.m15;
}

static void cb_matrix(const CborValue* map, const char* key, Matrix* out)
{
	float v[16];
	for (int k = 0; k < 16; k++) v[k] = (k % 5 == 0) ? 1.0f : 0.0f;
	CborGetFloats(map, key, v, 16);
	*out = mat_from_floats(v);
}

static void cb_vec3(const CborValue* map, const char* key, Vector3* out)
{
	float v[3] = {out->x, out->y, out->z};
	CborGetFloats(map, key, v, 3);
	out->x = v[0];
	out->y = v[1];
	out->z = v[2];
}

static void cb_vec4(const CborValue* map, const char* key, Vector4* out)
{
	float v[4] = {out->x, out->y, out->z, out->w};
	CborGetFloats(map, key, v, 4);
	out->x = v[0];
	out->y = v[1];
	out->z = v[2];
	out->w = v[3];
}

// ノード

static int load_custom_properties(const CborValue* v, YbrNode* node)
{
	const CborValue* arr = CborGet(v, "custom_properties");
	if (!arr || arr->type != CBOR_ARRAY || arr->count == 0) return 1;

	node->customPropertyCount = (int)arr->count;
	node->customProperties =
		(YbrCustomProperty*)YBR_CALLOC(arr->count, sizeof(YbrCustomProperty));
	if (!node->customProperties) return 0;

	for (int k = 0; k < node->customPropertyCount; k++) {
		const CborValue* e = &arr->items[k];
		YbrCustomProperty* p = &node->customProperties[k];
		p->key = CborGetStr(e, "key");
		p->type = (YbrCustomType)CborGetCode(e, "type", YBR_CUSTOM_FLOAT);

		const CborValue* val = CborGet(e, "value");
		if (!val) continue;
		if (p->type == YBR_CUSTOM_STRING) {
			p->text = CborDup(val);
		}
		else if (p->type == YBR_CUSTOM_ARRAY) {
			p->values = CborGetF32(e, "value", &p->valueCount);
		}
		else {
			p->number = CborGetNumV(val, 0.0);
		}
	}
	return 1;
}

static int load_node(const CborValue* v, YbrNode* node)
{
	if (!v || v->type != CBOR_MAP) return 0;

	node->name = CborGetStr(v, "name");
	node->dataId = CborGetStr(v, "data");
	node->type = (YbrNodeType)CborGetCode(v, "type", YBR_NODE_UNKNOWN);
	cb_matrix(v, "matrix", &node->matrix);
	if (!load_custom_properties(v, node)) return 0;

	const CborValue* kids = CborGet(v, "children");
	if (kids && kids->type == CBOR_ARRAY && 0 < kids->count) {
		node->childCount = (int)kids->count;
		node->children = (YbrNode*)YBR_CALLOC(kids->count, sizeof(YbrNode));
		if (!node->children) return 0;
		for (size_t k = 0; k < kids->count; k++)
			if (!load_node(&kids->items[k], &node->children[k])) return 0;
	}
	return 1;
}

static void free_node(YbrNode* n)
{
	if (!n) return;
	for (int k = 0; k < n->childCount; k++) free_node(&n->children[k]);
	YBR_FREE(n->children);
	for (int k = 0; k < n->customPropertyCount; k++) {
		YBR_FREE(n->customProperties[k].key);
		YBR_FREE(n->customProperties[k].text);
		YBR_FREE(n->customProperties[k].values);
	}
	YBR_FREE(n->customProperties);
	YBR_FREE(n->name);
	YBR_FREE(n->dataId);
}

// メッシュ

static int load_mesh(const CborValue* v, YbrMesh* m)
{
	int n;
	int nPos = 0, nNrm = 0, nTan = 0, nUv = 0, nCol = 0, nIdx = 0, nMat = 0,
		nMap = 0;
	m->id = CborGetStr(v, "id");
	m->vertexCount = CborGetInt(v, "vertex_count", 0);
	m->triangleCount = CborGetInt(v, "triangle_count", 0);
	if (m->vertexCount < 0) m->vertexCount = 0;
	if (m->triangleCount < 0) m->triangleCount = 0;

	m->positions = CborGetF32(v, "positions", &nPos);
	m->normals = CborGetF32(v, "normals", &nNrm);
	m->tangents = CborGetF32(v, "tangents", &nTan);
	m->texcoords = CborGetF32(v, "uvs", &nUv);
	m->colors = CborGetU8(v, "colors", &nCol);
	m->indices = CborGetU32(v, "indices", &nIdx);
	m->materialIndices = CborGetU32(v, "material_indices", &nMat);
	m->vertexMap = CborGetU32(v, "vertex_map", &nMap);

	// 長さが足りているか
	if (nPos / 3 < m->vertexCount) m->vertexCount = nPos / 3;
	if (nIdx / 3 < m->triangleCount) m->triangleCount = nIdx / 3;

	if (m->normals && nNrm < m->vertexCount * 3) {
		YBR_FREE(m->normals);
		m->normals = NULL;
	}
	if (m->tangents && nTan < m->vertexCount * 4) {
		YBR_FREE(m->tangents);
		m->tangents = NULL;
	}
	if (m->texcoords && nUv < m->vertexCount * 2) {
		YBR_FREE(m->texcoords);
		m->texcoords = NULL;
	}
	if (m->colors && nCol < m->vertexCount * 4) {
		YBR_FREE(m->colors);
		m->colors = NULL;
	}
	if (m->vertexMap && nMap < m->vertexCount) {
		YBR_FREE(m->vertexMap);
		m->vertexMap = NULL;
	}
	if (m->materialIndices && nMat < m->triangleCount) {
		YBR_FREE(m->materialIndices);
		m->materialIndices = NULL;
	}

	m->armature = CborGetStr(v, "armature");
	m->armatureData = CborGetStr(v, "armature_data");

	const CborValue* arr = CborGet(v, "materials");
	if (arr && arr->type == CBOR_ARRAY && 0 < arr->count) {
		m->materialCount = (int)arr->count;
		m->materials = (char**)YBR_CALLOC(arr->count, sizeof(char*));
		if (!m->materials) return 0;
		for (int k = 0; k < m->materialCount; k++)
			m->materials[k] = CborDup(&arr->items[k]);
	}

	const CborValue* sk = CborGet(v, "skin");
	if (sk && sk->type == CBOR_MAP) {
		int nJoint = 0, nWeight = 0;
		YbrSkin* s = (YbrSkin*)YBR_CALLOC(1, sizeof(YbrSkin));
		if (!s) return 0;
		m->skin = s;
		s->influences = CborGetInt(sk, "influences", 4);
		s->joints = CborGetU16(sk, "joints", &nJoint);
		s->weights = CborGetF32(sk, "weights", &nWeight);
		// 4 本ぶん揃っていないスキンは使えないので捨てる (スキン無し扱い)
		if (!s->joints || !s->weights || nJoint < m->vertexCount * 4 ||
			nWeight < m->vertexCount * 4) {
			YBR_FREE(s->joints);
			YBR_FREE(s->weights);
			YBR_FREE(s);
			m->skin = NULL;
		}
	}

	arr = CborGet(v, "vertex_groups");
	if (arr && arr->type == CBOR_ARRAY && 0 < arr->count) {
		m->vertexGroupCount = (int)arr->count;
		m->vertexGroups =
			(YbrVertexGroup*)YBR_CALLOC(arr->count, sizeof(YbrVertexGroup));
		if (!m->vertexGroups) return 0;
		for (int k = 0; k < m->vertexGroupCount; k++) {
			const CborValue* g = &arr->items[k];
			YbrVertexGroup* o = &m->vertexGroups[k];
			int nGi = 0, nGw = 0;
			o->name = CborGetStr(g, "name");
			o->index = CborGetInt(g, "index", k);
			o->count = CborGetInt(g, "count", 0);
			o->indices = CborGetU32(g, "indices", &nGi);
			o->weights = CborGetF32(g, "weights", &nGw);
			if (o->count < 0) o->count = 0;
			if (nGi < o->count) o->count = nGi;
			if (nGw < o->count) o->count = nGw;
		}
	}

	(void)n;
	return 1;
}

static void free_mesh(YbrMesh* m)
{
	YBR_FREE(m->id);
	YBR_FREE(m->positions);
	YBR_FREE(m->normals);
	YBR_FREE(m->tangents);
	YBR_FREE(m->texcoords);
	YBR_FREE(m->colors);
	YBR_FREE(m->indices);
	YBR_FREE(m->materialIndices);
	YBR_FREE(m->vertexMap);
	YBR_FREE(m->armature);
	YBR_FREE(m->armatureData);
	for (int k = 0; k < m->materialCount; k++) YBR_FREE(m->materials[k]);
	YBR_FREE(m->materials);
	if (m->skin) {
		YBR_FREE(m->skin->joints);
		YBR_FREE(m->skin->weights);
		YBR_FREE(m->skin);
	}
	for (int k = 0; k < m->vertexGroupCount; k++) {
		YBR_FREE(m->vertexGroups[k].name);
		YBR_FREE(m->vertexGroups[k].indices);
		YBR_FREE(m->vertexGroups[k].weights);
	}
	YBR_FREE(m->vertexGroups);
}

// アーマチュア

static int load_armature(const CborValue* v, YbrArmature* a)
{
	a->id = CborGetStr(v, "id");

	const CborValue* arr = CborGet(v, "bones");
	if (!arr || arr->type != CBOR_ARRAY || arr->count == 0) return 1;

	a->boneCount = (int)arr->count;
	a->bones = (YbrBone*)YBR_CALLOC(arr->count, sizeof(YbrBone));
	if (!a->bones) return 0;

	for (int k = 0; k < a->boneCount; k++) {
		const CborValue* b = &arr->items[k];
		YbrBone* o = &a->bones[k];
		o->name = CborGetStr(b, "name");
		o->parent = CborGetInt(b, "parent", -1);
		o->length = CborGetFlt(b, "length", 0.0f);
		cb_matrix(b, "rest", &o->rest);
		cb_matrix(b, "rest_parent", &o->restParent);
	}
	return 1;
}

static void free_armature(YbrArmature* a)
{
	YBR_FREE(a->id);
	for (int k = 0; k < a->boneCount; k++) YBR_FREE(a->bones[k].name);
	YBR_FREE(a->bones);
}

// カーブ

static int load_curve(const CborValue* v, YbrCurve* c)
{
	int n;
	c->id = CborGetStr(v, "id");
	c->is3d = CborGetBool(v, "is_3d", 1);

	const CborValue* arr = CborGet(v, "splines");
	if (!arr || arr->type != CBOR_ARRAY || arr->count == 0) return 1;

	c->splineCount = (int)arr->count;
	c->splines = (YbrSpline*)YBR_CALLOC(arr->count, sizeof(YbrSpline));
	if (!c->splines) return 0;

	for (int k = 0; k < c->splineCount; k++) {
		const CborValue* sp = &arr->items[k];
		YbrSpline* s = &c->splines[k];
		s->type = (YbrSplineType)CborGetCode(sp, "type", YBR_SPLINE_POLY);
		s->cyclic = CborGetBool(sp, "cyclic", 0);
		s->order = CborGetInt(sp, "order", 4);
		s->pointCount = CborGetInt(sp, "point_count", 0);

		s->points = CborGetF32(sp, "points", &n);
		s->handlesLeft = CborGetF32(sp, "handles_left", &n);
		s->handlesRight = CborGetF32(sp, "handles_right", &n);
		s->handleTypesLeft = CborGetU8(sp, "handle_types_left", &n);
		s->handleTypesRight = CborGetU8(sp, "handle_types_right", &n);
		s->weights = CborGetF32(sp, "weights", &n);
		s->tilts = CborGetF32(sp, "tilts", &n);
		s->radii = CborGetF32(sp, "radii", &n);
	}
	return 1;
}

static void free_curve(YbrCurve* c)
{
	YBR_FREE(c->id);
	for (int k = 0; k < c->splineCount; k++) {
		YbrSpline* s = &c->splines[k];
		YBR_FREE(s->points);
		YBR_FREE(s->handlesLeft);
		YBR_FREE(s->handlesRight);
		YBR_FREE(s->handleTypesLeft);
		YBR_FREE(s->handleTypesRight);
		YBR_FREE(s->weights);
		YBR_FREE(s->tilts);
		YBR_FREE(s->radii);
	}
	YBR_FREE(c->splines);
}

// ライト

static int load_light(const CborValue* v, YbrLight* l)
{
	l->id = CborGetStr(v, "id");
	l->type = (YbrLightType)CborGetCode(v, "type", YBR_LIGHT_POINT);
	l->color.x = l->color.y = l->color.z = 1.0f;
	cb_vec3(v, "color", &l->color);
	l->energy = CborGetFlt(v, "energy", 100.0f);
	l->useShadow = CborGetBool(v, "use_shadow", 1);
	l->radius = CborGetFlt(v, "radius", 0.0f);
	l->angle = CborGetFlt(v, "angle", 0.0f);
	l->spotSize = CborGetFlt(v, "spot_size", 0.0f);
	l->spotBlend = CborGetFlt(v, "spot_blend", 0.0f);
	l->shape = CborGetStr(v, "shape");
	l->size = CborGetFlt(v, "size", 0.0f);
	l->sizeY = CborGetFlt(v, "size_y", 0.0f);

	l->hasCutoff = CborGet(v, "cutoff_distance") ? 1 : 0;
	l->cutoffDistance = CborGetFlt(v, "cutoff_distance", 0.0f);
	return 1;
}

static void free_light(YbrLight* l)
{
	YBR_FREE(l->id);
	YBR_FREE(l->shape);
}

// カメラ

static int load_camera(const CborValue* v, YbrCamera* c)
{
	c->id = CborGetStr(v, "id");
	c->type = (YbrCameraType)CborGetCode(v, "type", YBR_CAMERA_PERSP);
	c->lens = CborGetFlt(v, "lens", 50.0f);
	c->sensorWidth = CborGetFlt(v, "sensor_width", 36.0f);
	c->sensorHeight = CborGetFlt(v, "sensor_height", 24.0f);
	c->sensorFit =
		(YbrSensorFit)CborGetCode(v, "sensor_fit", YBR_SENSOR_FIT_UNKNOWN);
	c->fovX = CborGetFlt(v, "fov_x", 0.0f);
	c->fovY = CborGetFlt(v, "fov_y", 0.0f);
	c->clipStart = CborGetFlt(v, "clip_start", 0.1f);
	c->clipEnd = CborGetFlt(v, "clip_end", 1000.0f);
	c->orthoScale = CborGetFlt(v, "ortho_scale", 6.0f);
	c->shiftX = CborGetFlt(v, "shift_x", 0.0f);
	c->shiftY = CborGetFlt(v, "shift_y", 0.0f);
	return 1;
}

static void free_camera(YbrCamera* c) { YBR_FREE(c->id); }

// マテリアル

static YbrTexture* load_texture_ref(const CborValue* map, const char* key)
{
	const CborValue* v = CborGet(map, key);
	if (!v || v->type != CBOR_MAP) return NULL;

	YbrTexture* t = (YbrTexture*)YBR_CALLOC(1, sizeof(YbrTexture));
	if (!t) return NULL;
	t->image = CborGetStr(v, "image");
	t->filepath = CborGetStr(v, "filepath");
	t->colorspace =
		(YbrColorSpace)CborGetCode(v, "colorspace", YBR_COLORSPACE_UNKNOWN);
	t->extension = (YbrTexWrap)CborGetCode(v, "extension", YBR_TEXWRAP_UNKNOWN);
	t->interpolation =
		(YbrTexFilter)CborGetCode(v, "interpolation", YBR_TEXFILTER_UNKNOWN);
	return t;
}

static void free_texture_ref(YbrTexture* t)
{
	if (!t) return;
	YBR_FREE(t->image);
	YBR_FREE(t->filepath);
	YBR_FREE(t);
}

static void load_socket(const CborValue* v, YbrShaderSocket* s)
{
	s->name = CborGetStr(v, "name");
	s->type = (YbrShaderSocketType)CborGetCode(v, "type", YBR_SS_UNKNOWN);

	const CborValue* dv = CborGet(v, "default");
	if (!dv) return;
	if (dv->type == CBOR_ARRAY) {
		float tmp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		int n = 0;
		while (n < 4 && (size_t)n < dv->count) {
			tmp[n] = (float)CborGetNumV(&dv->items[n], 0.0);
			n++;
		}
		s->value.x = tmp[0];
		s->value.y = tmp[1];
		s->value.z = tmp[2];
		s->value.w = tmp[3];
		s->valueCount = n;
	}
	else if (dv->type == CBOR_UINT || dv->type == CBOR_NINT ||
			 dv->type == CBOR_FLOAT || dv->type == CBOR_BOOL) {
		s->value.x = (float)CborGetNumV(dv, 0.0);
		s->valueCount = 1;
	}
}

static int load_sockets(const CborValue* node, const char* key,
						YbrShaderSocket** out, int* countOut)
{
	*out = NULL;
	*countOut = 0;
	const CborValue* arr = CborGet(node, key);
	if (!arr || arr->type != CBOR_ARRAY || arr->count == 0) return 1;

	*countOut = (int)arr->count;
	*out = (YbrShaderSocket*)YBR_CALLOC(arr->count, sizeof(YbrShaderSocket));
	if (!*out) return 0;
	for (int k = 0; k < *countOut; k++) load_socket(&arr->items[k], &(*out)[k]);
	return 1;
}

static int load_props(const CborValue* node, YbrShaderNode* n)
{
	const CborValue* props = CborGet(node, "props");
	if (!props || props->type != CBOR_MAP || props->count == 0) return 1;

	n->propCount = (int)props->count;
	n->props = (YbrProp*)YBR_CALLOC(props->count, sizeof(YbrProp));
	if (!n->props) return 0;

	for (int i = 0; i < n->propCount; i++) {
		const CborValue* k = &props->keys[i];
		const CborValue* v = &props->items[i];
		YbrProp* p = &n->props[i];
		p->name = CborDup(k);
		switch (v->type) {
			case CBOR_TEXT:
				p->type = YBR_PROP_STRING;
				p->text = CborDup(v);
				break;
			case CBOR_BOOL:
				p->type = YBR_PROP_BOOL;
				p->number = v->b ? 1.0 : 0.0;
				break;
			case CBOR_UINT:
			case CBOR_NINT:
				p->type = YBR_PROP_INT;
				p->number = CborGetNumV(v, 0.0);
				break;
			case CBOR_FLOAT:
				p->type = YBR_PROP_FLOAT;
				p->number = CborGetNumV(v, 0.0);
				break;
			case CBOR_ARRAY:
				p->type = YBR_PROP_ARRAY;
				p->valueCount = (int)v->count;
				if (0 < p->valueCount) {
					p->values = (float*)YBR_CALLOC((size_t)p->valueCount,
												   sizeof(float));
					if (p->values)
						for (int j = 0; j < p->valueCount; j++)
							p->values[j] =
								(float)CborGetNumV(&v->items[j], 0.0);
				}
				break;
			default:
				p->type = YBR_PROP_FLOAT;
				p->number = 0.0;
				break;
		}
	}
	return 1;
}

// nodes / links を読み込む (MATERIAL と NODEGROUP で共用)
static int load_shader_graph(const CborValue* v, YbrShaderNode** nodesOut,
							 int* nodeCountOut, YbrShaderLink** linksOut,
							 int* linkCountOut)
{
	const CborValue* arr = CborGet(v, "nodes");
	if (arr && arr->type == CBOR_ARRAY && 0 < arr->count) {
		*nodeCountOut = (int)arr->count;
		*nodesOut =
			(YbrShaderNode*)YBR_CALLOC(arr->count, sizeof(YbrShaderNode));
		if (!*nodesOut) return 0;
		for (int k = 0; k < *nodeCountOut; k++) {
			const CborValue* sn = &arr->items[k];
			YbrShaderNode* n = &(*nodesOut)[k];
			n->name = CborGetStr(sn, "name");
			n->type =
				(YbrShaderNodeType)CborGetCode(sn, "type", YBR_SN_UNKNOWN);
			n->label = CborGetStr(sn, "label");
			if (!load_sockets(sn, "inputs", &n->inputs, &n->inputCount))
				return 0;
			if (!load_sockets(sn, "outputs", &n->outputs, &n->outputCount))
				return 0;
			if (!load_props(sn, n)) return 0;
		}
	}

	arr = CborGet(v, "links");
	if (arr && arr->type == CBOR_ARRAY && 0 < arr->count) {
		*linkCountOut = (int)arr->count;
		*linksOut =
			(YbrShaderLink*)YBR_CALLOC(arr->count, sizeof(YbrShaderLink));
		if (!*linksOut) return 0;
		for (int k = 0; k < *linkCountOut; k++) {
			const CborValue* l = &arr->items[k];
			(*linksOut)[k].fromNode = CborGetInt(l, "from_node", -1);
			(*linksOut)[k].fromSocket = CborGetInt(l, "from_socket", -1);
			(*linksOut)[k].toNode = CborGetInt(l, "to_node", -1);
			(*linksOut)[k].toSocket = CborGetInt(l, "to_socket", -1);
		}
	}
	return 1;
}

static int load_node_group(const CborValue* v, YbrNodeGroup* g)
{
	g->id = CborGetStr(v, "id");
	if (!load_sockets(v, "inputs", &g->inputs, &g->inputCount)) return 0;
	if (!load_sockets(v, "outputs", &g->outputs, &g->outputCount)) return 0;
	return load_shader_graph(v, &g->nodes, &g->nodeCount, &g->links,
							 &g->linkCount);
}

static int load_material(const CborValue* v, YbrMaterial* m)
{
	m->id = CborGetStr(v, "id");
	m->renderMethod = CborGetStr(v, "render_method");
	m->backfaceCulling = CborGetBool(v, "backface_culling", 0);
	m->transparent = CborGetBool(v, "transparent", 0);
	m->mode = CborIs(v, "mode", "PRO") ? YBR_MATERIAL_PRO : YBR_MATERIAL_SIMPLE;

	if (m->mode == YBR_MATERIAL_SIMPLE) {
		m->baseColor.x = m->baseColor.y = m->baseColor.z = 0.8f;
		m->baseColor.w = 1.0f;
		cb_vec4(v, "base_color", &m->baseColor);
		m->specular = CborGetFlt(v, "specular", 0.5f);
		m->metallic = CborGetFlt(v, "metallic", 0.0f);
		m->roughness = CborGetFlt(v, "roughness", 0.5f);
		m->alpha = CborGetFlt(v, "alpha", 1.0f);
		m->normalStrength = CborGetFlt(v, "normal_strength", 1.0f);

		m->baseColorMap = load_texture_ref(v, "base_color_map");
		m->specularMap = load_texture_ref(v, "specular_map");
		m->metallicMap = load_texture_ref(v, "metallic_map");
		m->roughnessMap = load_texture_ref(v, "roughness_map");
		m->alphaMap = load_texture_ref(v, "alpha_map");
		m->normalMap = load_texture_ref(v, "normal_map");
		return 1;
	}

	// PROモード
	return load_shader_graph(v, &m->nodes, &m->nodeCount, &m->links,
							 &m->linkCount);
}

static void free_sockets(YbrShaderSocket* s, int count)
{
	for (int k = 0; k < count; k++) YBR_FREE(s[k].name);
	YBR_FREE(s);
}

static void free_shader_graph(YbrShaderNode* nodes, int nodeCount,
							  YbrShaderLink* links)
{
	for (int k = 0; k < nodeCount; k++) {
		YbrShaderNode* n = &nodes[k];
		YBR_FREE(n->name);
		YBR_FREE(n->label);
		free_sockets(n->inputs, n->inputCount);
		free_sockets(n->outputs, n->outputCount);
		for (int p = 0; p < n->propCount; p++) {
			YBR_FREE(n->props[p].name);
			YBR_FREE(n->props[p].text);
			YBR_FREE(n->props[p].values);
		}
		YBR_FREE(n->props);
	}
	YBR_FREE(nodes);
	YBR_FREE(links);
}

static void free_node_group(YbrNodeGroup* g)
{
	YBR_FREE(g->id);
	free_sockets(g->inputs, g->inputCount);
	free_sockets(g->outputs, g->outputCount);
	free_shader_graph(g->nodes, g->nodeCount, g->links);
}

static void free_material(YbrMaterial* m)
{
	YBR_FREE(m->id);
	YBR_FREE(m->renderMethod);
	free_texture_ref(m->baseColorMap);
	free_texture_ref(m->specularMap);
	free_texture_ref(m->metallicMap);
	free_texture_ref(m->roughnessMap);
	free_texture_ref(m->alphaMap);
	free_texture_ref(m->normalMap);

	free_shader_graph(m->nodes, m->nodeCount, m->links);
}

// テクスチャ本体

static int load_texture_data(const CborValue* v, YbrTextureData* t)
{
	int n;
	t->id = CborGetStr(v, "id");
	t->name = CborGetStr(v, "name");
	t->filepath = CborGetStr(v, "filepath");
	t->colorspace =
		(YbrColorSpace)CborGetCode(v, "colorspace", YBR_COLORSPACE_UNKNOWN);
	t->width = CborGetInt(v, "width", 0);
	t->height = CborGetInt(v, "height", 0);
	t->embedded = CborGetBool(v, "embedded", 0);
	t->quality = CborGetInt(v, "quality", 0);

	if (CborIs(v, "compression", "PNG"))
		t->compression = YBR_TEX_PNG;
	else if (CborIs(v, "compression", "JPEG"))
		t->compression = YBR_TEX_JPEG;
	else
		t->compression = YBR_TEX_RAW;

	if (!t->embedded) return 1;

	if (t->compression == YBR_TEX_RAW) {
		t->pixels = CborGetU8(v, "pixels", &n);
		if (!t->pixels || n < t->width * t->height * 4) {
			YBR_FREE(t->pixels);
			t->pixels = NULL;
			t->embedded = 0;
		}
	}
	else {
		t->data = CborGetU8(v, "data", &n);
		t->dataSize = n;
		if (!t->data || n <= 0) {
			YBR_FREE(t->data);
			t->data = NULL;
			t->dataSize = 0;
			t->embedded = 0;
		}
	}
	return 1;
}

static void free_texture_data(YbrTextureData* t)
{
	YBR_FREE(t->id);
	YBR_FREE(t->name);
	YBR_FREE(t->filepath);
	YBR_FREE(t->pixels);
	YBR_FREE(t->data);
}

const char* YbrTextureFileExt(const YbrTextureData* tex)
{
	if (!tex) return "";
	switch (tex->compression) {
		case YBR_TEX_PNG:
			return ".png";
		case YBR_TEX_JPEG:
			return ".jpg";
		default:
			return "";
	}
}

// アニメーション (ベイク済み)

// ポーズマーカー。frame の昇順に並べ直してから返す。
// name が無いものと frame が負のものは捨てる。
static int load_markers(const CborValue* v, YbrAnimation* a)
{
	const CborValue* arr = CborGet(v, "markers");
	if (!arr || arr->type != CBOR_ARRAY || arr->count == 0) return 1;

	a->markers = (YbrAnimMarker*)YBR_CALLOC(arr->count, sizeof(YbrAnimMarker));
	if (!a->markers) return 0;

	int n = 0;
	for (size_t k = 0; k < arr->count; k++) {
		const CborValue* e = &arr->items[k];
		if (!e || e->type != CBOR_MAP) continue;
		char* name = CborGetStr(e, "name");
		int frame = CborGetInt(e, "frame", -1);
		if (!name || frame < 0) {
			YBR_FREE(name);
			continue;
		}
		a->markers[n].name = name;
		a->markers[n].frame = frame;
		n++;
	}
	a->markerCount = n;

	// 挿入ソート (マーカーはたいてい数個で、たいてい既に整列している)
	for (int i = 1; i < n; i++) {
		YbrAnimMarker t = a->markers[i];
		int j = i - 1;
		while (0 <= j && t.frame < a->markers[j].frame) {
			a->markers[j + 1] = a->markers[j];
			j--;
		}
		a->markers[j + 1] = t;
	}
	return 1;
}

static int load_animation(const CborValue* v, YbrAnimation* a)
{
	a->id = CborGetStr(v, "id");
	a->object = CborGetStr(v, "object");
	a->fps = CborGetFlt(v, "fps", 24.0f);
	a->frameCount = CborGetInt(v, "frame_count", 0);
	a->sincA = CborGetInt(v, "sinc_a", YBR_SINC_A_DEFAULT);
	if (a->sincA < 1) a->sincA = 1;
	if (YBR_SINC_A_MAX < a->sincA) a->sincA = YBR_SINC_A_MAX;

	if (!load_markers(v, a)) return 0;

	const CborValue* arr = CborGet(v, "tracks");
	if (!arr || arr->type != CBOR_ARRAY || arr->count == 0) return 1;

	a->trackCount = (int)arr->count;
	a->tracks = (YbrAnimTrack*)YBR_CALLOC(arr->count, sizeof(YbrAnimTrack));
	if (!a->tracks) return 0;

	for (int k = 0; k < a->trackCount; k++) {
		const CborValue* tr = &arr->items[k];
		YbrAnimTrack* o = &a->tracks[k];
		o->object = CborGetStr(tr, "object");
		o->bone = CborGetStr(tr, "bone");

		int nFrames = 0, nTypes = 0, nInterps = 0, nMat = 0, nTan = 0;
		unsigned int* frames = CborGetU32(tr, "frames", &nFrames);
		unsigned char* types = CborGetU8(tr, "types", &nTypes);
		unsigned char* interps = CborGetU8(tr, "interps", &nInterps);
		float* mats = CborGetF32(tr, "transforms", &nMat);
		float* tans = CborGetF32(tr, "tangents", &nTan);

		int count = CborGetInt(tr, "frame_count", nFrames);
		if (nFrames < count) count = nFrames;
		if (nMat / 16 < count) count = nMat / 16;
		if (count < 0) count = 0;

		if (0 < count && frames && mats) {
			o->frames =
				(YbrAnimFrame*)YBR_CALLOC((size_t)count, sizeof(YbrAnimFrame));
			if (!o->frames) {
				YBR_FREE(frames);
				YBR_FREE(types);
				YBR_FREE(interps);
				YBR_FREE(mats);
				YBR_FREE(tans);
				return 0;
			}

			// HERMITE 用の接線 (無ければ NULL のまま)。
			// 要素数が count*10 なら in == out として書かれている。
			int perKey = 0;
			if (tans && count * 2 * YBR_TANGENT_FLOATS <= nTan)
				perKey = 2;
			else if (tans && count * YBR_TANGENT_FLOATS <= nTan)
				perKey = 1;

			if (0 < perKey) {
				o->tangents = (YbrAnimTangent*)YBR_CALLOC(
					(size_t)count * 2, sizeof(YbrAnimTangent));
				if (!o->tangents) {
					YBR_FREE(frames);
					YBR_FREE(types);
					YBR_FREE(interps);
					YBR_FREE(mats);
					YBR_FREE(tans);
					return 0;
				}
			}

			// 知らないフレームタイプのキーは読み飛ばす (前方互換)。
			// 詰めて格納するので、接線も同じ位置へ移し替える。
			int out = 0;
			for (int i = 0; i < count; i++) {
				int type =
					(types && i < nTypes) ? (int)types[i] : YBR_FRAME_TRANSFORM;
				if (type != YBR_FRAME_TRANSFORM) continue;

				YbrAnimFrame* f = &o->frames[out];
				f->frame = (int)frames[i];
				f->type = YBR_FRAME_TRANSFORM;
				f->interp =
					(YbrInterp)((interps && i < nInterps) ? interps[i] : 0);
				if (YBR_INTERP_COUNT <= (int)f->interp)
					f->interp = YBR_INTERP_LINEAR;
				f->transform = mat_from_floats(mats + (size_t)i * 16);

				if (o->tangents) {
					for (int side = 0; side < 2; side++) {
						int src = (perKey == 2) ? (i * 2 + side) : i;
						const float* t =
							tans + (size_t)src * YBR_TANGENT_FLOATS;
						YbrAnimTangent d;
						d.translation.x = t[0];
						d.translation.y = t[1];
						d.translation.z = t[2];
						d.rotation.x = t[3];
						d.rotation.y = t[4];
						d.rotation.z = t[5];
						d.rotation.w = t[6];
						d.scale.x = t[7];
						d.scale.y = t[8];
						d.scale.z = t[9];
						o->tangents[out * 2 + side] = d;
					}
				}
				out++;
			}
			o->frameCount = out;
			if (out == 0) {
				YBR_FREE(o->frames);
				o->frames = NULL;
				YBR_FREE(o->tangents);
				o->tangents = NULL;
			}
		}

		YBR_FREE(frames);
		YBR_FREE(types);
		YBR_FREE(interps);
		YBR_FREE(mats);
		YBR_FREE(tans);
	}
	return 1;
}

static void free_animation(YbrAnimation* a)
{
	YBR_FREE(a->id);
	YBR_FREE(a->object);
	for (int k = 0; k < a->trackCount; k++) {
		YBR_FREE(a->tracks[k].object);
		YBR_FREE(a->tracks[k].bone);
		YBR_FREE(a->tracks[k].frames);
		YBR_FREE(a->tracks[k].tangents);
	}
	YBR_FREE(a->tracks);

	for (int k = 0; k < a->markerCount; k++) YBR_FREE(a->markers[k].name);
	YBR_FREE(a->markers);
}

// 読み込んだ内容の検証

// 検証中の注意書き。読み込み自体は続ける (最後の 1 件が YbrGetError() に残る)
static void ybr_warn(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(ybrError, sizeof(ybrError), fmt, ap);
	va_end(ap);
}

static int validate_mesh(YbrMesh* m, int index)
{
	char owner[128];
	snprintf(owner, sizeof(owner), "mesh[%d] '%s'", index, m->id ? m->id : "?");

	if (m->vertexCount < 0 || m->triangleCount < 0) {
		ybr_err("YBR: %s: negative vertex / triangle count", owner);
		return 0;
	}
	if (0 < m->vertexCount && !m->positions) {
		ybr_err("YBR: %s: vertex_count is %d but there are no positions", owner,
				m->vertexCount);
		return 0;
	}
	if (0 < m->triangleCount && !m->indices) {
		ybr_err("YBR: %s: triangle_count is %d but there are no indices", owner,
				m->triangleCount);
		return 0;
	}

	// index が頂点数の外を指していたら弾く (描画で確実に落ちるため)
	for (int i = 0; i < m->triangleCount * 3; i++) {
		if (m->vertexCount <= (int)m->indices[i]) {
			ybr_err("YBR: %s: index %u is out of range (vertex_count %d)",
					owner, m->indices[i], m->vertexCount);
			return 0;
		}
	}
	if (m->materialIndices) {
		int groups = (0 < m->materialCount) ? m->materialCount : 1;
		for (int i = 0; i < m->triangleCount; i++) {
			if (groups <= (int)m->materialIndices[i]) {
				ybr_err(
					"YBR: %s: material index %u is out of range (%d materials)",
					owner, m->materialIndices[i], groups);
				return 0;
			}
		}
	}

	// 頂点グループは無くても動くので、壊れていれば捨てる
	for (int k = 0; k < m->vertexGroupCount; k++) {
		YbrVertexGroup* g = &m->vertexGroups[k];
		if (!g->indices || !g->weights) {
			YBR_FREE(g->indices);
			g->indices = NULL;
			YBR_FREE(g->weights);
			g->weights = NULL;
			g->count = 0;
			continue;
		}
		for (int i = 0; i < g->count; i++) {
			if (m->vertexCount <= (int)g->indices[i]) {
				ybr_warn(
					"YBR: %s: vertex group '%s' points outside the mesh, "
					"dropped",
					owner, g->name ? g->name : "?");
				YBR_FREE(g->indices);
				g->indices = NULL;
				YBR_FREE(g->weights);
				g->weights = NULL;
				g->count = 0;
				break;
			}
		}
	}
	return 1;
}

static int validate_armature(const YbrArmature* a, int index)
{
	for (int i = 0; i < a->boneCount; i++) {
		int p = a->bones[i].parent;
		// 親は必ず自分より前 (読み込み側はその前提で行列を積む)
		if (p < -1 || i <= p) {
			ybr_err("YBR: armature[%d] '%s': bone %d has a bad parent index %d",
					index, a->id ? a->id : "?", i, p);
			return 0;
		}
	}
	return 1;
}

static int validate_animation(YbrAnimation* a, int index)
{
	char owner[128];
	snprintf(owner, sizeof(owner), "animation[%d] '%s'", index,
			 a->id ? a->id : "?");

	if (a->frameCount < 0) a->frameCount = 0;
	if (!(0.0f < a->fps)) a->fps = 24.0f;

	for (int k = 0; k < a->trackCount; k++) {
		const YbrAnimTrack* t = &a->tracks[k];
		for (int i = 1; i < t->frameCount; i++) {
			if (t->frames[i].frame <= t->frames[i - 1].frame) {
				ybr_err(
					"YBR: %s: track %d: frame numbers are not increasing "
					"(%d after %d)",
					owner, k, t->frames[i].frame, t->frames[i - 1].frame);
				return 0;
			}
		}
	}
	return 1;
}

static int validate_texture(YbrTextureData* t, int index)
{
	if (t->width < 0 || t->height < 0) {
		ybr_err("YBR: texture[%d] '%s': negative size", index,
				t->id ? t->id : "?");
		return 0;
	}
	// 画素データの長さは load_texture_data() が読み込むそばから確かめて、
	return 1;
}

static int validate_scene(YbrScene* sc)
{
	for (int i = 0; i < sc->meshCount; i++)
		if (!validate_mesh(&sc->meshes[i], i)) return 0;
	if (sc->armature && !validate_armature(sc->armature, 0)) return 0;
	for (int i = 0; i < sc->animationCount; i++)
		if (!validate_animation(&sc->animations[i], i)) return 0;
	for (int i = 0; i < sc->textureCount; i++)
		if (!validate_texture(&sc->textures[i], i)) return 0;

	// スキンのボーン番号はアーマチュアが分かって初めて調べられる
	for (int i = 0; i < sc->meshCount; i++) {
		YbrMesh* m = &sc->meshes[i];
		if (!m->skin || !m->skin->joints) continue;
		const YbrArmature* arm = sc->armature;
		if (!arm || !id_eq(arm->id, m->armatureData))
			continue;  // 参照先が無いのは描画側で弾く
		for (int v = 0; v < m->vertexCount * 4; v++) {
			if (arm->boneCount <= (int)m->skin->joints[v]) {
				ybr_err(
					"YBR: mesh[%d] '%s': skin joint %u is out of range "
					"(%d bones)",
					i, m->id ? m->id : "?", m->skin->joints[v], arm->boneCount);
				return 0;
			}
		}
	}
	return 1;
}

// ロード

YbrScene* YbrLoadFromMemory(const unsigned char* data, size_t size)
{
	ybrError[0] = '\0';

	if (!data || size < 4) {
		ybr_err("YBR: empty data");
		return NULL;
	}

	CborValue root;
	if (!CborParse(data, size, &root)) {
		CborFree(&root);
		ybr_err("YBR: CBOR parse failed");
		return NULL;
	}
	if (root.type != CBOR_ARRAY || root.count < 4) {
		CborFree(&root);
		ybr_err("YBR: root must be an array of 4 elements");
		return NULL;
	}

	const CborValue* magic = &root.items[0];
	const CborValue* verval = &root.items[1];
	const CborValue* tree = &root.items[2];
	const CborValue* blocks = &root.items[3];

	if (magic->type != CBOR_TEXT || magic->len != 3 ||
		memcmp(magic->bytes, YBR_MAGIC, 3) != 0) {
		CborFree(&root);
		ybr_err("YBR: bad magic (expected \"%s\")", YBR_MAGIC);
		return NULL;
	}

	int version = (int)CborGetNumV(verval, 0.0);
	if (version != YBR_SUPPORTED_VERSION) {
		CborFree(&root);
		ybr_err("YBR: unsupported version %d (expected %d)", version,
				YBR_SUPPORTED_VERSION);
		return NULL;
	}
	if (tree->type != CBOR_ARRAY || blocks->type != CBOR_ARRAY) {
		CborFree(&root);
		ybr_err("YBR: scene tree / data section must be arrays");
		return NULL;
	}

	YbrScene* sc = (YbrScene*)YBR_CALLOC(1, sizeof(YbrScene));
	if (!sc) {
		CborFree(&root);
		ybr_err("YBR: out of memory");
		return NULL;
	}
	sc->version = version;

	// シーンツリー
	if (0 < tree->count) {
		sc->rootCount = (int)tree->count;
		sc->roots = (YbrNode*)YBR_CALLOC(tree->count, sizeof(YbrNode));
		if (!sc->roots) goto fail;
		for (int k = 0; k < sc->rootCount; k++)
			if (!load_node(&tree->items[k], &sc->roots[k])) {
				ybr_err("YBR: bad node");
				goto fail;
			}
	}

	// データブロック : 種類ごとに数える
	int armatureBlocks = 0;
	for (size_t k = 0; k < blocks->count; k++) {
		const CborValue* b = &blocks->items[k];
		if (CborIs(b, "kind", "MESH"))
			sc->meshCount++;
		else if (CborIs(b, "kind", "ARMATURE"))
			armatureBlocks++;
		else if (CborIs(b, "kind", "CURVE"))
			sc->curveCount++;
		else if (CborIs(b, "kind", "LIGHT"))
			sc->lightCount++;
		else if (CborIs(b, "kind", "EMPTY"))
			sc->emptyCount++;
		else if (CborIs(b, "kind", "MATERIAL"))
			sc->materialCount++;
		else if (CborIs(b, "kind", "ANIMATION"))
			sc->animationCount++;
		else if (CborIs(b, "kind", "NODEGROUP"))
			sc->nodeGroupCount++;
		else if (CborIs(b, "kind", "TEXTURE"))
			sc->textureCount++;
		else if (CborIs(b, "kind", "CAMERA"))
			sc->cameraCount++;
	}

	if (sc->meshCount)
		sc->meshes = (YbrMesh*)YBR_CALLOC(sc->meshCount, sizeof(YbrMesh));
	// アーマチュアブロックは1つしかないハズなので複数ある場合はエラー扱い
	if (0 < armatureBlocks) {
		if (1 < armatureBlocks)
			ybr_err(
				"YBR: %d ARMATURE blocks were found. Only the first one is "
				"used "
				"(the exporter merges armatures into one)",
				armatureBlocks);
		sc->armature = (YbrArmature*)YBR_CALLOC(1, sizeof(YbrArmature));
		if (!sc->armature) goto fail;
	}
	if (sc->curveCount)
		sc->curves = (YbrCurve*)YBR_CALLOC(sc->curveCount, sizeof(YbrCurve));
	if (sc->lightCount)
		sc->lights = (YbrLight*)YBR_CALLOC(sc->lightCount, sizeof(YbrLight));
	if (sc->emptyCount)
		sc->empties = (YbrEmpty*)YBR_CALLOC(sc->emptyCount, sizeof(YbrEmpty));
	if (sc->materialCount)
		sc->materials =
			(YbrMaterial*)YBR_CALLOC(sc->materialCount, sizeof(YbrMaterial));
	if (sc->nodeGroupCount)
		sc->nodeGroups =
			(YbrNodeGroup*)YBR_CALLOC(sc->nodeGroupCount, sizeof(YbrNodeGroup));
	if (sc->animationCount)
		sc->animations =
			(YbrAnimation*)YBR_CALLOC(sc->animationCount, sizeof(YbrAnimation));
	if (sc->textureCount)
		sc->textures = (YbrTextureData*)YBR_CALLOC(sc->textureCount,
												   sizeof(YbrTextureData));
	if (sc->cameraCount)
		sc->cameras =
			(YbrCamera*)YBR_CALLOC(sc->cameraCount, sizeof(YbrCamera));

	{
		int mi = 0, ai = 0, ci = 0, li = 0, ei = 0, ti = 0, ni = 0, xi = 0,
			ki = 0, gi = 0;
		for (size_t k = 0; k < blocks->count; k++) {
			const CborValue* b = &blocks->items[k];
			if (CborIs(b, "kind", "MESH")) {
				if (!load_mesh(b, &sc->meshes[mi++])) goto fail;
			}
			else if (CborIs(b, "kind", "ARMATURE")) {
				if (ai == 0 && !load_armature(b, sc->armature)) goto fail;
				ai++;
			}
			else if (CborIs(b, "kind", "CURVE")) {
				if (!load_curve(b, &sc->curves[ci++])) goto fail;
			}
			else if (CborIs(b, "kind", "LIGHT")) {
				if (!load_light(b, &sc->lights[li++])) goto fail;
			}
			else if (CborIs(b, "kind", "EMPTY")) {
				YbrEmpty* e = &sc->empties[ei++];
				e->id = CborGetStr(b, "id");
				e->displayType = CborGetStr(b, "display_type");
				e->displaySize = CborGetFlt(b, "display_size", 1.0f);
			}
			else if (CborIs(b, "kind", "MATERIAL")) {
				if (!load_material(b, &sc->materials[ti++])) goto fail;
			}
			else if (CborIs(b, "kind", "NODEGROUP")) {
				if (!load_node_group(b, &sc->nodeGroups[gi++])) goto fail;
			}
			else if (CborIs(b, "kind", "ANIMATION")) {
				if (!load_animation(b, &sc->animations[ni++])) goto fail;
			}
			else if (CborIs(b, "kind", "TEXTURE")) {
				if (!load_texture_data(b, &sc->textures[xi++])) goto fail;
			}
			else if (CborIs(b, "kind", "CAMERA")) {
				if (!load_camera(b, &sc->cameras[ki++])) goto fail;
			}
		}
	}

	// 壊れたファイルか？
	if (!validate_scene(sc)) goto fail;

	CborFree(&root);
	return sc;

fail:
	CborFree(&root);
	YbrUnload(sc);
	if (ybrError[0] == '\0') ybr_err("YBR: load failed");
	return NULL;
}

YbrScene* YbrLoad(const char* fileName)
{
	ybrError[0] = '\0';

	FILE* f = fopen(fileName, "rb");
	if (!f) {
		ybr_err("YBR: cannot open '%s'", fileName);
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0) {
		fclose(f);
		ybr_err("YBR: empty file");
		return NULL;
	}

	unsigned char* buf = (unsigned char*)YBR_MALLOC((size_t)size);
	if (!buf) {
		fclose(f);
		ybr_err("YBR: out of memory");
		return NULL;
	}

	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size) {
		YBR_FREE(buf);
		ybr_err("YBR: read error");
		return NULL;
	}

	YbrScene* sc = YbrLoadFromMemory(buf, got);
	YBR_FREE(buf);
	return sc;
}

void YbrUnload(YbrScene* scene)
{
	if (!scene) return;

	for (int k = 0; k < scene->rootCount; k++) free_node(&scene->roots[k]);
	YBR_FREE(scene->roots);

	for (int k = 0; k < scene->meshCount; k++) free_mesh(&scene->meshes[k]);
	YBR_FREE(scene->meshes);

	if (scene->armature) free_armature(scene->armature);
	YBR_FREE(scene->armature);

	for (int k = 0; k < scene->curveCount; k++) free_curve(&scene->curves[k]);
	YBR_FREE(scene->curves);

	for (int k = 0; k < scene->lightCount; k++) free_light(&scene->lights[k]);
	YBR_FREE(scene->lights);

	for (int k = 0; k < scene->emptyCount; k++) {
		YBR_FREE(scene->empties[k].id);
		YBR_FREE(scene->empties[k].displayType);
	}
	YBR_FREE(scene->empties);

	for (int k = 0; k < scene->materialCount; k++)
		free_material(&scene->materials[k]);
	YBR_FREE(scene->materials);

	for (int k = 0; k < scene->nodeGroupCount; k++)
		free_node_group(&scene->nodeGroups[k]);
	YBR_FREE(scene->nodeGroups);

	for (int k = 0; k < scene->animationCount; k++)
		free_animation(&scene->animations[k]);
	YBR_FREE(scene->animations);

	for (int k = 0; k < scene->textureCount; k++)
		free_texture_data(&scene->textures[k]);
	YBR_FREE(scene->textures);

	for (int k = 0; k < scene->cameraCount; k++)
		free_camera(&scene->cameras[k]);
	YBR_FREE(scene->cameras);

	YBR_FREE(scene);
}

// 検索 / 走査

#define YBR_FINDER(fn, type, field, count)                      \
	const type* fn(const YbrScene* s, const char* id)           \
	{                                                           \
		if (!s || !id) return NULL;                             \
		for (int k = 0; k < s->count; k++)                      \
			if (id_eq(s->field[k].id, id)) return &s->field[k]; \
		return NULL;                                            \
	}

YBR_FINDER(YbrFindMesh, YbrMesh, meshes, meshCount)
YBR_FINDER(YbrFindCurve, YbrCurve, curves, curveCount)
YBR_FINDER(YbrFindLight, YbrLight, lights, lightCount)
YBR_FINDER(YbrFindEmpty, YbrEmpty, empties, emptyCount)
YBR_FINDER(YbrFindMaterial, YbrMaterial, materials, materialCount)
YBR_FINDER(YbrFindTexture, YbrTextureData, textures, textureCount)
YBR_FINDER(YbrFindAnimation, YbrAnimation, animations, animationCount)
YBR_FINDER(YbrFindCamera, YbrCamera, cameras, cameraCount)

// アーマチュアは 1 つしかないので、id で探す関数は用意していない。
const YbrArmature* YbrGetArmature(const YbrScene* s)
{
	return s ? s->armature : NULL;
}

// マーカー
const YbrAnimMarker* YbrFindAnimMarker(const YbrAnimation* anim,
									   const char* name)
{
	if (!anim || !name) return NULL;
	for (int i = 0; i < anim->markerCount; i++)
		if (id_eq(anim->markers[i].name, name)) return &anim->markers[i];
	return NULL;
}

YBR_FINDER(YbrFindNodeGroup, YbrNodeGroup, nodeGroups, nodeGroupCount)

const YbrCustomProperty* YbrFindCustomProperty(const YbrNode* node,
											   const char* key)
{
	if (!node || !key) return NULL;
	for (int i = 0; i < node->customPropertyCount; i++) {
		const YbrCustomProperty* p = &node->customProperties[i];
		if (p->key && strcmp(p->key, key) == 0) return p;
	}
	return NULL;
}

double YbrGetCustomNumber(const YbrNode* node, const char* key, double fallback)
{
	const YbrCustomProperty* p = YbrFindCustomProperty(node, key);
	if (!p) return fallback;
	if (p->type != YBR_CUSTOM_BOOL && p->type != YBR_CUSTOM_INT &&
		p->type != YBR_CUSTOM_FLOAT)
		return fallback;
	return p->number;
}

const char* YbrGetCustomText(const YbrNode* node, const char* key,
							 const char* fallback)
{
	const YbrCustomProperty* p = YbrFindCustomProperty(node, key);
	if (!p || p->type != YBR_CUSTOM_STRING || !p->text) return fallback;
	return p->text;
}

int YbrAnimTrackTangentsSymmetric(const YbrAnimTrack* tr)
{
	if (!tr || !tr->tangents) return 1;
	for (int i = 0; i < tr->frameCount; i++) {
		const YbrAnimTangent* a = &tr->tangents[i * 2 + 0];
		const YbrAnimTangent* b = &tr->tangents[i * 2 + 1];
		if (memcmp(a, b, sizeof(YbrAnimTangent)) != 0) return 0;
	}
	return 1;
}

static const YbrNode* find_node_rec(const YbrNode* n, const char* name)
{
	if (id_eq(n->name, name)) return n;
	for (int k = 0; k < n->childCount; k++) {
		const YbrNode* r = find_node_rec(&n->children[k], name);
		if (r) return r;
	}
	return NULL;
}

const YbrNode* YbrFindNode(const YbrScene* scene, const char* name)
{
	if (!scene || !name) return NULL;
	for (int k = 0; k < scene->rootCount; k++) {
		const YbrNode* r = find_node_rec(&scene->roots[k], name);
		if (r) return r;
	}
	return NULL;
}

static void walk_rec(const YbrNode* n, const YbrNode* parent, int depth,
					 YbrNodeVisitor visitor, void* ud)
{
	if (!visitor(n, parent, depth, ud)) return;
	for (int k = 0; k < n->childCount; k++)
		walk_rec(&n->children[k], n, depth + 1, visitor, ud);
}

// ノードツリーからデータ ID の一致するノードのワールド行列を探す
static int find_node_world(const YbrNode* n, Matrix parent, YbrNodeType type,
						   const char* dataId, const char* nodeName,
						   Matrix* out)
{
	Matrix world = MatrixMultiply(n->matrix, parent);
	if (n->type == type) {
		int nameOk = (!nodeName || id_eq(n->name, nodeName));
		int dataOk = (!dataId || id_eq(n->dataId, dataId));
		if (nameOk && dataOk) {
			*out = world;
			return 1;
		}
	}
	for (int i = 0; i < n->childCount; i++)
		if (find_node_world(&n->children[i], world, type, dataId, nodeName,
							out))
			return 1;
	return 0;
}

int YbrSceneFindNodeWorld(const YbrScene* scene, YbrNodeType type,
						  const char* dataId, const char* nodeName, Matrix* out)
{
	if (!scene || !out) return 0;
	for (int r = 0; r < scene->rootCount; r++)
		if (find_node_world(&scene->roots[r], MatrixIdentity(), type, dataId,
							nodeName, out))
			return 1;
	return 0;
}

void YbrWalkNodes(const YbrScene* scene, YbrNodeVisitor visitor, void* userData)
{
	if (!scene || !visitor) return;
	for (int k = 0; k < scene->rootCount; k++)
		walk_rec(&scene->roots[k], NULL, 0, visitor, userData);
}

// ここから書き出し (YbrSave / YbrSaveToMemory)

// ノード

static void write_custom_property(CborWriter* w, const YbrCustomProperty* p)
{
	CborWriteMapHeader(w, 3);  // key, type, value
	CborWriteKeyText(w, "key", p->key);
	CborWriteKeyInt(w, "type", (long long)p->type);

	CborWriteText(w, "value");
	switch (p->type) {
		case YBR_CUSTOM_STRING:
			CborWriteTextOrNull(w, p->text);
			break;
		case YBR_CUSTOM_BOOL:
			CborWriteBool(w, p->number != 0.0);
			break;
		case YBR_CUSTOM_ARRAY:
			CborWriteF32Array(w, p->values, (size_t)p->valueCount);
			break;
		case YBR_CUSTOM_INT:
			CborWriteInt(w, (long long)p->number);
			break;
		default:  // YBR_CUSTOM_FLOAT
			CborWriteFloat(w, p->number);
			break;
	}
}

static void write_node(CborWriter* w, const YbrNode* n)
{
	// name, type, data, matrix, custom_properties, children
	CborWriteMapHeader(w, 6);
	CborWriteKeyText(w, "name", n->name);
	CborWriteKeyInt(w, "type", (long long)n->type);
	CborWriteKeyText(w, "data", n->dataId);

	float m[16];
	mat_to_floats(n->matrix, m);
	CborWriteKeyFloatArray(w, "matrix", m, 16);	 // CBOR 配列（blob ではない）

	CborWriteKeyArrayHeader(w, "custom_properties",
							(size_t)n->customPropertyCount);
	for (int i = 0; i < n->customPropertyCount; i++)
		write_custom_property(w, &n->customProperties[i]);

	CborWriteKeyArrayHeader(w, "children", (size_t)n->childCount);
	for (int i = 0; i < n->childCount; i++) write_node(w, &n->children[i]);
}

static void write_scene_tree(CborWriter* w, const YbrScene* scene)
{
	CborWriteArrayHeader(w, (size_t)scene->rootCount);
	for (int i = 0; i < scene->rootCount; i++) write_node(w, &scene->roots[i]);
}

// メッシュ

static void write_skin(CborWriter* w, const YbrSkin* s, int vertexCount)
{
	CborWriteMapHeader(w, 3);  // influences, joints, weights
	CborWriteKeyInt(w, "influences", s->influences);
	CborWriteKeyU16Array(w, "joints", s->joints, (size_t)vertexCount * 4);
	CborWriteKeyF32Array(w, "weights", s->weights, (size_t)vertexCount * 4);
}

static void write_vertex_group(CborWriter* w, const YbrVertexGroup* g)
{
	CborWriteMapHeader(w, 5);  // name, index, count, indices, weights
	CborWriteKeyText(w, "name", g->name);
	CborWriteKeyInt(w, "index", g->index);
	CborWriteKeyInt(w, "count", g->count);
	CborWriteKeyU32Array(w, "indices", g->indices, (size_t)g->count);
	CborWriteKeyF32Array(w, "weights", g->weights, (size_t)g->count);
}

static void write_mesh(CborWriter* w, const YbrMesh* m)
{
	// kind, id, vertex_count, triangle_count, positions, normals, uvs, colors,
	CborWriteMapHeader(w, 17);
	CborWriteKeyText(w, "kind", "MESH");
	CborWriteKeyText(w, "id", m->id);
	CborWriteKeyInt(w, "vertex_count", m->vertexCount);
	CborWriteKeyInt(w, "triangle_count", m->triangleCount);
	CborWriteKeyF32Array(w, "positions", m->positions,
						 (size_t)m->vertexCount * 3);
	CborWriteKeyF32Array(w, "normals", m->normals, (size_t)m->vertexCount * 3);
	CborWriteKeyF32Array(w, "uvs", m->texcoords, (size_t)m->vertexCount * 2);
	CborWriteKeyU8Array(w, "colors", m->colors, (size_t)m->vertexCount * 4);
	CborWriteKeyF32Array(w, "tangents", m->tangents,
						 (size_t)m->vertexCount * 4);
	CborWriteKeyU32Array(w, "indices", m->indices,
						 (size_t)m->triangleCount * 3);
	CborWriteKeyU32Array(w, "material_indices", m->materialIndices,
						 (size_t)m->triangleCount);
	CborWriteKeyU32Array(w, "vertex_map", m->vertexMap, (size_t)m->vertexCount);

	CborWriteKeyArrayHeader(w, "materials", (size_t)m->materialCount);
	for (int i = 0; i < m->materialCount; i++)
		CborWriteTextOrNull(w, m->materials[i]);

	CborWriteKeyText(w, "armature", m->armature);
	CborWriteKeyText(w, "armature_data", m->armatureData);

	CborWriteText(w, "skin");
	if (m->skin)
		write_skin(w, m->skin, m->vertexCount);
	else
		CborWriteNull(w);

	CborWriteKeyArrayHeader(w, "vertex_groups", (size_t)m->vertexGroupCount);
	for (int i = 0; i < m->vertexGroupCount; i++)
		write_vertex_group(w, &m->vertexGroups[i]);
}

// アーマチュア

static void write_bone(CborWriter* w, const YbrBone* b)
{
	CborWriteMapHeader(w, 5);  // name, parent, rest, rest_parent, length
	CborWriteKeyText(w, "name", b->name);
	CborWriteKeyInt(w, "parent", b->parent);

	float rest[16], restParent[16];
	mat_to_floats(b->rest, rest);
	mat_to_floats(b->restParent, restParent);
	CborWriteKeyFloatArray(w, "rest", rest, 16);			   // CBOR 配列
	CborWriteKeyFloatArray(w, "rest_parent", restParent, 16);  // CBOR 配列
	CborWriteKeyFloat(w, "length", b->length);
}

static void write_armature(CborWriter* w, const YbrArmature* a)
{
	CborWriteMapHeader(w, 3);  // kind, id, bones
	CborWriteKeyText(w, "kind", "ARMATURE");
	CborWriteKeyText(w, "id", a->id);
	CborWriteKeyArrayHeader(w, "bones", (size_t)a->boneCount);
	for (int i = 0; i < a->boneCount; i++) write_bone(w, &a->bones[i]);
}

// カーブ

static void write_spline(CborWriter* w, const YbrSpline* s)
{
	// type, cyclic, order, point_count, points, handles_left, handles_right,
	// handle_types_left, handle_types_right, weights, tilts, radii
	CborWriteMapHeader(w, 12);
	CborWriteKeyInt(w, "type", (long long)s->type);
	CborWriteKeyBool(w, "cyclic", s->cyclic);
	CborWriteKeyInt(w, "order", s->order);
	CborWriteKeyInt(w, "point_count", s->pointCount);
	CborWriteKeyF32Array(w, "points", s->points, (size_t)s->pointCount * 3);
	CborWriteKeyF32Array(w, "handles_left", s->handlesLeft,
						 (size_t)s->pointCount * 3);
	CborWriteKeyF32Array(w, "handles_right", s->handlesRight,
						 (size_t)s->pointCount * 3);
	CborWriteKeyU8Array(w, "handle_types_left", s->handleTypesLeft,
						(size_t)s->pointCount);
	CborWriteKeyU8Array(w, "handle_types_right", s->handleTypesRight,
						(size_t)s->pointCount);
	CborWriteKeyF32Array(w, "weights", s->weights, (size_t)s->pointCount);
	CborWriteKeyF32Array(w, "tilts", s->tilts, (size_t)s->pointCount);
	CborWriteKeyF32Array(w, "radii", s->radii, (size_t)s->pointCount);
}

static void write_curve(CborWriter* w, const YbrCurve* c)
{
	CborWriteMapHeader(w, 4);  // kind, id, is_3d, splines
	CborWriteKeyText(w, "kind", "CURVE");
	CborWriteKeyText(w, "id", c->id);
	CborWriteKeyBool(w, "is_3d", c->is3d);
	CborWriteKeyArrayHeader(w, "splines", (size_t)c->splineCount);
	for (int i = 0; i < c->splineCount; i++) write_spline(w, &c->splines[i]);
}

// ライト

static void write_light(CborWriter* w, const YbrLight* l)
{
	// kind, id, type, color, energy, use_shadow, radius, angle, spot_size,
	// spot_blend, shape, size, size_y [, cutoff_distance]
	size_t count = 13;
	if (l->hasCutoff) count++;

	CborWriteMapHeader(w, count);
	CborWriteKeyText(w, "kind", "LIGHT");
	CborWriteKeyText(w, "id", l->id);
	CborWriteKeyInt(w, "type", (long long)l->type);

	float col[3] = {l->color.x, l->color.y, l->color.z};
	CborWriteKeyFloatArray(w, "color", col, 3);	 // CBOR 配列

	CborWriteKeyFloat(w, "energy", l->energy);
	CborWriteKeyBool(w, "use_shadow", l->useShadow);
	CborWriteKeyFloat(w, "radius", l->radius);
	CborWriteKeyFloat(w, "angle", l->angle);
	CborWriteKeyFloat(w, "spot_size", l->spotSize);
	CborWriteKeyFloat(w, "spot_blend", l->spotBlend);
	CborWriteKeyText(w, "shape", l->shape);
	CborWriteKeyFloat(w, "size", l->size);
	CborWriteKeyFloat(w, "size_y", l->sizeY);

	// hasCutoff が偽のときはキー自体を書かない (存在チェックで読まれるため)
	if (l->hasCutoff)
		CborWriteKeyFloat(w, "cutoff_distance", l->cutoffDistance);
}

// カメラ

static void write_camera(CborWriter* w, const YbrCamera* c)
{
	// kind, id, type, lens, sensor_width, sensor_height, sensor_fit,
	// fov_x, fov_y, clip_start, clip_end, ortho_scale, shift_x, shift_y
	CborWriteMapHeader(w, 14);
	CborWriteKeyText(w, "kind", "CAMERA");
	CborWriteKeyText(w, "id", c->id);
	CborWriteKeyInt(w, "type", (long long)c->type);
	CborWriteKeyFloat(w, "lens", c->lens);
	CborWriteKeyFloat(w, "sensor_width", c->sensorWidth);
	CborWriteKeyFloat(w, "sensor_height", c->sensorHeight);
	CborWriteKeyInt(w, "sensor_fit", (long long)c->sensorFit);
	CborWriteKeyFloat(w, "fov_x", c->fovX);
	CborWriteKeyFloat(w, "fov_y", c->fovY);
	CborWriteKeyFloat(w, "clip_start", c->clipStart);
	CborWriteKeyFloat(w, "clip_end", c->clipEnd);
	CborWriteKeyFloat(w, "ortho_scale", c->orthoScale);
	CborWriteKeyFloat(w, "shift_x", c->shiftX);
	CborWriteKeyFloat(w, "shift_y", c->shiftY);
}

// マテリアル

static void write_texture_ref(CborWriter* w, const YbrTexture* t)
{
	if (!t) {
		CborWriteNull(w);
		return;
	}
	// image, filepath, colorspace, extension, interpolation
	CborWriteMapHeader(w, 5);
	CborWriteKeyText(w, "image", t->image);
	CborWriteKeyText(w, "filepath", t->filepath);
	CborWriteKeyInt(w, "colorspace", (long long)t->colorspace);
	CborWriteKeyInt(w, "extension", (long long)t->extension);
	CborWriteKeyInt(w, "interpolation", (long long)t->interpolation);
}

static void write_shader_socket(CborWriter* w, const YbrShaderSocket* s)
{
	// name, type, default
	CborWriteMapHeader(w, 3);
	CborWriteKeyText(w, "name", s->name);
	CborWriteKeyInt(w, "type", (long long)s->type);

	CborWriteText(w, "default");
	if (s->valueCount <= 0) {
		CborWriteNull(w);
	}
	else if (s->valueCount == 1) {
		CborWriteFloat(w, s->value.x);
	}
	else {
		float v[4] = {s->value.x, s->value.y, s->value.z, s->value.w};
		CborWriteArrayHeader(w, (size_t)s->valueCount);
		for (int i = 0; i < s->valueCount && i < 4; i++)
			CborWriteFloat(w, v[i]);
	}
}

static void write_prop(CborWriter* w, const YbrProp* p)
{
	CborWriteText(w, p->name);
	switch (p->type) {
		case YBR_PROP_STRING:
			CborWriteTextOrNull(w, p->text);
			break;
		case YBR_PROP_BOOL:
			CborWriteBool(w, p->number != 0.0);
			break;
		case YBR_PROP_INT:
			CborWriteInt(w, (long long)p->number);
			break;
		case YBR_PROP_ARRAY:
			CborWriteArrayHeader(w, (size_t)p->valueCount);
			for (int i = 0; i < p->valueCount; i++)
				CborWriteFloat(w, (double)p->values[i]);
			break;
		default:  // YBR_PROP_FLOAT
			CborWriteFloat(w, p->number);
			break;
	}
}

static void write_shader_node(CborWriter* w, const YbrShaderNode* n)
{
	// name, type, label, inputs, outputs, props
	CborWriteMapHeader(w, 6);

	CborWriteKeyText(w, "name", n->name);
	CborWriteKeyInt(w, "type", (long long)n->type);
	CborWriteKeyText(w, "label", n->label);

	CborWriteKeyArrayHeader(w, "inputs", (size_t)n->inputCount);
	for (int i = 0; i < n->inputCount; i++)
		write_shader_socket(w, &n->inputs[i]);

	CborWriteKeyArrayHeader(w, "outputs", (size_t)n->outputCount);
	for (int i = 0; i < n->outputCount; i++)
		write_shader_socket(w, &n->outputs[i]);

	CborWriteKeyMapHeader(w, "props", (size_t)n->propCount);
	for (int i = 0; i < n->propCount; i++) write_prop(w, &n->props[i]);
}

static void write_shader_link(CborWriter* w, const YbrShaderLink* l)
{
	// from_node, from_socket, to_node, to_socket
	CborWriteMapHeader(w, 4);
	CborWriteKeyInt(w, "from_node", l->fromNode);
	CborWriteKeyInt(w, "from_socket", l->fromSocket);
	CborWriteKeyInt(w, "to_node", l->toNode);
	CborWriteKeyInt(w, "to_socket", l->toSocket);
}

static void write_node_group(CborWriter* w, const YbrNodeGroup* g)
{
	// kind, id, inputs, outputs, nodes, links
	CborWriteMapHeader(w, 6);
	CborWriteKeyText(w, "kind", "NODEGROUP");
	CborWriteKeyText(w, "id", g->id);

	CborWriteKeyArrayHeader(w, "inputs", (size_t)g->inputCount);
	for (int i = 0; i < g->inputCount; i++)
		write_shader_socket(w, &g->inputs[i]);

	CborWriteKeyArrayHeader(w, "outputs", (size_t)g->outputCount);
	for (int i = 0; i < g->outputCount; i++)
		write_shader_socket(w, &g->outputs[i]);

	CborWriteKeyArrayHeader(w, "nodes", (size_t)g->nodeCount);
	for (int i = 0; i < g->nodeCount; i++) write_shader_node(w, &g->nodes[i]);

	CborWriteKeyArrayHeader(w, "links", (size_t)g->linkCount);
	for (int i = 0; i < g->linkCount; i++) write_shader_link(w, &g->links[i]);
}

static void write_material(CborWriter* w, const YbrMaterial* m)
{
	if (m->mode == YBR_MATERIAL_SIMPLE) {
		// kind, id, mode, render_method, backface_culling, transparent,
		CborWriteMapHeader(w, 18);
		CborWriteKeyText(w, "kind", "MATERIAL");
		CborWriteKeyText(w, "id", m->id);
		CborWriteKeyText(w, "mode", "SIMPLE");
		CborWriteKeyText(w, "render_method", m->renderMethod);
		CborWriteKeyBool(w, "backface_culling", m->backfaceCulling);
		CborWriteKeyBool(w, "transparent", m->transparent);

		float base[4] = {m->baseColor.x, m->baseColor.y, m->baseColor.z,
						 m->baseColor.w};
		CborWriteKeyFloatArray(w, "base_color", base, 4);  // CBOR 配列
		CborWriteKeyFloat(w, "specular", m->specular);
		CborWriteKeyFloat(w, "metallic", m->metallic);
		CborWriteKeyFloat(w, "roughness", m->roughness);
		CborWriteKeyFloat(w, "alpha", m->alpha);
		CborWriteKeyFloat(w, "normal_strength", m->normalStrength);

		CborWriteText(w, "base_color_map");
		write_texture_ref(w, m->baseColorMap);
		CborWriteText(w, "specular_map");
		write_texture_ref(w, m->specularMap);
		CborWriteText(w, "metallic_map");
		write_texture_ref(w, m->metallicMap);
		CborWriteText(w, "roughness_map");
		write_texture_ref(w, m->roughnessMap);
		CborWriteText(w, "alpha_map");
		write_texture_ref(w, m->alphaMap);
		CborWriteText(w, "normal_map");
		write_texture_ref(w, m->normalMap);
		return;
	}

	// PRO
	// kind, id, mode, render_method, backface_culling, transparent, nodes,
	// links
	CborWriteMapHeader(w, 8);
	CborWriteKeyText(w, "kind", "MATERIAL");
	CborWriteKeyText(w, "id", m->id);
	CborWriteKeyText(w, "mode", "PRO");
	CborWriteKeyText(w, "render_method", m->renderMethod);
	CborWriteKeyBool(w, "backface_culling", m->backfaceCulling);
	CborWriteKeyBool(w, "transparent", m->transparent);

	CborWriteKeyArrayHeader(w, "nodes", (size_t)m->nodeCount);
	for (int i = 0; i < m->nodeCount; i++) write_shader_node(w, &m->nodes[i]);

	CborWriteKeyArrayHeader(w, "links", (size_t)m->linkCount);
	for (int i = 0; i < m->linkCount; i++) write_shader_link(w, &m->links[i]);
}

// テクスチャ本体

static void write_texture_data(CborWriter* w, const YbrTextureData* t)
{
	// kind, id, name, filepath, colorspace, width, height, embedded,
	// compression, format, quality, pixels, data
	CborWriteMapHeader(w, 13);
	CborWriteKeyText(w, "kind", "TEXTURE");
	CborWriteKeyText(w, "id", t->id);
	CborWriteKeyText(w, "name", t->name);
	CborWriteKeyText(w, "filepath", t->filepath);
	CborWriteKeyInt(w, "colorspace", (long long)t->colorspace);
	CborWriteKeyInt(w, "width", t->width);
	CborWriteKeyInt(w, "height", t->height);
	CborWriteKeyBool(w, "embedded", t->embedded);

	const char* comp = (t->compression == YBR_TEX_PNG)	  ? "PNG"
					   : (t->compression == YBR_TEX_JPEG) ? "JPEG"
														  : "NONE";
	CborWriteKeyText(w, "compression", comp);
	CborWriteKeyText(w, "format", "RGBA8");
	CborWriteKeyInt(w, "quality", t->quality);

	if (t->embedded && t->compression == YBR_TEX_RAW)
		CborWriteKeyU8Array(w, "pixels", t->pixels,
							(size_t)t->width * (size_t)t->height * 4);
	else
		CborWriteKeyNull(w, "pixels");

	if (t->embedded && t->compression != YBR_TEX_RAW)
		CborWriteKeyU8Array(w, "data", t->data, (size_t)t->dataSize);
	else
		CborWriteKeyNull(w, "data");
}

// アニメーション

static void write_anim_track(CborWriter* w, const YbrAnimTrack* tr)
{
	// object, bone, frame_count, frames, types, interps, transforms, tangents
	CborWriteMapHeader(w, 8);
	CborWriteKeyText(w, "object", tr->object);
	CborWriteKeyText(w, "bone", tr->bone);
	CborWriteKeyInt(w, "frame_count", tr->frameCount);

	int n = tr->frameCount;
	if (n <= 0) {
		CborWriteKeyNull(w, "frames");
		CborWriteKeyNull(w, "types");
		CborWriteKeyNull(w, "interps");
		CborWriteKeyNull(w, "transforms");
		CborWriteKeyNull(w, "tangents");
		return;
	}

	unsigned int* frames =
		(unsigned int*)YBR_MALLOC((size_t)n * sizeof(unsigned int));
	unsigned char* types = (unsigned char*)YBR_MALLOC((size_t)n);
	unsigned char* interps = (unsigned char*)YBR_MALLOC((size_t)n);
	float* mats = (float*)YBR_MALLOC((size_t)n * 16 * sizeof(float));

	if (frames && types && interps && mats) {
		for (int i = 0; i < n; i++) {
			frames[i] = (unsigned int)tr->frames[i].frame;
			types[i] = (unsigned char)tr->frames[i].type;
			interps[i] = (unsigned char)tr->frames[i].interp;
			mat_to_floats(tr->frames[i].transform, mats + (size_t)i * 16);
		}
		CborWriteKeyU32Array(w, "frames", frames, (size_t)n);
		CborWriteKeyU8Array(w, "types", types, (size_t)n);
		CborWriteKeyU8Array(w, "interps", interps, (size_t)n);
		CborWriteKeyF32Array(w, "transforms", mats, (size_t)n * 16);

		// 接線は HERMITE を使うキーがあるときだけ書く (無ければ null)。
		// in == out ならキーあたり 1 つだけ書いてサイズを半分にする。
		int needTangents = 0;
		if (tr->tangents) {
			for (int i = 0; i < n; i++)
				if (tr->frames[i].interp == YBR_INTERP_HERMITE) {
					needTangents = 1;
					break;
				}
		}
		if (needTangents) {
			int perKey = YbrAnimTrackTangentsSymmetric(tr) ? 1 : 2;
			size_t total = (size_t)n * (size_t)perKey * YBR_TANGENT_FLOATS;
			float* tans = (float*)YBR_MALLOC(total * sizeof(float));
			if (tans) {
				for (int i = 0; i < n * perKey; i++) {
					const YbrAnimTangent* s =
						(perKey == 2) ? &tr->tangents[i] : &tr->tangents[i * 2];
					float* t = tans + (size_t)i * YBR_TANGENT_FLOATS;
					t[0] = s->translation.x;
					t[1] = s->translation.y;
					t[2] = s->translation.z;
					t[3] = s->rotation.x;
					t[4] = s->rotation.y;
					t[5] = s->rotation.z;
					t[6] = s->rotation.w;
					t[7] = s->scale.x;
					t[8] = s->scale.y;
					t[9] = s->scale.z;
				}
				CborWriteKeyF32Array(w, "tangents", tans, total);
				YBR_FREE(tans);
			}
			else {
				CborWriteKeyNull(w, "tangents");
				w->failed = 1;
			}
		}
		else {
			CborWriteKeyNull(w, "tangents");
		}
	}
	else {
		CborWriteKeyNull(w, "frames");
		CborWriteKeyNull(w, "types");
		CborWriteKeyNull(w, "interps");
		CborWriteKeyNull(w, "transforms");
		CborWriteKeyNull(w, "tangents");
		w->failed = 1;
	}

	YBR_FREE(frames);
	YBR_FREE(types);
	YBR_FREE(interps);
	YBR_FREE(mats);
}

static void write_anim_marker(CborWriter* w, const YbrAnimMarker* m)
{
	// name, frame
	CborWriteMapHeader(w, 2);
	CborWriteKeyText(w, "name", m->name);
	CborWriteKeyInt(w, "frame", m->frame);
}

static void write_animation(CborWriter* w, const YbrAnimation* a)
{
	// kind, id, object, fps, frame_count, sinc_a, tracks, markers
	int sincA = a->sincA;
	if (sincA < 1) sincA = YBR_SINC_A_DEFAULT;
	if (YBR_SINC_A_MAX < sincA) sincA = YBR_SINC_A_MAX;

	CborWriteMapHeader(w, 8);
	CborWriteKeyText(w, "kind", "ANIMATION");
	CborWriteKeyText(w, "id", a->id);
	CborWriteKeyText(w, "object", a->object);
	CborWriteKeyFloat(w, "fps", a->fps);
	CborWriteKeyInt(w, "frame_count", a->frameCount);
	CborWriteKeyInt(w, "sinc_a", sincA);

	CborWriteKeyArrayHeader(w, "tracks", (size_t)a->trackCount);
	for (int i = 0; i < a->trackCount; i++) write_anim_track(w, &a->tracks[i]);

	CborWriteKeyArrayHeader(w, "markers", (size_t)a->markerCount);
	for (int i = 0; i < a->markerCount; i++)
		write_anim_marker(w, &a->markers[i]);
}

// エンプティ

static void write_empty(CborWriter* w, const YbrEmpty* e)
{
	// kind, id, display_type, display_size
	CborWriteMapHeader(w, 4);
	CborWriteKeyText(w, "kind", "EMPTY");
	CborWriteKeyText(w, "id", e->id);
	CborWriteKeyText(w, "display_type", e->displayType);
	CborWriteKeyFloat(w, "display_size", e->displaySize);
}

// トップレベル

static void write_data_blocks(CborWriter* w, const YbrScene* scene)
{
	size_t total = (size_t)scene->meshCount + (scene->armature ? 1u : 0u) +
				   (size_t)scene->curveCount + (size_t)scene->lightCount +
				   (size_t)scene->materialCount + (size_t)scene->textureCount +
				   (size_t)scene->cameraCount + (size_t)scene->animationCount +
				   (size_t)scene->emptyCount + (size_t)scene->nodeGroupCount;
	CborWriteArrayHeader(w, total);

	for (int i = 0; i < scene->meshCount; i++) write_mesh(w, &scene->meshes[i]);
	if (scene->armature) write_armature(w, scene->armature);
	for (int i = 0; i < scene->curveCount; i++)
		write_curve(w, &scene->curves[i]);
	for (int i = 0; i < scene->lightCount; i++)
		write_light(w, &scene->lights[i]);
	for (int i = 0; i < scene->materialCount; i++)
		write_material(w, &scene->materials[i]);
	for (int i = 0; i < scene->nodeGroupCount; i++)
		write_node_group(w, &scene->nodeGroups[i]);
	for (int i = 0; i < scene->textureCount; i++)
		write_texture_data(w, &scene->textures[i]);
	for (int i = 0; i < scene->cameraCount; i++)
		write_camera(w, &scene->cameras[i]);
	for (int i = 0; i < scene->animationCount; i++)
		write_animation(w, &scene->animations[i]);
	for (int i = 0; i < scene->emptyCount; i++)
		write_empty(w, &scene->empties[i]);
}

unsigned char* YbrSaveToMemory(const YbrScene* scene, size_t* outSize)
{
	ybrError[0] = '\0';
	if (outSize) *outSize = 0;

	if (!scene) {
		ybr_err("YBR: scene is NULL");
		return NULL;
	}

	CborWriter w;
	CborWriterInit(&w);

	// ["YUI", version, tree, blocks]
	CborWriteArrayHeader(&w, 4);
	CborWriteText(&w, YBR_MAGIC);
	CborWriteInt(&w, YBR_SUPPORTED_VERSION);
	write_scene_tree(&w, scene);
	write_data_blocks(&w, scene);

	if (w.failed) {
		CborWriterFree(&w);
		ybr_err("YBR: out of memory while writing");
		return NULL;
	}

	size_t len;
	unsigned char* buf = CborWriterTake(&w, &len);
	if (!buf) {
		ybr_err("YBR: out of memory while writing");
		return NULL;
	}
	if (outSize) *outSize = len;
	return buf;
}

int YbrSave(const YbrScene* scene, const char* fileName)
{
	size_t size;
	unsigned char* buf = YbrSaveToMemory(scene, &size);
	if (!buf) return 0;

	FILE* f = fopen(fileName, "wb");
	if (!f) {
		ybr_err("YBR: cannot open '%s' for writing", fileName);
		YBR_FREE(buf);
		return 0;
	}
	size_t written = fwrite(buf, 1, size, f);
	fclose(f);
	YBR_FREE(buf);

	if (written != size) {
		ybr_err("YBR: write error on '%s'", fileName);
		return 0;
	}
	return 1;
}
