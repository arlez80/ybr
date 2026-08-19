/*
	Yui Blender to Raylib - シェーダー
		Programed by あるる（きのもと 結衣）
*/

#include "ybr_shader.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// uniform の上限。ノードが多い PRO マテリアルは
// テクスチャ / 定数 / ライト (1 灯 4 本) でそこそこ増えるので余裕を持たせる。
#define YBR_SHADER_MAX_UNIFORMS 192

// 文字列ビルダ

typedef struct StrBuf {
	char* data;
	size_t len;
	size_t cap;
	int failed;
} StrBuf;

static void sb_free(StrBuf* b)
{
	YBR_FREE(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

static int sb_reserve(StrBuf* b, size_t extra)
{
	if (b->failed) return 0;
	size_t need = b->len + extra + 1;
	if (need <= b->cap) return 1;

	size_t cap = b->cap ? b->cap : 1024;
	while (cap < need) cap *= 2;

	char* p = (char*)YBR_MALLOC(cap);
	if (!p) {
		b->failed = 1;
		return 0;
	}
	if (b->data) {
		memcpy(p, b->data, b->len + 1);
		YBR_FREE(b->data);
	}
	else {
		p[0] = '\0';
	}
	b->data = p;
	b->cap = cap;
	return 1;
}

static void sb_puts(StrBuf* b, const char* s)
{
	if (!s) return;
	size_t n = strlen(s);
	if (!sb_reserve(b, n)) return;
	memcpy(b->data + b->len, s, n + 1);
	b->len += n;
}

// 512 バイトに収まらない場合はいったんヒープに書き出す。
// 固定バッファだけで済ませると長い GLSL を黙って切り落としてしまう。
static void sb_printf(StrBuf* b, const char* fmt, ...)
{
	char tmp[512];
	va_list ap, ap2;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);

	if (n < 0) {
		va_end(ap2);
		b->failed = 1;
		return;
	}
	if ((size_t)n < sizeof(tmp)) {
		va_end(ap2);
		sb_puts(b, tmp);
		return;
	}

	char* big = (char*)YBR_MALLOC((size_t)n + 1);
	if (!big) {
		va_end(ap2);
		b->failed = 1;
		return;
	}
	vsnprintf(big, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	sb_puts(b, big);
	YBR_FREE(big);
}

static char* sb_take(StrBuf* b)
{
	if (b->failed || !b->data) {
		sb_free(b);
		return NULL;
	}
	char* p = b->data;
	b->data = NULL;
	b->len = b->cap = 0;
	return p;
}

static char* ybr_strdup(const char* s)
{
	if (!s) return NULL;
	size_t n = strlen(s) + 1;
	char* p = (char*)YBR_MALLOC(n);
	if (p) memcpy(p, s, n);
	return p;
}

// GLSL の方言

typedef struct GlslDialect {
	const char* version;	// #version 行
	const char* precision;	// precision 指定
	const char* vsIn;		// 頂点属性のキーワード
	const char* vsOut;		// 頂点 -> フラグメント（VS 側）
	const char* fsIn;		// 頂点 -> フラグメント（FS 側）
	const char* texture2D;	// テクスチャ取得関数名
	int hasFragColor;		// 1 なら out vec4 を自前で宣言
} GlslDialect;

static int glsl_dialect(int glVersion, GlslDialect* out)
{
	switch (glVersion) {
		case RL_OPENGL_21:
			out->version = "#version 120\n";
			out->precision = "";
			out->vsIn = "attribute";
			out->vsOut = "varying";
			out->fsIn = "varying";
			out->texture2D = "texture2D";
			out->hasFragColor = 0;
			return 1;

		case RL_OPENGL_33:
		case RL_OPENGL_43:
			out->version = "#version 330\n";
			out->precision = "";
			out->vsIn = "in";
			out->vsOut = "out";
			out->fsIn = "in";
			out->texture2D = "texture";
			out->hasFragColor = 1;
			return 1;

		case RL_OPENGL_ES_20:
			out->version = "#version 100\n";
			out->precision = "precision mediump float;\n";
			out->vsIn = "attribute";
			out->vsOut = "varying";
			out->fsIn = "varying";
			out->texture2D = "texture2D";
			out->hasFragColor = 0;
			return 1;

		case RL_OPENGL_ES_30:
			out->version = "#version 300 es\n";
			out->precision = "precision mediump float;\n";
			out->vsIn = "in";
			out->vsOut = "out";
			out->fsIn = "in";
			out->texture2D = "texture";
			out->hasFragColor = 1;
			return 1;

		default:  // RL_OPENGL_11 など
			return 0;
	}
}

// uniform 収集

// set->overflow : 上限超過 (メモリ不足と区別してメッセージを出し分ける)
typedef struct UniformSet {
	YbrShaderUniform items[YBR_SHADER_MAX_UNIFORMS];
	int count;
	int failed;
	int overflow;
} UniformSet;

// TEX_IMAGE / TEX_ENVIRONMENT ノードのラップ / フィルタ設定を読む
static void node_tex_props(const YbrShaderNode* n, int* wrap, int* filter);

static void uni_add_tex(UniformSet* set, const char* name,
						YbrShaderUniformType type, int locIndex, int autoSet,
						const float* value, int valueCount,
						const char* textureId, int texWrap, int texFilter);

static void uni_add(UniformSet* set, const char* name,
					YbrShaderUniformType type, int locIndex, int autoSet,
					const float* value, int valueCount, const char* textureId)
{
	uni_add_tex(set, name, type, locIndex, autoSet, value, valueCount,
				textureId, -1, -1);
}

static void uni_add_tex(UniformSet* set, const char* name,
						YbrShaderUniformType type, int locIndex, int autoSet,
						const float* value, int valueCount,
						const char* textureId, int texWrap, int texFilter)
{
	if (YBR_SHADER_MAX_UNIFORMS <= set->count) {
		set->failed = 1;
		set->overflow = 1;
		return;
	}
	if (set->failed) return;
	YbrShaderUniform* u = &set->items[set->count];
	memset(u, 0, sizeof(*u));
	u->name = ybr_strdup(name);
	if (!u->name) {
		set->failed = 1;
		return;
	}
	u->type = type;
	u->locIndex = locIndex;
	u->autoSet = autoSet;
	u->valueCount = valueCount;
	u->texWrap = texWrap;
	u->texFilter = texFilter;
	for (int i = 0; i < valueCount && i < 4; i++) u->value[i] = value[i];
	if (textureId) {
		u->textureId = ybr_strdup(textureId);
		if (!u->textureId) {
			set->failed = 1;
			return;
		}
	}
	set->count++;
}

static void uni_free_all(UniformSet* set)
{
	for (int i = 0; i < set->count; i++) {
		YBR_FREE(set->items[i].name);
		YBR_FREE(set->items[i].textureId);
	}
	set->count = 0;
}

// 頂点シェーダー

// skinBones > 0 なら GPU スキニング付きの頂点シェーダーを作る。
static char* build_vertex_ex(const GlslDialect* d, int skinBones)
{
	StrBuf b = {0};
	sb_puts(&b, d->version);
	sb_puts(&b, d->precision);
	sb_printf(&b, "%s vec3 vertexPosition;\n", d->vsIn);
	sb_printf(&b, "%s vec2 vertexTexCoord;\n", d->vsIn);
	sb_printf(&b, "%s vec3 vertexNormal;\n", d->vsIn);
	sb_printf(&b, "%s vec4 vertexColor;\n", d->vsIn);
	// xyz = 接線 / w = 従法線の向き。無いメッシュでは既定値 (0,0,0,0) が入る
	sb_printf(&b, "%s vec4 vertexTangent;\n", d->vsIn);
	if (0 < skinBones) {
		sb_printf(&b, "%s vec4 %s;\n", d->vsIn, YBR_SHADER_ATTRIB_BONE_IDS);
		sb_printf(&b, "%s vec4 %s;\n", d->vsIn, YBR_SHADER_ATTRIB_BONE_WEIGHTS);
	}
	sb_printf(&b, "%s vec2 fragTexCoord;\n", d->vsOut);
	sb_printf(&b, "%s vec4 fragColor;\n", d->vsOut);
	sb_printf(&b, "%s vec3 fragNormal;\n", d->vsOut);
	sb_printf(&b, "%s vec4 fragTangent;\n", d->vsOut);
	sb_printf(&b, "%s vec3 fragPosition;\n", d->vsOut);
	sb_puts(&b, "uniform mat4 mvp;\n");
	sb_puts(&b, "uniform mat4 matModel;\n");
	sb_puts(&b, "uniform mat4 matNormal;\n");

	if (0 < skinBones) {
		sb_printf(&b, "uniform vec4 %s[%d];\n", YBR_SHADER_UNIFORM_BONES,
				  skinBones * 4);
		sb_printf(&b,
				  "\n"
				  "mat4 ybrBoneMatrix(float index)\n"
				  "{\n"
				  "    int i = int(index)*4;\n"
				  "    return mat4(%s[i], %s[i + 1], %s[i + 2], %s[i + 3]);\n"
				  "}\n",
				  YBR_SHADER_UNIFORM_BONES, YBR_SHADER_UNIFORM_BONES,
				  YBR_SHADER_UNIFORM_BONES, YBR_SHADER_UNIFORM_BONES);
	}

	sb_puts(&b, "\nvoid main()\n{\n");
	sb_puts(&b, "    fragTexCoord = vertexTexCoord;\n");
	sb_puts(&b, "    fragColor = vertexColor;\n");

	if (0 < skinBones) {
		// skin は if の外で宣言する。中で宣言すると、
		// ブロックを抜けた接線の計算から見えなくなる (GLSL のスコープ)。
		sb_printf(&b,
				  "    vec3 pos = vertexPosition;\n"
				  "    vec3 nrm = vertexNormal;\n"
				  "    vec3 tan3 = vertexTangent.xyz;\n"
				  "    float wsum = dot(%s, vec4(1.0));\n"
				  "    mat4 skin = mat4(1.0);\n"
				  "    if (0.0 < wsum) {\n"
				  "        skin = ybrBoneMatrix(%s.x)*%s.x\n"
				  "             + ybrBoneMatrix(%s.y)*%s.y\n"
				  "             + ybrBoneMatrix(%s.z)*%s.z\n"
				  "             + ybrBoneMatrix(%s.w)*%s.w;\n"
				  "        skin = skin*(1.0/wsum);\n"
				  "        pos  = vec3(skin*vec4(vertexPosition, 1.0));\n"
				  "        nrm  = mat3(skin)*vertexNormal;\n"
				  "        tan3 = mat3(skin)*vertexTangent.xyz;\n"
				  "    }\n",
				  YBR_SHADER_ATTRIB_BONE_WEIGHTS, YBR_SHADER_ATTRIB_BONE_IDS,
				  YBR_SHADER_ATTRIB_BONE_WEIGHTS, YBR_SHADER_ATTRIB_BONE_IDS,
				  YBR_SHADER_ATTRIB_BONE_WEIGHTS, YBR_SHADER_ATTRIB_BONE_IDS,
				  YBR_SHADER_ATTRIB_BONE_WEIGHTS, YBR_SHADER_ATTRIB_BONE_IDS,
				  YBR_SHADER_ATTRIB_BONE_WEIGHTS);
		sb_puts(
			&b,
			"    fragNormal = normalize(vec3(matNormal*vec4(nrm, 1.0)));\n");
		sb_puts(&b,
				"    fragTangent = vec4(vec3(matModel*vec4(tan3, 0.0)),\n"
				"                       vertexTangent.w);\n");
		sb_puts(&b, "    fragPosition = vec3(matModel*vec4(pos, 1.0));\n");
		sb_puts(&b, "    gl_Position = mvp*vec4(pos, 1.0);\n");
	}
	else {
		sb_puts(&b,
				"    fragNormal = normalize(vec3(matNormal*vec4(vertexNormal, "
				"1.0)));\n");
		sb_puts(&b,
				"    fragTangent = vec4(vec3(matModel*vec4(vertexTangent.xyz, "
				"0.0)),\n"
				"                       vertexTangent.w);\n");
		sb_puts(
			&b,
			"    fragPosition = vec3(matModel*vec4(vertexPosition, 1.0));\n");
		sb_puts(&b, "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n");
	}
	sb_puts(&b, "}\n");
	return sb_take(&b);
}

static void emit_fs_header(StrBuf* b, const GlslDialect* d)
{
	sb_puts(b, d->version);
	sb_puts(b, d->precision);
	sb_printf(b, "%s vec2 fragTexCoord;\n", d->fsIn);
	sb_printf(b, "%s vec4 fragColor;\n", d->fsIn);
	sb_printf(b, "%s vec3 fragNormal;\n", d->fsIn);
	sb_printf(b, "%s vec4 fragTangent;\n", d->fsIn);
	sb_printf(b, "%s vec3 fragPosition;\n", d->fsIn);
	if (d->hasFragColor) sb_puts(b, "out vec4 finalColor;\n");
}

static void emit_fs_write(StrBuf* b, const GlslDialect* d, const char* expr)
{
	if (d->hasFragColor)
		sb_printf(b, "    finalColor = %s;\n", expr);
	else
		sb_printf(b, "    gl_FragColor = %s;\n", expr);
}

// 各ノードの変換

static int clamp_light_count(int n)
{
	if (n < 0) return 0;
	if (YBR_SHADER_MAX_LIGHTS < n) return YBR_SHADER_MAX_LIGHTS;
	return n;
}

// 影を落とせるのは先頭の N 灯まで (灯数と深度テクスチャの枚数で頭打ち)
static int clamp_shadow_count(int shadows, int lights)
{
	lights = clamp_light_count(lights);
	if (shadows < 0) shadows = 0;
	if (YBR_SHADER_MAX_SHADOWS < shadows) shadows = YBR_SHADER_MAX_SHADOWS;
	if (lights < shadows) shadows = lights;
	return shadows;
}

// viewPos は「視点」であってライトではない。Fresnel / Camera Data などが
// ライト無しでも使うので、独立して 1 度だけ宣言する。
static void emit_view_uniform(StrBuf* b, UniformSet* set, int* emitted)
{
	static const float viewPos[3] = {0.0f, 0.0f, 0.0f};
	if (emitted) {
		if (*emitted) return;
		*emitted = 1;
	}
	sb_puts(b, "uniform vec3 " YBR_SHADER_UNIFORM_VIEW_POS ";\n");
	uni_add(set, YBR_SHADER_UNIFORM_VIEW_POS, YBR_UNIFORM_VEC3,
			RL_SHADER_LOC_VECTOR_VIEW, 0, viewPos, 3, NULL);
}

// 共通のライティング uniform を宣言する。lightCount が 0 なら何も出さない。
static void emit_light_uniforms(StrBuf* b, UniformSet* set, int lightCount,
								int shadowCount, int* viewEmitted)
{
	// 既定の向きは少しずつずらして、複数灯でも全部同じにならないようにする
	static const float dirs[YBR_SHADER_MAX_LIGHTS][3] = {
		{-0.577f, -0.577f, -0.577f},
		{0.577f, -0.577f, 0.577f},
		{0.000f, -1.000f, 0.000f},
		{0.577f, -0.302f, -0.759f},
	};
	static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	static const float ambient[4] = {0.1f, 0.1f, 0.1f, 1.0f};
	static const float origin[3] = {0.0f, 0.0f, 0.0f};
	// 既定は方向光 (種類 0)。距離とコーンは使わない
	static const float params[4] = {0.0f, 0.0f, 1.0f, 0.0f};

	lightCount = clamp_light_count(lightCount);
	if (lightCount <= 0) return;

	for (int i = 0; i < lightCount; i++) {
		char nameDir[32], nameCol[32], namePos[32], namePar[32];
		snprintf(nameDir, sizeof(nameDir), "%s%d", YBR_SHADER_UNIFORM_LIGHT_DIR,
				 i);
		snprintf(nameCol, sizeof(nameCol), "%s%d",
				 YBR_SHADER_UNIFORM_LIGHT_COLOR, i);
		snprintf(namePos, sizeof(namePos), "%s%d", YBR_SHADER_UNIFORM_LIGHT_POS,
				 i);
		snprintf(namePar, sizeof(namePar), "%s%d",
				 YBR_SHADER_UNIFORM_LIGHT_PARAMS, i);
		sb_printf(b, "uniform vec3 %s;\n", nameDir);
		sb_printf(b, "uniform vec4 %s;\n", nameCol);
		sb_printf(b, "uniform vec3 %s;\n", namePos);
		sb_printf(b, "uniform vec4 %s;\n", namePar);
		uni_add(set, nameDir, YBR_UNIFORM_VEC3, -1, 0, dirs[i], 3, NULL);
		uni_add(set, nameCol, YBR_UNIFORM_VEC4, -1, 0, white, 4, NULL);
		uni_add(set, namePos, YBR_UNIFORM_VEC3, -1, 0, origin, 3, NULL);
		uni_add(set, namePar, YBR_UNIFORM_VEC4, -1, 0, params, 4, NULL);
	}
	// 影
	shadowCount = clamp_shadow_count(shadowCount, lightCount);
	for (int i = 0; i < shadowCount; i++) {
		char nameMap[40], nameVP[40], namePar[40];
		// (有効, 深度バイアス, テクセルの大きさ, 予備)。
		// 既定は「影なし」なので、描画側が入れなければ何も起きない。
		static const float shadowOff[4] = {0.0f, 0.002f, 1.0f / 2048.0f, 0.0f};
		snprintf(nameMap, sizeof(nameMap), "%s%d",
				 YBR_SHADER_UNIFORM_SHADOW_MAP, i);
		snprintf(nameVP, sizeof(nameVP), "%s%d", YBR_SHADER_UNIFORM_LIGHT_VP,
				 i);
		snprintf(namePar, sizeof(namePar), "%s%d",
				 YBR_SHADER_UNIFORM_SHADOW_PARAMS, i);
		sb_printf(b, "uniform sampler2D %s;\n", nameMap);
		sb_printf(b, "uniform mat4 %s;\n", nameVP);
		sb_printf(b, "uniform vec4 %s;\n", namePar);
		// サンプラーとしては登録しない (テクスチャ ID を持たないため)。
		// 束ねるのは描画側 (ybr_model.c)。
		uni_add(set, namePar, YBR_UNIFORM_VEC4, -1, 0, shadowOff, 4, NULL);
	}

	sb_puts(b, "uniform vec4 " YBR_SHADER_UNIFORM_AMBIENT ";\n");
	uni_add(set, YBR_SHADER_UNIFORM_AMBIENT, YBR_UNIFORM_VEC4,
			RL_SHADER_LOC_COLOR_AMBIENT, 0, ambient, 4, NULL);
	emit_view_uniform(b, set, viewEmitted);
}

// ybrShade / ybrShadeN を宣言する。
static void emit_shade_decl(StrBuf* b, int lightCount, int shadowCount,
							const char* texFn)
{
	lightCount = clamp_light_count(lightCount);
	shadowCount = clamp_shadow_count(shadowCount, lightCount);

	if (0 < shadowCount) {
		// 深度テクスチャと見比べて、日向なら 1 / 日陰なら 0 を返す。
		// 3x3 の平均 (PCF) で縁を少しだけぼかす。
		sb_printf(
			b,
			"\n"
			"/* params = (有効, 深度バイアス, テクセルの大きさ, 予備) */\n"
			"float ybrShadowAt(sampler2D shadowMap, mat4 lightVP, vec4 "
			"params)\n"
			"{\n"
			"    if (params.x < 0.5) return 1.0;\n"
			"\n"
			"    vec4 lp = lightVP*vec4(fragPosition, 1.0);\n"
			"    vec3 proj = lp.xyz/lp.w;\n"
			"    proj = proj*0.5 + 0.5;             /* -1..1 -> 0..1 */\n"
			"\n"
			"    /* 深度テクスチャの外は影なし扱い "
			"(端で急に暗くならないように) "
			"*/\n"
			"    if (proj.z > 1.0) return 1.0;\n"
			"    if (proj.x < 0.0 || 1.0 < proj.x) return 1.0;\n"
			"    if (proj.y < 0.0 || 1.0 < proj.y) return 1.0;\n"
			"\n"
			"    float cur = proj.z - params.y;     /* "
			"バイアスぶん手前にずらす "
			"*/\n"
			"    float sum = 0.0;\n"
			"    for (int y = -1; y <= 1; y++) {\n"
			"        for (int x = -1; x <= 1; x++) {\n"
			"            vec2 uv = proj.xy + vec2(float(x), "
			"float(y))*params.z;\n"
			"            float d = %s(shadowMap, uv).r;\n"
			"            sum += (cur <= d) ? 1.0 : 0.0;\n"
			"        }\n"
			"    }\n"
			"    return sum/9.0;\n"
			"}\n",
			texFn);
	}

	if (0 < lightCount) {
		// ライト 1 灯ぶんの「面からライトへ向かう向き」と減衰を求める。
		sb_puts(
			b,
			"\n"
			"/* params = (種類, 届く距離, 内側コーンの cos, 外側コーンの cos)\n"
			"   種類 : 0 = 平行光 / 1 = 点光源 / 2 = スポット\n"
			"   戻り値は減衰 (0..1)、l に面からライトへ向かう向きが入る */\n"
			"float ybrLightSample(vec3 dir, vec3 lpos, vec4 params, out vec3 "
			"l)\n"
			"{\n"
			"    vec3 d = normalize(dir);\n"
			"    if (params.x < 0.5) { l = -d; return 1.0; }\n"
			"\n"
			"    vec3 toLight = lpos - fragPosition;\n"
			"    float dist = length(toLight);\n"
			"    l = (0.000001 < dist) ? toLight/dist : -d;\n"
			"\n"
			"    /* 逆二乗。ライトに重なったところで発散しないよう 1 を足す "
			"*/\n"
			"    float att = 1.0/(1.0 + dist*dist);\n"
			"    if (0.0 < params.y) {\n"
			"        /* 届く距離でなめらかに 0 へ落とす */\n"
			"        float t = clamp(1.0 - pow(dist/params.y, 4.0), 0.0, "
			"1.0);\n"
			"        att *= t*t;\n"
			"    }\n"
			"    if (1.5 < params.x) {\n"
			"        /* スポット : 軸からの開き具合でコーンの縁をぼかす */\n"
			"        float cd = dot(d, -l);\n"
			"        att *= clamp((cd - params.w)/max(params.z - params.w, "
			"0.0001),\n"
			"                     0.0, 1.0);\n"
			"    }\n"
			"    return att;\n"
			"}\n");
	}

	sb_puts(
		b,
		"\n"
		"/* 法線を指定して陰影を付ける (法線マップなどで曲げた法線を渡す) */\n"
		"vec3 ybrShadeN(vec3 albedo, vec3 n, float metallic, float roughness, "
		"float specular)\n"
		"{\n");
	if (lightCount <= 0) {
		sb_puts(
			b,
			"    /* ライト 0 灯 : 陰影を付けず、指定した色をそのまま返す */\n"
			"    return albedo;\n"
			"}\n");
	}
	else {
		sb_puts(b,
				"    vec3 v = normalize(" YBR_SHADER_UNIFORM_VIEW_POS
				" - fragPosition);\n"
				"    float r = clamp(roughness, 0.0, 1.0);\n"
				"    float shininess = mix(256.0, 2.0, r);\n"
				"    float m = clamp(metallic, 0.0, 1.0);\n"
				"    vec3 specColor = mix(vec3(1.0), albedo, m);\n"
				"    vec3 sum = albedo*" YBR_SHADER_UNIFORM_AMBIENT ".rgb;\n");
		for (int i = 0; i < lightCount; i++) {
			sb_printf(
				b,
				"    {\n"
				"        vec3 l;\n"
				"        float att = ybrLightSample(%s%d, %s%d, %s%d, l);\n",
				YBR_SHADER_UNIFORM_LIGHT_DIR, i, YBR_SHADER_UNIFORM_LIGHT_POS,
				i, YBR_SHADER_UNIFORM_LIGHT_PARAMS, i);

			// 影を落とせるのは先頭の N 灯 (深度テクスチャの枚数ぶん)
			if (i < shadowCount)
				sb_printf(b, "        att *= ybrShadowAt(%s%d, %s%d, %s%d);\n",
						  YBR_SHADER_UNIFORM_SHADOW_MAP, i,
						  YBR_SHADER_UNIFORM_LIGHT_VP, i,
						  YBR_SHADER_UNIFORM_SHADOW_PARAMS, i);

			sb_printf(
				b,
				"        vec3 h = normalize(l + v);\n"
				"        float ndotl = max(dot(n, l), 0.0);\n"
				"        float sp = (0.0 < ndotl) ? pow(max(dot(n, h), 0.0), "
				"shininess) : 0.0;\n"
				"        sp *= specular*(1.0 - r);\n"
				"        sum += (albedo*(1.0 - m)*%s%d.rgb*ndotl\n"
				"                + specColor*%s%d.rgb*sp)*att;\n"
				"    }\n",
				YBR_SHADER_UNIFORM_LIGHT_COLOR, i,
				YBR_SHADER_UNIFORM_LIGHT_COLOR, i);
		}
		sb_puts(b, "    return sum;\n}\n");
	}
	sb_puts(b,
			"vec3 ybrShade(vec3 albedo, float metallic, float roughness, "
			"float specular)\n"
			"{\n"
			"    return ybrShadeN(albedo, normalize(fragNormal), metallic, "
			"roughness, specular);\n"
			"}\n");
}

int YbrShaderIsNodeSupported(YbrShaderNodeType type)
{
	switch (type) {
		// 出力 / 入力
		case YBR_SN_REROUTE:
		case YBR_SN_RGB:
		case YBR_SN_VALUE:
		case YBR_SN_ATTRIBUTE:
		case YBR_SN_NEW_GEOMETRY:
		case YBR_SN_TEX_COORD:
		case YBR_SN_UVMAP:
		case YBR_SN_OBJECT_INFO:
		case YBR_SN_CAMERA:
		case YBR_SN_LIGHT_PATH:
		case YBR_SN_PARTICLE_INFO:
		case YBR_SN_HAIR_INFO:
		case YBR_SN_POINT_INFO:
		case YBR_SN_VOLUME_INFO:
		case YBR_SN_WIREFRAME:
		case YBR_SN_BEVEL:
		case YBR_SN_AMBIENT_OCCLUSION:
		case YBR_SN_TANGENT:
		case YBR_SN_FRESNEL:
		case YBR_SN_LAYER_WEIGHT:
		// テクスチャ
		case YBR_SN_TEX_IMAGE:
		case YBR_SN_TEX_ENVIRONMENT:
		case YBR_SN_TEX_NOISE:
		case YBR_SN_TEX_CHECKER:
		case YBR_SN_TEX_BRICK:
		case YBR_SN_TEX_GRADIENT:
		case YBR_SN_TEX_MAGIC:
		case YBR_SN_TEX_VORONOI:
		case YBR_SN_TEX_WAVE:
		case YBR_SN_TEX_WHITE_NOISE:
		case YBR_SN_TEX_MUSGRAVE:
		case YBR_SN_TEX_SKY:
		case YBR_SN_TEX_IES:
		case YBR_SN_TEX_POINTDENSITY:
		// カラー / 変換
		case YBR_SN_MIX_RGB:
		case YBR_SN_MIX:
		case YBR_SN_VALTORGB:
		case YBR_SN_RGBTOBW:
		case YBR_SN_MATH:
		case YBR_SN_VECT_MATH:
		case YBR_SN_VECT_TRANSFORM:
		case YBR_SN_SEPRGB:
		case YBR_SN_COMBRGB:
		case YBR_SN_SEPHSV:
		case YBR_SN_COMBHSV:
		case YBR_SN_SEPXYZ:
		case YBR_SN_COMBXYZ:
		case YBR_SN_HUE_SAT:
		case YBR_SN_BRIGHTCONTRAST:
		case YBR_SN_GAMMA:
		case YBR_SN_INVERT:
		case YBR_SN_CURVE_RGB:
		case YBR_SN_CURVE_VEC:
		case YBR_SN_CURVE_FLOAT:
		case YBR_SN_FLOAT_CURVE:
		case YBR_SN_CLAMP:
		case YBR_SN_MAP_RANGE:
		case YBR_SN_BLACKBODY:
		case YBR_SN_WAVELENGTH:
		// ベクトル / 法線
		case YBR_SN_NORMAL:
		case YBR_SN_NORMAL_MAP:
		case YBR_SN_BUMP:
		case YBR_SN_DISPLACEMENT:
		case YBR_SN_VECTOR_DISPLACEMENT:
		case YBR_SN_MAPPING:
		case YBR_SN_VECTOR_ROTATE:
		// シェーダー
		case YBR_SN_BSDF_PRINCIPLED:
		case YBR_SN_BSDF_DIFFUSE:
		case YBR_SN_BSDF_GLOSSY:
		case YBR_SN_BSDF_GLASS:
		case YBR_SN_BSDF_REFRACTION:
		case YBR_SN_BSDF_TRANSLUCENT:
		case YBR_SN_BSDF_TRANSPARENT:
		case YBR_SN_BSDF_VELVET:
		case YBR_SN_BSDF_TOON:
		case YBR_SN_BSDF_ANISOTROPIC:
		case YBR_SN_SUBSURFACE_SCATTERING:
		case YBR_SN_EMISSION:
		case YBR_SN_BACKGROUND:
		case YBR_SN_HOLDOUT:
		case YBR_SN_ADD_SHADER:
		case YBR_SN_MIX_SHADER:
		case YBR_SN_SHADERTORGB:
		case YBR_SN_LIGHT_FALLOFF:
		case YBR_SN_VOLUME_ABSORPTION:
		case YBR_SN_VOLUME_SCATTER:
		case YBR_SN_PRINCIPLED_VOLUME:
		case YBR_SN_BSDF_HAIR:
		case YBR_SN_BSDF_HAIR_PRINCIPLED:
		// グループ
		case YBR_SN_GROUP:
		case YBR_SN_GROUP_INPUT:
		case YBR_SN_GROUP_OUTPUT:
			return 1;
		default:
			return 0;
	}
}

int YbrShaderUniformFormat(YbrShaderUniformType type)
{
	switch (type) {
		case YBR_UNIFORM_FLOAT:
			return SHADER_UNIFORM_FLOAT;
		case YBR_UNIFORM_VEC2:
			return SHADER_UNIFORM_VEC2;
		case YBR_UNIFORM_VEC3:
			return SHADER_UNIFORM_VEC3;
		case YBR_UNIFORM_VEC4:
			return SHADER_UNIFORM_VEC4;
		case YBR_UNIFORM_INT:
			return SHADER_UNIFORM_INT;
		case YBR_UNIFORM_SAMPLER2D:
			return SHADER_UNIFORM_SAMPLER2D;
		default:
			return 0;
	}
}

static YbrShaderResult shader_error(YbrShaderError err, const char* msg)
{
	YbrShaderResult r;
	memset(&r, 0, sizeof(r));
	r.glVersion = YBR_GL_UNKNOWN;
	r.error = err;
	r.errorMessage = msg;
	return r;
}

// ノードグラフ全体の変換

static char shaderErrorBuf[256];

const char* YbrShaderErrorString(YbrShaderError error)
{
	switch (error) {
		case YBR_SHADER_OK:
			return "ok";
		case YBR_SHADER_ERR_NULL_NODE:
			return "null node";
		case YBR_SHADER_ERR_UNSUPPORTED_GL:
			return "unsupported OpenGL version";
		case YBR_SHADER_ERR_UNSUPPORTED_NODE:
			return "unsupported shader node";
		case YBR_SHADER_ERR_MISSING_SOCKET:
			return "missing socket";
		case YBR_SHADER_ERR_OUT_OF_MEMORY:
			return "out of memory";
		case YBR_SHADER_ERR_TOO_MANY_UNIFORMS:
			return "too many uniforms";
		default:
			return "unknown error";
	}
}

const char* YbrShaderNodeTypeName(YbrShaderNodeType type)
{
	switch (type) {
		case YBR_SN_UNKNOWN:
			return "UNKNOWN";
		case YBR_SN_OUTPUT_MATERIAL:
			return "OUTPUT_MATERIAL";
		case YBR_SN_OUTPUT_WORLD:
			return "OUTPUT_WORLD";
		case YBR_SN_OUTPUT_LIGHT:
			return "OUTPUT_LIGHT";
		case YBR_SN_OUTPUT_AOV:
			return "OUTPUT_AOV";
		case YBR_SN_OUTPUT_LINESTYLE:
			return "OUTPUT_LINESTYLE";
		case YBR_SN_BSDF_PRINCIPLED:
			return "BSDF_PRINCIPLED";
		case YBR_SN_BSDF_DIFFUSE:
			return "BSDF_DIFFUSE";
		case YBR_SN_BSDF_GLOSSY:
			return "BSDF_GLOSSY";
		case YBR_SN_BSDF_GLASS:
			return "BSDF_GLASS";
		case YBR_SN_BSDF_REFRACTION:
			return "BSDF_REFRACTION";
		case YBR_SN_BSDF_TRANSLUCENT:
			return "BSDF_TRANSLUCENT";
		case YBR_SN_BSDF_TRANSPARENT:
			return "BSDF_TRANSPARENT";
		case YBR_SN_BSDF_VELVET:
			return "BSDF_VELVET/BSDF_SHEEN";
		case YBR_SN_BSDF_TOON:
			return "BSDF_TOON";
		case YBR_SN_BSDF_HAIR:
			return "BSDF_HAIR";
		case YBR_SN_BSDF_HAIR_PRINCIPLED:
			return "BSDF_HAIR_PRINCIPLED";
		case YBR_SN_BSDF_ANISOTROPIC:
			return "BSDF_ANISOTROPIC";
		case YBR_SN_SUBSURFACE_SCATTERING:
			return "SUBSURFACE_SCATTERING";
		case YBR_SN_EMISSION:
			return "EMISSION";
		case YBR_SN_BACKGROUND:
			return "BACKGROUND";
		case YBR_SN_HOLDOUT:
			return "HOLDOUT";
		case YBR_SN_ADD_SHADER:
			return "ADD_SHADER";
		case YBR_SN_MIX_SHADER:
			return "MIX_SHADER";
		case YBR_SN_VOLUME_ABSORPTION:
			return "VOLUME_ABSORPTION";
		case YBR_SN_VOLUME_SCATTER:
			return "VOLUME_SCATTER";
		case YBR_SN_PRINCIPLED_VOLUME:
			return "PRINCIPLED_VOLUME";
		case YBR_SN_SHADERTORGB:
			return "SHADERTORGB";
		case YBR_SN_LIGHT_FALLOFF:
			return "LIGHT_FALLOFF";
		case YBR_SN_TEX_IMAGE:
			return "TEX_IMAGE";
		case YBR_SN_TEX_ENVIRONMENT:
			return "TEX_ENVIRONMENT";
		case YBR_SN_TEX_NOISE:
			return "TEX_NOISE";
		case YBR_SN_TEX_CHECKER:
			return "TEX_CHECKER";
		case YBR_SN_TEX_BRICK:
			return "TEX_BRICK";
		case YBR_SN_TEX_GRADIENT:
			return "TEX_GRADIENT";
		case YBR_SN_TEX_MAGIC:
			return "TEX_MAGIC";
		case YBR_SN_TEX_VORONOI:
			return "TEX_VORONOI";
		case YBR_SN_TEX_WAVE:
			return "TEX_WAVE";
		case YBR_SN_TEX_WHITE_NOISE:
			return "TEX_WHITE_NOISE";
		case YBR_SN_TEX_SKY:
			return "TEX_SKY";
		case YBR_SN_TEX_IES:
			return "TEX_IES";
		case YBR_SN_TEX_POINTDENSITY:
			return "TEX_POINTDENSITY";
		case YBR_SN_TEX_MUSGRAVE:
			return "TEX_MUSGRAVE";
		case YBR_SN_TEX_COORD:
			return "TEX_COORD";
		case YBR_SN_UVMAP:
			return "UVMAP";
		case YBR_SN_ATTRIBUTE:
			return "ATTRIBUTE";
		case YBR_SN_NEW_GEOMETRY:
			return "NEW_GEOMETRY";
		case YBR_SN_OBJECT_INFO:
			return "OBJECT_INFO";
		case YBR_SN_PARTICLE_INFO:
			return "PARTICLE_INFO";
		case YBR_SN_HAIR_INFO:
			return "HAIR_INFO";
		case YBR_SN_POINT_INFO:
			return "POINT_INFO";
		case YBR_SN_VOLUME_INFO:
			return "VOLUME_INFO";
		case YBR_SN_CAMERA:
			return "CAMERA";
		case YBR_SN_LIGHT_PATH:
			return "LIGHT_PATH";
		case YBR_SN_FRESNEL:
			return "FRESNEL";
		case YBR_SN_LAYER_WEIGHT:
			return "LAYER_WEIGHT";
		case YBR_SN_WIREFRAME:
			return "WIREFRAME";
		case YBR_SN_BEVEL:
			return "BEVEL";
		case YBR_SN_AMBIENT_OCCLUSION:
			return "AMBIENT_OCCLUSION";
		case YBR_SN_TANGENT:
			return "TANGENT";
		case YBR_SN_RGB:
			return "RGB";
		case YBR_SN_VALUE:
			return "VALUE";
		case YBR_SN_MIX_RGB:
			return "MIX_RGB";
		case YBR_SN_MIX:
			return "MIX";
		case YBR_SN_VALTORGB:
			return "VALTORGB";
		case YBR_SN_RGBTOBW:
			return "RGBTOBW";
		case YBR_SN_MATH:
			return "MATH";
		case YBR_SN_VECT_MATH:
			return "VECT_MATH";
		case YBR_SN_VECT_TRANSFORM:
			return "VECT_TRANSFORM";
		case YBR_SN_SEPRGB:
			return "SEPRGB/SEPARATE_COLOR";
		case YBR_SN_COMBRGB:
			return "COMBRGB/COMBINE_COLOR";
		case YBR_SN_SEPHSV:
			return "SEPHSV";
		case YBR_SN_COMBHSV:
			return "COMBHSV";
		case YBR_SN_SEPXYZ:
			return "SEPXYZ";
		case YBR_SN_COMBXYZ:
			return "COMBXYZ";
		case YBR_SN_HUE_SAT:
			return "HUE_SAT";
		case YBR_SN_BRIGHTCONTRAST:
			return "BRIGHTCONTRAST";
		case YBR_SN_GAMMA:
			return "GAMMA";
		case YBR_SN_INVERT:
			return "INVERT";
		case YBR_SN_CURVE_RGB:
			return "CURVE_RGB";
		case YBR_SN_CURVE_VEC:
			return "CURVE_VEC";
		case YBR_SN_CURVE_FLOAT:
			return "CURVE_FLOAT";
		case YBR_SN_CLAMP:
			return "CLAMP";
		case YBR_SN_MAP_RANGE:
			return "MAP_RANGE";
		case YBR_SN_FLOAT_CURVE:
			return "FLOAT_CURVE";
		case YBR_SN_BLACKBODY:
			return "BLACKBODY";
		case YBR_SN_WAVELENGTH:
			return "WAVELENGTH";
		case YBR_SN_NORMAL:
			return "NORMAL";
		case YBR_SN_NORMAL_MAP:
			return "NORMAL_MAP";
		case YBR_SN_BUMP:
			return "BUMP";
		case YBR_SN_DISPLACEMENT:
			return "DISPLACEMENT";
		case YBR_SN_VECTOR_DISPLACEMENT:
			return "VECTOR_DISPLACEMENT";
		case YBR_SN_MAPPING:
			return "MAPPING";
		case YBR_SN_VECTOR_ROTATE:
			return "VECTOR_ROTATE";
		case YBR_SN_GROUP:
			return "GROUP";
		case YBR_SN_GROUP_INPUT:
			return "GROUP_INPUT";
		case YBR_SN_GROUP_OUTPUT:
			return "GROUP_OUTPUT";
		case YBR_SN_REROUTE:
			return "REROUTE";
		default:
			return "(unknown)";
	}
}

#define YBR_VAR_LEN 32
#define YBR_MAX_OUTPUTS 16
#define YBR_MAX_DEPTH 64

// グループ関数を 1 回だけ書き出すためのキャッシュ
#define YBR_MAX_GROUP_FNS 64
typedef struct GroupFn {
	const YbrNodeGroup* group;
	int outSocket;
	char name[YBR_VAR_LEN];
} GroupFn;

typedef struct GraphCtx {
	// 現在たどっているグラフ (マテリアル本体 / ノードグループの中身)
	const YbrShaderNode* nodes;
	int nodeCount;
	const YbrShaderLink* links;
	int linkCount;
	const YbrScene* scene;	// ノードグループの解決用（NULL可）
	GlslDialect d;
	UniformSet* set;
	StrBuf decls;  // uniform と関数の宣言
	StrBuf body;   // main() の中身
	char* memo;	   // nodeCount*YBR_MAX_OUTPUTS 個の変数名
	int tmp;
	int texCount;
	int depth;
	int litEmitted;
	int viewEmitted;   // viewPos を宣言済みか
	int lightCount;	   // ライトの数 (0 なら陰影なし)
	int shadowLights;  // 影を落とすライトの数 (先頭から)
	unsigned int lib;  // 出力済みの GLSL ヘルパー (YBR_LIB_*)
	int modelMatrix;   // matModel を宣言済みか
	// ノードグループ
	StrBuf funcs;  // グループ関数の本体 (decls の後ろに付ける)
	GroupFn groupFns[YBR_MAX_GROUP_FNS];
	int groupFnCount;
	int groupDepth;		   // 入れ子の深さ (再帰よけ)
	const char* groupArg;  // GROUP_INPUT の展開先 (NULL ならトップ)
	const char* uniTag;	   // uniform 名の名前空間 ("" / "G0" など)
	YbrShaderError error;
} GraphCtx;

// 生成する GLSL ヘルパー関数
#define YBR_LIB_HASH (1u << 0)
#define YBR_LIB_PERLIN (1u << 1)
#define YBR_LIB_FBM (1u << 2)
#define YBR_LIB_VORONOI (1u << 3)
#define YBR_LIB_HSV (1u << 4)
#define YBR_LIB_BLACKBODY (1u << 5)
#define YBR_LIB_WAVELENGTH (1u << 6)
#define YBR_LIB_EULER (1u << 7)
#define YBR_LIB_MAGIC (1u << 8)
#define YBR_LIB_MUSGRAVE (1u << 9)
#define YBR_LIB_SKY (1u << 10)
#define YBR_LIB_LAST YBR_LIB_SKY

static int graph_fail(GraphCtx* c, YbrShaderError err, const char* fmt, ...)
{
	if (c->error == YBR_SHADER_OK) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(shaderErrorBuf, sizeof(shaderErrorBuf), fmt, ap);
		va_end(ap);
		c->error = err;
	}
	return 0;
}

static char* memo_slot(GraphCtx* c, int nodeIndex, int outSocket)
{
	if (outSocket < 0 || YBR_MAX_OUTPUTS <= outSocket) return NULL;
	return c->memo +
		   ((size_t)nodeIndex * YBR_MAX_OUTPUTS + outSocket) * YBR_VAR_LEN;
}

static void new_var(GraphCtx* c, char* out, size_t sz)
{
	snprintf(out, sz, "ybrV%d", c->tmp++);
}

// プロパティ参照
static const char* prop_string(const YbrShaderNode* n, const char* name)
{
	for (int i = 0; i < n->propCount; i++) {
		if (n->props[i].name && strcmp(n->props[i].name, name) == 0)
			return (n->props[i].type == YBR_PROP_STRING) ? n->props[i].text
														 : NULL;
	}
	return NULL;
}

static double prop_number(const YbrShaderNode* n, const char* name, double def)
{
	for (int i = 0; i < n->propCount; i++) {
		if (n->props[i].name && strcmp(n->props[i].name, name) == 0) {
			if (n->props[i].type == YBR_PROP_STRING) return def;
			return n->props[i].number;
		}
	}
	return def;
}

// 配列プロパティ (sun_direction など)。無ければ 0 を返す。
static int prop_vector(const YbrShaderNode* n, const char* name, float* out,
					   int count)
{
	for (int i = 0; i < count; i++) out[i] = 0.0f;
	for (int i = 0; i < n->propCount; i++) {
		if (!n->props[i].name || strcmp(n->props[i].name, name) != 0) continue;
		if (n->props[i].type != YBR_PROP_ARRAY || !n->props[i].values) return 0;
		for (int k = 0; k < count && k < n->props[i].valueCount; k++)
			out[k] = n->props[i].values[k];
		return 1;
	}
	return 0;
}

// Image Texture ノードのラップ / フィルタ (分からなければ -1)
static void node_tex_props(const YbrShaderNode* n, int* wrap, int* filter)
{
	const char* e = prop_string(n, "extension");
	const char* i = prop_string(n, "interpolation");
	*wrap = -1;
	*filter = -1;
	if (e) {
		if (!strcmp(e, "REPEAT"))
			*wrap = YBR_TEXWRAP_REPEAT;
		else if (!strcmp(e, "EXTEND"))
			*wrap = YBR_TEXWRAP_EXTEND;
		else if (!strcmp(e, "CLIP"))
			*wrap = YBR_TEXWRAP_CLIP;
		else if (!strcmp(e, "MIRROR"))
			*wrap = YBR_TEXWRAP_MIRROR;
	}
	if (i) {
		if (!strcmp(i, "Linear"))
			*filter = YBR_TEXFILTER_LINEAR;
		else if (!strcmp(i, "Closest"))
			*filter = YBR_TEXFILTER_CLOSEST;
		else if (!strcmp(i, "Cubic"))
			*filter = YBR_TEXFILTER_CUBIC;
		else if (!strcmp(i, "Smart"))
			*filter = YBR_TEXFILTER_SMART;
	}
}
static int input_index(const YbrShaderNode* n, const char* name)
{
	for (int i = 0; i < n->inputCount; i++) {
		if (n->inputs[i].name && strcmp(n->inputs[i].name, name) == 0) return i;
	}
	return -1;
}

static int input_index_any(const YbrShaderNode* n, const char* const* names,
						   int count)
{
	for (int i = 0; i < count; i++) {
		int idx = input_index(n, names[i]);
		if (0 <= idx) return idx;
	}
	return -1;
}

static const YbrShaderLink* find_link(const YbrShaderLink* links, int linkCount,
									  int toNode, int toSocket)
{
	for (int i = 0; i < linkCount; i++) {
		if (links[i].toNode == toNode && links[i].toSocket == toSocket)
			return &links[i];
	}
	return NULL;
}

static int emit_node(GraphCtx* c, int nodeIndex, int outSocket, char* out,
					 size_t sz);

// 入力ソケットを vec4 の変数として得る。未接続ならデフォルト値の uniform
// にする。
static int emit_input(GraphCtx* c, int nodeIndex, int socketIndex, char* out,
					  size_t sz)
{
	const YbrShaderNode* n = &c->nodes[nodeIndex];
	if (socketIndex < 0 || n->inputCount <= socketIndex)
		return graph_fail(c, YBR_SHADER_ERR_MISSING_SOCKET,
						  "node '%s': expected input socket is missing",
						  n->name ? n->name : "?");

	const YbrShaderLink* l =
		find_link(c->links, c->linkCount, nodeIndex, socketIndex);
	if (l) {
		if (l->fromNode < 0 || c->nodeCount <= l->fromNode)
			return graph_fail(c, YBR_SHADER_ERR_UNSUPPORTED_NODE,
							  "node '%s': link points outside the graph",
							  n->name ? n->name : "?");
		return emit_node(c, l->fromNode, l->fromSocket, out, sz);
	}

	const YbrShaderSocket* s = &n->inputs[socketIndex];
	char uname[YBR_VAR_LEN];
	snprintf(uname, sizeof(uname), "ybr%sIn%d_%d", c->uniTag, nodeIndex,
			 socketIndex);

	float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	for (int i = 0; i < s->valueCount && i < 4; i++) v[i] = (&s->value.x)[i];

	new_var(c, out, sz);
	if (s->valueCount <= 1) {
		sb_printf(&c->decls, "uniform float %s;\n", uname);
		uni_add(c->set, uname, YBR_UNIFORM_FLOAT, -1, 0, v, 1, NULL);
		sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s), 1.0);\n", out, uname);
	}
	else {
		if (s->valueCount < 4) v[3] = 1.0f;
		sb_printf(&c->decls, "uniform vec4 %s;\n", uname);
		uni_add(c->set, uname, YBR_UNIFORM_VEC4, -1, 0, v, 4, NULL);
		sb_printf(&c->body, "    vec4 %s = %s;\n", out, uname);
	}
	return 1;
}

