/*
	Yui Blender to Raylib - テストプログラムの入口
		Programed by あるる（きのもと 結衣）

	  ./bin/ybr_test              内蔵の合成シーンでテストする
	  ./bin/ybr_test scene.ybr    読み込んだシーンでも追加のテストを行う

	.ybr ファイルを用意しなくても動くように、シーンはコード内で組み立てる。
	当たり判定は「8 分木を使った結果」と「全三角形を総当たりした結果」を
	突き合わせて検証している (8 分木のノード枝刈りにミスがあれば落ちる)。

	起動時に隠しウィンドウを開き、GL コンテキストが取れたときだけ
	GLSL のコンパイルや VAO / VBO の生成といった GPU 側のテストも走らせる。
	ヘッドレス環境では GPU のテストだけ飛ばす (失敗にはしない)。
*/
#include "ybr_test.h"

int main(int argc, char** argv)
{
	printf("ybr_test\n");

	/* GPU のテストのために隠しウィンドウを開く。
	 * 開けなければ GL が要らないテストだけを走らせる。 */
	SetTraceLogLevel(LOG_WARNING);
	SetConfigFlags(FLAG_WINDOW_HIDDEN);
	InitWindow(320, 240, "ybr_test");
	int hasGL = IsWindowReady();
	if (hasGL)
		printf("GL version code: %d\n", rlGetVersion());
	else
		printf("GL context is not available, skipping the GPU tests\n");

	TestScene ts;
	make_scene(&ts);

	YbrSolid* col = YbrSolidBuild(&ts.scene, NULL);
	if (!col) {
		printf("FATAL: YbrSolidBuild failed\n");
		if (hasGL) CloseWindow();
		return 1;
	}

	test_geometry();
	test_build(col);
	test_segment(col);
	test_sphere(col);
	test_capsule(col);
	test_triangle(col);
	test_overlap_and_tags(&ts.scene, col);
	test_options(&ts.scene);
	test_pose();
	test_blend_tree();
	test_split_mesh();
	test_shader_nodes();
	test_simple_shader();
	test_light_count();
	test_gpu_skinning_shader();
	test_node_group();
	test_hermite();
	test_sweep();
	test_dynamic();
	test_dynamic_world();
	test_visibility();
	test_mesh_opt();
	test_material_override();
	test_instances();
	test_instance_bounds();
	test_attachment();
	test_scene_lights_camera();
	test_generated_glsl_scope();
	test_embedded_texture_survives();
	test_pose_markers();
	test_yabt();
	test_frustum();
	test_point_spot_lights();
	test_broken_files();
	test_byte_order();

	if (hasGL) {
		test_gl_simple_shaders();
		test_gl_all_nodes();
		test_gl_light_kinds();
		test_gl_model();
		test_gl_shadows();
	}

	YbrSolidUnload(col);

	for (int i = 1; i < argc; i++) {
		test_loaded(argv[i]);
		if (hasGL) test_gl_scene_file(argv[i]);
	}

	if (hasGL) CloseWindow();

	printf("\n==================================\n");
	printf("passed : %d\n", g_pass);
	printf("failed : %d\n", g_fail);
	printf("==================================\n");
	return g_fail == 0 ? 0 : 1;
}
