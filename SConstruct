# -*- mode: python -*-
#
# Yui Blender to Raylib
#
#   scons                ビルドする (= scons all)
#   scons all            ライブラリ / ツール / テスト / サンプルを全部
#   scons lib            ライブラリだけ (bin/libybr, bin/libybrtool)
#   scons tool           ツールだけ (bin/ybr_tool)
#   scons test           テストだけ (bin/ybr_test)
#   scons example        サンプルだけ (bin/example_*)
#   scons -c             クリーン
#
#   mode=release         最適化する (既定)
#   mode=debug           デバッグ情報を付けて最適化しない
#
#   raylib_src=<dir>     raylib.h / raymath.h / rlgl.h の場所
#   raylib_lib=<dir>     raylib のライブラリの場所

import os
import sys
import glob as pyglob

mode = ARGUMENTS.get('mode', 'release')
if mode not in ('release', 'debug'):
    print('scons: mode は release か debug です (指定: %s)' % mode)
    Exit(1)

raylib_src = ARGUMENTS.get('raylib_src', os.environ.get('RAYLIB_SRC', ''))
raylib_lib = ARGUMENTS.get('raylib_lib', os.environ.get('RAYLIB_LIB', ''))

debug = (mode == 'debug')

# ---------------------------------------------------------------- 環境
env = Environment(ENV=os.environ, CPPPATH=['src/lib', 'src/tool'])
if raylib_src:
    env.Append(CPPPATH=[raylib_src])
if raylib_lib:
    env.Append(LIBPATH=[raylib_lib])

is_msvc = (os.path.basename(str(env.get('CC', ''))).lower().split('.')[0] == 'cl')

if is_msvc:
    # /TC : C として扱う  /utf-8 : ソースは UTF-8 (BOM 無し)
    env.Append(CCFLAGS=['/nologo', '/W3', '/TC', '/utf-8', '/std:c11'])
    env.Append(CPPDEFINES=['_CRT_SECURE_NO_WARNINGS'])
    if debug:
        # /Z7 はデバッグ情報を .obj に埋めるので、静的ライブラリでも
        # PDB の取り回しで悩まずに済む
        env.Append(CCFLAGS=['/Od', '/Z7', '/MDd'], LINKFLAGS=['/DEBUG'])
    else:
        env.Append(CCFLAGS=['/O2', '/MD'])
else:
    env.Append(CCFLAGS=['-std=c11', '-Wall', '-Wextra'])
    env.Append(CCFLAGS=['-g', '-O0'] if debug else ['-O2'])
    env.Append(LIBS=['m'])
    # C11 スレッド (ybr_anim_opt)。glibc 2.34 以降は libc に入っている
    if os.name != 'nt' and sys.platform != 'darwin':
        env.Append(LIBS=['pthread'])

# raylib 本体のリンクに要るもの
if is_msvc:
    raylibs = ['raylib', 'opengl32', 'gdi32', 'winmm',
               'shell32', 'user32', 'kernel32']
elif os.name == 'nt':
    raylibs = ['raylib', 'opengl32', 'gdi32', 'winmm']
elif sys.platform == 'darwin':
    raylibs = ['raylib']
    env.Append(LINKFLAGS=['-framework', 'OpenGL', '-framework', 'Cocoa',
                          '-framework', 'IOKit', '-framework', 'CoreVideo'])
else:
    raylibs = ['raylib', 'GL', 'dl', 'rt', 'X11']

# ---------------------------------------------------------------- ソース
for sub in ('lib', 'tool', 'test', 'examples'):
    env.VariantDir(os.path.join('bin', 'obj', sub),
                   os.path.join('src', sub), duplicate=0)


def sources(subdir, exclude=()):
    """src/<subdir>/*.c を bin/obj/<subdir>/*.o として扱う"""
    out = []
    for f in sorted(pyglob.glob(os.path.join('src', subdir, '*.c'))):
        name = os.path.basename(f)
        if name in exclude:
            continue
        out.append(os.path.join('bin', 'obj', subdir, name))
    return out


def obj(subdir, name):
    return os.path.join('bin', 'obj', subdir, name)


# ---------------------------------------------------------------- ターゲット
# src/lib/  : ライブラリ本体
# src/tool/ : ybr_tool 専用。キー削減やメッシュ最適化は実行時には要らないので
#             ライブラリ本体には入れない
ybr_lib = env.StaticLibrary('bin/ybr', sources('lib'))
ybr_toollib = env.StaticLibrary('bin/ybrtool',
                                sources('tool', exclude=('ybr_tool.c',)))

base_libs = list(env.get('LIBS', []))
tool_libs = [ybr_toollib, ybr_lib] + raylibs + base_libs

ybr_tool = env.Program('bin/ybr_tool', [obj('tool', 'ybr_tool.c')],
                       LIBS=tool_libs)
ybr_test = env.Program('bin/ybr_test', sources('test'), LIBS=tool_libs)

examples = []
for src in sorted(pyglob.glob(os.path.join('src', 'examples', '*.c'))):
    name = os.path.splitext(os.path.basename(src))[0]
    examples.append(env.Program(os.path.join('bin', name),
                                [obj('examples', os.path.basename(src))],
                                LIBS=[ybr_lib] + raylibs + base_libs))

lib_target = [ybr_lib, ybr_toollib]

env.Alias('lib', lib_target)
env.Alias('tool', ybr_tool)
env.Alias('test', ybr_test)
env.Alias('example', examples)
env.Alias('all', lib_target + [ybr_tool, ybr_test] + examples)

Default('all')

Help("""
Yui Blender to Raylib

  scons                ビルドする (= scons all)
  scons all            ライブラリ / ツール / テスト / サンプルを全部
  scons lib            ライブラリだけ
  scons tool           ツールだけ (bin/ybr_tool)
  scons test           テストだけ (bin/ybr_test)
  scons example        サンプルだけ
  scons -c             クリーン

  mode=release         最適化する (既定)
  mode=debug           デバッグ情報を付けて最適化しない

  raylib_src=<dir>     raylib.h / raymath.h / rlgl.h の場所 (RAYLIB_SRC でも可)
  raylib_lib=<dir>     raylib のライブラリの場所 (RAYLIB_LIB でも可)

テストとサンプルは raylib 本体のリンクが必要です。
bin/ybr_test は GL コンテキストが取れないときは GPU のテストだけ飛ばします。
""")