// 出力ソケットのデフォルト値を uniform として出す。
// ノードグループのように中身を辿れないノードの逃げ道に使う。
static int emit_output_default(GraphCtx* c, int nodeIndex, int outSocket,
							   char* out, size_t sz)
{
	const YbrShaderNode* n = &c->nodes[nodeIndex];
	char uname[YBR_VAR_LEN];
	float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	int count = 0;

	if (0 <= outSocket && outSocket < n->outputCount) {
		const YbrShaderSocket* s = &n->outputs[outSocket];
		count = s->valueCount;
		if (0 < count) {
			const float src[4] = {s->value.x, s->value.y, s->value.z,
								  s->value.w};
			for (int i = 0; i < count && i < 4; i++) v[i] = src[i];
		}
	}
	snprintf(uname, sizeof(uname), "ybr%sOut%d_%d", c->uniTag, nodeIndex,
			 outSocket < 0 ? 0 : outSocket);
	new_var(c, out, sz);
	if (count <= 1) {
		sb_printf(&c->decls, "uniform float %s;\n", uname);
		uni_add(c->set, uname, YBR_UNIFORM_FLOAT, -1, 0, v, 1, NULL);
		sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s), 1.0);\n", out, uname);
	}
	else {
		if (count < 4) v[3] = 1.0f;
		sb_printf(&c->decls, "uniform vec4 %s;\n", uname);
		uni_add(c->set, uname, YBR_UNIFORM_VEC4, -1, 0, v, 4, NULL);
		sb_printf(&c->body, "    vec4 %s = %s;\n", out, uname);
	}
	return 1;
}

