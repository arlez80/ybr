/*
	Yui Blender to Raylib 用 ツール
		Programed by あるる（きのもと 結衣）
*/
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ybr.h"
#include "ybr_anim.h"
#include "ybr_anim_opt.h"
#include "ybr_curve.h"
#include "ybr_mesh_opt.h"
#include "ybr_shader.h"

static const char* node_type_name(YbrNodeType t)
{
	switch (t) {
		case YBR_NODE_EMPTY:
			return "EMPTY";
		case YBR_NODE_MESH:
			return "MESH";
		case YBR_NODE_ARMATURE:
			return "ARMATURE";
		case YBR_NODE_CURVE:
			return "CURVE";
		case YBR_NODE_LIGHT:
			return "LIGHT";
		case YBR_NODE_CAMERA:
			return "CAMERA";
		case YBR_NODE_OBJECT:
			return "OBJECT";
		case YBR_NODE_COLLECTION:
			return "COLLECTION";
		default:
			return "UNKNOWN";
	}
}

static int print_node(const YbrNode* n, const YbrNode* parent, int depth,
					  void* ud)
{
	(void)parent;
	(void)ud;
	for (int i = 0; i < depth; i++) printf("  ");
	printf("- %s [%s]", n->name ? n->name : "(noname)",
		   node_type_name(n->type));
	if (n->dataId) printf(" data=\"%s\"", n->dataId);
	if (n->type == YBR_NODE_COLLECTION)
		printf(" children=%d", n->childCount);
	else
		printf(" pos=(%.3f, %.3f, %.3f)", n->matrix.m12, n->matrix.m13,
			   n->matrix.m14);
	printf("\n");

	for (int i = 0; i < n->customPropertyCount; i++) {
		const YbrCustomProperty* p = &n->customProperties[i];
		for (int k = 0; k < depth + 1; k++) printf("  ");
		printf("* %s = ", p->key ? p->key : "?");
		switch (p->type) {
			case YBR_CUSTOM_STRING:
				printf("\"%s\" (string)", p->text ? p->text : "");
				break;
			case YBR_CUSTOM_BOOL:
				printf("%s (bool)", p->number != 0.0 ? "true" : "false");
				break;
			case YBR_CUSTOM_INT:
				printf("%d (int)", (int)p->number);
				break;
			case YBR_CUSTOM_ARRAY:
				printf("[");
				for (int j = 0; j < p->valueCount; j++)
					printf("%s%.3f", j ? ", " : "", p->values[j]);
				printf("] (array)");
				break;
			default:
				printf("%.4f (float)", p->number);
				break;
		}
		printf("\n");
	}
	return 1;
}

static const char* cs_name(YbrColorSpace c)
{
	switch (c) {
		case YBR_COLORSPACE_SRGB:
			return "sRGB";
		case YBR_COLORSPACE_LINEAR:
			return "Linear";
		case YBR_COLORSPACE_NON_COLOR:
			return "Non-Color";
		default:
			return "unsupported";
	}
}

static void print_tex(const char* label, const YbrTexture* t)
{
	if (!t) return;
	printf("    %-12s image=%s cs=%s wrap=%d filter=%d\n", label,
		   t->image ? t->image : "?", cs_name(t->colorspace), (int)t->extension,
		   (int)t->interpolation);
}

static int parse_gl(const char* s)
{
	if (!strcmp(s, "11")) return RL_OPENGL_11;
	if (!strcmp(s, "21")) return RL_OPENGL_21;
	if (!strcmp(s, "33")) return RL_OPENGL_33;
	if (!strcmp(s, "43")) return RL_OPENGL_43;
	if (!strcmp(s, "es20")) return RL_OPENGL_ES_20;
	if (!strcmp(s, "es30")) return RL_OPENGL_ES_30;
	return RL_OPENGL_33;
}

static void dump_shader(const YbrScene* sc, const YbrMaterial* m, int glVersion,
						int showCode)
{
	// シーンを渡すとノードグループが GLSL の関数として展開される
	YbrShaderOptions o = YbrShaderOptionsDefaults(glVersion);
	o.scene = sc;
	YbrShaderResult r = YbrShaderFromMaterialEx(m, &o);

	printf("    [shader] graph -> GLSL: %s", YbrShaderErrorString(r.error));
	if (r.error != YBR_SHADER_OK) {
		printf(" : %s\n", r.errorMessage ? r.errorMessage : "");
		printf("             fragmentCode=%s glVersion=%d\n",
			   r.fragmentCode ? "(not NULL!)" : "NULL", r.glVersion);
		YbrUnloadShaderResult(&r);
		return;
	}

	printf(" (glVersion=%d, uniforms=%d)\n", r.glVersion, r.uniformCount);
	for (int i = 0; i < r.uniformCount; i++) {
		const YbrShaderUniform* u = &r.uniforms[i];
		printf("             %-14s type=%d loc=%-3d auto=%d fmt=%d", u->name,
			   (int)u->type, u->locIndex, u->autoSet,
			   YbrShaderUniformFormat(u->type));
		if (u->textureId)
			printf(" texture=\"%s\"", u->textureId);
		else if (0 < u->valueCount) {
			printf(" value=(");
			for (int j = 0; j < u->valueCount; j++)
				printf("%s%.3f", j ? ", " : "", u->value[j]);
			printf(")");
		}
		printf("\n");
	}

	if (showCode) {
		printf("\n----- vertex -----\n%s", r.vertexCode);
		printf("----- fragment -----\n%s", r.fragmentCode);
		printf("--------------------\n\n");
	}
	YbrUnloadShaderResult(&r);
}

