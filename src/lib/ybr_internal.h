/*
	Yui Blender to Raylib - 実装のためのユーティリティ
		Programed by あるる（きのもと 結衣）
*/
#ifndef YBR_INTERNAL_H
#define YBR_INTERNAL_H

#include <string.h>

#include "ybr.h"

// 可変長配列を伸ばす
static inline void* YbrGrowBuffer(void* buf, int* cap, int need, size_t elem)
{
	if (need <= *cap) return buf;
	if (need < 0) return NULL;
	int n = *cap ? *cap : 16;
	while (n < need) {
		// int の桁あふれを避ける
		if ((1 << 30) <= n) {
			n = need;
			break;
		}
		n *= 2;
	}
	void* p = YBR_MALLOC((size_t)n * elem);
	if (!p) return NULL;
	if (buf) {
		memcpy(p, buf, (size_t)*cap * elem);
		YBR_FREE(buf);
	}
	*cap = n;
	return p;
}

// NULL 安全な文字列比較
static inline int YbrStrEq(const char* a, const char* b)
{
	if (!a || !b) return 0;
	return strcmp(a, b) == 0;
}

#endif /* YBR_INTERNAL_H */