// ライティング関数を 1 度だけ出力する
static void emit_lit_function(GraphCtx* c)
{
	if (c->litEmitted) return;
	c->litEmitted = 1;

	emit_light_uniforms(&c->decls, c->set, c->lightCount, c->shadowLights,
						&c->viewEmitted);
	emit_shade_decl(&c->decls, c->lightCount, c->shadowLights, c->d.texture2D);
}

// GLSL ヘルパー関数
// プロシージャルテクスチャなどで使う関数を、必要になったものだけ
// 1 度ずつ宣言部へ出力する。
// GLSL ES 1.00 でも通るよう、ループの回数はすべて定数にしてある。

static void emit_lib(GraphCtx* c, unsigned int flags);

static void emit_lib_one(GraphCtx* c, unsigned int flag)
{
	if (c->lib & flag) return;
	c->lib |= flag;

	switch (flag) {
		case YBR_LIB_HASH:
			sb_puts(&c->decls,
					"\n"
					"float ybrHash13(vec3 p)\n"
					"{\n"
					"    p = fract(p*0.3183099 + vec3(0.1, 0.2, 0.3));\n"
					"    p *= 17.0;\n"
					"    return fract(p.x*p.y*p.z*(p.x + p.y + p.z));\n"
					"}\n"
					"vec3 ybrHash33(vec3 p)\n"
					"{\n"
					"    vec3 q = vec3(dot(p, vec3(127.1, 311.7, 74.7)),\n"
					"                  dot(p, vec3(269.5, 183.3, 246.1)),\n"
					"                  dot(p, vec3(113.5, 271.9, 124.6)));\n"
					"    return fract(sin(q)*43758.5453123)*2.0 - 1.0;\n"
					"}\n");
			break;

		case YBR_LIB_PERLIN:
			emit_lib(c, YBR_LIB_HASH);
			sb_puts(
				&c->decls,
				"\n"
				"/* 3D グラディエントノイズ (おおよそ -1..1) */\n"
				"float ybrPerlin(vec3 p)\n"
				"{\n"
				"    vec3 i = floor(p);\n"
				"    vec3 f = p - i;\n"
				"    vec3 u = f*f*f*(f*(f*6.0 - 15.0) + 10.0);\n"
				"    float n000 = dot(ybrHash33(i + vec3(0.0, 0.0, 0.0)), f - "
				"vec3(0.0, 0.0, 0.0));\n"
				"    float n100 = dot(ybrHash33(i + vec3(1.0, 0.0, 0.0)), f - "
				"vec3(1.0, 0.0, 0.0));\n"
				"    float n010 = dot(ybrHash33(i + vec3(0.0, 1.0, 0.0)), f - "
				"vec3(0.0, 1.0, 0.0));\n"
				"    float n110 = dot(ybrHash33(i + vec3(1.0, 1.0, 0.0)), f - "
				"vec3(1.0, 1.0, 0.0));\n"
				"    float n001 = dot(ybrHash33(i + vec3(0.0, 0.0, 1.0)), f - "
				"vec3(0.0, 0.0, 1.0));\n"
				"    float n101 = dot(ybrHash33(i + vec3(1.0, 0.0, 1.0)), f - "
				"vec3(1.0, 0.0, 1.0));\n"
				"    float n011 = dot(ybrHash33(i + vec3(0.0, 1.0, 1.0)), f - "
				"vec3(0.0, 1.0, 1.0));\n"
				"    float n111 = dot(ybrHash33(i + vec3(1.0, 1.0, 1.0)), f - "
				"vec3(1.0, 1.0, 1.0));\n"
				"    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, "
				"u.x), u.y),\n"
				"               mix(mix(n001, n101, u.x), mix(n011, n111, "
				"u.x), u.y), "
				"u.z)*1.4;\n"
				"}\n");
			break;

		case YBR_LIB_FBM:
			emit_lib(c, YBR_LIB_PERLIN);
			sb_puts(
				&c->decls,
				"\n"
				"/* detail は 0..4 でクランプされる (ES 2.0 "
				"のためループ回数は固定) */\n"
				"float ybrFbm(vec3 p, float detail, float roughness, float "
				"lacunarity)\n"
				"{\n"
				"    float sum = 0.0, amp = 1.0, norm = 0.0, sc = 1.0;\n"
				"    for (int i = 0; i < 4; i++) {\n"
				"        float w = clamp(detail - float(i) + 1.0, 0.0, 1.0);\n"
				"        sum  += ybrPerlin(p*sc)*amp*w;\n"
				"        norm += amp*w;\n"
				"        amp *= max(roughness, 0.0);\n"
				"        sc  *= max(lacunarity, 1e-4);\n"
				"    }\n"
				"    return (0.0 < norm) ? sum/norm : 0.0;\n"
				"}\n"
				"float ybrNoise01(vec3 p, float detail, float roughness, float "
				"lacunarity)\n"
				"{\n"
				"    return clamp(0.5 + 0.5*ybrFbm(p, detail, roughness, "
				"lacunarity), 0.0, 1.0);\n"
				"}\n");
			break;

		case YBR_LIB_VORONOI:
			emit_lib(c, YBR_LIB_HASH);
			sb_puts(
				&c->decls,
				"\n"
				"/* 距離関数 : metric 0=EUCLIDEAN 1=MANHATTAN 2=CHEBYCHEV "
				"3=MINKOWSKI "
				"*/\n"
				"float ybrVoroDist(vec3 d, float metric, float e)\n"
				"{\n"
				"    if (metric < 0.5) return length(d);\n"
				"    if (metric < 1.5) return abs(d.x) + abs(d.y) + abs(d.z);\n"
				"    if (metric < 2.5) return max(max(abs(d.x), abs(d.y)), "
				"abs(d.z));\n"
				"    float ex = max(e, 1e-3);\n"
				"    return pow(pow(abs(d.x), ex) + pow(abs(d.y), ex) + "
				"pow(abs(d.z), "
				"ex), 1.0/ex);\n"
				"}\n"
				"\n"
				"/* Blender の Voronoi Texture 相当。3x3x3 の近傍だけを見る。\n"
				" *   f1     : もっとも近い特徴点までの距離\n"
				" *   f2     : 2 番目に近い特徴点までの距離\n"
				" *   sf1    : f1 を smoothness でならしたもの (Smooth F1)\n"
				" *   edge   : セル境界までの距離 (Distance to Edge)\n"
				" *   radius : 特徴点どうしが接する球の半径 (N-Sphere Radius)\n"
				" *   cell   : 特徴点のセル座標 (色付け用)\n"
				" *   pos    : 特徴点の位置 (入力空間)                         "
				" */\n"
				"void ybrVoronoiEx(vec3 p, float randomness, float "
				"smoothness,\n"
				"                  float metric, float expo,\n"
				"                  out float f1, out float f2, out float sf1,\n"
				"                  out float edge, out float radius,\n"
				"                  out vec3 cell, out vec3 pos)\n"
				"{\n"
				"    vec3 ip = floor(p);\n"
				"    vec3 fp = p - ip;\n"
				"    float rnd = clamp(randomness, 0.0, 1.0);\n"
				"    float sm = max(smoothness, 0.0);\n"
				"    f1 = 1e6; f2 = 1e6; sf1 = 1e6;\n"
				"    cell = ip; pos = p;\n"
				"    vec3 best = vec3(0.0);\n"
				"\n"
				"    for (int k = -1; k <= 1; k++) {\n"
				"    for (int j = -1; j <= 1; j++) {\n"
				"    for (int i = -1; i <= 1; i++) {\n"
				"        vec3 off = vec3(float(i), float(j), float(k));\n"
				"        vec3 seed = ip + off;\n"
				"        vec3 pnt = off + (ybrHash33(seed)*0.5 + 0.5)*rnd;\n"
				"        float d = ybrVoroDist(pnt - fp, metric, expo);\n"
				"        if (d < f1) { f2 = f1; f1 = d; cell = seed; best = "
				"pnt; }\n"
				"        else if (d < f2) { f2 = d; }\n"
				"        /* 指数的 smooth-min で Smooth F1 */\n"
				"        if (1e-5 < sm) {\n"
				"            float h = clamp(0.5 + 0.5*(sf1 - d)/sm, 0.0, "
				"1.0);\n"
				"            sf1 = mix(sf1, d, h) - sm*h*(1.0 - h);\n"
				"        } else if (d < sf1) sf1 = d;\n"
				"    }}}\n"
				"    pos = ip + best;\n"
				"\n"
				"    /* 2 パス目 : "
				"もっとも近い特徴点を基準に境界と半径を求める */\n"
				"    edge = 1e6; radius = 1e6;\n"
				"    for (int k2 = -1; k2 <= 1; k2++) {\n"
				"    for (int j2 = -1; j2 <= 1; j2++) {\n"
				"    for (int i2 = -1; i2 <= 1; i2++) {\n"
				"        vec3 off = vec3(float(i2), float(j2), float(k2));\n"
				"        vec3 pnt = off + (ybrHash33(ip + off)*0.5 + "
				"0.5)*rnd;\n"
				"        vec3 r = pnt - best;\n"
				"        float rl = length(r);\n"
				"        if (1e-5 < rl) {\n"
				"            edge   = min(edge, dot(0.5*(best + pnt) - fp, "
				"r/rl));\n"
				"            radius = min(radius, rl*0.5);\n"
				"        }\n"
				"    }}}\n"
				"    edge = max(edge, 0.0);\n"
				"    if (1e5 < radius) radius = 0.0;\n"
				"}\n");
			break;

		case YBR_LIB_HSV:
			sb_puts(
				&c->decls,
				"\n"
				"vec3 ybrRgbToHsv(vec3 c)\n"
				"{\n"
				"    vec4 k = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);\n"
				"    vec4 p = mix(vec4(c.bg, k.wz), vec4(c.gb, k.xy), "
				"step(c.b, "
				"c.g));\n"
				"    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), "
				"step(p.x, "
				"c.r));\n"
				"    float d = q.x - min(q.w, q.y);\n"
				"    float e = 1.0e-10;\n"
				"    return vec3(abs(q.z + (q.w - q.y)/(6.0*d + e)), d/(q.x + "
				"e), "
				"q.x);\n"
				"}\n"
				"vec3 ybrHsvToRgb(vec3 c)\n"
				"{\n"
				"    vec4 k = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);\n"
				"    vec3 p = abs(fract(c.xxx + k.xyz)*6.0 - k.www);\n"
				"    return c.z*mix(k.xxx, clamp(p - k.xxx, 0.0, 1.0), c.y);\n"
				"}\n");
			break;

		case YBR_LIB_BLACKBODY:
			sb_puts(&c->decls,
					"\n"
					"/* 色温度 (K) -> RGB の近似 (Tanner Helland の式) */\n"
					"vec3 ybrBlackbody(float kelvin)\n"
					"{\n"
					"    float t = clamp(kelvin, 1000.0, 40000.0)/100.0;\n"
					"    float r, g, b;\n"
					"    if (t <= 66.0) {\n"
					"        r = 1.0;\n"
					"        g = clamp(0.39008158*log(max(t, 1.0)) - "
					"0.63184144, 0.0, 1.0);\n"
					"        b = (t <= 19.0) ? 0.0\n"
					"          : clamp(0.54320679*log(max(t - 10.0, 1.0)) - "
					"1.19625408, 0.0, 1.0);\n"
					"    } else {\n"
					"        r = clamp(1.29293618*pow(t - 60.0, "
					"-0.1332047592), 0.0, 1.0);\n"
					"        g = clamp(1.12989086*pow(t - 60.0, "
					"-0.0755148492), 0.0, 1.0);\n"
					"        b = 1.0;\n"
					"    }\n"
					"    return vec3(r, g, b);\n"
					"}\n");
			break;

		case YBR_LIB_WAVELENGTH:
			sb_puts(&c->decls,
					"\n"
					"/* 波長 (nm) -> RGB の近似 */\n"
					"vec3 ybrWavelength(float nm)\n"
					"{\n"
					"    float w = clamp(nm, 380.0, 780.0);\n"
					"    vec3 c = vec3(0.0);\n"
					"    if (w < 440.0)      c = vec3(-(w - 440.0)/60.0, 0.0, "
					"1.0);\n"
					"    else if (w < 490.0) c = vec3(0.0, (w - 440.0)/50.0, "
					"1.0);\n"
					"    else if (w < 510.0) c = vec3(0.0, 1.0, -(w - "
					"510.0)/20.0);\n"
					"    else if (w < 580.0) c = vec3((w - 510.0)/70.0, 1.0, "
					"0.0);\n"
					"    else if (w < 645.0) c = vec3(1.0, -(w - 645.0)/65.0, "
					"0.0);\n"
					"    else                c = vec3(1.0, 0.0, 0.0);\n"
					"    float f = 1.0;\n"
					"    if (w < 420.0)      f = 0.3 + 0.7*(w - 380.0)/40.0;\n"
					"    else if (700.0 < w) f = 0.3 + 0.7*(780.0 - w)/80.0;\n"
					"    return clamp(c*f, 0.0, 1.0);\n"
					"}\n");
			break;

		case YBR_LIB_EULER:
			sb_puts(
				&c->decls,
				"\n"
				"/* Blender と同じ XYZ 順のオイラー角 -> 回転行列 */\n"
				"mat3 ybrEulerXYZ(vec3 r)\n"
				"{\n"
				"    vec3 s = sin(r), c = cos(r);\n"
				"    mat3 rx = mat3(1.0, 0.0, 0.0,  0.0, c.x, s.x,  0.0, -s.x, "
				"c.x);\n"
				"    mat3 ry = mat3(c.y, 0.0, -s.y, 0.0, 1.0, 0.0,  s.y, 0.0, "
				"c.y);\n"
				"    mat3 rz = mat3(c.z, s.z, 0.0, -s.z, c.z, 0.0,  0.0, 0.0, "
				"1.0);\n"
				"    return rz*ry*rx;\n"
				"}\n"
				"vec3 ybrRotateAxis(vec3 v, vec3 axis, float angle)\n"
				"{\n"
				"    vec3 a = normalize(axis);\n"
				"    float s = sin(angle), co = cos(angle);\n"
				"    return v*co + cross(a, v)*s + a*dot(a, v)*(1.0 - co);\n"
				"}\n");
			break;

		case YBR_LIB_MUSGRAVE:
			emit_lib(c, YBR_LIB_PERLIN);
			sb_puts(
				&c->decls,
				"\n"
				"/* Musgrave の各バリエーション (Blender 4.1 "
				"で廃止されたノード用)。\n"
				" *   kind 0=MULTIFRACTAL 1=RIDGED_MULTIFRACTAL "
				"2=HYBRID_MULTIFRACTAL\n"
				" *        3=FBM          4=HETERO_TERRAIN\n"
				" * ES 2.0 に合わせてオクターブ数は 8 "
				"で固定し、端数は重みで扱う。 */\n"
				"float ybrMusgrave(vec3 p, float kind, float H, float "
				"lacunarity,\n"
				"                  float octaves, float offset, float gain)\n"
				"{\n"
				"    float lac = max(lacunarity, 1e-4);\n"
				"    float pw  = pow(lac, -max(H, 0.0));\n"
				"    float sc  = 1.0;\n"
				"    float amp = 1.0;\n"
				"    float value;\n"
				"    float weight;\n"
				"\n"
				"    if (kind < 0.5) {          /* MULTIFRACTAL */\n"
				"        value = 1.0;\n"
				"        for (int i = 0; i < 8; i++) {\n"
				"            float w = clamp(octaves - float(i), 0.0, 1.0);\n"
				"            if (w <= 0.0) break;\n"
				"            value *= mix(1.0, ybrPerlin(p*sc)*amp + 1.0, w);\n"
				"            amp *= pw; sc *= lac;\n"
				"        }\n"
				"        return value - 1.0;\n"
				"    }\n"
				"    if (kind < 1.5) {          /* RIDGED_MULTIFRACTAL */\n"
				"        float signal = offset - abs(ybrPerlin(p));\n"
				"        signal *= signal;\n"
				"        value = signal;\n"
				"        weight = 1.0;\n"
				"        sc = lac;\n"
				"        amp = pw;\n"
				"        for (int i = 1; i < 8; i++) {\n"
				"            float w = clamp(octaves - float(i), 0.0, 1.0);\n"
				"            if (w <= 0.0) break;\n"
				"            weight = clamp(signal*gain, 0.0, 1.0);\n"
				"            signal = offset - abs(ybrPerlin(p*sc));\n"
				"            signal *= signal*weight;\n"
				"            value += signal*amp*w;\n"
				"            amp *= pw; sc *= lac;\n"
				"        }\n"
				"        return value;\n"
				"    }\n"
				"    if (kind < 2.5) {          /* HYBRID_MULTIFRACTAL */\n"
				"        float signal = ybrPerlin(p) + offset;\n"
				"        value = signal;\n"
				"        weight = signal;\n"
				"        sc = lac;\n"
				"        amp = pw;\n"
				"        for (int i = 1; i < 8; i++) {\n"
				"            float w = clamp(octaves - float(i), 0.0, 1.0);\n"
				"            if (w <= 0.0) break;\n"
				"            weight = min(weight, 1.0);\n"
				"            signal = amp*(ybrPerlin(p*sc) + offset);\n"
				"            value += weight*signal*w;\n"
				"            weight *= signal;\n"
				"            amp *= pw; sc *= lac;\n"
				"        }\n"
				"        return value;\n"
				"    }\n"
				"    if (kind < 3.5) {          /* FBM */\n"
				"        value = 0.0;\n"
				"        for (int i = 0; i < 8; i++) {\n"
				"            float w = clamp(octaves - float(i), 0.0, 1.0);\n"
				"            if (w <= 0.0) break;\n"
				"            value += ybrPerlin(p*sc)*amp*w;\n"
				"            amp *= pw; sc *= lac;\n"
				"        }\n"
				"        return value;\n"
				"    }\n"
				"    /* HETERO_TERRAIN */\n"
				"    value = offset + ybrPerlin(p);\n"
				"    sc = lac;\n"
				"    amp = pw;\n"
				"    for (int i = 1; i < 8; i++) {\n"
				"        float w = clamp(octaves - float(i), 0.0, 1.0);\n"
				"        if (w <= 0.0) break;\n"
				"        float increment = (ybrPerlin(p*sc) + "
				"offset)*amp*value;\n"
				"        value += increment*w;\n"
				"        amp *= pw; sc *= lac;\n"
				"    }\n"
				"    return value;\n"
				"}\n");
			break;

		case YBR_LIB_SKY:
			sb_puts(
				&c->decls,
				"\n"
				"/* Preetham の空モデル (解析的な昼空)。\n"
				" * Blender の NISHITA / HOSEK_WILKIE もこれで近似する。 */\n"
				"float ybrPerez(float cosTheta, float gamma, float cosGamma,\n"
				"               float A, float B, float C, float D, float E)\n"
				"{\n"
				"    return (1.0 + A*exp(B/max(cosTheta, 0.02)))*\n"
				"           (1.0 + C*exp(D*gamma) + E*cosGamma*cosGamma);\n"
				"}\n"
				"vec3 ybrSky(vec3 dir, vec3 sunDir, float turbidity, float "
				"albedo,\n"
				"            float sunDisc, float sunSize, float "
				"sunIntensity)\n"
				"{\n"
				"    dir = normalize(dir);\n"
				"    sunDir = normalize(sunDir);\n"
				"    float T = clamp(turbidity, 1.0, 10.0);\n"
				"    float cosTheta = dir.y;\n"
				"    float cosGamma = clamp(dot(dir, sunDir), -1.0, 1.0);\n"
				"    float gamma = acos(cosGamma);\n"
				"    float thetaS = acos(clamp(sunDir.y, -1.0, 1.0));\n"
				"\n"
				"    float Ay =  0.1787*T - 1.4630, By = -0.3554*T + 0.4275;\n"
				"    float Cy = -0.0227*T + 5.3251, Dy =  0.1206*T - 2.5771, "
				"Ey = "
				"-0.0670*T + 0.3703;\n"
				"    float Ax = -0.0193*T - 0.2592, Bx = -0.0665*T + 0.0008;\n"
				"    float Cx = -0.0004*T + 0.2125, Dx = -0.0641*T - 0.8989, "
				"Ex = "
				"-0.0033*T + 0.0452;\n"
				"    float Az = -0.0167*T - 0.2608, Bz = -0.0950*T + 0.0092;\n"
				"    float Cz = -0.0079*T + 0.2102, Dz = -0.0441*T - 1.6537, "
				"Ez = "
				"-0.0109*T + 0.0529;\n"
				"\n"
				"    float ts2 = thetaS*thetaS, ts3 = ts2*thetaS, T2 = T*T;\n"
				"    float chi = (4.0/9.0 - T/120.0)*(3.14159265 - "
				"2.0*thetaS);\n"
				"    float Yz = (4.0453*T - 4.9710)*tan(chi) - 0.2155*T + "
				"2.4192;\n"
				"    float xz = ( 0.00166*ts3 - 0.00375*ts2 + "
				"0.00209*thetaS)*T2 +\n"
				"               (-0.02903*ts3 + 0.06377*ts2 - 0.03202*thetaS + "
				"0.00394)*T +\n"
				"               ( 0.11693*ts3 - 0.21196*ts2 + 0.06052*thetaS + "
				"0.25886);\n"
				"    float yz = ( 0.00275*ts3 - 0.00610*ts2 + "
				"0.00317*thetaS)*T2 +\n"
				"               (-0.04214*ts3 + 0.08970*ts2 - 0.04153*thetaS + "
				"0.00516)*T +\n"
				"               ( 0.15346*ts3 - 0.26756*ts2 + 0.06670*thetaS + "
				"0.26688);\n"
				"\n"
				"    float cs = cos(thetaS);\n"
				"    float Y = Yz*ybrPerez(cosTheta, gamma, cosGamma, "
				"Ay,By,Cy,Dy,Ey)/\n"
				"                 max(ybrPerez(1.0, thetaS, cs, "
				"Ay,By,Cy,Dy,Ey), "
				"1e-4);\n"
				"    float x = xz*ybrPerez(cosTheta, gamma, cosGamma, "
				"Ax,Bx,Cx,Dx,Ex)/\n"
				"                 max(ybrPerez(1.0, thetaS, cs, "
				"Ax,Bx,Cx,Dx,Ex), "
				"1e-4);\n"
				"    float y = yz*ybrPerez(cosTheta, gamma, cosGamma, "
				"Az,Bz,Cz,Dz,Ez)/\n"
				"                 max(ybrPerez(1.0, thetaS, cs, "
				"Az,Bz,Cz,Dz,Ez), "
				"1e-4);\n"
				"\n"
				"    /* xyY -> XYZ -> linear sRGB */\n"
				"    y = max(y, 1e-4);\n"
				"    float Yn = Y*0.05;\n"
				"    vec3 XYZ = vec3(Yn*x/y, Yn, Yn*(1.0 - x - y)/y);\n"
				"    vec3 rgb = vec3(\n"
				"        dot(XYZ, vec3( 3.2404542, -1.5371385, -0.4985314)),\n"
				"        dot(XYZ, vec3(-0.9692660,  1.8760108,  0.0415560)),\n"
				"        dot(XYZ, vec3( 0.0556434, -0.2040259,  1.0572252)));\n"
				"    rgb = max(rgb, vec3(0.0));\n"
				"\n"
				"    /* 地平線から下は地面の反射色でつぶす */\n"
				"    float ground = clamp(-cosTheta*20.0, 0.0, 1.0);\n"
				"    rgb = mix(rgb, vec3(max(albedo, 0.0))*max(sunDir.y, 0.0), "
				"ground);\n"
				"\n"
				"    /* 太陽の円盤 */\n"
				"    if (0.5 < sunDisc) {\n"
				"        float ang = max(sunSize, 1e-4);\n"
				"        float disc = 1.0 - smoothstep(ang*0.8, ang, gamma);\n"
				"        rgb += vec3(disc*max(sunIntensity, 0.0))*step(0.0, "
				"cosTheta);\n"
				"    }\n"
				"    return rgb;\n"
				"}\n");
			break;

		case YBR_LIB_MAGIC:
			sb_puts(
				&c->decls,
				"\n"
				"/* Blender のマジックテクスチャ (深さは 10 "
				"で固定してループを定数化) */\n"
				"vec3 ybrMagic(vec3 p, float depth, float distortion)\n"
				"{\n"
				"    float x = sin((p.x + p.y + p.z)*5.0);\n"
				"    float y = cos((-p.x + p.y - p.z)*5.0);\n"
				"    float z = -cos((-p.x - p.y + p.z)*5.0);\n"
				"    float d = max(distortion, 1e-4);\n"
				"    x *= d; y *= d; z *= d;\n"
				"    for (int i = 1; i < 10; i++) {\n"
				"        if (depth <= float(i)) break;\n"
				"        float nx = x, ny = y, nz = z;\n"
				"        if (i == 1)      { nx = cos(x - y + z); ny = -cos(-x "
				"+ y "
				"- z); nz = cos(-x - y + z); }\n"
				"        else if (i == 2) { nx = cos(x - y - z);  ny = -cos(-x "
				"- y "
				"+ z); nz = -sin(-x + y - z); }\n"
				"        else if (i == 3) { nx = -cos(-x + y + z); ny = -cos(x "
				"- y "
				"+ z); nz = sin(x + y - z); }\n"
				"        else if (i == 4) { nx = -sin(-x - y + z); ny = "
				"-cos(-x + "
				"y - z); nz = cos(x + y + z); }\n"
				"        else if (i == 5) { nx = cos(x + y + z);   ny = sin(x "
				"- y "
				"- z);   nz = -cos(-x + y + z); }\n"
				"        else if (i == 6) { nx = sin(x + y - z);   ny = "
				"-cos(-x + "
				"y - z); nz = -sin(-x - y + z); }\n"
				"        else if (i == 7) { nx = -cos(-x - y + z); ny = -sin(x "
				"- y "
				"+ z);  nz = sin(-x + y + z); }\n"
				"        else if (i == 8) { nx = cos(x - y + z);   ny = sin(-x "
				"+ y "
				"- z);  nz = -cos(x + y + z); }\n"
				"        else             { nx = -sin(x + y + z);  ny = cos(x "
				"- y "
				"- z);   nz = sin(-x - y + z); }\n"
				"        x = nx*d; y = ny*d; z = nz*d;\n"
				"    }\n"
				"    if (d != 0.0) { x /= 2.0*d; y /= 2.0*d; z /= 2.0*d; }\n"
				"    return clamp(vec3(0.5 - x, 0.5 - y, 0.5 - z), 0.0, 1.0);\n"
				"}\n");
			break;

		default:
			break;
	}
}

