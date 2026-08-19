/*
	Yui Blender to Raylib - 最小 CBOR (RFC 8949) リーダー / ライター実装
		Programed by あるる（きのもと 結衣）
*/
#include "ybr_cbor.h"

#include <math.h>
#include <string.h>

// バイト順 (エンディアン)
// サイズの前提。満たさない処理系はここで止まる。
typedef char ybr_cbor_float_is_32bit[(sizeof(float) == 4) ? 1 : -1];
typedef char ybr_cbor_double_is_64bit[(sizeof(double) == 8) ? 1 : -1];

// double だけ 32bit ワードの順番が整数と逆になる処理系 (旧 ARM FPA など)
// を吸収する。 判定できない処理系では入れ替えない。
#if defined(__FLOAT_WORD_ORDER__) && defined(__BYTE_ORDER__) && \
	(__FLOAT_WORD_ORDER__ != __BYTE_ORDER__)
#define YBR_CBOR_SWAP_DOUBLE_WORDS 1
#else
#define YBR_CBOR_SWAP_DOUBLE_WORDS 0
#endif

static unsigned int cbor_f32_bits(float f)
{
	unsigned int u;
	memcpy(&u, &f, 4);
	return u;
}

static float cbor_bits_f32(unsigned int u)
{
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static unsigned long long cbor_f64_bits(double d)
{
	unsigned long long u;
	memcpy(&u, &d, 8);
#if YBR_CBOR_SWAP_DOUBLE_WORDS
	u = (u >> 32) | (u << 32);
#endif
	return u;
}

static double cbor_bits_f64(unsigned long long u)
{
	double d;
#if YBR_CBOR_SWAP_DOUBLE_WORDS
	u = (u >> 32) | (u << 32);
#endif
	memcpy(&d, &u, 8);
	return d;
}

int CborByteOrderOk(void)
{
	// 1.0f = 0x3F800000 / 1.0 = 0x3FF0000000000000 (IEEE 754) を往復させて、
	// ビットパターンの取り出しと組み立てが噛み合っているかを見る。
	if (cbor_f32_bits(1.0f) != 0x3F800000u) return 0;
	if (cbor_bits_f32(0x3F800000u) != 1.0f) return 0;
	if (cbor_f64_bits(1.0) != 0x3FF0000000000000ULL) return 0;
	if (cbor_bits_f64(0x3FF0000000000000ULL) != 1.0) return 0;

	// リトルエンディアンのバイト列 -> float の経路も一応確かめる
	// (0x3F800000 をリトルエンディアンで並べたもの = 1.0f)
	{
		const unsigned char le[4] = {0x00, 0x00, 0x80, 0x3F};
		unsigned int u = (unsigned int)le[0] | ((unsigned int)le[1] << 8) |
						 ((unsigned int)le[2] << 16) |
						 ((unsigned int)le[3] << 24);
		if (cbor_bits_f32(u) != 1.0f) return 0;
	}
	return 1;
}

// ----------------------------------------------------------------------------
// リーダー

typedef struct {
	const unsigned char *p, *end;
} CborReader;

static double cbor_half(unsigned short h)
{
	int exp = (h >> 10) & 0x1F;
	int mant = h & 0x3FF;
	double v;
	if (exp == 0)
		v = ldexp((double)mant, -24);
	else if (exp != 31)
		v = ldexp((double)(mant + 1024), exp - 25);
	else
		v = (mant == 0) ? HUGE_VAL : NAN;
	return (h & 0x8000) ? -v : v;
}

static int cbor_head(CborReader* r, int* mt, int* ai, unsigned long long* val)
{
	if (r->end <= r->p) return 0;
	unsigned char ib = *r->p++;
	*mt = ib >> 5;
	*ai = ib & 0x1F;

	unsigned long long v = 0;
	int n = 0;
	if (*ai < 24)
		v = (unsigned long long)(*ai);
	else if (*ai == 24)
		n = 1;
	else if (*ai == 25)
		n = 2;
	else if (*ai == 26)
		n = 4;
	else if (*ai == 27)
		n = 8;
	else
		return 0; // 31 = indefinite length : 非対応

	if (n) {
		if ((size_t)(r->end - r->p) < (size_t)n) return 0;
		for (int k = 0; k < n; k++) v = (v << 8) | r->p[k];
		r->p += n;
	}
	*val = v;
	return 1;
}

void CborFree(CborValue* v)
{
	if (!v) return;
	if (v->items) {
		for (size_t k = 0; k < v->count; k++) CborFree(&v->items[k]);
		YBR_FREE(v->items);
		v->items = NULL;
	}
	if (v->keys) {
		for (size_t k = 0; k < v->count; k++) CborFree(&v->keys[k]);
		YBR_FREE(v->keys);
		v->keys = NULL;
	}
}

// 残りバイト数 (r->p は常に r->end 以下)
static size_t cbor_left(const CborReader* r) { return (size_t)(r->end - r->p); }

// 「これから最低 need バイトは必要」を、size_t のオーバーフロー無しで確かめる。
// 要素数 v は 64bit なので、そのまま掛けると溢れる可能性がある。
static int cbor_need(const CborReader* r, unsigned long long count,
					 unsigned long long perItem)
{
	unsigned long long left = (unsigned long long)cbor_left(r);
	if (perItem != 0 && count > left / perItem) return 0;
	return count * perItem <= left;
}

static int cbor_parse_one(CborReader* r, CborValue* out, int depth)
{
	int mt, ai;
	unsigned long long v;

	memset(out, 0, sizeof(*out));
	// 入れ子が深すぎるものは弾く (壊れたファイルでの再帰爆発を防ぐ)
	if (CBOR_MAX_DEPTH < depth) return 0;
	if (!cbor_head(r, &mt, &ai, &v)) return 0;

	switch (mt) {
		case 0:
			out->type = CBOR_UINT;
			out->u = v;
			out->i = (long long)v;
			out->f = (double)v;
			return 1;

		case 1:
			out->type = CBOR_NINT;
			out->i = -1 - (long long)v;
			out->f = (double)out->i;
			return 1;

		case 2:
		case 3:
			if ((unsigned long long)cbor_left(r) < v) return 0;
			out->type = (mt == 2) ? CBOR_BYTES : CBOR_TEXT;
			out->bytes = r->p;
			out->len = (size_t)v;
			r->p += (size_t)v;
			return 1;

		case 4:
			// 1 要素は最低 1 バイトなので、残量を超える個数は不正。
			// 先に弾いておかないと、巨大な count で確保だけ走ってしまう。
			if (!cbor_need(r, v, 1)) return 0;
			out->type = CBOR_ARRAY;
			out->count = (size_t)v;
			if (out->count) {
				out->items =
					(CborValue*)YBR_CALLOC(out->count, sizeof(CborValue));
				if (!out->items) return 0;
			}
			for (size_t k = 0; k < out->count; k++)
				if (!cbor_parse_one(r, &out->items[k], depth + 1)) return 0;
			return 1;

		case 5:
			// ペアなので 1 組につき最低 2 バイト
			if (!cbor_need(r, v, 2)) return 0;
			out->type = CBOR_MAP;
			out->count = (size_t)v;
			if (out->count) {
				out->keys =
					(CborValue*)YBR_CALLOC(out->count, sizeof(CborValue));
				out->items =
					(CborValue*)YBR_CALLOC(out->count, sizeof(CborValue));
				if (!out->keys || !out->items) return 0;
			}
			for (size_t k = 0; k < out->count; k++) {
				if (!cbor_parse_one(r, &out->keys[k], depth + 1)) return 0;
				if (!cbor_parse_one(r, &out->items[k], depth + 1)) return 0;
			}
			return 1;

		case 6:
			// タグ : 中身をそのまま値として扱う (未知のタグは無視する)
			return cbor_parse_one(r, out, depth + 1);

		case 7:
			if (ai == 20) {
				out->type = CBOR_BOOL;
				out->b = 0;
				return 1;
			}
			if (ai == 21) {
				out->type = CBOR_BOOL;
				out->b = 1;
				return 1;
			}
			if (ai == 22 || ai == 23) {
				out->type = CBOR_NULL;
				return 1;
			}
			if (ai == 25) {
				out->type = CBOR_FLOAT;
				out->f = cbor_half((unsigned short)v);
			}
			else if (ai == 26) {
				out->type = CBOR_FLOAT;
				out->f = (double)cbor_bits_f32((unsigned int)v);
			}
			else if (ai == 27) {
				out->type = CBOR_FLOAT;
				out->f = cbor_bits_f64(v);
			}
			else
				return 0;
			// NaN / Inf を整数にすると未定義動作になるので、そこだけ 0 にする
			out->i =
				(out->f >= -9.2e18 && out->f <= 9.2e18) ? (long long)out->f : 0;
			return 1;

		default:
			return 0;
	}
}

int CborParse(const unsigned char* data, size_t size, CborValue* out)
{
	if (!out) return 0;
	memset(out, 0, sizeof(*out)); // 失敗しても CborFree() できるようにする
	if (!data) return 0;
	if (!CborByteOrderOk()) return 0;
	CborReader r = {data, data + size};
	return cbor_parse_one(&r, out, 0);
}

// ----------------------------------------------------------------------------
// アクセサ

static int cbor_key_is(const CborValue* k, const char* name)
{
	size_t n = strlen(name);
	return k->type == CBOR_TEXT && k->len == n &&
		   memcmp(k->bytes, name, n) == 0;
}

const CborValue* CborGet(const CborValue* map, const char* key)
{
	if (!map || map->type != CBOR_MAP) return NULL;
	for (size_t k = 0; k < map->count; k++)
		if (cbor_key_is(&map->keys[k], key)) return &map->items[k];
	return NULL;
}

char* CborDup(const CborValue* v)
{
	if (!v || (v->type != CBOR_TEXT && v->type != CBOR_BYTES)) return NULL;
	char* s = (char*)YBR_MALLOC(v->len + 1);
	if (!s) return NULL;
	memcpy(s, v->bytes, v->len);
	s[v->len] = '\0';
	return s;
}

char* CborGetStr(const CborValue* map, const char* key)
{
	return CborDup(CborGet(map, key));
}

double CborGetNumV(const CborValue* v, double def)
{
	if (!v) return def;
	if (v->type == CBOR_UINT || v->type == CBOR_NINT || v->type == CBOR_FLOAT)
		return v->f;
	if (v->type == CBOR_BOOL) return v->b ? 1.0 : 0.0;
	return def;
}

double CborGetNum(const CborValue* map, const char* key, double def)
{
	return CborGetNumV(CborGet(map, key), def);
}

int CborGetInt(const CborValue* m, const char* k, int def)
{
	return (int)CborGetNum(m, k, (double)def);
}
float CborGetFlt(const CborValue* m, const char* k, float def)
{
	return (float)CborGetNum(m, k, (double)def);
}

int CborGetBool(const CborValue* map, const char* key, int def)
{
	const CborValue* v = CborGet(map, key);
	if (!v) return def;
	if (v->type == CBOR_BOOL) return v->b;
	return (int)CborGetNumV(v, (double)def);
}

int CborGetCode(const CborValue* map, const char* key, int def)
{
	const CborValue* v = CborGet(map, key);
	if (!v || v->type == CBOR_NULL) return def;
	return (int)CborGetNumV(v, (double)def);
}

int CborGetFloats(const CborValue* map, const char* key, float* out, int max)
{
	const CborValue* v = CborGet(map, key);
	if (!v || v->type != CBOR_ARRAY) return 0;
	int n = 0;
	while (n < max && (size_t)n < v->count) {
		out[n] = (float)CborGetNumV(&v->items[n], 0.0);
		n++;
	}
	return n;
}

// バイト列 -> 数値配列（ファイル内はリトルエンディアン固定）
static float le_f32(const unsigned char* p)
{
	unsigned int u = (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
					 ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
	return cbor_bits_f32(u);
}

static unsigned int le_u32(const unsigned char* p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
		   ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

float* CborGetF32(const CborValue* map, const char* key, int* countOut)
{
	if (countOut) *countOut = 0;
	const CborValue* v = CborGet(map, key);
	if (!v || v->type != CBOR_BYTES || v->len < 4) return NULL;
	size_t n = v->len / 4;
	float* a = (float*)YBR_MALLOC(n * sizeof(float));
	if (!a) return NULL;
	for (size_t k = 0; k < n; k++) a[k] = le_f32(v->bytes + k * 4);
	if (countOut) *countOut = (int)n;
	return a;
}

unsigned int* CborGetU32(const CborValue* map, const char* key, int* countOut)
{
	if (countOut) *countOut = 0;
	const CborValue* v = CborGet(map, key);
	if (!v || v->type != CBOR_BYTES || v->len < 4) return NULL;
	size_t n = v->len / 4;
	unsigned int* a = (unsigned int*)YBR_MALLOC(n * sizeof(unsigned int));
	if (!a) return NULL;
	for (size_t k = 0; k < n; k++) a[k] = le_u32(v->bytes + k * 4);
	if (countOut) *countOut = (int)n;
	return a;
}

unsigned short* CborGetU16(const CborValue* map, const char* key, int* countOut)
{
	if (countOut) *countOut = 0;
	const CborValue* v = CborGet(map, key);
	if (!v || v->type != CBOR_BYTES || v->len < 2) return NULL;
	size_t n = v->len / 2;
	unsigned short* a = (unsigned short*)YBR_MALLOC(n * sizeof(unsigned short));
	if (!a) return NULL;
	for (size_t k = 0; k < n; k++)
		a[k] = (unsigned short)((unsigned int)v->bytes[k * 2] |
								((unsigned int)v->bytes[k * 2 + 1] << 8));
	if (countOut) *countOut = (int)n;
	return a;
}

unsigned char* CborGetU8(const CborValue* map, const char* key, int* countOut)
{
	if (countOut) *countOut = 0;
	const CborValue* v = CborGet(map, key);
	if (!v || v->type != CBOR_BYTES || v->len == 0) return NULL;
	unsigned char* a = (unsigned char*)YBR_MALLOC(v->len);
	if (!a) return NULL;
	memcpy(a, v->bytes, v->len);
	if (countOut) *countOut = (int)v->len;
	return a;
}

int CborIs(const CborValue* map, const char* key, const char* expect)
{
	const CborValue* v = CborGet(map, key);
	if (!v || v->type != CBOR_TEXT) return 0;
	size_t n = strlen(expect);
	return v->len == n && memcmp(v->bytes, expect, n) == 0;
}

// ----------------------------------------------------------------------------
// ライター

void CborWriterInit(CborWriter* w) { memset(w, 0, sizeof(*w)); }

void CborWriterFree(CborWriter* w)
{
	YBR_FREE(w->data);
	memset(w, 0, sizeof(*w));
}

unsigned char* CborWriterTake(CborWriter* w, size_t* outLen)
{
	if (w->failed) {
		CborWriterFree(w);
		if (outLen) *outLen = 0;
		return NULL;
	}
	unsigned char* p = w->data;
	if (outLen) *outLen = w->len;
	w->data = NULL;
	w->len = w->cap = 0;
	return p;
}

static int cbor_reserve(CborWriter* w, size_t extra)
{
	if (w->failed) return 0;
	size_t need = w->len + extra;
	if (need <= w->cap) return 1;

	size_t cap = w->cap ? w->cap : 4096;
	while (cap < need) cap *= 2;

	unsigned char* p = (unsigned char*)YBR_MALLOC(cap);
	if (!p) {
		w->failed = 1;
		return 0;
	}
	if (w->data) {
		memcpy(p, w->data, w->len);
		YBR_FREE(w->data);
	}
	w->data = p;
	w->cap = cap;
	return 1;
}

static void cbor_push(CborWriter* w, const void* data, size_t len)
{
	if (!cbor_reserve(w, len)) return;
	memcpy(w->data + w->len, data, len);
	w->len += len;
}

static void cbor_push_u8(CborWriter* w, unsigned char b)
{
	cbor_push(w, &b, 1);
}

// RFC 8949 の head を書く (major type + additional info + 拡張長さ)
static void cbor_write_head(CborWriter* w, int majorType, unsigned long long v)
{
	unsigned char mt = (unsigned char)(majorType << 5);

	if (v < 24) {
		cbor_push_u8(w, (unsigned char)(mt | v));
	}
	else if (v <= 0xFFULL) {
		unsigned char buf[2] = {(unsigned char)(mt | 24), (unsigned char)v};
		cbor_push(w, buf, 2);
	}
	else if (v <= 0xFFFFULL) {
		unsigned char buf[3];
		buf[0] = (unsigned char)(mt | 25);
		buf[1] = (unsigned char)(v >> 8);
		buf[2] = (unsigned char)v;
		cbor_push(w, buf, 3);
	}
	else if (v <= 0xFFFFFFFFULL) {
		unsigned char buf[5];
		buf[0] = (unsigned char)(mt | 26);
		buf[1] = (unsigned char)(v >> 24);
		buf[2] = (unsigned char)(v >> 16);
		buf[3] = (unsigned char)(v >> 8);
		buf[4] = (unsigned char)v;
		cbor_push(w, buf, 5);
	}
	else {
		unsigned char buf[9];
		buf[0] = (unsigned char)(mt | 27);
		for (int i = 0; i < 8; i++)
			buf[1 + i] = (unsigned char)(v >> (56 - i * 8));
		cbor_push(w, buf, 9);
	}
}

void CborWriteUInt(CborWriter* w, unsigned long long v)
{
	cbor_write_head(w, 0, v);
}

void CborWriteInt(CborWriter* w, long long v)
{
	if (0 <= v)
		cbor_write_head(w, 0, (unsigned long long)v);
	else
		cbor_write_head(w, 1, (unsigned long long)(-1 - v));
}

void CborWriteFloat(CborWriter* w, double v)
{
	unsigned char buf[9];
	unsigned long long u = cbor_f64_bits(v);
	buf[0] = (7 << 5) | 27;
	for (int i = 0; i < 8; i++) buf[1 + i] = (unsigned char)(u >> (56 - i * 8));
	cbor_push(w, buf, 9);
}

void CborWriteBool(CborWriter* w, int v) { cbor_push_u8(w, v ? 0xF5 : 0xF4); }

void CborWriteNull(CborWriter* w) { cbor_push_u8(w, 0xF6); }

void CborWriteBytes(CborWriter* w, const void* data, size_t len)
{
	cbor_write_head(w, 2, (unsigned long long)len);
	if (len && data) cbor_push(w, data, len);
}

void CborWriteText(CborWriter* w, const char* s)
{
	size_t n = s ? strlen(s) : 0;
	cbor_write_head(w, 3, (unsigned long long)n);
	if (n) cbor_push(w, s, n);
}

void CborWriteArrayHeader(CborWriter* w, size_t count)
{
	cbor_write_head(w, 4, (unsigned long long)count);
}

void CborWriteFloatArray(CborWriter* w, const float* v, size_t count)
{
	CborWriteArrayHeader(w, count);
	for (size_t i = 0; i < count; i++)
		CborWriteFloat(w, (double)(v ? v[i] : 0.0f));
}

void CborWriteKeyFloatArray(CborWriter* w, const char* key, const float* v,
							size_t count)
{
	CborWriteText(w, key);
	CborWriteFloatArray(w, v, count);
}

void CborWriteMapHeader(CborWriter* w, size_t count)
{
	cbor_write_head(w, 5, (unsigned long long)count);
}

void CborWriteTextOrNull(CborWriter* w, const char* s)
{
	if (s)
		CborWriteText(w, s);
	else
		CborWriteNull(w);
}

void CborWriteBytesOrNull(CborWriter* w, const void* data, size_t len)
{
	if (data && 0 < len)
		CborWriteBytes(w, data, len);
	else
		CborWriteNull(w);
}

// キー付きヘルパー
void CborWriteKeyText(CborWriter* w, const char* key, const char* value)
{
	CborWriteText(w, key);
	CborWriteTextOrNull(w, value);
}

void CborWriteKeyInt(CborWriter* w, const char* key, long long value)
{
	CborWriteText(w, key);
	CborWriteInt(w, value);
}

void CborWriteKeyUInt(CborWriter* w, const char* key, unsigned long long value)
{
	CborWriteText(w, key);
	CborWriteUInt(w, value);
}

void CborWriteKeyFloat(CborWriter* w, const char* key, double value)
{
	CborWriteText(w, key);
	CborWriteFloat(w, value);
}

void CborWriteKeyBool(CborWriter* w, const char* key, int value)
{
	CborWriteText(w, key);
	CborWriteBool(w, value);
}

void CborWriteKeyBytes(CborWriter* w, const char* key, const void* data,
					   size_t len)
{
	CborWriteText(w, key);
	CborWriteBytesOrNull(w, data, len);
}

void CborWriteKeyNull(CborWriter* w, const char* key)
{
	CborWriteText(w, key);
	CborWriteNull(w);
}

void CborWriteKeyArrayHeader(CborWriter* w, const char* key, size_t count)
{
	CborWriteText(w, key);
	CborWriteArrayHeader(w, count);
}

void CborWriteKeyMapHeader(CborWriter* w, const char* key, size_t count)
{
	CborWriteText(w, key);
	CborWriteMapHeader(w, count);
}

// 数値配列 -> リトルエンディアンのバイト列
void CborWriteF32Array(CborWriter* w, const float* v, size_t count)
{
	if (!v || count == 0) {
		CborWriteBytes(w, NULL, 0);
		return;
	}
	cbor_write_head(w, 2, (unsigned long long)count * 4);
	for (size_t i = 0; i < count; i++) {
		unsigned int u = cbor_f32_bits(v[i]);
		unsigned char b[4] = {(unsigned char)u, (unsigned char)(u >> 8),
							  (unsigned char)(u >> 16),
							  (unsigned char)(u >> 24)};
		cbor_push(w, b, 4);
	}
}

void CborWriteU32Array(CborWriter* w, const unsigned int* v, size_t count)
{
	if (!v || count == 0) {
		CborWriteBytes(w, NULL, 0);
		return;
	}
	cbor_write_head(w, 2, (unsigned long long)count * 4);
	for (size_t i = 0; i < count; i++) {
		unsigned int u = v[i];
		unsigned char b[4] = {(unsigned char)u, (unsigned char)(u >> 8),
							  (unsigned char)(u >> 16),
							  (unsigned char)(u >> 24)};
		cbor_push(w, b, 4);
	}
}

void CborWriteU16Array(CborWriter* w, const unsigned short* v, size_t count)
{
	if (!v || count == 0) {
		CborWriteBytes(w, NULL, 0);
		return;
	}
	cbor_write_head(w, 2, (unsigned long long)count * 2);
	for (size_t i = 0; i < count; i++) {
		unsigned short u = v[i];
		unsigned char b[2] = {(unsigned char)u, (unsigned char)(u >> 8)};
		cbor_push(w, b, 2);
	}
}

void CborWriteU8Array(CborWriter* w, const unsigned char* v, size_t count)
{
	CborWriteBytes(w, v, count);
}

void CborWriteKeyF32Array(CborWriter* w, const char* key, const float* v,
						  size_t count)
{
	CborWriteText(w, key);
	if (v && 0 < count)
		CborWriteF32Array(w, v, count);
	else
		CborWriteNull(w);
}

void CborWriteKeyU32Array(CborWriter* w, const char* key, const unsigned int* v,
						  size_t count)
{
	CborWriteText(w, key);
	if (v && 0 < count)
		CborWriteU32Array(w, v, count);
	else
		CborWriteNull(w);
}

void CborWriteKeyU16Array(CborWriter* w, const char* key,
						  const unsigned short* v, size_t count)
{
	CborWriteText(w, key);
	if (v && 0 < count)
		CborWriteU16Array(w, v, count);
	else
		CborWriteNull(w);
}

void CborWriteKeyU8Array(CborWriter* w, const char* key, const unsigned char* v,
						 size_t count)
{
	CborWriteText(w, key);
	if (v && 0 < count)
		CborWriteU8Array(w, v, count);
	else
		CborWriteNull(w);
}