// シーンのダンプ

static void dump_scene(const YbrScene* sc, int showShader, int glVersion,
					   int showCode)
{
	printf("=== %s / version %d ===\n", YBR_FORMAT_NAME, sc->version);
	printf(
		"roots=%d meshes=%d armatures=%d curves=%d lights=%d empties=%d "
		"materials=%d animations=%d textures=%d\n\n",
		sc->rootCount, sc->meshCount, (sc->armature ? 1 : 0), sc->curveCount,
		sc->lightCount, sc->emptyCount, sc->materialCount, sc->animationCount,
		sc->textureCount);

	printf("--- scene tree ---\n");
	YbrWalkNodes(sc, print_node, NULL);

	printf("\n--- meshes ---\n");
	for (int i = 0; i < sc->meshCount; i++) {
		const YbrMesh* m = &sc->meshes[i];
		printf("[%s] verts=%d tris=%d normals=%s uvs=%s colors=%s mats=%d\n",
			   m->id, m->vertexCount, m->triangleCount,
			   m->normals ? "yes" : "no", m->texcoords ? "yes" : "no",
			   m->colors ? "yes" : "no", m->materialCount);
		if (m->armature)
			printf("    armature obj=\"%s\" data=\"%s\"\n", m->armature,
				   m->armatureData ? m->armatureData : "-");
		if (m->skin) {
			printf("    skin influences=%d (joint = bone index)\n",
				   m->skin->influences);
			if (0 < m->vertexCount && m->skin->joints && m->skin->weights)
				printf("        v0 -> j(%u %u %u %u) w(%.3f %.3f %.3f %.3f)\n",
					   m->skin->joints[0], m->skin->joints[1],
					   m->skin->joints[2], m->skin->joints[3],
					   m->skin->weights[0], m->skin->weights[1],
					   m->skin->weights[2], m->skin->weights[3]);
		}
		for (int g = 0; g < m->vertexGroupCount; g++)
			printf("    vgroup[%d] %-20s entries=%d\n",
				   m->vertexGroups[g].index, m->vertexGroups[g].name,
				   m->vertexGroups[g].count);
		if (0 < m->vertexCount && m->positions)
			printf("    v0 = (%.3f, %.3f, %.3f)\n", m->positions[0],
				   m->positions[1], m->positions[2]);
	}

	printf("\n--- armature ---\n");
	for (int i = 0; i < (sc->armature ? 1 : 0); i++) {
		const YbrArmature* a = sc->armature;
		printf("[%s] bones=%d\n", a->id, a->boneCount);
		for (int b = 0; b < a->boneCount; b++)
			printf("    %2d %-24s parent=%d len=%.3f\n", b, a->bones[b].name,
				   a->bones[b].parent, a->bones[b].length);
	}

	printf("\n--- curves ---\n");
	for (int i = 0; i < sc->curveCount; i++) {
		const YbrCurve* c = &sc->curves[i];
		printf("[%s] %s splines=%d\n", c->id, c->is3d ? "3D" : "2D",
			   c->splineCount);
		for (int s = 0; s < c->splineCount; s++) {
			const YbrSpline* sp = &c->splines[s];
			const char* tn = (sp->type == YBR_SPLINE_BEZIER)  ? "BEZIER"
							 : (sp->type == YBR_SPLINE_NURBS) ? "NURBS"
															  : "POLY";
			printf("    %-6s points=%d cyclic=%d order=%d\n", tn,
				   sp->pointCount, sp->cyclic, sp->order);
			if (2 <= sp->pointCount) {
				float len = YbrCurveGetLength(c, s, 24);
				Vector3 mid = YbrCurveGetPoint(c, s, 0.5f, 24);
				Vector3 half =
					YbrCurveGetPointAtDistance(c, s, len * 0.5f, 24, 8);
				float w = YbrCurveGetWeightAtDistance(c, s, len * 0.5f, 24, 8);
				printf("        length=%.4f  t=0.5 -> (%.3f, %.3f, %.3f)\n",
					   len, mid.x, mid.y, mid.z);
				printf("        half distance -> w=%.4f (%.3f, %.3f, %.3f)\n",
					   w, half.x, half.y, half.z);
			}
		}
	}

	printf("\n--- lights ---\n");
	for (int i = 0; i < sc->lightCount; i++) {
		const YbrLight* l = &sc->lights[i];
		const char* ln = (l->type == YBR_LIGHT_SUN)	   ? "SUN"
						 : (l->type == YBR_LIGHT_SPOT) ? "SPOT"
						 : (l->type == YBR_LIGHT_AREA) ? "AREA"
													   : "POINT";
		printf("[%s] %s energy=%.2f color=(%.2f %.2f %.2f) shadow=%d\n", l->id,
			   ln, l->energy, l->color.x, l->color.y, l->color.z, l->useShadow);
		if (l->type == YBR_LIGHT_SPOT)
			printf("    spot size=%.4f blend=%.3f\n", l->spotSize,
				   l->spotBlend);
		if (l->type == YBR_LIGHT_AREA)
			printf("    area shape=%s size=%.3f/%.3f\n",
				   l->shape ? l->shape : "?", l->size, l->sizeY);
		if (l->type == YBR_LIGHT_SUN) printf("    sun angle=%.4f\n", l->angle);
	}

	printf("\n--- materials ---\n");
	for (int i = 0; i < sc->materialCount; i++) {
		const YbrMaterial* m = &sc->materials[i];
		if (m->mode == YBR_MATERIAL_SIMPLE) {
			printf("[%s] SIMPLE\n", m->id);
			printf(
				"    base=(%.3f %.3f %.3f %.3f) spec=%.3f metal=%.3f "
				"rough=%.3f "
				"alpha=%.3f\n",
				m->baseColor.x, m->baseColor.y, m->baseColor.z, m->baseColor.w,
				m->specular, m->metallic, m->roughness, m->alpha);
			print_tex("base_color", m->baseColorMap);
			print_tex("metallic", m->metallicMap);
			print_tex("roughness", m->roughnessMap);
			print_tex("alpha", m->alphaMap);
			print_tex("normal", m->normalMap);
		}
		else {
			printf("[%s] PRO nodes=%d links=%d\n", m->id, m->nodeCount,
				   m->linkCount);
			for (int n = 0; n < m->nodeCount; n++) {
				const YbrShaderNode* sn = &m->nodes[n];
				printf("    %2d %-22s in=%d out=%d props=%d\n", n,
					   YbrShaderNodeTypeName(sn->type), sn->inputCount,
					   sn->outputCount, sn->propCount);
			}
			for (int l = 0; l < m->linkCount; l++)
				printf("    link %d:%d -> %d:%d\n", m->links[l].fromNode,
					   m->links[l].fromSocket, m->links[l].toNode,
					   m->links[l].toSocket);
			if (showShader) dump_shader(sc, m, glVersion, showCode);
		}
	}

	printf("\n--- node groups ---\n");
	for (int i = 0; i < sc->nodeGroupCount; i++) {
		const YbrNodeGroup* g = &sc->nodeGroups[i];
		printf("[%s] in=%d out=%d nodes=%d links=%d\n", g->id ? g->id : "?",
			   g->inputCount, g->outputCount, g->nodeCount, g->linkCount);
		for (int k = 0; k < g->inputCount; k++)
			printf("    in  %2d %s\n", k,
				   g->inputs[k].name ? g->inputs[k].name : "?");
		for (int k = 0; k < g->outputCount; k++)
			printf("    out %2d %s\n", k,
				   g->outputs[k].name ? g->outputs[k].name : "?");
		for (int n = 0; n < g->nodeCount; n++)
			printf("    %2d %-22s in=%d out=%d props=%d\n", n,
				   YbrShaderNodeTypeName(g->nodes[n].type),
				   g->nodes[n].inputCount, g->nodes[n].outputCount,
				   g->nodes[n].propCount);
	}

	printf("\n--- cameras ---\n");
	for (int i = 0; i < sc->cameraCount; i++) {
		const YbrCamera* c = &sc->cameras[i];
		const char* ct = (c->type == YBR_CAMERA_ORTHO)		 ? "ORTHO"
						 : (c->type == YBR_CAMERA_PANORAMIC) ? "PANORAMIC"
															 : "PERSP";
		printf("[%s] %s lens=%.2fmm fov=(%.4f, %.4f) clip=%.3f..%.1f\n", c->id,
			   ct, c->lens, c->fovX, c->fovY, c->clipStart, c->clipEnd);
		if (c->type == YBR_CAMERA_ORTHO)
			printf("    ortho scale=%.3f\n", c->orthoScale);
	}

	printf("\n--- animations ---\n");
	for (int i = 0; i < sc->animationCount; i++) {
		const YbrAnimation* a = &sc->animations[i];
		printf("[%s] object=%s fps=%.2f frames=%d sinc_a=%d tracks=%d",
			   a->id ? a->id : "-", a->object ? a->object : "-", (double)a->fps,
			   a->frameCount, a->sincA, a->trackCount);
		if (0 < a->markerCount) printf(" markers=%d", a->markerCount);
		printf("\n");
		for (int t = 0; t < a->trackCount; t++) {
			const YbrAnimTrack* tr = &a->tracks[t];
			printf("    %-16s bone=%-12s keys=%d",
				   tr->object ? tr->object : "?", tr->bone ? tr->bone : "-",
				   tr->frameCount);
			if (0 < tr->frameCount) {
				const YbrAnimFrame* f = &tr->frames[0];
				printf("  f%d type=%d pos=(%.3f, %.3f, %.3f)", f->frame,
					   (int)f->type, f->transform.m12, f->transform.m13,
					   f->transform.m14);
			}
			printf("\n");
			if (1 < tr->frameCount) {
				int use[YBR_INTERP_COUNT] = {0};
				for (int k = 1; k < tr->frameCount; k++) {
					int c = (int)tr->frames[k].interp;
					if (0 <= c && c < YBR_INTERP_COUNT) use[c]++;
				}
				printf("        interp:");
				for (int c = 0; c < YBR_INTERP_COUNT; c++)
					if (use[c])
						printf(" %s=%d", YbrInterpName((YbrInterp)c), use[c]);
				printf("\n");
			}
		}
		for (int k = 0; k < a->markerCount; k++)
			printf("    marker f%-6d %s\n", a->markers[k].frame,
				   a->markers[k].name ? a->markers[k].name : "?");
	}

	printf("\n--- textures ---\n");
	for (int i = 0; i < sc->textureCount; i++) {
		const YbrTextureData* t = &sc->textures[i];
		const char* comp = (t->compression == YBR_TEX_PNG)	  ? "PNG"
						   : (t->compression == YBR_TEX_JPEG) ? "JPEG"
															  : "RAW";
		printf("[%s] %dx%d cs=%s %s\n", t->name ? t->name : t->id, t->width,
			   t->height, cs_name(t->colorspace),
			   t->embedded ? "embedded" : "external");
		if (t->embedded && t->compression == YBR_TEX_RAW && t->pixels)
			printf("    RAW RGBA8  px[0] = %u %u %u %u\n", t->pixels[0],
				   t->pixels[1], t->pixels[2], t->pixels[3]);
		else if (t->embedded && t->data)
			printf(
				"    %s %d bytes (q=%d) ext=%s  head = %02X %02X %02X %02X\n",
				comp, t->dataSize, t->quality, YbrTextureFileExt(t), t->data[0],
				t->data[1], t->data[2], t->data[3]);
		else if (t->filepath)
			printf("    path = %s\n", t->filepath);
	}

	printf("\n--- empties ---\n");
	for (int i = 0; i < sc->emptyCount; i++)
		printf("[%s] display=%s size=%.3f\n", sc->empties[i].id,
			   sc->empties[i].displayType ? sc->empties[i].displayType : "?",
			   sc->empties[i].displaySize);
}