static void emit_lib(GraphCtx* c, unsigned int flags)
{
	for (unsigned int bit = 1u; bit <= YBR_LIB_LAST; bit <<= 1)
		if (flags & bit) emit_lib_one(c, bit);
}

// matModel は raylib が自動で入れてくれるので、必要になったら宣言する
static void emit_model_matrix(GraphCtx* c)
{
	if (c->modelMatrix) return;
	c->modelMatrix = 1;
	sb_puts(&c->decls, "uniform mat4 matModel;\n");
	uni_add(c->set, "matModel", YBR_UNIFORM_VEC4, RL_SHADER_LOC_MATRIX_MODEL, 1,
			NULL, 0, NULL);
}

// GLSL の float リテラル
static void fmt_float(char* buf, size_t sz, double v)
{
	snprintf(buf, sz, "%.6g", v);
	if (!strpbrk(buf, ".eEnN")) {
		size_t n = strlen(buf);
		if (n + 2 < sz) {
			buf[n] = '.';
			buf[n + 1] = '0';
			buf[n + 2] = '\0';
		}
	}
}

// 配列プロパティ
static const float* prop_array(const YbrShaderNode* n, const char* name,
							   int* outCount)
{
	if (outCount) *outCount = 0;
	for (int i = 0; i < n->propCount; i++) {
		if (n->props[i].name && strcmp(n->props[i].name, name) == 0 &&
			n->props[i].type == YBR_PROP_ARRAY && n->props[i].values) {
			if (outCount) *outCount = n->props[i].valueCount;
			return n->props[i].values;
		}
	}
	return NULL;
}

// MATH / VECT_MATH の演算
static int emit_math_op(GraphCtx* c, const YbrShaderNode* n, const char* op,
						const char* a, const char* b, const char* cc,
						const char* out, int vector)
{
	const char* sw = vector ? ".xyz" : ".x";
	const char* ty = vector ? "vec3" : "float";
	char expr[256];

	if (!strcmp(op, "ADD"))
		snprintf(expr, sizeof(expr), "%s%s + %s%s", a, sw, b, sw);
	else if (!strcmp(op, "SUBTRACT"))
		snprintf(expr, sizeof(expr), "%s%s - %s%s", a, sw, b, sw);
	else if (!strcmp(op, "MULTIPLY"))
		snprintf(expr, sizeof(expr), "%s%s * %s%s", a, sw, b, sw);
	else if (!strcmp(op, "DIVIDE"))
		snprintf(expr, sizeof(expr), "%s%s / max(%s%s, %s(1e-6))", a, sw, b, sw,
				 ty);
	else if (!strcmp(op, "MINIMUM"))
		snprintf(expr, sizeof(expr), "min(%s%s, %s%s)", a, sw, b, sw);
	else if (!strcmp(op, "MAXIMUM"))
		snprintf(expr, sizeof(expr), "max(%s%s, %s%s)", a, sw, b, sw);
	else if (!strcmp(op, "POWER"))
		snprintf(expr, sizeof(expr), "pow(max(%s%s, %s(0.0)), %s%s)", a, sw, ty,
				 b, sw);
	else if (!strcmp(op, "ABSOLUTE"))
		snprintf(expr, sizeof(expr), "abs(%s%s)", a, sw);
	else if (!strcmp(op, "SQRT"))
		snprintf(expr, sizeof(expr), "sqrt(max(%s%s, %s(0.0)))", a, sw, ty);
	else if (!strcmp(op, "FRACT"))
		snprintf(expr, sizeof(expr), "fract(%s%s)", a, sw);
	else if (!strcmp(op, "FLOOR"))
		snprintf(expr, sizeof(expr), "floor(%s%s)", a, sw);
	else if (!strcmp(op, "CEIL"))
		snprintf(expr, sizeof(expr), "ceil(%s%s)", a, sw);
	else if (!strcmp(op, "SINE"))
		snprintf(expr, sizeof(expr), "sin(%s%s)", a, sw);
	else if (!strcmp(op, "COSINE"))
		snprintf(expr, sizeof(expr), "cos(%s%s)", a, sw);
	else if (!strcmp(op, "MODULO"))
		snprintf(expr, sizeof(expr), "mod(%s%s, %s%s)", a, sw, b, sw);
	else if (!strcmp(op, "CLAMP"))
		snprintf(expr, sizeof(expr), "clamp(%s%s, %s(0.0), %s(1.0))", a, sw, ty,
				 ty);
	else if (!strcmp(op, "SIGN"))
		snprintf(expr, sizeof(expr), "sign(%s%s)", a, sw);
	else if (!strcmp(op, "ROUND"))
		snprintf(expr, sizeof(expr), "floor(%s%s + %s(0.5))", a, sw, ty);
	else if (!strcmp(op, "TRUNC"))
		snprintf(expr, sizeof(expr), "sign(%s%s)*floor(abs(%s%s))", a, sw, a,
				 sw);
	else if (!strcmp(op, "SNAP"))
		snprintf(expr, sizeof(expr), "floor(%s%s/max(%s%s, %s(1e-6)))*%s%s", a,
				 sw, b, sw, ty, b, sw);
	else if (!strcmp(op, "WRAP"))
		snprintf(expr, sizeof(expr),
				 "%s%s + mod(%s%s - %s%s, max(%s%s - %s%s, %s(1e-6)))", cc, sw,
				 a, sw, cc, sw, b, sw, cc, sw, ty);
	else if (!strcmp(op, "SINE"))
		snprintf(expr, sizeof(expr), "sin(%s%s)", a, sw);
	else if (!strcmp(op, "COSINE"))
		snprintf(expr, sizeof(expr), "cos(%s%s)", a, sw);
	else if (!strcmp(op, "TANGENT"))
		snprintf(expr, sizeof(expr), "tan(%s%s)", a, sw);
	else if (!strcmp(op, "FLOORED_MODULO"))
		snprintf(expr, sizeof(expr), "mod(%s%s, %s%s)", a, sw, b, sw);
	else if (!strcmp(op, "INVERSE_SQRT"))
		snprintf(expr, sizeof(expr), "inversesqrt(max(%s%s, %s(1e-6)))", a, sw,
				 ty);
	// スカラー
	else if (!vector && !strcmp(op, "MULTIPLY_ADD"))
		snprintf(expr, sizeof(expr), "%s.x*%s.x + %s.x", a, b, cc);
	else if (!vector && !strcmp(op, "ARCSINE"))
		snprintf(expr, sizeof(expr), "asin(clamp(%s.x, -1.0, 1.0))", a);
	else if (!vector && !strcmp(op, "ARCCOSINE"))
		snprintf(expr, sizeof(expr), "acos(clamp(%s.x, -1.0, 1.0))", a);
	else if (!vector && !strcmp(op, "ARCTANGENT"))
		snprintf(expr, sizeof(expr), "atan(%s.x)", a);
	else if (!vector && !strcmp(op, "ARCTAN2"))
		snprintf(expr, sizeof(expr), "atan(%s.x, %s.x)", a, b);
	else if (!vector && !strcmp(op, "SINH"))
		snprintf(expr, sizeof(expr), "0.5*(exp(%s.x) - exp(-%s.x))", a, a);
	else if (!vector && !strcmp(op, "COSH"))
		snprintf(expr, sizeof(expr), "0.5*(exp(%s.x) + exp(-%s.x))", a, a);
	else if (!vector && !strcmp(op, "TANH"))
		snprintf(expr, sizeof(expr),
				 "(exp(2.0*%s.x) - 1.0)/(exp(2.0*%s.x) + 1.0)", a, a);
	else if (!vector && !strcmp(op, "LOGARITHM"))
		snprintf(expr, sizeof(expr),
				 "log(max(%s.x, 1e-6))/log(max(%s.x, 1e-6))", a, b);
	else if (!vector && !strcmp(op, "EXPONENT"))
		snprintf(expr, sizeof(expr), "exp(%s.x)", a);
	else if (!vector && !strcmp(op, "LESS_THAN"))
		snprintf(expr, sizeof(expr), "step(%s.x, %s.x) - step(%s.x, %s.x)", a,
				 b, b, a);
	else if (!vector && !strcmp(op, "GREATER_THAN"))
		snprintf(expr, sizeof(expr), "step(%s.x, %s.x) - step(%s.x, %s.x)", b,
				 a, a, b);
	else if (!vector && !strcmp(op, "COMPARE"))
		snprintf(expr, sizeof(expr), "1.0 - step(%s.x, abs(%s.x - %s.x))", cc,
				 a, b);
	else if (!vector && !strcmp(op, "SMOOTH_MIN"))
		snprintf(
			expr, sizeof(expr),
			"mix(min(%s.x, %s.x), min(%s.x, %s.x) - 0.25*%s.x, step(abs(%s.x "
			"- %s.x), max(%s.x, 1e-6)))",
			a, b, a, b, cc, a, b, cc);
	else if (!vector && !strcmp(op, "SMOOTH_MAX"))
		snprintf(
			expr, sizeof(expr),
			"mix(max(%s.x, %s.x), max(%s.x, %s.x) + 0.25*%s.x, step(abs(%s.x "
			"- %s.x), max(%s.x, 1e-6)))",
			a, b, a, b, cc, a, b, cc);
	else if (!vector && !strcmp(op, "PINGPONG"))
		snprintf(expr, sizeof(expr),
				 "abs(mod(%s.x, 2.0*max(%s.x, 1e-6)) - max(%s.x, 1e-6))", a, b,
				 b);
	else if (!vector && !strcmp(op, "RADIANS"))
		snprintf(expr, sizeof(expr), "radians(%s.x)", a);
	else if (!vector && !strcmp(op, "DEGREES"))
		snprintf(expr, sizeof(expr), "degrees(%s.x)", a);
	// ベクトル関連
	else if (vector && !strcmp(op, "DOT_PRODUCT"))
		snprintf(expr, sizeof(expr), "vec3(dot(%s.xyz, %s.xyz))", a, b);
	else if (vector && !strcmp(op, "CROSS_PRODUCT"))
		snprintf(expr, sizeof(expr), "cross(%s.xyz, %s.xyz)", a, b);
	else if (vector && !strcmp(op, "NORMALIZE"))
		snprintf(expr, sizeof(expr), "normalize(%s.xyz)", a);
	else if (vector && !strcmp(op, "LENGTH"))
		snprintf(expr, sizeof(expr), "vec3(length(%s.xyz))", a);
	else if (vector && !strcmp(op, "DISTANCE"))
		snprintf(expr, sizeof(expr), "vec3(distance(%s.xyz, %s.xyz))", a, b);
	else if (vector && !strcmp(op, "SCALE"))
		snprintf(expr, sizeof(expr), "%s.xyz*%s.x", a, b);
	else if (vector && !strcmp(op, "MULTIPLY_ADD"))
		snprintf(expr, sizeof(expr), "%s.xyz*%s.xyz + %s.xyz", a, b, cc);
	else if (vector && !strcmp(op, "PROJECT"))
		snprintf(expr, sizeof(expr),
				 "%s.xyz*(dot(%s.xyz, %s.xyz)/max(dot(%s.xyz, %s.xyz), 1e-6))",
				 b, a, b, b, b);
	else if (vector && !strcmp(op, "REFLECT"))
		snprintf(expr, sizeof(expr), "reflect(%s.xyz, normalize(%s.xyz))", a,
				 b);
	else if (vector && !strcmp(op, "REFRACT"))
		snprintf(expr, sizeof(expr),
				 "refract(normalize(%s.xyz), normalize(%s.xyz), %s.x)", a, b,
				 cc);
	else if (vector && !strcmp(op, "FACEFORWARD"))
		snprintf(expr, sizeof(expr), "faceforward(%s.xyz, %s.xyz, %s.xyz)", a,
				 b, cc);
	else
		return graph_fail(c, YBR_SHADER_ERR_UNSUPPORTED_NODE,
						  "node '%s': %s operation '%s' is not supported",
						  n->name ? n->name : "?",
						  vector ? "vector math" : "math", op);

	if (vector)
		sb_printf(&c->body, "    vec4 %s = vec4(%s, 1.0);\n", out, expr);
	else
		sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s), 1.0);\n", out, expr);
	return 1;
}

// MIX_RGB の blend_type

static int emit_blend(GraphCtx* c, const YbrShaderNode* n, const char* mode,
					  const char* fac, const char* a, const char* b,
					  const char* out)
{
	char expr[256];
	if (!strcmp(mode, "MIX"))
		snprintf(expr, sizeof(expr), "mix(%s.rgb, %s.rgb, %s.x)", a, b, fac);
	else if (!strcmp(mode, "ADD"))
		snprintf(expr, sizeof(expr), "%s.rgb + %s.rgb*%s.x", a, b, fac);
	else if (!strcmp(mode, "MULTIPLY"))
		snprintf(expr, sizeof(expr), "mix(%s.rgb, %s.rgb*%s.rgb, %s.x)", a, a,
				 b, fac);
	else if (!strcmp(mode, "SUBTRACT"))
		snprintf(expr, sizeof(expr), "%s.rgb - %s.rgb*%s.x", a, b, fac);
	else if (!strcmp(mode, "SCREEN"))
		snprintf(expr, sizeof(expr),
				 "mix(%s.rgb, vec3(1.0) - (vec3(1.0) - %s.rgb)*(vec3(1.0) - "
				 "%s.rgb), %s.x)",
				 a, a, b, fac);
	else if (!strcmp(mode, "DIVIDE"))
		snprintf(expr, sizeof(expr),
				 "mix(%s.rgb, %s.rgb/max(%s.rgb, vec3(1e-6)), %s.x)", a, a, b,
				 fac);
	else if (!strcmp(mode, "DARKEN"))
		snprintf(expr, sizeof(expr), "mix(%s.rgb, min(%s.rgb, %s.rgb), %s.x)",
				 a, a, b, fac);
	else if (!strcmp(mode, "LIGHTEN"))
		snprintf(expr, sizeof(expr), "mix(%s.rgb, max(%s.rgb, %s.rgb), %s.x)",
				 a, a, b, fac);
	else if (!strcmp(mode, "DIFFERENCE"))
		snprintf(expr, sizeof(expr), "mix(%s.rgb, abs(%s.rgb - %s.rgb), %s.x)",
				 a, a, b, fac);
	else if (!strcmp(mode, "EXCLUSION"))
		snprintf(expr, sizeof(expr),
				 "mix(%s.rgb, %s.rgb + %s.rgb - 2.0*%s.rgb*%s.rgb, %s.x)", a, a,
				 b, a, b, fac);
	else if (!strcmp(mode, "OVERLAY"))
		snprintf(
			expr, sizeof(expr),
			"mix(%s.rgb, mix(2.0*%s.rgb*%s.rgb, vec3(1.0) - 2.0*(vec3(1.0) - "
			"%s.rgb)*(vec3(1.0) - %s.rgb), step(vec3(0.5), %s.rgb)), %s.x)",
			a, a, b, a, b, a, fac);
	else if (!strcmp(mode, "SOFT_LIGHT"))
		snprintf(expr, sizeof(expr),
				 "mix(%s.rgb, (vec3(1.0) - %s.rgb)*%s.rgb*%s.rgb + "
				 "%s.rgb*(vec3(1.0) - "
				 "(vec3(1.0) - %s.rgb)*(vec3(1.0) - %s.rgb)), %s.x)",
				 a, b, a, a, a, a, b, fac);
	else if (!strcmp(mode, "LINEAR_LIGHT"))
		snprintf(expr, sizeof(expr), "%s.rgb + %s.x*(2.0*%s.rgb - vec3(1.0))",
				 a, fac, b);
	else if (!strcmp(mode, "DODGE"))
		snprintf(
			expr, sizeof(expr),
			"mix(%s.rgb, %s.rgb/max(vec3(1.0) - %s.rgb, vec3(1e-4)), %s.x)", a,
			a, b, fac);
	else if (!strcmp(mode, "BURN"))
		snprintf(expr, sizeof(expr),
				 "mix(%s.rgb, vec3(1.0) - (vec3(1.0) - %s.rgb)/max(%s.rgb, "
				 "vec3(1e-4)), %s.x)",
				 a, a, b, fac);
	else if (!strcmp(mode, "HUE") || !strcmp(mode, "SATURATION") ||
			 !strcmp(mode, "COLOR") || !strcmp(mode, "VALUE")) {
		// HSV 空間で成分ごとに差し替える
		emit_lib(c, YBR_LIB_HSV);
		sb_printf(&c->body,
				  "    vec3 %s_ha = ybrRgbToHsv(%s.rgb);\n"
				  "    vec3 %s_hb = ybrRgbToHsv(%s.rgb);\n",
				  out, a, out, b);
		char pick[128];
		if (!strcmp(mode, "HUE"))
			snprintf(pick, sizeof(pick), "vec3(%s_hb.x, %s_ha.y, %s_ha.z)", out,
					 out, out);
		else if (!strcmp(mode, "SATURATION"))
			snprintf(pick, sizeof(pick), "vec3(%s_ha.x, %s_hb.y, %s_ha.z)", out,
					 out, out);
		else if (!strcmp(mode, "COLOR"))
			snprintf(pick, sizeof(pick), "vec3(%s_hb.x, %s_hb.y, %s_ha.z)", out,
					 out, out);
		else
			snprintf(pick, sizeof(pick), "vec3(%s_ha.x, %s_ha.y, %s_hb.z)", out,
					 out, out);
		snprintf(expr, sizeof(expr), "mix(%s.rgb, ybrHsvToRgb(%s), %s.x)", a,
				 pick, fac);
	}
	else
		return graph_fail(c, YBR_SHADER_ERR_UNSUPPORTED_NODE,
						  "node '%s': blend type '%s' is not supported",
						  n->name ? n->name : "?", mode);

	sb_printf(&c->body, "    vec4 %s = vec4(%s, %s.a);\n", out, expr, a);
	if (prop_number(n, "use_clamp", 0.0) != 0.0)
		sb_printf(&c->body, "    %s = clamp(%s, 0.0, 1.0);\n", out, out);
	return 1;
}

