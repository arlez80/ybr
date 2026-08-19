/*
	Yui Blender to Raylib - 最小 CBOR (RFC 8949) リーダー / ライター実装
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_CBOR_H
#define YBR_CBOR_H

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YBR_MALLOC
#ifdef RL_MALLOC
#define YBR_MALLOC(sz) RL_MALLOC(sz)
#else
#define YBR_MALLOC(sz) malloc(sz)
#endif
#endif
#ifndef YBR_CALLOC
#ifdef RL_CALLOC
#define YBR_CALLOC(n, sz) RL_CALLOC(n, sz)
#else
#define YBR_CALLOC(n, sz) calloc(n, sz)
#endif
#endif
#ifndef YBR_FREE
#ifdef RL_FREE
#define YBR_FREE(p) RL_FREE(p)
#else
#define YBR_FREE(p) free(p)
#endif
#endif

// バイト順 (エンディアン)
// この処理系で .ybr の数値を正しく読み書きできるか (1 なら可)。
// 起動時に 1 度呼んで確かめるのに使える。
int CborByteOrderOk(void);

// ----------------------------------------------------------------------------
// リーダー

// 入れ子の深さの上限。壊れた / 悪意のあるファイルで
// 再帰が深くなりすぎてスタックを食い潰さないようにする。
#define CBOR_MAX_DEPTH 64

typedef enum {
	CBOR_UINT,	 // u / i / f が有効（非負整数）
	CBOR_NINT,	 // i / f が有効（負の整数）
	CBOR_BYTES,	 // bytes / len が有効
	CBOR_TEXT,	 // bytes / len が有効（UTF-8、NUL 終端ではない）
	CBOR_ARRAY,	 // items / count が有効
	CBOR_MAP,	 // keys / items / count が有効
	CBOR_FLOAT,	 // f が有効（半 / 単 / 倍精度いずれも double に統一）
	CBOR_BOOL,	 // b が有効
	CBOR_NULL
} CborType;

typedef struct CborValue {
	CborType type;
	unsigned long long u;
	long long i;
	double f;
	int b;
	const unsigned char* bytes;	 // 元バッファを指す（コピーしない）
	size_t len;
	size_t count;			  // 配列の要素数 / マップのペア数
	struct CborValue* items;  // 配列要素 / マップの値
	struct CborValue* keys;	  // マップのキー（map のときのみ）
} CborValue;

// data[0..size) を 1 個の CBOR 値として読む。
int CborParse(const unsigned char* data, size_t size, CborValue* out);
void CborFree(CborValue* v);

// マップ / 値アクセサ
const CborValue* CborGet(const CborValue* map, const char* key);
int CborIs(const CborValue* map, const char* key, const char* expect);

char* CborDup(const CborValue* v);
char* CborGetStr(const CborValue* map, const char* key);
double CborGetNumV(const CborValue* v, double def);
double CborGetNum(const CborValue* map, const char* key, double def);
int CborGetInt(const CborValue* map, const char* key, int def);
float CborGetFlt(const CborValue* map, const char* key, float def);
int CborGetBool(const CborValue* map, const char* key, int def);
// 値が null のときだけ def を返す (未対応の enum コード読み取り用)
int CborGetCode(const CborValue* map, const char* key, int def);

// CBOR 配列 (数値の配列) を float[] へ。戻り値は読めた個数。
int CborGetFloats(const CborValue* map, const char* key, float* out, int max);

// バイト列 (リトルエンディアン固定) を各種配列へ複製する。
// 戻り値は YBR_MALLOC 済みの配列 (YBR_FREE で解放)。読めなければ NULL。
float* CborGetF32(const CborValue* map, const char* key, int* countOut);
unsigned int* CborGetU32(const CborValue* map, const char* key, int* countOut);
unsigned short* CborGetU16(const CborValue* map, const char* key,
						   int* countOut);
unsigned char* CborGetU8(const CborValue* map, const char* key, int* countOut);

// ----------------------------------------------------------------------------
// ライター

typedef struct CborWriter {
	unsigned char* data;
	size_t len;
	size_t cap;
	int failed;	 // 1 度でもメモリ確保に失敗したら立つ
} CborWriter;

void CborWriterInit(CborWriter* w);
void CborWriterFree(CborWriter* w);
unsigned char* CborWriterTake(CborWriter* w,
							  size_t* outLen);	// 所有権を移して w をリセット

// 生の値を書き込む
void CborWriteUInt(CborWriter* w, unsigned long long v);
void CborWriteInt(CborWriter* w, long long v);
void CborWriteFloat(CborWriter* w, double v);
void CborWriteBool(CborWriter* w, int v);
void CborWriteNull(CborWriter* w);
void CborWriteBytes(CborWriter* w, const void* data, size_t len);
void CborWriteText(CborWriter* w, const char* s);
void CborWriteArrayHeader(CborWriter* w, size_t count);
void CborWriteMapHeader(CborWriter* w, size_t count);

// 固定長の小さな float 配列を「CBOR 配列」として書く（major type 4）
void CborWriteFloatArray(CborWriter* w, const float* v, size_t count);
void CborWriteKeyFloatArray(CborWriter* w, const char* key, const float* v,
							size_t count);

// 値が NULL / 0 のとき CBOR null を書く（「省略可能なフィールド」用）
void CborWriteTextOrNull(CborWriter* w, const char* s);
void CborWriteBytesOrNull(CborWriter* w, const void* data, size_t len);

// キー + 値をまとめて書くヘルパー（マップの中身を書くときに使う）
// CborWriteMapHeader() でペア数を書いてから、そのペア数ぶん呼び出すこと。
void CborWriteKeyText(CborWriter* w, const char* key, const char* value);
void CborWriteKeyInt(CborWriter* w, const char* key, long long value);
void CborWriteKeyUInt(CborWriter* w, const char* key, unsigned long long value);
void CborWriteKeyFloat(CborWriter* w, const char* key, double value);
void CborWriteKeyBool(CborWriter* w, const char* key, int value);
void CborWriteKeyBytes(CborWriter* w, const char* key, const void* data,
					   size_t len);
void CborWriteKeyNull(CborWriter* w, const char* key);
void CborWriteKeyArrayHeader(CborWriter* w, const char* key, size_t count);
void CborWriteKeyMapHeader(CborWriter* w, const char* key, size_t count);

// 数値配列 -> リトルエンディアンのバイト列として書く
void CborWriteF32Array(CborWriter* w, const float* v, size_t count);
void CborWriteU32Array(CborWriter* w, const unsigned int* v, size_t count);
void CborWriteU16Array(CborWriter* w, const unsigned short* v, size_t count);
void CborWriteU8Array(CborWriter* w, const unsigned char* v, size_t count);

void CborWriteKeyF32Array(CborWriter* w, const char* key, const float* v,
						  size_t count);
void CborWriteKeyU32Array(CborWriter* w, const char* key, const unsigned int* v,
						  size_t count);
void CborWriteKeyU16Array(CborWriter* w, const char* key,
						  const unsigned short* v, size_t count);
void CborWriteKeyU8Array(CborWriter* w, const char* key, const unsigned char* v,
						 size_t count);

#ifdef __cplusplus
}
#endif

#endif /* YBR_CBOR_H */