// アニメーション最適化

static void usage(const char* prog)
{
	printf(
		"usage: %s <file.ybr> [options]\n"
		"\n"
		"  -s / -S                     PRO マテリアルを GLSL へ変換して表示 "
		"(-s "
		"はコードも)\n"
		"  --gl 11|21|33|43|es20|es30  変換先の GL バージョン\n"
		"  -o <out.ybr>                読み込んだシーンを書き出す\n"
		"  -q                          シーンのダンプを省略する\n"
		"\n"
		"アニメーション最適化 (キー削減 / 補間方法の選び直し):\n"
		"  --anime-opt                 最適化を行う。-o "
		"が無ければ「どれだけ小さくなるか」\n"
		"                              を確認するだけのドライランになる\n"
		"  --anime-opt-pos <float>     平行移動の許容誤差 (シーン単位)      "
		"既定 "
		"0.0005\n"
		"  --anime-opt-rot <float>     回転の許容誤差 (度)                  "
		"既定 "
		"0.05\n"
		"  --anime-opt-scale <float>   スケールの相対許容誤差               "
		"既定 "
		"0.001\n"
		"  --anime-opt-tolerance <f>   上の 3 つをまとめて倍率で緩める/締める "
		"既定 1.0\n"
		"  --anime-opt-interp <list>   使ってよい補間方法をカンマ区切りで指定\n"
		"                              step,linear,cubic,sinc,hermite / all   "
		"既定 all\n"
		"  --anime-opt-subsample <n>   1 フレームあたりの誤差検査点数 (1..16) "
		"既定 2\n"
		"  --anime-opt-rounds <n>      選び直し<->間引きの往復回数 (1..8)     "
		"既定 4\n"
		"  --anime-opt-sinc-a <n>      SINC (Lanczos) の a (1..16)          "
		"既定 "
		"3\n"
		"  --anime-opt-threads <n>     同時に処理するトラック数 (0=自動 / "
		"1=逐次)\n"
		"  --anime-opt-verbose         トラックごとの結果を表示する\n"
		"\n"
		"メッシュ最適化 (--mesh-opt):\n"
		"  --mesh-opt                  重複頂点のマージ / 未使用頂点の削除 /\n"
		"                              頂点キャッシュ最適化 / 頂点並べ替え\n"
		"  --mesh-opt-cache <n>        想定する頂点キャッシュ段数 (4..64)     "
		"既定 32\n"
		"  --mesh-opt-pos-grid <f>     位置を 1/f 単位に丸める (0 で丸めない) "
		"既定 0\n"
		"  --mesh-opt-normal-bits <n>  法線を 2^n 段階に丸める (0..16)        "
		"既定 0\n"
		"  --mesh-opt-uv-bits <n>      UV を 2^n 段階に丸める (0..16)         "
		"既定 0\n"
		"  --mesh-opt-tex-max <n>      テクスチャの長辺の上限 (RAW のみ)      "
		"既定 0\n"
		"  --mesh-opt-no-merge         重複頂点をまとめない\n"
		"  --mesh-opt-no-cache         三角形を並べ替えない\n"
		"\n"
		"検証:\n"
		"  --verify                    -o "
		"で書き出したあと読み戻して元と比べる\n"
		"  --verify <other.ybr>        2 つの .ybr を比べる "
		"(書き出し無しでも可)\n"
		"\n"
		"  ※ sinc-a は .ybr のアニメーションごとに保存されるので、再生側で\n"
		"     設定し直す必要はない (YbrAnimSamplerInitFromAnimation "
		"を使う)。\n",
		prog);
}