// 入力ソケットの取得ヘルパー
// 無ければ fallback の GLSL 式を使う
static int in_or(GraphCtx* c, int ni, const char* name, char* out, size_t sz,
				 const char* fallback)
{
	const YbrShaderNode* n = &c->nodes[ni];
	int idx = input_index(n, name);
	if (0 <= idx) return emit_input(c, ni, idx, out, sz);
	snprintf(out, sz, "%s", fallback);
	return 1;
}

// テクスチャノードの Vector 入力。未接続なら位置を使う (Generated 相当)
static int in_vector(GraphCtx* c, int ni, char* out, size_t sz)
{
	const YbrShaderNode* n = &c->nodes[ni];
	int idx = input_index(n, "Vector");
	if (0 <= idx && find_link(c->links, c->linkCount, ni, idx))
		return emit_input(c, ni, idx, out, sz);
	snprintf(out, sz, "vec4(fragPosition, 1.0)");
	return 1;
}

// 位置 -> 色 の対応表 (カラーランプ / カーブ) を mix の連鎖として書き出す。
// 動的な配列参照は GLSL ES 1.00 で使えないため、あえて展開している。
static void emit_ramp_chain(GraphCtx* c, const char* var, const char* fac,
							const float* pos, const float* col, int stops,
							int constantInterp)
{
	char p0[32], p1[32], r[32], g[32], b[32], al[32];
	fmt_float(r, sizeof(r), col[0]);
	fmt_float(g, sizeof(g), col[1]);
	fmt_float(b, sizeof(b), col[2]);
	fmt_float(al, sizeof(al), col[3]);
	sb_printf(&c->body, "    vec4 %s = vec4(%s, %s, %s, %s);\n", var, r, g, b,
			  al);

	for (int i = 1; i < stops; i++) {
		fmt_float(p0, sizeof(p0), pos[i - 1]);
		fmt_float(p1, sizeof(p1), pos[i]);
		fmt_float(r, sizeof(r), col[i * 4 + 0]);
		fmt_float(g, sizeof(g), col[i * 4 + 1]);
		fmt_float(b, sizeof(b), col[i * 4 + 2]);
		fmt_float(al, sizeof(al), col[i * 4 + 3]);
		if (constantInterp)
			sb_printf(&c->body,
					  "    %s = mix(%s, vec4(%s, %s, %s, %s), step(%s, %s));\n",
					  var, var, r, g, b, al, p1, fac);
		else
			sb_printf(
				&c->body,
				"    %s = mix(%s, vec4(%s, %s, %s, %s), clamp((%s - %s)/max(%s "
				"- %s, 1e-6), 0.0, 1.0));\n",
				var, var, r, g, b, al, fac, p0, p1, p0);
	}
}

// 等間隔にサンプルされたカーブ (0..1 -> 値) を mix の連鎖にする
static void emit_lut_chain(GraphCtx* c, const char* var, const char* fac,
						   const float* lut, int count)
{
	char v[32];
	fmt_float(v, sizeof(v), lut[0]);
	sb_printf(&c->body, "    float %s = %s;\n", var, v);
	for (int i = 1; i < count; i++) {
		char p0[32], p1[32];
		fmt_float(p0, sizeof(p0), (float)(i - 1) / (float)(count - 1));
		fmt_float(p1, sizeof(p1), (float)i / (float)(count - 1));
		fmt_float(v, sizeof(v), lut[i]);
		sb_printf(
			&c->body,
			"    %s = mix(%s, %s, clamp((%s - %s)/max(%s - %s, 1e-6), 0.0, "
			"1.0));\n",
			var, var, v, fac, p0, p1, p0);
	}
}

// ----------------------------------------------------------------------------
// ノード本体

// ノードグループ

#define YBR_MAX_GROUP_ARGS 16
#define YBR_MAX_GROUP_DEPTH 8

// GROUP ノードが指しているノードグループを探す
static const YbrNodeGroup* find_node_group(GraphCtx* c, const YbrShaderNode* n)
{
	if (!c->scene) return NULL;
	const char* id = prop_string(n, "node_tree");
	if (!id) return NULL;
	return YbrFindNodeGroup(c->scene, id);
}

// グループの出力ソケット 1 本ぶんを GLSL の関数として書き出し、関数名を返す。
// すでに書き出してあれば、その名前を使い回す。
static int emit_group_function(GraphCtx* c, const YbrNodeGroup* g,
							   int outSocket, char* outName, size_t sz)
{
	if (outSocket < 0) outSocket = 0;

	for (int i = 0; i < c->groupFnCount; i++) {
		if (c->groupFns[i].group == g &&
			c->groupFns[i].outSocket == outSocket) {
			snprintf(outName, sz, "%s", c->groupFns[i].name);
			return 1;
		}
	}
	if (YBR_MAX_GROUP_DEPTH <= c->groupDepth)
		return graph_fail(c, YBR_SHADER_ERR_UNSUPPORTED_NODE,
						  "node group '%s' is nested too deeply",
						  g->id ? g->id : "?");

	// グループの出口 (GROUP_OUTPUT) を探す
	int outNode = -1;
	for (int i = 0; i < g->nodeCount; i++)
		if (g->nodes[i].type == YBR_SN_GROUP_OUTPUT) {
			outNode = i;
			break;
		}
	if (outNode < 0) return 0;
	if (g->nodes[outNode].inputCount <= outSocket) return 0;

	char name[YBR_VAR_LEN];
	snprintf(name, sizeof(name), "ybrGroup%d_%d", c->groupFnCount, outSocket);

	int argc = g->inputCount;
	if (YBR_MAX_GROUP_ARGS < argc) argc = YBR_MAX_GROUP_ARGS;

	// グラフを差し替えて本体を作る
	const YbrShaderNode* saveNodes = c->nodes;
	const YbrShaderLink* saveLinks = c->links;
	int saveNodeCount = c->nodeCount, saveLinkCount = c->linkCount;
	char* saveMemo = c->memo;
	StrBuf saveBody = c->body;
	const char* saveArg = c->groupArg;
	const char* saveTag = c->uniTag;
	// グループの中で作る uniform 名が親のものとぶつからないようにする
	char tag[16];
	snprintf(tag, sizeof(tag), "G%d", c->groupFnCount);

	char* memo = (char*)YBR_CALLOC(
		(size_t)(0 < g->nodeCount ? g->nodeCount : 1) * YBR_MAX_OUTPUTS,
		YBR_VAR_LEN);
	if (!memo) {
		c->error = YBR_SHADER_ERR_OUT_OF_MEMORY;
		return 0;
	}

	c->nodes = g->nodes;
	c->nodeCount = g->nodeCount;
	c->links = g->links;
	c->linkCount = g->linkCount;
	c->memo = memo;
	c->groupArg = "ybrGArg";
	c->uniTag = tag;
	c->groupDepth++;
	memset(&c->body, 0, sizeof(c->body));

	char result[YBR_VAR_LEN];
	int ok = emit_input(c, outNode, outSocket, result, sizeof(result));
	StrBuf inner = c->body;

	c->nodes = saveNodes;
	c->nodeCount = saveNodeCount;
	c->links = saveLinks;
	c->linkCount = saveLinkCount;
	c->memo = saveMemo;
	c->body = saveBody;
	c->groupArg = saveArg;
	c->uniTag = saveTag;
	c->groupDepth--;
	YBR_FREE(memo);

	if (!ok || inner.failed) {
		sb_free(&inner);
		return 0;
	}

	// 関数として書き出す
	sb_printf(&c->funcs, "\n/* node group '%s' -> %s */\nvec4 %s(",
			  g->id ? g->id : "?",
			  (outSocket < g->outputCount && g->outputs[outSocket].name)
				  ? g->outputs[outSocket].name
				  : "output",
			  name);
	if (argc <= 0) {
		sb_puts(&c->funcs, "void");
	}
	else {
		for (int i = 0; i < argc; i++)
			sb_printf(&c->funcs, "%svec4 ybrGArg%d", (0 < i) ? ", " : "", i);
	}
	sb_puts(&c->funcs, ")\n{\n");
	sb_puts(&c->funcs, inner.data ? inner.data : "");
	sb_printf(&c->funcs, "    return %s;\n}\n", result);
	sb_free(&inner);

	if (c->groupFnCount < YBR_MAX_GROUP_FNS) {
		c->groupFns[c->groupFnCount].group = g;
		c->groupFns[c->groupFnCount].outSocket = outSocket;
		snprintf(c->groupFns[c->groupFnCount].name,
				 sizeof(c->groupFns[c->groupFnCount].name), "%s", name);
		c->groupFnCount++;
	}
	snprintf(outName, sz, "%s", name);
	return 1;
}

