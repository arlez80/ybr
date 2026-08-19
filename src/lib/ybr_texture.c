/*
	Yui Blender to Raylib - テクスチャの読み込みとキャッシュ
		Programed by あるる（きのもと 結衣）
*/
#include <string.h>

#include "ybr_internal.h"
#include "ybr_model_internal.h"

struct YbrTexCacheEntry {
	const char* id;	 // YbrScene 内の文字列を指す
	Texture2D texture;
};

// ラップ / フィルタを反映する。分からない (-1) ものは触らない。
static void apply_texture_settings(Texture2D tex, int wrap, int filter)
{
	if (tex.id == 0) return;

	if (0 <= filter) {
		switch (filter) {
			case YBR_TEXFILTER_CLOSEST:
				SetTextureFilter(tex, TEXTURE_FILTER_POINT);
				break;
			case YBR_TEXFILTER_CUBIC:
			case YBR_TEXFILTER_SMART:
				// 異方性まではやらない。ミップマップ付きの三線形にしておく
				GenTextureMipmaps(&tex);
				SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
				break;
			default:
				SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
				break;
		}
	}
	if (0 <= wrap) {
		switch (wrap) {
			case YBR_TEXWRAP_EXTEND:
				SetTextureWrap(tex, TEXTURE_WRAP_CLAMP);
				break;
			case YBR_TEXWRAP_CLIP:
				SetTextureWrap(tex, TEXTURE_WRAP_CLAMP);
				break;
			case YBR_TEXWRAP_MIRROR:
				SetTextureWrap(tex, TEXTURE_WRAP_MIRROR_REPEAT);
				break;
			default:
				SetTextureWrap(tex, TEXTURE_WRAP_REPEAT);
				break;
		}
	}
}

Texture2D YbrModelGetTexture(YbrModel* m, const YbrScene* scene,
							 const char* imageId, int wrap, int filter)
{
	Texture2D none;
	memset(&none, 0, sizeof(none));
	if (!imageId) return none;

	for (int i = 0; i < m->texCacheCount; i++)
		if (YbrStrEq(m->texCache[i].id, imageId)) return m->texCache[i].texture;

	const YbrTextureData* tex = YbrFindTexture(scene, imageId);
	if (!tex) {
		TraceLog(
			LOG_WARNING,
			"YBR: texture '%s' is referenced by a material but there is no "
			"TEXTURE block for it",
			imageId);
		return none;
	}

	Texture2D loaded = YbrLoadTexture(tex);
	if (loaded.id != 0) {
		apply_texture_settings(loaded, wrap, filter);
		m->ownTextures = 1;
	}

	void* nb = YbrGrowBuffer(m->texCache, &m->texCacheCap, m->texCacheCount + 1,
							 sizeof(struct YbrTexCacheEntry));
	if (nb) {
		m->texCache = (struct YbrTexCacheEntry*)nb;
		// ここに入れる id は「モデルより長生きするもの」でないといけない。
		// 呼び出し元が渡してくる imageId は YbrShaderResult が持っている
		// 文字列で、シェーダーを作り終えた時点で解放されてしまう。
		// YbrScene 側の id (シーンが解放されるまで生きている) を控える。
		m->texCache[m->texCacheCount].id = tex->id;
		m->texCache[m->texCacheCount].texture = loaded;
		m->texCacheCount++;
	}
	return loaded;
}

Texture2D YbrLoadTexture(const YbrTextureData* tex)
{
	Texture2D out;
	memset(&out, 0, sizeof(out));
	if (!tex) return out;

	const char* name = tex->id ? tex->id : "?";

	if (!tex->embedded) {
		TraceLog(LOG_WARNING,
				 "YBR: texture '%s' has no embedded pixel data "
				 "(re-export with 'Embed Textures' enabled)",
				 name);
		return out;
	}

	// 画素を持たない Image を LoadTextureFromImage() へ渡すと、中身を
	// 一度も書き込んでいないテクスチャが出来上がる。GL はそれを未定義の
	// まま残すので、直前まで GPU メモリにあった別のものが見えてしまう。
	// そうならないよう、アップロードの手前で必ず中身を確かめる。
	Image img;
	memset(&img, 0, sizeof(img));

	if (tex->compression == YBR_TEX_RAW) {
		if (!tex->pixels || tex->width <= 0 || tex->height <= 0) {
			TraceLog(LOG_WARNING,
					 "YBR: texture '%s': raw pixels are missing or the size is "
					 "invalid (%dx%d)",
					 name, tex->width, tex->height);
			return out;
		}
		img.data = (void*)tex->pixels;	// LoadTextureFromImage はコピーする
		img.width = tex->width;
		img.height = tex->height;
		img.mipmaps = 1;
		img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

		out = LoadTextureFromImage(img);
		if (out.id == 0)
			TraceLog(LOG_WARNING, "YBR: texture '%s': the upload failed", name);
		return out;
	}

	if (!tex->data || tex->dataSize <= 0) {
		TraceLog(LOG_WARNING,
				 "YBR: texture '%s': the compressed data block is empty", name);
		return out;
	}

	img = LoadImageFromMemory(YbrTextureFileExt(tex), tex->data, tex->dataSize);
	if (!img.data || img.width <= 0 || img.height <= 0) {
		// 拡張子が合っていない / raylib がその形式を組み込んでいない場合など。
		TraceLog(LOG_WARNING,
				 "YBR: texture '%s': could not decode the %s data (%d bytes). "
				 "Is that image format compiled into raylib?",
				 name, YbrTextureFileExt(tex), tex->dataSize);
		UnloadImage(img);
		return out;
	}

	out = LoadTextureFromImage(img);
	UnloadImage(img);
	if (out.id == 0)
		TraceLog(LOG_WARNING, "YBR: texture '%s': the upload failed", name);
	return out;
}

void YbrModelUnloadTextures(YbrModel* model)
{
	if (!model) return;
	// テクスチャは複数マテリアルで共有しているので、キャッシュ単位で 1
	// 回だけ解放
	if (model->ownTextures) {
		for (int i = 0; i < model->texCacheCount; i++)
			if (model->texCache[i].texture.id)
				UnloadTexture(model->texCache[i].texture);
	}
	YBR_FREE(model->texCache);
	model->texCache = NULL;
	model->texCacheCount = 0;
	model->texCacheCap = 0;
	model->ownTextures = 0;
}