static int parse_interp_list(const char* list, unsigned int* mask)
{
	char buf[256];
	size_t n = strlen(list);
	if (sizeof(buf) <= n) return 0;
	memcpy(buf, list, n + 1);

	unsigned int m = 0;
	char* p = buf;
	while (*p) {
		char* q = p;
		while (*q && *q != ',') q++;
		int last = (*q == 0);
		*q = 0;
		if (*p) {
			if (!strcmp(p, "all") || !strcmp(p, "ALL"))
				m = YBR_INTERP_ALL;
			else if (!strcmp(p, "none")) { /* 何も足さない */
			}
			else {
				YbrInterp iv;
				if (!YbrInterpParse(p, &iv)) {
					printf("ERROR: unknown interp \"%s\"\n", p);
					return 0;
				}
				m |= YBR_INTERP_BIT(iv);
			}
		}
		if (last) break;
		p = q + 1;
	}
	if (m == 0) {
		printf("ERROR: --anime-opt-interp is empty\n");
		return 0;
	}
	*mask = m;
	return 1;
}

static void print_interp_mask(unsigned int mask)
{
	for (int i = 0; i < YBR_INTERP_COUNT; i++)
		if (mask & YBR_INTERP_BIT(i))
			printf(" %s", YbrInterpName((YbrInterp)i));
}