static int emit_node(GraphCtx* c, int nodeIndex, int outSocket, char* out,
					 size_t sz)
{
	if (c->error != YBR_SHADER_OK) return 0;
	if (nodeIndex < 0 || c->nodeCount <= nodeIndex)
		return graph_fail(c, YBR_SHADER_ERR_UNSUPPORTED_NODE,
						  "link points outside the graph");
	if (YBR_MAX_DEPTH < ++c->depth)
		return graph_fail(c, YBR_SHADER_ERR_UNSUPPORTED_NODE,
						  "node graph is too deep or has a cycle");

	// viewPos (視点) を使うノードは、ライトの有無に関わらず宣言が要る
	switch (c->nodes[nodeIndex].type) {
		case YBR_SN_TEX_COORD:
		case YBR_SN_FRESNEL:
		case YBR_SN_LAYER_WEIGHT:
		case YBR_SN_CAMERA:
		case YBR_SN_BSDF_GLASS:
		case YBR_SN_BSDF_REFRACTION:
			emit_view_uniform(&c->decls, c->set, &c->viewEmitted);
			break;
		default:
			break;
	}

	char* slot = memo_slot(c, nodeIndex, outSocket);
	if (slot && slot[0]) {
		snprintf(out, sz, "%s", slot);
		c->depth--;
		return 1;
	}

	const YbrShaderNode* n = &c->nodes[nodeIndex];
	int ok = 1;
	char a[YBR_VAR_LEN], b[YBR_VAR_LEN], t[YBR_VAR_LEN];

	switch (n->type) {
		case YBR_SN_REROUTE:
			ok = emit_input(c, nodeIndex, 0, out, sz);
			c->depth--;
			return ok;

		case YBR_SN_RGB:
		case YBR_SN_VALUE: {
			const YbrShaderSocket* s =
				(0 < n->outputCount) ? &n->outputs[0] : NULL;
			float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			int cnt = s ? s->valueCount : 0;
			for (int i = 0; i < cnt && i < 4; i++) v[i] = (&s->value.x)[i];
			char uname[YBR_VAR_LEN];
			snprintf(uname, sizeof(uname), "ybr%sConst%d", c->uniTag,
					 nodeIndex);
			new_var(c, out, sz);
			if (n->type == YBR_SN_VALUE) {
				sb_printf(&c->decls, "uniform float %s;\n", uname);
				uni_add(c->set, uname, YBR_UNIFORM_FLOAT, -1, 0, v, 1, NULL);
				sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s), 1.0);\n", out,
						  uname);
			}
			else {
				if (cnt < 4) v[3] = 1.0f;
				sb_printf(&c->decls, "uniform vec4 %s;\n", uname);
				uni_add(c->set, uname, YBR_UNIFORM_VEC4, -1, 0, v, 4, NULL);
				sb_printf(&c->body, "    vec4 %s = %s;\n", out, uname);
			}
		} break;

		case YBR_SN_TEX_IMAGE: {
			char uv[YBR_VAR_LEN];
			int vecIdx = input_index(n, "Vector");
			int haveUV = 0;
			if (0 <= vecIdx &&
				find_link(c->links, c->linkCount, nodeIndex, vecIdx)) {
				if (!emit_input(c, nodeIndex, vecIdx, uv, sizeof(uv))) {
					c->depth--;
					return 0;
				}
				haveUV = 1;
			}
			char tname[YBR_VAR_LEN];
			int locIndex = -1, autoSet = 0;
			if (c->texCount == 0) {
				snprintf(tname, sizeof(tname), "texture0");
				locIndex = RL_SHADER_LOC_MAP_ALBEDO;
				autoSet = 1;
			}
			else if (c->texCount == 1) {
				snprintf(tname, sizeof(tname), "texture1");
				locIndex = RL_SHADER_LOC_MAP_METALNESS;
				autoSet = 1;
			}
			else if (c->texCount == 2) {
				snprintf(tname, sizeof(tname), "texture2");
				locIndex = RL_SHADER_LOC_MAP_NORMAL;
				autoSet = 1;
			}
			else
				snprintf(tname, sizeof(tname), "ybrTexture%d", c->texCount);
			c->texCount++;
			sb_printf(&c->decls, "uniform sampler2D %s;\n", tname);
			{
				int tw, tf;
				node_tex_props(n, &tw, &tf);
				uni_add_tex(c->set, tname, YBR_UNIFORM_SAMPLER2D, locIndex,
							autoSet, NULL, 0, prop_string(n, "image"), tw, tf);
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = %s(%s, %s);\n", out,
					  c->d.texture2D, tname, haveUV ? uv : "fragTexCoord");
			if (haveUV)
				sb_printf(&c->body, "    %s = %s(%s, %s.xy);\n", out,
						  c->d.texture2D, tname, uv);
			if (outSocket == 1) {
				// Alpha 出力
				sb_printf(&c->body, "    %s = vec4(vec3(%s.a), 1.0);\n", out,
						  out);
			}
		} break;

		case YBR_SN_TEX_COORD:
		case YBR_SN_UVMAP:
			// 出力: 0 Generated / 1 Normal / 2 UV / 3 Object / 4 Camera /
			// 5 Window / 6 Reflection
			new_var(c, out, sz);
			if (n->type == YBR_SN_UVMAP || outSocket == 2)
				sb_printf(&c->body,
						  "    vec4 %s = vec4(fragTexCoord, 0.0, 1.0);\n", out);
			else if (outSocket == 1)
				sb_printf(&c->body,
						  "    vec4 %s = vec4(normalize(fragNormal), 1.0);\n",
						  out);
			else if (outSocket == 4) {
				emit_lit_function(c);
				sb_printf(&c->body,
						  "    vec4 %s = vec4(fragPosition - viewPos, 1.0);\n",
						  out);
			}
			else if (outSocket == 5)
				sb_printf(&c->body,
						  "    vec4 %s = vec4(fragTexCoord, 0.0, 1.0);\n", out);
			else if (outSocket == 6) {
				emit_lit_function(c);
				sb_printf(&c->body,
						  "    vec4 %s = vec4(reflect(normalize(fragPosition - "
						  "viewPos), "
						  "normalize(fragNormal)), 1.0);\n",
						  out);
			}
			else
				sb_printf(&c->body, "    vec4 %s = vec4(fragPosition, 1.0);\n",
						  out);
			break;

		case YBR_SN_ATTRIBUTE:
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = fragColor;\n", out);
			break;

		case YBR_SN_NEW_GEOMETRY:
			new_var(c, out, sz);
			if (outSocket == 1)
				sb_printf(&c->body,
						  "    vec4 %s = vec4(normalize(fragNormal), 1.0);\n",
						  out);
			else
				sb_printf(&c->body, "    vec4 %s = vec4(fragPosition, 1.0);\n",
						  out);
			break;

		case YBR_SN_MATH: {
			char cc[YBR_VAR_LEN];
			const char* op = prop_string(n, "operation");
			if (!op) op = "ADD";
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (1 < n->inputCount) {
				if (!emit_input(c, nodeIndex, 1, b, sizeof(b))) {
					c->depth--;
					return 0;
				}
			}
			else
				snprintf(b, sizeof(b), "vec4(0.0)");
			if (2 < n->inputCount) {
				if (!emit_input(c, nodeIndex, 2, cc, sizeof(cc))) {
					c->depth--;
					return 0;
				}
			}
			else
				snprintf(cc, sizeof(cc), "vec4(0.0)");
			new_var(c, out, sz);
			ok = emit_math_op(c, n, op, a, b, cc, out, 0);
		} break;

		case YBR_SN_VECT_MATH: {
			const char* op = prop_string(n, "operation");
			if (!op) op = "ADD";
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (1 < n->inputCount) {
				if (!emit_input(c, nodeIndex, 1, b, sizeof(b))) {
					c->depth--;
					return 0;
				}
			}
			else
				snprintf(b, sizeof(b), "vec4(0.0)");
			new_var(c, out, sz);
			ok = emit_math_op(c, n, op, a, b, b, out, 1);
		} break;

		case YBR_SN_MIX_RGB: {
			const char* mode = prop_string(n, "blend_type");
			if (!mode) mode = "MIX";
			if (!emit_input(c, nodeIndex, 0, t, sizeof(t))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 1, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 2, b, sizeof(b))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			ok = emit_blend(c, n, mode, t, a, b, out);
		} break;

		case YBR_SN_MIX: {
			const char* mode = prop_string(n, "blend_type");
			if (!mode) mode = "MIX";
			int fi = input_index(n, "Factor");
			int ai = input_index(n, "A");
			int bi = input_index(n, "B");
			if (fi < 0 || ai < 0 || bi < 0) {
				ok = graph_fail(
					c, YBR_SHADER_ERR_MISSING_SOCKET,
					"node '%s': Mix node inputs (Factor/A/B) not found",
					n->name ? n->name : "?");
				break;
			}
			if (!emit_input(c, nodeIndex, fi, t, sizeof(t))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, ai, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, bi, b, sizeof(b))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			ok = emit_blend(c, n, mode, t, a, b, out);
		} break;

		case YBR_SN_INVERT:
			if (!emit_input(c, nodeIndex, 0, t, sizeof(t))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 1, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec4 %s = vec4(mix(%s.rgb, vec3(1.0) - %s.rgb, "
					  "%s.x), %s.a);\n",
					  out, a, a, t, a);
			break;

		case YBR_SN_GAMMA:
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 1, b, sizeof(b))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec4 %s = vec4(pow(max(%s.rgb, vec3(0.0)), "
					  "vec3(%s.x)), %s.a);\n",
					  out, a, b, a);
			break;

		case YBR_SN_BRIGHTCONTRAST: {
			char cc[YBR_VAR_LEN];
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 1, b, sizeof(b))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 2, cc, sizeof(cc))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(
				&c->body,
				"    vec4 %s = vec4(clamp((%s.rgb - 0.5)*(1.0 + %s.x) + 0.5 + "
				"%s.x, 0.0, 1.0), %s.a);\n",
				out, a, cc, b, a);
		} break;

		case YBR_SN_RGBTOBW:
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(
				&c->body,
				"    vec4 %s = vec4(vec3(dot(%s.rgb, vec3(0.2126, 0.7152, "
				"0.0722))), 1.0);\n",
				out, a);
			break;

		case YBR_SN_SEPRGB:
		case YBR_SN_SEPXYZ: {
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			const char* comp = (outSocket == 1)	  ? "y"
							   : (outSocket == 2) ? "z"
												  : "x";
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s.%s), 1.0);\n", out,
					  a, comp);
		} break;

		case YBR_SN_COMBRGB:
		case YBR_SN_COMBXYZ: {
			char cc[YBR_VAR_LEN];
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 1, b, sizeof(b))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 2, cc, sizeof(cc))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = vec4(%s.x, %s.x, %s.x, 1.0);\n",
					  out, a, b, cc);
		} break;

		case YBR_SN_CLAMP: {
			char cc[YBR_VAR_LEN];
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 1, b, sizeof(b))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 2, cc, sizeof(cc))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(
				&c->body,
				"    vec4 %s = vec4(vec3(clamp(%s.x, %s.x, %s.x)), 1.0);\n",
				out, a, b, cc);
		} break;

		case YBR_SN_MAP_RANGE: {
			char f2[YBR_VAR_LEN], t1[YBR_VAR_LEN], t2[YBR_VAR_LEN];
			if (n->inputCount < 5) {
				ok = graph_fail(c, YBR_SHADER_ERR_MISSING_SOCKET,
								"node '%s': Map Range needs 5 inputs",
								n->name ? n->name : "?");
				break;
			}
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 1, b, sizeof(b))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 2, f2, sizeof(f2))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 3, t1, sizeof(t1))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 4, t2, sizeof(t2))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(
				&c->body,
				"    float %s_t = (%s.x - %s.x)/max(%s.x - %s.x, 1e-6);\n", out,
				a, b, f2, b);
			sb_printf(
				&c->body,
				"    vec4 %s = vec4(vec3(mix(%s.x, %s.x, clamp(%s_t, 0.0, "
				"1.0))), 1.0);\n",
				out, t1, t2, out);
		} break;

		case YBR_SN_FRESNEL:
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(
				&c->body,
				"    float %s_f = pow(1.0 - max(dot(normalize(fragNormal), "
				"normalize(viewPos - fragPosition)), 0.0), max(%s.x, 1.0));\n",
				out, a);
			sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s_f), 1.0);\n", out,
					  out);
			emit_lit_function(c);
			break;

		case YBR_SN_LAYER_WEIGHT:
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    float %s_f = 1.0 - max(dot(normalize(fragNormal), "
					  "normalize(viewPos - fragPosition)), 0.0);\n",
					  out);
			sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s_f), 1.0);\n", out,
					  out);
			emit_lit_function(c);
			break;

		case YBR_SN_NORMAL_MAP: {
			// 接線があれば TBN で解く。無ければワールド法線で近似する。
			char strength[YBR_VAR_LEN];
			if (!in_or(c, nodeIndex, "Strength", strength, sizeof(strength),
					   "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Color", a, sizeof(a),
					   "vec4(0.5, 0.5, 1.0, 1.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec3 %s_n = normalize(fragNormal);\n",
					  out);
			sb_printf(&c->body,
					  "    vec3 %s_m = %s.rgb*2.0 - 1.0;\n"
					  "    %s_m.xy *= %s.x;\n",
					  out, a, out, strength);
			sb_printf(
				&c->body,
				"    if (0.0001 < dot(fragTangent.xyz, fragTangent.xyz)) {\n"
				"        vec3 %s_t = normalize(fragTangent.xyz -\n"
				"                              %s_n*dot(%s_n, "
				"fragTangent.xyz));\n"
				"        vec3 %s_b = cross(%s_n, %s_t)*fragTangent.w;\n"
				"        %s_n = normalize(mat3(%s_t, %s_b, "
				"%s_n)*normalize(%s_m));\n"
				"    }\n",
				out, out, out, out, out, out, out, out, out, out, out);
			sb_printf(&c->body, "    vec4 %s = vec4(%s_n, 1.0);\n", out, out);
		} break;

		// ---------------- シェーダーノード (結果はライティング済みの色)
		// ---------
		case YBR_SN_BSDF_PRINCIPLED:
		case YBR_SN_BSDF_GLOSSY: {
			static const char* const baseNames[] = {"Base Color", "Color"};
			static const char* const specNames[] = {"Specular IOR Level",
													"Specular"};
			static const char* const emitNames[] = {"Emission Color",
													"Emission"};
			char metal[YBR_VAR_LEN], rough[YBR_VAR_LEN], spec[YBR_VAR_LEN];
			char alpha[YBR_VAR_LEN], emis[YBR_VAR_LEN], estr[YBR_VAR_LEN];

			emit_lit_function(c);

			int idx = input_index_any(n, baseNames, 2);
			if (idx < 0) {
				ok = graph_fail(c, YBR_SHADER_ERR_MISSING_SOCKET,
								"node '%s': base color input not found",
								n->name ? n->name : "?");
				break;
			}
			if (!emit_input(c, nodeIndex, idx, a, sizeof(a))) {
				c->depth--;
				return 0;
			}

			idx = input_index(n, "Metallic");
			if (0 <= idx) {
				if (!emit_input(c, nodeIndex, idx, metal, sizeof(metal))) {
					c->depth--;
					return 0;
				}
			}
			else
				snprintf(metal, sizeof(metal), "%s",
						 (n->type == YBR_SN_BSDF_GLOSSY) ? "vec4(1.0)"
														 : "vec4(0.0)");

			idx = input_index(n, "Roughness");
			if (0 <= idx) {
				if (!emit_input(c, nodeIndex, idx, rough, sizeof(rough))) {
					c->depth--;
					return 0;
				}
			}
			else
				snprintf(rough, sizeof(rough), "vec4(0.5)");

			idx = input_index_any(n, specNames, 2);
			if (0 <= idx) {
				if (!emit_input(c, nodeIndex, idx, spec, sizeof(spec))) {
					c->depth--;
					return 0;
				}
			}
			else
				snprintf(spec, sizeof(spec), "vec4(0.5)");

			idx = input_index(n, "Alpha");
			if (0 <= idx) {
				if (!emit_input(c, nodeIndex, idx, alpha, sizeof(alpha))) {
					c->depth--;
					return 0;
				}
			}
			else
				snprintf(alpha, sizeof(alpha), "vec4(1.0)");

			idx = input_index_any(n, emitNames, 2);
			if (0 <= idx) {
				if (!emit_input(c, nodeIndex, idx, emis, sizeof(emis))) {
					c->depth--;
					return 0;
				}
			}
			else
				snprintf(emis, sizeof(emis), "vec4(0.0)");

			idx = input_index(n, "Emission Strength");
			if (0 <= idx) {
				if (!emit_input(c, nodeIndex, idx, estr, sizeof(estr))) {
					c->depth--;
					return 0;
				}
			}
			else
				snprintf(estr, sizeof(estr), "vec4(0.0)");

			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec4 %s = vec4(ybrShade(%s.rgb, %s.x, %s.x, %s.x) + "
					  "%s.rgb*%s.x, %s.x);\n",
					  out, a, metal, rough, spec, emis, estr, alpha);
		} break;

		case YBR_SN_BSDF_DIFFUSE: {
			char rough[YBR_VAR_LEN];
			emit_lit_function(c);
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			int idx = input_index(n, "Roughness");
			if (0 <= idx) {
				if (!emit_input(c, nodeIndex, idx, rough, sizeof(rough))) {
					c->depth--;
					return 0;
				}
			}
			else
				snprintf(rough, sizeof(rough), "vec4(1.0)");
			new_var(c, out, sz);
			sb_printf(
				&c->body,
				"    vec4 %s = vec4(ybrShade(%s.rgb, 0.0, %s.x, 0.0), %s.a);\n",
				out, a, rough, a);
		} break;

		case YBR_SN_EMISSION:
		case YBR_SN_BACKGROUND: {
			char strength[YBR_VAR_LEN];
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			int idx = input_index(n, "Strength");
			if (0 <= idx) {
				if (!emit_input(c, nodeIndex, idx, strength,
								sizeof(strength))) {
					c->depth--;
					return 0;
				}
			}
			else
				snprintf(strength, sizeof(strength), "vec4(1.0)");
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = vec4(%s.rgb*%s.x, %s.a);\n", out,
					  a, strength, a);
		} break;

		case YBR_SN_BSDF_TRANSPARENT:
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = vec4(%s.rgb, 0.0);\n", out, a);
			break;

		case YBR_SN_MIX_SHADER: {
			char fac[YBR_VAR_LEN];
			if (!emit_input(c, nodeIndex, 0, fac, sizeof(fac))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 1, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 2, b, sizeof(b))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec4 %s = mix(%s, %s, clamp(%s.x, 0.0, 1.0));\n",
					  out, a, b, fac);
		} break;

		case YBR_SN_ADD_SHADER:
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 1, b, sizeof(b))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec4 %s = vec4(%s.rgb + %s.rgb, max(%s.a, %s.a));\n",
					  out, a, b, a, b);
			break;

		// ================= プロシージャルテクスチャ =================
		case YBR_SN_TEX_NOISE: {
			char sc[YBR_VAR_LEN], det[YBR_VAR_LEN], rgh[YBR_VAR_LEN];
			char lac[YBR_VAR_LEN], dst[YBR_VAR_LEN], w[YBR_VAR_LEN];
			emit_lib(c, YBR_LIB_FBM);
			if (!in_vector(c, nodeIndex, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "W", w, sizeof(w), "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Scale", sc, sizeof(sc), "vec4(5.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Detail", det, sizeof(det), "vec4(2.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Roughness", rgh, sizeof(rgh),
					   "vec4(0.5)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Lacunarity", lac, sizeof(lac),
					   "vec4(2.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Distortion", dst, sizeof(dst),
					   "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec3 %s_p = %s.xyz*%s.x + vec3(%s.x);\n",
					  out, a, sc, w);
			sb_printf(&c->body,
					  "    %s_p += %s.x*vec3(ybrPerlin(%s_p + vec3(13.5)),\n"
					  "                      ybrPerlin(%s_p + vec3(27.1)),\n"
					  "                      ybrPerlin(%s_p + vec3(41.7)));\n",
					  out, dst, out, out, out);
			sb_printf(&c->body,
					  "    float %s_f = ybrNoise01(%s_p, %s.x, %s.x, %s.x);\n",
					  out, out, det, rgh, lac);
			if (outSocket == 1) {
				// Color 出力
				sb_printf(&c->body,
						  "    vec4 %s = vec4(%s_f,\n"
						  "                   ybrNoise01(%s_p + vec3(7.3), "
						  "%s.x, %s.x, %s.x),\n"
						  "                   ybrNoise01(%s_p + vec3(19.7), "
						  "%s.x, %s.x, %s.x), "
						  "1.0);\n",
						  out, out, out, det, rgh, lac, out, det, rgh, lac);
			}
			else {
				sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s_f), 1.0);\n",
						  out, out);
			}
		} break;

		case YBR_SN_TEX_MUSGRAVE: {
			// Blender 4.1 で廃止されたノード。5 種類のフラクタルを再現する
			char sc[YBR_VAR_LEN], det[YBR_VAR_LEN], dim[YBR_VAR_LEN];
			char lac[YBR_VAR_LEN], off[YBR_VAR_LEN], gan[YBR_VAR_LEN],
				w[YBR_VAR_LEN];
			const char* kind = prop_string(n, "musgrave_type");
			if (!kind) kind = "FBM";
			const char* kid = "3.0";  // FBM
			if (!strcmp(kind, "MULTIFRACTAL"))
				kid = "0.0";
			else if (!strcmp(kind, "RIDGED_MULTIFRACTAL"))
				kid = "1.0";
			else if (!strcmp(kind, "HYBRID_MULTIFRACTAL"))
				kid = "2.0";
			else if (!strcmp(kind, "HETERO_TERRAIN"))
				kid = "4.0";

			emit_lib(c, YBR_LIB_MUSGRAVE);
			if (!in_vector(c, nodeIndex, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "W", w, sizeof(w), "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Scale", sc, sizeof(sc), "vec4(5.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Detail", det, sizeof(det), "vec4(2.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Dimension", dim, sizeof(dim),
					   "vec4(2.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Lacunarity", lac, sizeof(lac),
					   "vec4(2.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Offset", off, sizeof(off), "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Gain", gan, sizeof(gan), "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec3 %s_p = %s.xyz*%s.x + vec3(%s.x);\n",
					  out, a, sc, w);
			sb_printf(&c->body,
					  "    float %s_f = ybrMusgrave(%s_p, %s, %s.x, %s.x, "
					  "%s.x, %s.x, "
					  "%s.x);\n",
					  out, out, kid, dim, lac, det, off, gan);
			sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s_f), 1.0);\n", out,
					  out);
		} break;

		case YBR_SN_TEX_WHITE_NOISE: {
			emit_lib(c, YBR_LIB_HASH);
			if (!in_vector(c, nodeIndex, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			if (outSocket == 1) {
				// Color
				sb_printf(
					&c->body,
					"    vec4 %s = vec4(ybrHash13(%s.xyz), ybrHash13(%s.xyz + "
					"vec3(3.7)),\n"
					"                   ybrHash13(%s.xyz + vec3(9.1)), 1.0);\n",
					out, a, a, a);
			}
			else {
				sb_printf(&c->body,
						  "    vec4 %s = vec4(vec3(ybrHash13(%s.xyz)), 1.0);\n",
						  out, a);
			}
		} break;

		case YBR_SN_TEX_CHECKER: {
			char c1[YBR_VAR_LEN], c2[YBR_VAR_LEN], sc[YBR_VAR_LEN];
			if (!in_vector(c, nodeIndex, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Color1", c1, sizeof(c1),
					   "vec4(0.8, 0.8, 0.8, 1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Color2", c2, sizeof(c2),
					   "vec4(0.2, 0.2, 0.2, 1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Scale", sc, sizeof(sc), "vec4(5.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec3 %s_p = floor(%s.xyz*%s.x + 1e-5);\n",
					  out, a, sc);
			sb_printf(&c->body,
					  "    float %s_f = mod(%s_p.x + %s_p.y + %s_p.z, 2.0);\n",
					  out, out, out, out);
			if (outSocket == 1) {
				// Fac
				sb_printf(&c->body,
						  "    vec4 %s = vec4(vec3(1.0 - %s_f), 1.0);\n", out,
						  out);
			}
			else {
				sb_printf(&c->body, "    vec4 %s = mix(%s, %s, %s_f);\n", out,
						  c1, c2, out);
			}
		} break;

		case YBR_SN_TEX_BRICK: {
			char c1[YBR_VAR_LEN], c2[YBR_VAR_LEN], mc[YBR_VAR_LEN];
			char sc[YBR_VAR_LEN], mrt[YBR_VAR_LEN], bw[YBR_VAR_LEN],
				rh[YBR_VAR_LEN];
			char bias[YBR_VAR_LEN];
			double offset = prop_number(n, "offset", 0.5);
			double offreq = prop_number(n, "offset_frequency", 2.0);
			double squash = prop_number(n, "squash", 1.0);
			double sqfreq = prop_number(n, "squash_frequency", 2.0);
			char offs[32], offf[32], sqs[32], sqf[32];
			fmt_float(offs, sizeof(offs), offset);
			fmt_float(offf, sizeof(offf), offreq < 1.0 ? 1.0 : offreq);
			fmt_float(sqs, sizeof(sqs), squash);
			fmt_float(sqf, sizeof(sqf), sqfreq < 1.0 ? 1.0 : sqfreq);

			if (!in_vector(c, nodeIndex, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Color1", c1, sizeof(c1),
					   "vec4(0.8, 0.8, 0.8, 1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Color2", c2, sizeof(c2),
					   "vec4(0.2, 0.2, 0.2, 1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Mortar", mc, sizeof(mc),
					   "vec4(0.0, 0.0, 0.0, 1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Scale", sc, sizeof(sc), "vec4(5.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Mortar Size", mrt, sizeof(mrt),
					   "vec4(0.02)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Brick Width", bw, sizeof(bw),
					   "vec4(0.5)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Row Height", rh, sizeof(rh),
					   "vec4(0.25)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Bias", bias, sizeof(bias), "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			emit_lib(c, YBR_LIB_HASH);
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec3 %s_p = %s.xyz*%s.x;\n", out, a, sc);
			sb_printf(&c->body,
					  "    float %s_row = floor(%s_p.y/max(%s.x, 1e-4));\n",
					  out, out, rh);
			sb_printf(
				&c->body,
				"    float %s_shift = (1.0 <= mod(%s_row, %s)) ? %s : 0.0;\n",
				out, out, offf, offs);
			// squash_frequency 行ごとにレンガの幅を squash 倍する
			sb_printf(
				&c->body,
				"    float %s_sq = (1.0 <= mod(%s_row, %s)) ? %s : 1.0;\n", out,
				out, sqf, sqs);
			sb_printf(
				&c->body,
				"    float %s_bx = %s_p.x/max(%s.x*%s_sq, 1e-4) + %s_shift;\n",
				out, out, bw, out, out);
			sb_printf(&c->body, "    float %s_col = floor(%s_bx);\n", out, out);
			sb_printf(&c->body,
					  "    vec2 %s_uv = vec2(fract(%s_bx), "
					  "fract(%s_p.y/max(%s.x, 1e-4)));\n",
					  out, out, out, rh);
			sb_printf(&c->body,
					  "    float %s_m = min(min(%s_uv.x, 1.0 - %s_uv.x), "
					  "min(%s_uv.y, "
					  "1.0 - %s_uv.y));\n",
					  out, out, out, out, out);
			sb_printf(&c->body,
					  "    float %s_f = 1.0 - step(%s_m, max(%s.x, 0.0));\n",
					  out, out, mrt);
			sb_printf(
				&c->body,
				"    float %s_t = clamp(ybrHash13(vec3(%s_col, %s_row, 0.0)) + "
				"%s.x, 0.0, 1.0);\n",
				out, out, out, bias);
			sb_printf(&c->body, "    vec4 %s_brick = mix(%s, %s, %s_t);\n", out,
					  c1, c2, out);
			if (outSocket == 1) {
				// Fac (モルタル部分が 1)
				sb_printf(&c->body,
						  "    vec4 %s = vec4(vec3(1.0 - %s_f), 1.0);\n", out,
						  out);
			}
			else {
				sb_printf(&c->body, "    vec4 %s = mix(%s, %s_brick, %s_f);\n",
						  out, mc, out, out);
			}
		} break;

		case YBR_SN_TEX_GRADIENT: {
			const char* kind = prop_string(n, "gradient_type");
			if (!kind) kind = "LINEAR";
			if (!in_vector(c, nodeIndex, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			if (!strcmp(kind, "QUADRATIC"))
				sb_printf(&c->body,
						  "    float %s_f = max(%s.x, 0.0)*max(%s.x, 0.0);\n",
						  out, a, a);
			else if (!strcmp(kind, "EASING"))
				sb_printf(&c->body,
						  "    float %s_f = smoothstep(0.0, 1.0, %s.x);\n", out,
						  a);
			else if (!strcmp(kind, "DIAGONAL"))
				sb_printf(&c->body, "    float %s_f = (%s.x + %s.y)*0.5;\n",
						  out, a, a);
			else if (!strcmp(kind, "SPHERICAL"))
				sb_printf(&c->body,
						  "    float %s_f = max(1.0 - length(%s.xyz), 0.0);\n",
						  out, a);
			else if (!strcmp(kind, "QUADRATIC_SPHERE"))
				sb_printf(&c->body,
						  "    float %s_f = pow(max(1.0 - length(%s.xyz), "
						  "0.0), 2.0);\n",
						  out, a);
			else if (!strcmp(kind, "RADIAL"))
				sb_printf(
					&c->body,
					"    float %s_f = atan(%s.y, %s.x)/6.2831853 + 0.5;\n", out,
					a, a);
			else
				sb_printf(&c->body, "    float %s_f = %s.x;\n", out, a);
			// 出力は Color と Fac の 2 本あるが、どちらも同じグレースケール値
			sb_printf(&c->body,
					  "    vec4 %s = vec4(vec3(clamp(%s_f, 0.0, 1.0)), 1.0);\n",
					  out, out);
		} break;

		case YBR_SN_TEX_MAGIC: {
			char sc[YBR_VAR_LEN], dis[YBR_VAR_LEN];
			double depth = prop_number(n, "turbulence_depth", 2.0);
			char dep[32];
			fmt_float(dep, sizeof(dep), depth);
			emit_lib(c, YBR_LIB_MAGIC);
			if (!in_vector(c, nodeIndex, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Scale", sc, sizeof(sc), "vec4(5.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Distortion", dis, sizeof(dis),
					   "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec3 %s_c = ybrMagic(%s.xyz*%s.x, %s, %s.x);\n", out,
					  a, sc, dep, dis);
			if (outSocket == 1) {
				// Fac
				sb_printf(&c->body,
						  "    vec4 %s = vec4(vec3((%s_c.x + %s_c.y + "
						  "%s_c.z)/3.0), 1.0);\n",
						  out, out, out, out);
			}
			else {
				sb_printf(&c->body, "    vec4 %s = vec4(%s_c, 1.0);\n", out,
						  out);
			}
		} break;

		case YBR_SN_TEX_VORONOI: {
			char sc[YBR_VAR_LEN], rnd[YBR_VAR_LEN], smo[YBR_VAR_LEN],
				exp_[YBR_VAR_LEN], w[YBR_VAR_LEN];
			const char* feature = prop_string(n, "feature");
			const char* metric = prop_string(n, "distance");
			if (!feature) feature = "F1";
			if (!metric) metric = "EUCLIDEAN";

			const char* mid = "0.0";  // EUCLIDEAN
			if (!strcmp(metric, "MANHATTAN"))
				mid = "1.0";
			else if (!strcmp(metric, "CHEBYCHEV"))
				mid = "2.0";
			else if (!strcmp(metric, "MINKOWSKI"))
				mid = "3.0";

			emit_lib(c, YBR_LIB_VORONOI);
			emit_lib(c, YBR_LIB_HASH);
			if (!in_vector(c, nodeIndex, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "W", w, sizeof(w), "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Scale", sc, sizeof(sc), "vec4(5.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Randomness", rnd, sizeof(rnd),
					   "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Smoothness", smo, sizeof(smo),
					   "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Exponent", exp_, sizeof(exp_),
					   "vec4(0.5)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);

			// Smooth F1 以外では smoothness を効かせない
			const char* smoothArg =
				(!strcmp(feature, "SMOOTH_F1")) ? smo : NULL;
			sb_printf(&c->body, "    vec3 %s_p = %s.xyz*%s.x + vec3(%s.x);\n",
					  out, a, sc, w);
			sb_printf(&c->body,
					  "    float %s_f1, %s_f2, %s_sf1, %s_edge, %s_rad;\n"
					  "    vec3 %s_cell, %s_pos;\n",
					  out, out, out, out, out, out, out);
			if (smoothArg)
				sb_printf(
					&c->body,
					"    ybrVoronoiEx(%s_p, %s.x, %s.x, %s, %s.x,\n"
					"                 %s_f1, %s_f2, %s_sf1, %s_edge, %s_rad, "
					"%s_cell, %s_pos);\n",
					out, rnd, smoothArg, mid, exp_, out, out, out, out, out,
					out, out);
			else
				sb_printf(
					&c->body,
					"    ybrVoronoiEx(%s_p, %s.x, 0.0, %s, %s.x,\n"
					"                 %s_f1, %s_f2, %s_sf1, %s_edge, %s_rad, "
					"%s_cell, %s_pos);\n",
					out, rnd, mid, exp_, out, out, out, out, out, out, out);

			if (!strcmp(feature, "SMOOTH_F1"))
				sb_printf(&c->body, "    float %s_d = %s_sf1;\n", out, out);
			else if (!strcmp(feature, "F2"))
				sb_printf(&c->body, "    float %s_d = %s_f2;\n", out, out);
			else if (!strcmp(feature, "DISTANCE_TO_EDGE"))
				sb_printf(&c->body, "    float %s_d = %s_edge;\n", out, out);
			else if (!strcmp(feature, "N_SPHERE_RADIUS"))
				sb_printf(&c->body, "    float %s_d = %s_rad;\n", out, out);
			else
				sb_printf(&c->body, "    float %s_d = %s_f1;\n", out, out);

			// 出力ソケットは Distance / Color / Position / W / Radius の順。
			// feature によって並びが変わるので、名前で引き直す。
			const char* oname = (0 <= outSocket && outSocket < n->outputCount)
									? n->outputs[outSocket].name
									: NULL;
			if (oname && !strcmp(oname, "Color"))
				sb_printf(&c->body,
						  "    vec4 %s = vec4(ybrHash13(%s_cell), "
						  "ybrHash13(%s_cell + "
						  "vec3(5.2)),\n"
						  "                   ybrHash13(%s_cell + vec3(11.3)), "
						  "1.0);\n",
						  out, out, out, out);
			else if (oname && !strcmp(oname, "Position"))
				sb_printf(&c->body,
						  "    vec4 %s = vec4(%s_pos/max(%s.x, 1e-4), 1.0);\n",
						  out, out, sc);
			else if (oname && !strcmp(oname, "Radius"))
				sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s_rad), 1.0);\n",
						  out, out);
			else if (oname && !strcmp(oname, "W"))
				sb_printf(&c->body,
						  "    vec4 %s = vec4(vec3(%s_pos.x/max(%s.x, 1e-4)), "
						  "1.0);\n",
						  out, out, sc);
			else
				sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s_d), 1.0);\n",
						  out, out);
		} break;

		case YBR_SN_TEX_WAVE: {
			char sc[YBR_VAR_LEN], dis[YBR_VAR_LEN], det[YBR_VAR_LEN],
				ph[YBR_VAR_LEN];
			const char* kind = prop_string(n, "wave_type");
			const char* prof = prop_string(n, "wave_profile");
			const char* bdir = prop_string(n, "bands_direction");
			if (!kind) kind = "BANDS";
			if (!prof) prof = "SIN";
			if (!bdir) bdir = "X";
			emit_lib(c, YBR_LIB_FBM);
			if (!in_vector(c, nodeIndex, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Scale", sc, sizeof(sc), "vec4(5.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Distortion", dis, sizeof(dis),
					   "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Detail", det, sizeof(det), "vec4(2.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Phase Offset", ph, sizeof(ph),
					   "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec3 %s_p = %s.xyz*%s.x;\n", out, a, sc);
			if (!strcmp(kind, "RINGS")) {
				const char* rdir = prop_string(n, "rings_direction");
				if (!rdir) rdir = "X";
				if (!strcmp(rdir, "X"))
					sb_printf(&c->body,
							  "    float %s_n = length(%s_p.yz)*2.0;\n", out,
							  out);
				else if (!strcmp(rdir, "Y"))
					sb_printf(&c->body,
							  "    float %s_n = length(%s_p.xz)*2.0;\n", out,
							  out);
				else if (!strcmp(rdir, "Z"))
					sb_printf(&c->body,
							  "    float %s_n = length(%s_p.xy)*2.0;\n", out,
							  out);
				else
					sb_printf(&c->body, "    float %s_n = length(%s_p)*2.0;\n",
							  out, out);
			}
			else if (!strcmp(bdir, "Y"))
				sb_printf(&c->body, "    float %s_n = %s_p.y;\n", out, out);
			else if (!strcmp(bdir, "Z"))
				sb_printf(&c->body, "    float %s_n = %s_p.z;\n", out, out);
			else if (!strcmp(bdir, "DIAGONAL"))
				sb_printf(&c->body,
						  "    float %s_n = (%s_p.x + %s_p.y + %s_p.z)/3.0;\n",
						  out, out, out, out);
			else
				sb_printf(&c->body, "    float %s_n = %s_p.x;\n", out, out);
			sb_printf(&c->body,
					  "    %s_n = %s_n + %s.x + %s.x*ybrFbm(%s_p*2.0, %s.x, "
					  "0.5, 2.0);\n",
					  out, out, ph, dis, out, det);
			if (!strcmp(prof, "SAW"))
				sb_printf(&c->body,
						  "    float %s_f = fract(%s_n*0.15915494);\n", out,
						  out);
			else if (!strcmp(prof, "TRI"))
				sb_printf(
					&c->body,
					"    float %s_f = abs(fract(%s_n*0.15915494)*2.0 - 1.0);\n",
					out, out);
			else
				sb_printf(&c->body, "    float %s_f = 0.5 + 0.5*sin(%s_n);\n",
						  out, out);
			sb_printf(&c->body,
					  "    vec4 %s = vec4(vec3(clamp(%s_f, 0.0, 1.0)), 1.0);\n",
					  out, out);
		} break;

		case YBR_SN_TEX_ENVIRONMENT: {
			char tname[YBR_VAR_LEN];
			if (!in_vector(c, nodeIndex, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			snprintf(tname, sizeof(tname), "ybr%sEnv%d", c->uniTag, nodeIndex);
			sb_printf(&c->decls, "uniform sampler2D %s;\n", tname);
			{
				int tw, tf;
				node_tex_props(n, &tw, &tf);
				uni_add_tex(c->set, tname, YBR_UNIFORM_SAMPLER2D,
							(c->texCount == 0) ? RL_SHADER_LOC_MAP_ALBEDO : -1,
							(c->texCount == 0) ? 1 : 0, NULL, 0,
							prop_string(n, "image"), tw, tf);
			}
			c->texCount++;
			new_var(c, out, sz);
			// 正距円筒図法でサンプルする
			sb_printf(&c->body, "    vec3 %s_d = normalize(%s.xyz);\n", out, a);
			sb_printf(
				&c->body,
				"    vec2 %s_uv = vec2(atan(%s_d.z, %s_d.x)*0.15915494 + 0.5,\n"
				"                      acos(clamp(%s_d.y, -1.0, "
				"1.0))*0.31830989);\n",
				out, out, out, out);
			sb_printf(&c->body, "    vec4 %s = %s(%s, %s_uv);\n", out,
					  c->d.texture2D, tname, out);
		} break;

		case YBR_SN_TEX_SKY: {
			// PREETHAM の解析モデルで描く。HOSEK_WILKIE / NISHITA
			// も同じ式で近似する。
			const char* kind = prop_string(n, "sky_type");
			float sun[3];
			char sx[32], sy[32], sz2[32];
			char turb[32], alb[32], size[32], inten[32];
			double turbidity = prop_number(n, "turbidity", 2.2);
			double albedo = prop_number(n, "ground_albedo", 0.3);
			double sunSize = prop_number(n, "sun_size", 0.009512);
			double sunInt = prop_number(n, "sun_intensity", 1.0);
			int sunDisc = (prop_number(n, "sun_disc", 1.0) != 0.0);

			if (!kind) kind = "PREETHAM";
			if (!prop_vector(n, "sun_direction", sun, 3)) {
				// Blender の Z-up (elevation / rotation) から GL の Y-up へ
				double el = prop_number(n, "sun_elevation", 0.26);
				double ro = prop_number(n, "sun_rotation", 0.0);
				sun[0] = (float)(cos(el) * sin(ro));
				sun[1] = (float)(sin(el));
				sun[2] = (float)(-cos(el) * cos(ro));
			}
			else {
				// sun_direction は Blender 空間 (Z-up) なので入れ替える
				float bx = sun[0], by = sun[1], bz = sun[2];
				sun[0] = bx;
				sun[1] = bz;
				sun[2] = -by;
			}

			emit_lib(c, YBR_LIB_SKY);
			if (!in_vector(c, nodeIndex, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			fmt_float(sx, sizeof(sx), sun[0]);
			fmt_float(sy, sizeof(sy), sun[1]);
			fmt_float(sz2, sizeof(sz2), sun[2]);
			fmt_float(turb, sizeof(turb), turbidity);
			fmt_float(alb, sizeof(alb), albedo);
			fmt_float(size, sizeof(size), sunSize);
			fmt_float(inten, sizeof(inten), sunInt);
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec4 %s = vec4(ybrSky(%s.xyz, vec3(%s, %s, %s), %s, "
					  "%s, %s, "
					  "%s, %s), 1.0);\n",
					  out, a, sx, sy, sz2, turb, alb, sunDisc ? "1.0" : "0.0",
					  size, inten);
		} break;

		case YBR_SN_TEX_IES: {
			// IES のプロファイルは .ybr に入っていないので、
			// 配光は再現できない。Strength をそのまま Fac として返す。
			char st[YBR_VAR_LEN];
			if (!in_or(c, nodeIndex, "Strength", st, sizeof(st), "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec4 %s = vec4(vec3(max(%s.x, 0.0)), 1.0);\n", out,
					  st);
		} break;

		case YBR_SN_TEX_POINTDENSITY: {
			// 点群の密度ボリュームはシェーダー側に持ち込めない。
			// アプリから差し替えられるよう uniform にしておく。
			char uname[YBR_VAR_LEN];
			float def[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			snprintf(uname, sizeof(uname), "ybr%sPointDensity%d", c->uniTag,
					 nodeIndex);
			sb_printf(&c->decls, "uniform vec4 %s;\n", uname);
			uni_add(c->set, uname, YBR_UNIFORM_VEC4, -1, 0, def, 4, NULL);
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = %s;\n", out, uname);
		} break;

		// ================= 座標 / ベクトル =================
		case YBR_SN_MAPPING: {
			char loc[YBR_VAR_LEN], rot[YBR_VAR_LEN], scl[YBR_VAR_LEN];
			const char* vt = prop_string(n, "vector_type");
			if (!vt) vt = "POINT";
			emit_lib(c, YBR_LIB_EULER);
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Location", loc, sizeof(loc),
					   "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Rotation", rot, sizeof(rot),
					   "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Scale", scl, sizeof(scl), "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			if (!strcmp(vt, "TEXTURE"))
				sb_printf(&c->body,
						  "    vec4 %s = "
						  "vec4((transpose(ybrEulerXYZ(%s.xyz))*(%s.xyz - "
						  "%s.xyz))"
						  "/max(%s.xyz, vec3(1e-6)), 1.0);\n",
						  out, rot, a, loc, scl);
			else if (!strcmp(vt, "VECTOR"))
				sb_printf(&c->body,
						  "    vec4 %s = "
						  "vec4(ybrEulerXYZ(%s.xyz)*(%s.xyz*%s.xyz), 1.0);\n",
						  out, rot, a, scl);
			else if (!strcmp(vt, "NORMAL"))
				sb_printf(
					&c->body,
					"    vec4 %s = "
					"vec4(normalize(ybrEulerXYZ(%s.xyz)*(%s.xyz/max(%s.xyz, "
					"vec3(1e-6)))), 1.0);\n",
					out, rot, a, scl);
			else
				sb_printf(
					&c->body,
					"    vec4 %s = vec4(ybrEulerXYZ(%s.xyz)*(%s.xyz*%s.xyz) + "
					"%s.xyz, 1.0);\n",
					out, rot, a, scl, loc);
		} break;

		case YBR_SN_VECTOR_ROTATE: {
			char cen[YBR_VAR_LEN], axis[YBR_VAR_LEN], ang[YBR_VAR_LEN],
				eul[YBR_VAR_LEN];
			const char* rt = prop_string(n, "rotation_type");
			int inv = prop_number(n, "invert", 0.0) != 0.0;
			if (!rt) rt = "AXIS_ANGLE";
			emit_lib(c, YBR_LIB_EULER);
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Center", cen, sizeof(cen), "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Axis", axis, sizeof(axis),
					   "vec4(0.0, 0.0, 1.0, 1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Angle", ang, sizeof(ang), "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Rotation", eul, sizeof(eul),
					   "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec3 %s_v = %s.xyz - %s.xyz;\n", out, a,
					  cen);
			if (!strcmp(rt, "EULER_XYZ"))
				sb_printf(&c->body,
						  "    %s_v = %sybrEulerXYZ(%s.xyz)%s*%s_v;\n", out,
						  inv ? "transpose(" : "", eul, inv ? ")" : "", out);
			else {
				const char* ax = !strcmp(rt, "X_AXIS")	 ? "vec3(1.0, 0.0, 0.0)"
								 : !strcmp(rt, "Y_AXIS") ? "vec3(0.0, 1.0, 0.0)"
								 : !strcmp(rt, "Z_AXIS") ? "vec3(0.0, 0.0, 1.0)"
														 : NULL;
				// ソケットの値は vec4 なので、軸として渡すときは .xyz を付ける
				// (ybrRotateAxis は vec3 を取る。GLSL に暗黙変換は無い)
				char axisExpr[YBR_VAR_LEN + 8];
				if (ax)
					snprintf(axisExpr, sizeof(axisExpr), "%s", ax);
				else
					snprintf(axisExpr, sizeof(axisExpr), "%s.xyz", axis);

				sb_printf(&c->body,
						  "    %s_v = ybrRotateAxis(%s_v, %s, %s%s.x);\n", out,
						  out, axisExpr, inv ? "-" : "", ang);
			}
			sb_printf(&c->body, "    vec4 %s = vec4(%s_v + %s.xyz, 1.0);\n",
					  out, out, cen);
		} break;

		case YBR_SN_VECT_TRANSFORM: {
			// フラグメントシェーダーではワールド空間しか持っていないため、
			// OBJECT -> WORLD だけ matModel で変換し、他は素通しにする
			const char* from = prop_string(n, "convert_from");
			const char* to = prop_string(n, "convert_to");
			const char* vt = prop_string(n, "vector_type");
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			if (from && to && !strcmp(from, "OBJECT") && !strcmp(to, "WORLD")) {
				emit_model_matrix(c);
				if (vt && !strcmp(vt, "POINT"))
					sb_printf(&c->body,
							  "    vec4 %s = vec4((matModel*vec4(%s.xyz, "
							  "1.0)).xyz, 1.0);\n",
							  out, a);
				else
					sb_printf(&c->body,
							  "    vec4 %s = vec4((matModel*vec4(%s.xyz, "
							  "0.0)).xyz, 1.0);\n",
							  out, a);
			}
			else {
				sb_printf(&c->body, "    vec4 %s = %s;\n", out, a);
			}
		} break;

		case YBR_SN_NORMAL: {
			// 出力 0 は設定した法線、出力 1 は入力法線との内積
			const YbrShaderSocket* s =
				(0 < n->outputCount) ? &n->outputs[0] : NULL;
			float v[4] = {0.0f, 0.0f, 1.0f, 1.0f};
			for (int i = 0; i < (s ? s->valueCount : 0) && i < 4; i++)
				v[i] = (&s->value.x)[i];
			char uname[YBR_VAR_LEN];
			snprintf(uname, sizeof(uname), "ybr%sNormal%d", c->uniTag,
					 nodeIndex);
			v[3] = 1.0f;
			sb_printf(&c->decls, "uniform vec4 %s;\n", uname);
			uni_add(c->set, uname, YBR_UNIFORM_VEC4, -1, 0, v, 4, NULL);
			new_var(c, out, sz);
			if (outSocket == 1) {
				if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
					c->depth--;
					return 0;
				}
				sb_printf(&c->body,
						  "    vec4 %s = vec4(vec3(dot(normalize(%s.xyz), "
						  "normalize(%s.xyz))), 1.0);\n",
						  out, uname, a);
			}
			else {
				sb_printf(&c->body,
						  "    vec4 %s = vec4(normalize(%s.xyz), 1.0);\n", out,
						  uname);
			}
		} break;

		case YBR_SN_TANGENT:
			// 接線データが無いので、法線から適当な直交ベクトルを作る
			new_var(c, out, sz);
			sb_printf(
				&c->body,
				"    vec3 %s_n = normalize(fragNormal);\n"
				"    vec4 %s = vec4(normalize(cross(%s_n, abs(%s_n.y) < 0.99 ? "
				"vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0))), 1.0);\n",
				out, out, out, out);
			break;

		case YBR_SN_BUMP: {
			// 隣接画素の勾配から法線を傾ける (dFdx/dFdy は GLSL ES 1.00 に無い
			// ので、対応していない場合は法線をそのまま返す)
			char str[YBR_VAR_LEN], dist[YBR_VAR_LEN];
			if (!in_or(c, nodeIndex, "Strength", str, sizeof(str),
					   "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Distance", dist, sizeof(dist),
					   "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			int hi = input_index(n, "Height");
			if (0 <= hi && find_link(c->links, c->linkCount, nodeIndex, hi)) {
				if (!emit_input(c, nodeIndex, hi, a, sizeof(a))) {
					c->depth--;
					return 0;
				}
			}
			else {
				snprintf(a, sizeof(a), "vec4(0.0)");
			}
			new_var(c, out, sz);
			if (c->d.hasFragColor) {
				sb_printf(
					&c->body,
					"    vec3 %s_dp = normalize(fragNormal)\n"
					"        - (dFdx(%s.x)*normalize(dFdx(fragPosition))\n"
					"         + "
					"dFdy(%s.x)*normalize(dFdy(fragPosition)))*%s.x*%s.x;\n",
					out, a, a, str, dist);
				sb_printf(&c->body,
						  "    vec4 %s = vec4(normalize(%s_dp), 1.0);\n", out,
						  out);
			}
			else {
				sb_printf(&c->body,
						  "    vec4 %s = vec4(normalize(fragNormal), 1.0);\n",
						  out);
			}
		} break;

		case YBR_SN_DISPLACEMENT:
		case YBR_SN_VECTOR_DISPLACEMENT:
			// 頂点を動かす仕組みが無いので何もしない
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = vec4(0.0, 0.0, 0.0, 1.0);\n",
					  out);
			break;

		// ================= カラー =================
		case YBR_SN_VALTORGB: {
			int posCount = 0, colCount = 0;
			const float* pos = prop_array(n, "ramp_positions", &posCount);
			const float* col = prop_array(n, "ramp_colors", &colCount);
			const char* interp = prop_string(n, "ramp_interpolation");
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    float %s_t = clamp(%s.x, 0.0, 1.0);\n",
					  out, a);
			if (pos && col && 1 <= posCount && posCount * 4 <= colCount) {
				char tvar[YBR_VAR_LEN];
				snprintf(tvar, sizeof(tvar), "%s_t", out);
				emit_ramp_chain(c, out, tvar, pos, col, posCount,
								interp && !strcmp(interp, "CONSTANT"));
			}
			else {
				// ランプ情報が無い場合は黒 -> 白
				sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s_t), 1.0);\n",
						  out, out);
			}
			if (outSocket == 1) {
				// Alpha
				sb_printf(&c->body, "    %s = vec4(vec3(%s.a), 1.0);\n", out,
						  out);
			}
		} break;

		case YBR_SN_CURVE_RGB:
		case YBR_SN_CURVE_VEC:
		case YBR_SN_CURVE_FLOAT:
		case YBR_SN_FLOAT_CURVE: {
			int lutCount = 0;
			const float* lut = prop_array(n, "curve_lut", &lutCount);
			int chans = (int)prop_number(n, "curve_lut_channels", 0.0);
			int size = (int)prop_number(n, "curve_lut_size", 0.0);
			// どのカーブノードも 0 = Fac / 1 = 値 の並び
			int valIdx = 1;
			int facIdx = 0;
			if (n->inputCount < 2) {
				valIdx = 0;
				facIdx = -1;
			}
			if (!emit_input(c, nodeIndex, valIdx, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (0 <= facIdx) {
				if (!emit_input(c, nodeIndex, facIdx, t, sizeof(t))) {
					c->depth--;
					return 0;
				}
			}
			else
				snprintf(t, sizeof(t), "vec4(1.0)");
			new_var(c, out, sz);

			if (lut && 1 <= chans && 2 <= size && chans * size <= lutCount) {
				// 最後のチャンネルが「全体」カーブ (Blender の C チャンネル)
				const char* comp[3] = {"x", "y", "z"};
				int rgb = (n->type == YBR_SN_CURVE_RGB ||
						   n->type == YBR_SN_CURVE_VEC);
				sb_printf(&c->body, "    vec4 %s = %s;\n", out, a);
				for (int ch = 0; ch < (rgb ? 3 : 1) && ch < chans; ch++) {
					char var[YBR_VAR_LEN], fac[YBR_VAR_LEN + 32];
					snprintf(var, sizeof(var), "%s_c%d", out, ch);
					snprintf(fac, sizeof(fac), "clamp(%s.%s, 0.0, 1.0)", a,
							 rgb ? comp[ch] : "x");
					emit_lut_chain(c, var, fac, lut + (size_t)ch * size, size);
					sb_printf(
						&c->body,
						"    %s.%s = mix(%s.%s, %s, clamp(%s.x, 0.0, 1.0));\n",
						out, rgb ? comp[ch] : "x", out, rgb ? comp[ch] : "x",
						var, t);
				}
				if (!rgb)
					sb_printf(&c->body, "    %s = vec4(vec3(%s.x), 1.0);\n",
							  out, out);
			}
			else {
				// カーブ情報が無ければ素通し
				sb_printf(&c->body, "    vec4 %s = %s;\n", out, a);
			}
		} break;

		case YBR_SN_SEPHSV: {
			emit_lib(c, YBR_LIB_HSV);
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			const char* comp = (outSocket == 1)	  ? "y"
							   : (outSocket == 2) ? "z"
												  : "x";
			new_var(c, out, sz);
			sb_printf(
				&c->body,
				"    vec4 %s = vec4(vec3(ybrRgbToHsv(%s.rgb).%s), 1.0);\n", out,
				a, comp);
		} break;

		case YBR_SN_COMBHSV: {
			char cc[YBR_VAR_LEN];
			emit_lib(c, YBR_LIB_HSV);
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 1, b, sizeof(b))) {
				c->depth--;
				return 0;
			}
			if (!emit_input(c, nodeIndex, 2, cc, sizeof(cc))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec4 %s = vec4(ybrHsvToRgb(vec3(%s.x, %s.x, %s.x)), "
					  "1.0);\n",
					  out, a, b, cc);
		} break;

		case YBR_SN_HUE_SAT: {
			char sat[YBR_VAR_LEN], val[YBR_VAR_LEN], fac[YBR_VAR_LEN];
			emit_lib(c, YBR_LIB_HSV);
			if (!in_or(c, nodeIndex, "Hue", a, sizeof(a), "vec4(0.5)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Saturation", sat, sizeof(sat),
					   "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Value", val, sizeof(val), "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Fac", fac, sizeof(fac), "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Color", b, sizeof(b), "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec3 %s_h = ybrRgbToHsv(%s.rgb);\n", out,
					  b);
			sb_printf(&c->body,
					  "    %s_h = vec3(fract(%s_h.x + %s.x - 0.5), "
					  "clamp(%s_h.y*%s.x, "
					  "0.0, 1.0), %s_h.z*%s.x);\n",
					  out, out, a, out, sat, out, val);
			sb_printf(
				&c->body,
				"    vec4 %s = vec4(mix(%s.rgb, ybrHsvToRgb(%s_h), clamp(%s.x, "
				"0.0, 1.0)), %s.a);\n",
				out, b, out, fac, b);
		} break;

		case YBR_SN_BLACKBODY:
			emit_lib(c, YBR_LIB_BLACKBODY);
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec4 %s = vec4(ybrBlackbody(%s.x), 1.0);\n", out, a);
			break;

		case YBR_SN_WAVELENGTH:
			emit_lib(c, YBR_LIB_WAVELENGTH);
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec4 %s = vec4(ybrWavelength(%s.x), 1.0);\n", out,
					  a);
			break;

		// ================= 情報ノード =================
		case YBR_SN_OBJECT_INFO:
			emit_lit_function(c);
			emit_model_matrix(c);
			new_var(c, out, sz);
			if (outSocket == 0) {
				// Location
				sb_printf(&c->body,
						  "    vec4 %s = vec4(matModel[3].xyz, 1.0);\n", out);
			}
			else if (outSocket == 4) {
				// Random
				sb_printf(&c->body, "    vec4 %s = vec4(0.5, 0.5, 0.5, 1.0);\n",
						  out);
			}
			else {
				sb_printf(&c->body, "    vec4 %s = vec4(0.0, 0.0, 0.0, 1.0);\n",
						  out);
			}
			break;

		case YBR_SN_CAMERA:
			emit_lit_function(c);
			new_var(c, out, sz);
			if (outSocket == 0) {
				// View Vector
				sb_printf(&c->body,
						  "    vec4 %s = vec4(normalize(fragPosition - "
						  "viewPos), 1.0);\n",
						  out);
			}
			else {
				// View Z Depth / Distance
				sb_printf(&c->body,
						  "    vec4 %s = vec4(vec3(distance(viewPos, "
						  "fragPosition)), 1.0);\n",
						  out);
			}
			break;

		case YBR_SN_LIGHT_PATH:
			// ラスタライズではカメラレイしか無い
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s), 1.0);\n", out,
					  (outSocket == 0) ? "1.0" : "0.0");
			break;

		case YBR_SN_PARTICLE_INFO:
		case YBR_SN_HAIR_INFO:
		case YBR_SN_POINT_INFO:
		case YBR_SN_VOLUME_INFO:
		case YBR_SN_WIREFRAME:
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = vec4(0.0, 0.0, 0.0, 1.0);\n",
					  out);
			break;

		case YBR_SN_AMBIENT_OCCLUSION:
			// 遮蔽を計算する手段が無いので遮蔽なしとして扱う
			if (!in_or(c, nodeIndex, "Color", a, sizeof(a), "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			if (outSocket == 1)
				sb_printf(&c->body, "    vec4 %s = vec4(1.0);\n", out);
			else
				sb_printf(&c->body, "    vec4 %s = %s;\n", out, a);
			break;

		case YBR_SN_BEVEL:
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec4 %s = vec4(normalize(fragNormal), 1.0);\n", out);
			break;

		// ================= シェーダー =================
		case YBR_SN_BSDF_TRANSLUCENT:
		case YBR_SN_BSDF_VELVET:
		case YBR_SN_SUBSURFACE_SCATTERING: {
			char rough[YBR_VAR_LEN];
			emit_lit_function(c);
			if (!in_or(c, nodeIndex, "Color", a, sizeof(a),
					   "vec4(0.8, 0.8, 0.8, 1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Roughness", rough, sizeof(rough),
					   "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			// 透過 / ベルベットは陰の側にも回り込むので、環境光を強めに足す
			sb_printf(&c->body,
					  "    vec4 %s = vec4(ybrShade(%s.rgb, 0.0, %s.x, 0.0) + "
					  "%s.rgb*0.35, %s.a);\n",
					  out, a, rough, a, a);
		} break;

		case YBR_SN_BSDF_GLASS:
		case YBR_SN_BSDF_REFRACTION:
		case YBR_SN_BSDF_ANISOTROPIC: {
			char rough[YBR_VAR_LEN];
			emit_lit_function(c);
			if (!in_or(c, nodeIndex, "Color", a, sizeof(a), "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Roughness", rough, sizeof(rough),
					   "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			if (n->type == YBR_SN_BSDF_ANISOTROPIC) {
				sb_printf(&c->body,
						  "    vec4 %s = vec4(ybrShade(%s.rgb, 1.0, %s.x, "
						  "1.0), %s.a);\n",
						  out, a, rough, a);
			}
			else {
				// 屈折は解けないので、フレネルで反射と透過を混ぜた見た目にする
				sb_printf(
					&c->body,
					"    float %s_f = pow(1.0 - max(dot(normalize(fragNormal), "
					"normalize(viewPos - fragPosition)), 0.0), 3.0);\n",
					out);
				sb_printf(
					&c->body,
					"    vec4 %s = vec4(ybrShade(%s.rgb, 1.0, %s.x, 1.0), "
					"mix(0.15, 1.0, %s_f));\n",
					out, a, rough, out);
			}
		} break;

		case YBR_SN_BSDF_TOON: {
			char size2[YBR_VAR_LEN], smooth2[YBR_VAR_LEN];
			emit_lit_function(c);
			if (!in_or(c, nodeIndex, "Color", a, sizeof(a),
					   "vec4(0.8, 0.8, 0.8, 1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Size", size2, sizeof(size2),
					   "vec4(0.5)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Smooth", smooth2, sizeof(smooth2),
					   "vec4(0.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			if (c->lightCount <= 0) {
				// ライト 0 灯 : 色をそのまま出す
				sb_printf(&c->body, "    vec4 %s = %s;\n", out, a);
			}
			else {
				sb_printf(
					&c->body,
					"    vec3 %s_l;\n"
					"    float %s_att = ybrLightSample(%s0, %s0, %s0, %s_l);\n",
					out, out, YBR_SHADER_UNIFORM_LIGHT_DIR,
					YBR_SHADER_UNIFORM_LIGHT_POS,
					YBR_SHADER_UNIFORM_LIGHT_PARAMS, out);
				sb_printf(
					&c->body,
					"    float %s_d = max(dot(normalize(fragNormal), %s_l), "
					"0.0)*%s_att;\n",
					out, out, out);
				sb_printf(&c->body,
						  "    float %s_s = smoothstep(clamp(%s.x - %s.x*0.5, "
						  "0.0, 1.0),\n"
						  "                            clamp(%s.x + %s.x*0.5 + "
						  "1e-4, 0.0, "
						  "1.0), %s_d);\n",
						  out, size2, smooth2, size2, smooth2, out);
				sb_printf(&c->body,
						  "    vec4 %s = vec4(%s.rgb*(%s0.rgb*%s_s + %s.rgb), "
						  "%s.a);\n",
						  out, a, YBR_SHADER_UNIFORM_LIGHT_COLOR, out,
						  YBR_SHADER_UNIFORM_AMBIENT, a);
			}
		} break;

		case YBR_SN_HOLDOUT:
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = vec4(0.0, 0.0, 0.0, 0.0);\n",
					  out);
			break;

		case YBR_SN_SHADERTORGB:
			// このコンバータではシェーダーソケットも vec4 の色なので素通し
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = %s;\n", out, a);
			break;

		case YBR_SN_LIGHT_FALLOFF:
			if (!emit_input(c, nodeIndex, 0, a, sizeof(a))) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = vec4(vec3(%s.x), 1.0);\n", out,
					  a);
			break;

		case YBR_SN_VOLUME_ABSORPTION:
		case YBR_SN_VOLUME_SCATTER:
		case YBR_SN_PRINCIPLED_VOLUME:
			// ボリュームは扱えないので、密度ぶんだけ色を薄く乗せる
			if (!in_or(c, nodeIndex, "Color", a, sizeof(a),
					   "vec4(0.5, 0.5, 0.5, 1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Density", b, sizeof(b), "vec4(1.0)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			sb_printf(&c->body,
					  "    vec4 %s = vec4(%s.rgb, clamp(%s.x, 0.0, 1.0));\n",
					  out, a, b);
			break;

		case YBR_SN_BSDF_HAIR:
		case YBR_SN_BSDF_HAIR_PRINCIPLED: {
			// 毛の異方性反射は板ポリゴンでは解けないので、
			// 拡散 + 弱い透過として近似する。
			char rough[YBR_VAR_LEN];
			emit_lit_function(c);
			if (!in_or(c, nodeIndex, "Color", a, sizeof(a),
					   "vec4(0.8, 0.6, 0.4, 1.0)")) {
				c->depth--;
				return 0;
			}
			if (!in_or(c, nodeIndex, "Roughness", rough, sizeof(rough),
					   "vec4(0.3)")) {
				c->depth--;
				return 0;
			}
			new_var(c, out, sz);
			if (c->lightCount <= 0) {
				sb_printf(&c->body, "    vec4 %s = %s;\n", out, a);
			}
			else {
				// 毛は裏側からも光るので、半球ラップ + 拡散で近似する
				sb_printf(
					&c->body,
					"    vec3 %s_l;\n"
					"    float %s_att = ybrLightSample(%s0, %s0, %s0, %s_l);\n",
					out, out, YBR_SHADER_UNIFORM_LIGHT_DIR,
					YBR_SHADER_UNIFORM_LIGHT_POS,
					YBR_SHADER_UNIFORM_LIGHT_PARAMS, out);
				sb_printf(&c->body,
						  "    float %s_w = clamp(dot(normalize(fragNormal), "
						  "%s_l)*0.5 + "
						  "0.5, 0.0, 1.0)*%s_att;\n",
						  out, out, out);
				sb_printf(&c->body,
						  "    vec4 %s = vec4(mix(ybrShade(%s.rgb, 0.0, "
						  "clamp(%s.x, 0.0, "
						  "1.0), 0.35),\n"
						  "                       %s.rgb*%s0.rgb*%s_w, 0.5), "
						  "%s.a);\n",
						  out, a, rough, a, YBR_SHADER_UNIFORM_LIGHT_COLOR, out,
						  a);
			}
		} break;

		case YBR_SN_GROUP_OUTPUT: {
			// グループの出口。つながっている入力をそのまま素通しする
			int idx =
				(0 <= outSocket && outSocket < n->inputCount) ? outSocket : 0;
			if (n->inputCount <= 0) {
				ok = emit_output_default(c, nodeIndex, outSocket, out, sz);
				break;
			}
			ok = emit_input(c, nodeIndex, idx, out, sz);
		} break;

		case YBR_SN_GROUP: {
			// ノードグループは展開せず GLSL の関数にする。
			char fn[YBR_VAR_LEN];
			const YbrNodeGroup* grp = find_node_group(c, n);
			if (!grp ||
				!emit_group_function(c, grp, outSocket, fn, sizeof(fn))) {
				ok = emit_output_default(c, nodeIndex, outSocket, out, sz);
				break;
			}

			// 引数はグループノードの入力ソケットを順に評価したもの
			int argc = n->inputCount;
			if (YBR_MAX_GROUP_ARGS < argc) argc = YBR_MAX_GROUP_ARGS;

			char args[YBR_MAX_GROUP_ARGS][YBR_VAR_LEN];
			for (int i = 0; i < argc; i++) {
				if (!emit_input(c, nodeIndex, i, args[i], sizeof(args[i]))) {
					c->depth--;
					return 0;
				}
			}

			new_var(c, out, sz);
			sb_printf(&c->body, "    vec4 %s = %s(", out, fn);
			for (int i = 0; i < argc; i++)
				sb_printf(&c->body, "%s%s", (0 < i) ? ", " : "", args[i]);
			sb_puts(&c->body, ");\n");
		} break;

		case YBR_SN_GROUP_INPUT:
			// グループの中では関数の引数に、外では既定値になる
			if (c->groupArg) {
				new_var(c, out, sz);
				sb_printf(&c->body, "    vec4 %s = %s%d;\n", out, c->groupArg,
						  outSocket < 0 ? 0 : outSocket);
			}
			else {
				ok = emit_output_default(c, nodeIndex, outSocket, out, sz);
			}
			break;

		default:
			ok = graph_fail(c, YBR_SHADER_ERR_UNSUPPORTED_NODE,
							"node '%s' (type %s) cannot be converted to GLSL",
							n->name ? n->name : "?",
							YbrShaderNodeTypeName(n->type));
			break;
	}

	if (ok && slot) snprintf(slot, YBR_VAR_LEN, "%s", out);
	c->depth--;
	return ok && (c->error == YBR_SHADER_OK);
}

static YbrShaderResult shader_from_pro(const YbrMaterial* material,
									   int glVersion, int skinBones,
									   const YbrScene* scene, int lightCount,
									   int shadowLights)
{
	if (!material || material->nodeCount <= 0)
		return shader_error(YBR_SHADER_ERR_NULL_NODE, "material has no nodes");

	GraphCtx c;
	memset(&c, 0, sizeof(c));
	c.uniTag = "";
	c.lightCount = clamp_light_count(lightCount);
	c.shadowLights = clamp_shadow_count(shadowLights, lightCount);
	c.nodes = material->nodes;
	c.nodeCount = material->nodeCount;
	c.links = material->links;
	c.linkCount = material->linkCount;
	c.scene = scene;
	if (!glsl_dialect(glVersion, &c.d))
		return shader_error(YBR_SHADER_ERR_UNSUPPORTED_GL,
							"this OpenGL version has no GLSL support");

	// Material Output と、その Surface 入力を探す。
	// ワールド / ライトのノードツリーも同じ形なので出口として受け付ける。
	int outputIndex = -1;
	for (int i = 0; i < material->nodeCount; i++) {
		if (material->nodes[i].type == YBR_SN_OUTPUT_MATERIAL) {
			outputIndex = i;
			break;
		}
	}
	if (outputIndex < 0) {
		for (int i = 0; i < material->nodeCount; i++) {
			YbrShaderNodeType t = material->nodes[i].type;
			if (t == YBR_SN_OUTPUT_WORLD || t == YBR_SN_OUTPUT_LIGHT ||
				t == YBR_SN_OUTPUT_LINESTYLE || t == YBR_SN_OUTPUT_AOV) {
				outputIndex = i;
				break;
			}
		}
	}
	if (outputIndex < 0)
		return shader_error(YBR_SHADER_ERR_UNSUPPORTED_NODE,
							"material has no output node");

	int surfaceIndex = input_index(&material->nodes[outputIndex], "Surface");
	if (surfaceIndex < 0)
		surfaceIndex = input_index(&material->nodes[outputIndex], "Color");
	if (surfaceIndex < 0) surfaceIndex = 0;
	if (!find_link(material->links, material->linkCount, outputIndex,
				   surfaceIndex))
		return shader_error(YBR_SHADER_ERR_UNSUPPORTED_NODE,
							"Material Output has nothing linked to Surface");

	UniformSet set;
	memset(&set, 0, sizeof(set));
	c.set = &set;
	c.memo = (char*)YBR_CALLOC((size_t)material->nodeCount * YBR_MAX_OUTPUTS,
							   YBR_VAR_LEN);
	if (!c.memo)
		return shader_error(YBR_SHADER_ERR_OUT_OF_MEMORY, "out of memory");

	char result[YBR_VAR_LEN];
	int ok = emit_input(&c, outputIndex, surfaceIndex, result, sizeof(result));

	YBR_FREE(c.memo);

	if (!ok || c.error != YBR_SHADER_OK || c.decls.failed || c.body.failed ||
		c.funcs.failed || set.failed) {
		YbrShaderError err = (c.error != YBR_SHADER_OK) ? c.error
							 : set.overflow ? YBR_SHADER_ERR_TOO_MANY_UNIFORMS
											: YBR_SHADER_ERR_OUT_OF_MEMORY;
		const char* msg = (c.error != YBR_SHADER_OK) ? shaderErrorBuf
						  : set.overflow
							  ? "too many uniforms in this material graph "
								"(raise YBR_SHADER_MAX_UNIFORMS)"
							  : "out of memory";
		sb_free(&c.decls);
		sb_free(&c.body);
		sb_free(&c.funcs);
		uni_free_all(&set);
		return shader_error(err, msg);
	}

	// 全体を組み立てる
	StrBuf fs;
	memset(&fs, 0, sizeof(fs));
	emit_fs_header(&fs, &c.d);
	sb_puts(&fs, c.decls.data ? c.decls.data : "");
	// ノードグループは関数として出す。main() より前に置く必要がある
	sb_puts(&fs, c.funcs.data ? c.funcs.data : "");
	sb_puts(&fs, "\nvoid main()\n{\n");
	sb_puts(&fs, c.body.data ? c.body.data : "");
	{
		char expr[YBR_VAR_LEN + 8];
		snprintf(expr, sizeof(expr), "%s", result);
		emit_fs_write(&fs, &c.d, expr);
	}
	sb_puts(&fs, "}\n");

	sb_free(&c.decls);
	sb_free(&c.body);
	sb_free(&c.funcs);

	char* fragment = sb_take(&fs);
	char* vertex = build_vertex_ex(&c.d, skinBones);
	if (!fragment || !vertex) {
		YBR_FREE(fragment);
		YBR_FREE(vertex);
		uni_free_all(&set);
		return shader_error(YBR_SHADER_ERR_OUT_OF_MEMORY, "out of memory");
	}

	YbrShaderResult r;
	memset(&r, 0, sizeof(r));
	r.vertexCode = vertex;
	r.fragmentCode = fragment;
	r.glVersion = glVersion;
	r.error = YBR_SHADER_OK;

	if (0 < set.count) {
		r.uniforms = (YbrShaderUniform*)YBR_CALLOC((size_t)set.count,
												   sizeof(YbrShaderUniform));
		if (!r.uniforms) {
			YBR_FREE(fragment);
			YBR_FREE(vertex);
			uni_free_all(&set);
			return shader_error(YBR_SHADER_ERR_OUT_OF_MEMORY, "out of memory");
		}
		memcpy(r.uniforms, set.items,
			   sizeof(YbrShaderUniform) * (size_t)set.count);
		r.uniformCount = set.count;
	}
	return r;
}

void YbrUnloadShaderResult(YbrShaderResult* result)
{
	if (!result) return;
	YBR_FREE(result->vertexCode);
	YBR_FREE(result->fragmentCode);
	for (int i = 0; i < result->uniformCount; i++) {
		YBR_FREE(result->uniforms[i].name);
		YBR_FREE(result->uniforms[i].textureId);
	}
	YBR_FREE(result->uniforms);
	memset(result, 0, sizeof(*result));
	result->glVersion = YBR_GL_UNKNOWN;
}

// SIMPLE モードのマテリアル

// ノードグラフを持たないマテリアルを、PRO の Principled と同じ
// ライティングモデルで描くシェーダーにする。
static YbrShaderResult shader_from_simple(const YbrMaterial* m, int glVersion,
										  int skinBones, int lightCount,
										  int shadowLights)
{
	if (!m) return shader_error(YBR_SHADER_ERR_NULL_NODE, "material is NULL");
	shadowLights = clamp_shadow_count(shadowLights, lightCount);

	GlslDialect d;
	if (!glsl_dialect(glVersion, &d))
		return shader_error(YBR_SHADER_ERR_UNSUPPORTED_GL,
							"this OpenGL version has no GLSL support");

	UniformSet set;
	memset(&set, 0, sizeof(set));
	StrBuf fs = {0};

	float base[4] = {m->baseColor.x, m->baseColor.y, m->baseColor.z,
					 m->baseColor.w};
	if (0.0f <= m->alpha && m->alpha <= 1.0f) base[3] = m->alpha;
	float metallic = m->metallic;
	float roughness = m->roughness;
	float specular = m->specular;

	int hasNormalMap = (m->normalMap != NULL);
	float normalStrength = m->normalStrength;

	emit_fs_header(&fs, &d);
	sb_puts(&fs, "uniform float ybrMetallic;\n");
	sb_puts(&fs, "uniform float ybrRoughness;\n");
	sb_puts(&fs, "uniform float ybrSpecular;\n");
	emit_light_uniforms(&fs, &set, lightCount, shadowLights, NULL);
	sb_puts(&fs, "uniform sampler2D texture0;\n");
	sb_puts(&fs, "uniform vec4 colDiffuse;\n");

	uni_add(&set, "ybrMetallic", YBR_UNIFORM_FLOAT, -1, 0, &metallic, 1, NULL);
	uni_add(&set, "ybrRoughness", YBR_UNIFORM_FLOAT, -1, 0, &roughness, 1,
			NULL);
	uni_add(&set, "ybrSpecular", YBR_UNIFORM_FLOAT, -1, 0, &specular, 1, NULL);
	uni_add_tex(&set, "texture0", YBR_UNIFORM_SAMPLER2D,
				RL_SHADER_LOC_MAP_ALBEDO, 1, NULL, 0,
				(m->baseColorMap ? m->baseColorMap->image : NULL),
				(m->baseColorMap ? (int)m->baseColorMap->extension : -1),
				(m->baseColorMap ? (int)m->baseColorMap->interpolation : -1));
	uni_add(&set, "colDiffuse", YBR_UNIFORM_VEC4, RL_SHADER_LOC_COLOR_DIFFUSE,
			0, base, 4, NULL);

	// 追加のマップ
	if (m->roughnessMap) {
		sb_puts(&fs, "uniform sampler2D ybrRoughnessMap;\n");
		uni_add_tex(&set, "ybrRoughnessMap", YBR_UNIFORM_SAMPLER2D,
					RL_SHADER_LOC_MAP_ROUGHNESS, 0, NULL, 0,
					m->roughnessMap->image, (int)m->roughnessMap->extension,
					(int)m->roughnessMap->interpolation);
	}
	if (m->metallicMap) {
		sb_puts(&fs, "uniform sampler2D ybrMetallicMap;\n");
		uni_add_tex(&set, "ybrMetallicMap", YBR_UNIFORM_SAMPLER2D,
					RL_SHADER_LOC_MAP_METALNESS, 0, NULL, 0,
					m->metallicMap->image, (int)m->metallicMap->extension,
					(int)m->metallicMap->interpolation);
	}
	if (m->alphaMap) {
		sb_puts(&fs, "uniform sampler2D ybrAlphaMap;\n");
		uni_add_tex(&set, "ybrAlphaMap", YBR_UNIFORM_SAMPLER2D, -1, 0, NULL, 0,
					m->alphaMap->image, (int)m->alphaMap->extension,
					(int)m->alphaMap->interpolation);
	}
	if (hasNormalMap) {
		sb_puts(&fs, "uniform sampler2D ybrNormalMap;\n");
		sb_puts(&fs, "uniform float ybrNormalStrength;\n");
		uni_add_tex(&set, "ybrNormalMap", YBR_UNIFORM_SAMPLER2D,
					RL_SHADER_LOC_MAP_NORMAL, 0, NULL, 0, m->normalMap->image,
					(int)m->normalMap->extension,
					(int)m->normalMap->interpolation);
		uni_add(&set, "ybrNormalStrength", YBR_UNIFORM_FLOAT, -1, 0,
				&normalStrength, 1, NULL);
	}

	emit_shade_decl(&fs, lightCount, shadowLights, d.texture2D);

	sb_puts(&fs, "\nvoid main()\n{\n");
	sb_printf(
		&fs,
		"    vec4 albedo = %s(texture0, fragTexCoord)*colDiffuse*fragColor;\n",
		d.texture2D);
	if (m->alphaMap)
		sb_printf(&fs, "    albedo.a *= %s(ybrAlphaMap, fragTexCoord).r;\n",
				  d.texture2D);

	sb_puts(&fs, "    float rough = clamp(ybrRoughness, 0.0, 1.0);\n");
	if (m->roughnessMap)
		sb_printf(
			&fs,
			"    rough = clamp(rough*%s(ybrRoughnessMap, fragTexCoord).r, "
			"0.0, 1.0);\n",
			d.texture2D);
	sb_puts(&fs, "    float metal = clamp(ybrMetallic, 0.0, 1.0);\n");
	if (m->metallicMap)
		sb_printf(&fs,
				  "    metal = clamp(metal*%s(ybrMetallicMap, fragTexCoord).r, "
				  "0.0, 1.0);\n",
				  d.texture2D);

	sb_puts(&fs, "    vec3 n = normalize(fragNormal);\n");
	if (hasNormalMap) {
		// 接線があれば TBN で正しく解く。無いメッシュでは接線が 0 になるので、
		// 従来どおり法線をゆるく曲げるだけの近似に落ちる。
		sb_printf(
			&fs,
			"    vec3 nm = %s(ybrNormalMap, fragTexCoord).rgb*2.0 - 1.0;\n",
			d.texture2D);
		sb_puts(&fs, "    nm.xy *= ybrNormalStrength;\n");
		sb_puts(&fs,
				"    if (0.0001 < dot(fragTangent.xyz, fragTangent.xyz)) {\n"
				"        vec3 t = normalize(fragTangent.xyz - n*dot(n, "
				"fragTangent.xyz));\n"
				"        vec3 bt = cross(n, t)*fragTangent.w;\n"
				"        n = normalize(mat3(t, bt, n)*normalize(nm));\n"
				"    } else {\n"
				"        n = normalize(n + nm*ybrNormalStrength);\n"
				"    }\n");
	}
	sb_puts(&fs,
			"    vec3 lit = ybrShadeN(albedo.rgb, n, metal, rough, "
			"ybrSpecular);\n");
	emit_fs_write(&fs, &d, "vec4(lit, albedo.a)");
	sb_puts(&fs, "}\n");

	if (fs.failed || set.failed) {
		sb_free(&fs);
		uni_free_all(&set);
		if (set.overflow)
			return shader_error(YBR_SHADER_ERR_TOO_MANY_UNIFORMS,
								"too many uniforms in this material "
								"(raise YBR_SHADER_MAX_UNIFORMS)");
		return shader_error(YBR_SHADER_ERR_OUT_OF_MEMORY, "out of memory");
	}

	char* fragment = sb_take(&fs);
	char* vertex = build_vertex_ex(&d, skinBones);
	if (!fragment || !vertex) {
		YBR_FREE(fragment);
		YBR_FREE(vertex);
		uni_free_all(&set);
		return shader_error(YBR_SHADER_ERR_OUT_OF_MEMORY, "out of memory");
	}

	YbrShaderResult r;
	memset(&r, 0, sizeof(r));
	r.vertexCode = vertex;
	r.fragmentCode = fragment;
	r.glVersion = glVersion;
	r.error = YBR_SHADER_OK;

	if (0 < set.count) {
		r.uniforms = (YbrShaderUniform*)YBR_CALLOC((size_t)set.count,
												   sizeof(YbrShaderUniform));
		if (!r.uniforms) {
			YBR_FREE(fragment);
			YBR_FREE(vertex);
			uni_free_all(&set);
			return shader_error(YBR_SHADER_ERR_OUT_OF_MEMORY, "out of memory");
		}
		memcpy(r.uniforms, set.items,
			   sizeof(YbrShaderUniform) * (size_t)set.count);
		r.uniformCount = set.count;
	}
	return r;
}

// 公開関数 (オプション付き)

YbrShaderOptions YbrShaderOptionsDefaults(int glVersion)
{
	YbrShaderOptions o;
	o.glVersion = glVersion;
	o.lightCount = YBR_SHADER_LIGHTS_DEFAULT;
	o.skinning = 0;
	o.maxBones = YBR_SHADER_MAX_BONES;
	o.scene = NULL;
	return o;
}

YbrShaderResult YbrShaderFromMaterial(const YbrMaterial* material,
									  int glVersion)
{
	return shader_from_pro(material, glVersion, 0, NULL,
						   YBR_SHADER_LIGHTS_DEFAULT, 0);
}

YbrShaderResult YbrShaderFromSimpleMaterial(const YbrMaterial* material,
											int glVersion)
{
	return shader_from_simple(material, glVersion, 0, YBR_SHADER_LIGHTS_DEFAULT,
							  0);
}

YbrShaderResult YbrShaderFromMaterialEx(const YbrMaterial* material,
										const YbrShaderOptions* options)
{
	YbrShaderOptions o = YbrShaderOptionsDefaults(RL_OPENGL_33);
	if (options) o = *options;
	if (o.maxBones < 1) o.maxBones = 1;
	if (YBR_SHADER_MAX_BONES_CAP < o.maxBones)
		o.maxBones = YBR_SHADER_MAX_BONES_CAP;
	o.lightCount = clamp_light_count(o.lightCount);
	int bones = o.skinning ? o.maxBones : 0;

	if (!material)
		return shader_error(YBR_SHADER_ERR_NULL_NODE, "material is NULL");

	if (material->mode == YBR_MATERIAL_PRO && 0 < material->nodeCount) {
		YbrShaderResult r =
			shader_from_pro(material, o.glVersion, bones, o.scene, o.lightCount,
							o.shadowLights);
		if (r.error == YBR_SHADER_OK) return r;
		// ノードを変換できなかったときは SIMPLE のパラメータで作り直す
		YbrUnloadShaderResult(&r);
	}
	return shader_from_simple(material, o.glVersion, bones, o.lightCount,
							  o.shadowLights);
}