// 1 キーあたりのバイト数の目安 (frames u32 + types u8 + interps u8 + 16 float)
// キー / 接線のバイト数は ybr_anim_opt.h の定義を使う

static void optimize_animations(YbrScene* sc, const YbrAnimOptOptions* optIn,
								int verbose, int dryRun)
{
	// 表示と YbrAnimation.sincA への書き戻しのために、ライブラリと同じ
	// 丸めをここでも掛けておく
	YbrAnimOptOptions opts = *optIn;
	opts.interp = YbrInterpParamsSanitize(&opts.interp);
	const YbrAnimOptOptions* opt = &opts;

	printf("\n--- animation optimize (%s) ---\n",
		   dryRun ? "dry-run : -o が無いのでサイズ確認のみ"
				  : "書き出しに反映する");
	printf(
		"tolerance : pos=%g  rot=%g deg  scale=%g  subsample=%d  rounds=%d\n",
		(double)opt->posEps, (double)opt->rotEps, (double)opt->scaleEps,
		opt->subsample, opt->maxRounds);
	printf("interp    :");
	print_interp_mask(opt->interpMask);
	{
		int th = 0 < opt->threads ? opt->threads : YbrAnimOptDefaultThreads();
		printf("   (sinc a=%d / threads %d%s)\n", opt->interp.sincA, th,
			   YbrAnimOptHasThreads() ? "" : " : C11 threads 無しのため逐次");
	}

	size_t sizeBefore = 0, sizeAfter = 0;
	unsigned char* tmp = YbrSaveToMemory(sc, &sizeBefore);
	if (tmp)
		YBR_FREE(tmp);
	else
		sizeBefore = 0;

	YbrAnimOptStats total;
	memset(&total, 0, sizeof(total));

	for (int i = 0; i < sc->animationCount; i++) {
		YbrAnimation* a = &sc->animations[i];
		total.animCount++;
		// 使用した Lanczos の a をファイルへ持たせる
		// (トラック単位で最適化しているのでここで設定する)
		a->sincA = opt->interp.sincA;
		if (verbose)
			printf("\n[%s] tracks=%d\n", a->id ? a->id : "-", a->trackCount);

		for (int t = 0; t < a->trackCount; t++) {
			YbrAnimTrack* tr = &a->tracks[t];
			int before = tr->frameCount;

			YbrAnimOptStats one;
			memset(&one, 0, sizeof(one));
			if (!YbrOptimizeAnimTrack(tr, opt, &one)) {
				printf("  WARNING: out of memory (track %d)\n", t);
				continue;
			}

			total.trackCount += one.trackCount;
			total.keysBefore += one.keysBefore;
			total.keysAfter += one.keysAfter;
			for (int k = 0; k < YBR_INTERP_COUNT; k++)
				total.interpUse[k] += one.interpUse[k];
			if (total.maxPosErr < one.maxPosErr)
				total.maxPosErr = one.maxPosErr;
			if (total.maxRotErr < one.maxRotErr)
				total.maxRotErr = one.maxRotErr;
			if (total.maxScaleErr < one.maxScaleErr)
				total.maxScaleErr = one.maxScaleErr;

			if (verbose) {
				printf("  %-16s bone=%-14s %5d -> %5d (%5.1f%%)",
					   tr->object ? tr->object : "?", tr->bone ? tr->bone : "-",
					   before, tr->frameCount,
					   before ? 100.0 * tr->frameCount / before : 100.0);
				for (int k = 0; k < YBR_INTERP_COUNT; k++)
					if (one.interpUse[k])
						printf("  %s=%d", YbrInterpName((YbrInterp)k),
							   one.interpUse[k]);
				printf("   err pos=%.6f rot=%.4f scale=%.6f\n",
					   (double)one.maxPosErr, (double)one.maxRotErr,
					   (double)one.maxScaleErr);
			}
		}
	}

	tmp = YbrSaveToMemory(sc, &sizeAfter);
	if (tmp)
		YBR_FREE(tmp);
	else
		sizeAfter = 0;

	long long ab = (long long)total.keysBefore * YBR_ANIM_KEY_BYTES;
	long long aa = 0;
	for (int i = 0; i < sc->animationCount; i++)
		for (int t = 0; t < sc->animations[i].trackCount; t++)
			aa += (long long)YbrAnimTrackBytes(&sc->animations[i].tracks[t]);

	printf("\nanimations : %d  tracks : %d\n", total.animCount,
		   total.trackCount);
	printf(
		"keys       : %d -> %d  (%.1f%% / -%d)\n", total.keysBefore,
		total.keysAfter,
		total.keysBefore ? 100.0 * total.keysAfter / total.keysBefore : 100.0,
		total.keysBefore - total.keysAfter);
	printf("interp use :");
	for (int k = 0; k < YBR_INTERP_COUNT; k++)
		printf(" %s=%d", YbrInterpName((YbrInterp)k), total.interpUse[k]);
	printf("\n");
	printf("anim bytes : %lld -> %lld  (%.1f%%)\n", ab, aa,
		   ab ? 100.0 * (double)aa / (double)ab : 100.0);
	printf("max error  : pos=%.6f  rot=%.4f deg  scale=%.6f\n",
		   (double)total.maxPosErr, (double)total.maxRotErr,
		   (double)total.maxScaleErr);
	if (sizeBefore && sizeAfter)
		printf("file size  : %llu -> %llu bytes  (%.1f%%)\n",
			   (unsigned long long)sizeBefore, (unsigned long long)sizeAfter,
			   100.0 * (double)sizeAfter / (double)sizeBefore);
}

// メッシュ最適化

static void optimize_meshes(YbrScene* sc, const YbrMeshOptOptions* o)
{
	YbrMeshOptStats st;
	memset(&st, 0, sizeof(st));

	size_t before = 0, after = 0;
	unsigned char* tmp = YbrSaveToMemory(sc, &before);
	YBR_FREE(tmp);

	if (!YbrOptimizeScene(sc, o, &st))
		printf("\nWARNING: mesh optimize failed (out of memory?)\n");

	tmp = YbrSaveToMemory(sc, &after);
	YBR_FREE(tmp);

	printf("\n--- mesh optimize ---\n");
	printf("meshes     : %d\n", st.meshCount);
	printf(
		"vertices   : %d -> %d  (%.1f%%)\n", st.verticesBefore,
		st.verticesAfter,
		st.verticesBefore ? 100.0 * st.verticesAfter / st.verticesBefore : 0.0);
	printf("triangles  : %d -> %d\n", st.trianglesBefore, st.trianglesAfter);
	if (0 < st.meshCount)
		printf("ACMR       : %.3f -> %.3f  (cache %d / 小さいほど良い)\n",
			   (double)(st.acmrBefore / st.meshCount),
			   (double)(st.acmrAfter / st.meshCount), o->cacheSize);
	if (0 < o->maxTextureSize)
		printf("textures   : %d 中 %d 枚を縮小 (RAW のみ)\n", st.textureCount,
			   st.texturesResized);
	if (before && after)
		printf("file size  : %llu -> %llu bytes  (%.1f%%)\n",
			   (unsigned long long)before, (unsigned long long)after,
			   100.0 * (double)after / (double)before);
}

// 検証 : 2 つのシーンを突き合わせる

static int g_diffs;

static void diff(const char* fmt, ...)
{
	va_list ap;
	g_diffs++;
	if (20 < g_diffs) return; /* 出しすぎない */
	printf("  ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static int same_str(const char* a, const char* b)
{
	if (!a && !b) return 1;
	if (!a || !b) return 0;
	return strcmp(a, b) == 0;
}

static int same_f(float a, float b, float eps)
{
	float d = a - b;
	if (d < 0.0f) d = -d;
	return d <= eps;
}

static void verify_mesh(const YbrMesh* a, const YbrMesh* b)
{
	if (a->vertexCount != b->vertexCount)
		diff("mesh '%s': vertexCount %d -> %d", a->id, a->vertexCount,
			 b->vertexCount);
	if (a->triangleCount != b->triangleCount)
		diff("mesh '%s': triangleCount %d -> %d", a->id, a->triangleCount,
			 b->triangleCount);
	if (a->vertexCount != b->vertexCount ||
		a->triangleCount != b->triangleCount)
		return;

	int nv = a->vertexCount;
	for (int i = 0; i < nv * 3; i++)
		if (!same_f(a->positions[i], b->positions[i], 1e-6f)) {
			diff("mesh '%s': position[%d] %.6f -> %.6f", a->id, i,
				 (double)a->positions[i], (double)b->positions[i]);
			break;
		}
	if ((a->normals != NULL) != (b->normals != NULL))
		diff("mesh '%s': normals present %d -> %d", a->id, a->normals != NULL,
			 b->normals != NULL);
	if ((a->tangents != NULL) != (b->tangents != NULL))
		diff("mesh '%s': tangents present %d -> %d", a->id, a->tangents != NULL,
			 b->tangents != NULL);
	if ((a->skin != NULL) != (b->skin != NULL))
		diff("mesh '%s': skin present %d -> %d", a->id, a->skin != NULL,
			 b->skin != NULL);

	for (int i = 0; i < a->triangleCount * 3; i++)
		if (a->indices[i] != b->indices[i]) {
			diff("mesh '%s': indices differ at %d", a->id, i);
			break;
		}
}

static void verify_animation(const YbrAnimation* a, const YbrAnimation* b)
{
	if (a->trackCount != b->trackCount) {
		diff("animation '%s': trackCount %d -> %d", a->id, a->trackCount,
			 b->trackCount);
		return;
	}
	for (int t = 0; t < a->trackCount; t++) {
		const YbrAnimTrack *x = &a->tracks[t], *y = &b->tracks[t];
		if (!same_str(x->object, y->object) || !same_str(x->bone, y->bone)) {
			diff("animation '%s': track %d target differs", a->id, t);
			continue;
		}
		if (x->frameCount != y->frameCount) {
			diff("animation '%s': track %d frameCount %d -> %d", a->id, t,
				 x->frameCount, y->frameCount);
			continue;
		}
		for (int f = 0; f < x->frameCount; f++) {
			if (x->frames[f].frame != y->frames[f].frame ||
				x->frames[f].interp != y->frames[f].interp) {
				diff("animation '%s': track %d key %d differs", a->id, t, f);
				break;
			}
		}
		if ((x->tangents != NULL) != (y->tangents != NULL))
			diff("animation '%s': track %d tangents present %d -> %d", a->id, t,
				 x->tangents != NULL, y->tangents != NULL);
	}
}

// 書き出したものが元と同じ意味かを見る。戻り値は差分の数。
static int verify_scenes(const YbrScene* a, const YbrScene* b)
{
	g_diffs = 0;

	if (a->meshCount != b->meshCount)
		diff("meshCount %d -> %d", a->meshCount, b->meshCount);
	if (a->materialCount != b->materialCount)
		diff("materialCount %d -> %d", a->materialCount, b->materialCount);
	if ((a->armature ? 1 : 0) != (b->armature ? 1 : 0))
		diff("armature %d -> %d", (a->armature ? 1 : 0), (b->armature ? 1 : 0));
	if (a->animationCount != b->animationCount)
		diff("animationCount %d -> %d", a->animationCount, b->animationCount);
	if (a->textureCount != b->textureCount)
		diff("textureCount %d -> %d", a->textureCount, b->textureCount);
	if (a->curveCount != b->curveCount)
		diff("curveCount %d -> %d", a->curveCount, b->curveCount);
	if (a->nodeGroupCount != b->nodeGroupCount)
		diff("nodeGroupCount %d -> %d", a->nodeGroupCount, b->nodeGroupCount);
	if (a->rootCount != b->rootCount)
		diff("rootCount %d -> %d", a->rootCount, b->rootCount);

	for (int i = 0; i < a->meshCount; i++) {
		const YbrMesh* x = &a->meshes[i];
		const YbrMesh* y = YbrFindMesh(b, x->id);
		if (!y) {
			diff("mesh '%s' is missing", x->id ? x->id : "?");
			continue;
		}
		verify_mesh(x, y);
	}

	for (int i = 0; i < a->materialCount; i++) {
		const YbrMaterial* x = &a->materials[i];
		const YbrMaterial* y = YbrFindMaterial(b, x->id);
		if (!y) {
			diff("material '%s' is missing", x->id ? x->id : "?");
			continue;
		}
		if (x->mode != y->mode) diff("material '%s': mode differs", x->id);
		if (x->transparent != y->transparent)
			diff("material '%s': transparent %d -> %d", x->id, x->transparent,
				 y->transparent);
		if (x->nodeCount != y->nodeCount)
			diff("material '%s': nodeCount %d -> %d", x->id, x->nodeCount,
				 y->nodeCount);
		if (x->linkCount != y->linkCount)
			diff("material '%s': linkCount %d -> %d", x->id, x->linkCount,
				 y->linkCount);
		if (!same_f(x->alpha, y->alpha, 1e-6f))
			diff("material '%s': alpha differs", x->id);
	}

	for (int i = 0; i < a->textureCount; i++) {
		const YbrTextureData* x = &a->textures[i];
		const YbrTextureData* y = YbrFindTexture(b, x->id);
		if (!y) {
			diff("texture '%s' is missing", x->id ? x->id : "?");
			continue;
		}
		if (x->width != y->width || x->height != y->height)
			diff("texture '%s': size %dx%d -> %dx%d", x->id, x->width,
				 x->height, y->width, y->height);
		if (x->embedded != y->embedded)
			diff("texture '%s': embedded %d -> %d", x->id, x->embedded,
				 y->embedded);
		if (x->compression != y->compression)
			diff("texture '%s': compression differs", x->id);
		if (x->dataSize != y->dataSize)
			diff("texture '%s': dataSize %d -> %d", x->id, x->dataSize,
				 y->dataSize);
		else if (x->data && y->data &&
				 memcmp(x->data, y->data, (size_t)x->dataSize) != 0)
			diff("texture '%s': data differs", x->id);
	}

	for (int i = 0; i < a->animationCount; i++) {
		const YbrAnimation* x = &a->animations[i];
		const YbrAnimation* y = YbrFindAnimation(b, x->id);
		if (!y) {
			diff("animation '%s' is missing", x->id ? x->id : "?");
			continue;
		}
		verify_animation(x, y);
	}

	return g_diffs;
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	int showShader = 0, showCode = 0, glVersion = RL_OPENGL_33;
	int quiet = 0, animOpt = 0, animVerbose = 0;
	int meshOpt = 0, verify = 0;
	float animTolScale = 1.0f;
	const char* writeOut = NULL;
	const char* verifyAgainst = NULL;
	YbrAnimOptOptions aopt = YbrAnimOptDefaults();
	YbrMeshOptOptions mopt = YbrMeshOptDefaults();

	for (int i = 2; i < argc; i++) {
		const char* a = argv[i];
		if (!strcmp(a, "-s")) {
			showShader = 1;
			showCode = 1;
		}
		else if (!strcmp(a, "-S"))
			showShader = 1;
		else if (!strcmp(a, "-q"))
			quiet = 1;
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(argv[0]);
			return 0;
		}
		else if (!strcmp(a, "--gl") && i + 1 < argc) {
			glVersion = parse_gl(argv[++i]);
			showShader = 1;
		}
		else if (!strcmp(a, "-o") && i + 1 < argc) {
			writeOut = argv[++i];
		}
		// メッシュ最適化
		else if (!strcmp(a, "--mesh-opt"))
			meshOpt = 1;
		else if (!strcmp(a, "--mesh-opt-no-merge")) {
			meshOpt = 1;
			mopt.mergeVertices = 0;
		}
		else if (!strcmp(a, "--mesh-opt-no-cache")) {
			meshOpt = 1;
			mopt.optimizeCache = 0;
		}
		else if (!strncmp(a, "--mesh-opt-", 11)) {
			const char* key = a + 11;
			if (argc <= i + 1) {
				printf("ERROR: %s needs a value\n", a);
				return 1;
			}
			const char* v = argv[++i];
			meshOpt = 1;
			if (!strcmp(key, "cache"))
				mopt.cacheSize = atoi(v);
			else if (!strcmp(key, "pos-grid"))
				mopt.positionGrid = (float)atof(v);
			else if (!strcmp(key, "normal-bits"))
				mopt.normalBits = atoi(v);
			else if (!strcmp(key, "uv-bits"))
				mopt.uvBits = atoi(v);
			else if (!strcmp(key, "tex-max"))
				mopt.maxTextureSize = atoi(v);
			else {
				printf("ERROR: unknown option %s\n", a);
				return 1;
			}
		}
		// 検証
		else if (!strcmp(a, "--verify")) {
			verify = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				verifyAgainst = argv[++i];
		}
		// アニメーション最適化
		else if (!strcmp(a, "--anime-opt"))
			animOpt = 1;
		else if (!strcmp(a, "--anime-opt-verbose")) {
			animOpt = 1;
			animVerbose = 1;
		}
		else if (!strncmp(a, "--anime-opt-", 12)) {
			const char* key = a + 12;
			if (argc <= i + 1) {
				printf("ERROR: %s needs a value\n", a);
				return 1;
			}
			const char* v = argv[++i];
			animOpt = 1;
			if (!strcmp(key, "pos"))
				aopt.posEps = (float)atof(v);
			else if (!strcmp(key, "rot"))
				aopt.rotEps = (float)atof(v);
			else if (!strcmp(key, "scale"))
				aopt.scaleEps = (float)atof(v);
			else if (!strcmp(key, "tolerance"))
				animTolScale = (float)atof(v);
			else if (!strcmp(key, "subsample"))
				aopt.subsample = atoi(v);
			else if (!strcmp(key, "rounds"))
				aopt.maxRounds = atoi(v);
			else if (!strcmp(key, "sinc-a"))
				aopt.interp.sincA = atoi(v);
			else if (!strcmp(key, "threads"))
				aopt.threads = atoi(v);
			else if (!strcmp(key, "interp")) {
				if (!parse_interp_list(v, &aopt.interpMask)) return 1;
			}
			else {
				printf("ERROR: unknown option %s\n", a);
				return 1;
			}
		}
		else {
			printf("ERROR: unknown option %s\n", a);
			return 1;
		}
	}

	if (0.0f < animTolScale && animTolScale != 1.0f) {
		aopt.posEps *= animTolScale;
		aopt.rotEps *= animTolScale;
		aopt.scaleEps *= animTolScale;
	}

	YbrScene* sc = YbrLoad(argv[1]);
	if (!sc) {
		printf("ERROR: %s\n", YbrGetError());
		return 1;
	}

	if (!quiet) dump_scene(sc, showShader, glVersion, showCode);

	if (meshOpt) optimize_meshes(sc, &mopt);

	if (animOpt) {
		if (sc->animationCount == 0)
			printf(
				"\n--- animation optimize ---\nno animations in this file\n");
		else
			optimize_animations(sc, &aopt, animVerbose, writeOut == NULL);
	}

	int rc = 0;

	if (writeOut) {
		if (YbrSave(sc, writeOut))
			printf("\nwrote %s\n", writeOut);
		else {
			printf("\nwrite failed: %s\n", YbrGetError());
			rc = 1;
		}
	}

	// 検証：書き出したものを読み戻して元と比べる
	if (verify) {
		const char* other = verifyAgainst ? verifyAgainst : writeOut;
		if (!other) {
			printf(
				"\nERROR: --verify needs -o <out.ybr> or --verify "
				"<other.ybr>\n");
			rc = 1;
		}
		else {
			printf("\n--- verify (%s) ---\n", other);
			YbrScene* sc2 = YbrLoad(other);
			if (!sc2) {
				printf("  could not load: %s\n", YbrGetError());
				rc = 1;
			}
			else {
				int n = verify_scenes(sc, sc2);
				if (n == 0)
					printf("  OK : no differences\n");
				else {
					printf("  %d difference(s)\n", n);
					rc = 1;
				}
				YbrUnload(sc2);
			}
		}
	}

	YbrUnload(sc);
	return rc;
}
