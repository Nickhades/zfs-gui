project('zfs-gui', 'c',
  version: '1.0.0',
  license: 'BSD-2-Clause',
  meson_version: '>= 0.60.0',
  default_options: [
    'warning_level=3',
    'c_std=c17',
    'buildtype=debugoptimized',
  ],
)

gnome = import('gnome')

cc = meson.get_compiler('c')

# ---------------------------------------------------------------------------
# 依赖
# ---------------------------------------------------------------------------
glib_dep = dependency('glib-2.0', version: '>= 2.66')
gio_dep  = dependency('gio-2.0',  version: '>= 2.66')
gtk_dep  = dependency('gtk4',     version: '>= 4.6', required: get_option('gui'))

# 可选的 libzfs 只读适配层（pkg-config: sysutils/openzfs 或 libzfs 开发包）
libzfs_dep = declare_dependency()
use_libzfs = false
if get_option('libzfs') != 'disabled'
  libzfs_full = dependency('libzfs', required: get_option('libzfs') == 'enabled')
  if libzfs_full.found()
    # 只需要头文件/宏：运行时通过 dlopen 加载，不产生硬链接依赖
    libzfs_dep = libzfs_full.partial_dependency(compile_args: true, includes: true)
    use_libzfs = true
  endif
endif
pam_dep  = cc.find_library('pam', required: true)

# ---------------------------------------------------------------------------
# 编译告警（GTK 推荐的一组严格告警，先做编译器支持性检测）
# ---------------------------------------------------------------------------
warn_flags = [
  '-Wshadow',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wpointer-arith',
  '-Wwrite-strings',
  '-Wformat=2',
  '-Wpedantic',
  '-Werror=implicit-function-declaration',
  '-Werror=incompatible-pointer-types',
  '-Werror=return-type',
  '-Werror=int-conversion',
  '-Wno-unused-parameter',
  '-Wno-deprecated-declarations',
]
add_project_arguments(cc.get_supported_arguments(warn_flags), language: 'c')

common_inc = include_directories('common')

# ---------------------------------------------------------------------------
# 公共库：前端与守护进程共用（协议、JSON、命令表、参数校验、帧 IO）
# ---------------------------------------------------------------------------
zgcommon = static_library('zgcommon',
  files(
    'common/zg-json.c',
    'common/zg-util.c',
    'common/zg-opdef.c',
    'common/zg-argv.c',
    'common/zg-io.c',
  ),
  include_directories: common_inc,
  dependencies: [glib_dep],
  install: false,
)

zgcommon_dep = declare_dependency(
  link_with: zgcommon,
  include_directories: common_inc,
  dependencies: [glib_dep],
)

# ---------------------------------------------------------------------------
# 后端：特权守护进程 zfs-guid
# ---------------------------------------------------------------------------
scm_test = executable('scm_rights_test',
  files('tests/scm_rights_test.c'),
  build_by_default: true,
)

executable('zfs-helper',
  files('helper/zfs-helper.c'),
  dependencies: [zgcommon_dep, glib_dep],
  install: true,
  install_dir: get_option('sbindir'),
)

test('scm_rights', scm_test)

kqueue_test = executable('kqueue_frame_test',
  files('tests/kqueue_frame_test.c'),
  build_by_default: true,
)

test('kqueue_frame', kqueue_test)

frame_test = executable('frame_buffer_test',
  files('tests/frame_buffer_test.c', 'daemon/zg-frame.c'),
  dependencies: [glib_dep],
  include_directories: include_directories('daemon'),
  build_by_default: true,
)

test('frame_buffer', frame_test)

echo_test = executable('kqueue_echo_test',
  files('tests/kqueue_echo_test.c', 'daemon/zg-frame.c'),
  dependencies: [glib_dep],
  include_directories: include_directories('daemon'),
  build_by_default: true,
)

test('kqueue_echo', echo_test)

task_test = executable('kqueue_task_test',
  files('tests/kqueue_task_test.c', 'daemon/zg-task.c', 'daemon/zg-frame.c', 'common/zg-json.c'),
  dependencies: [glib_dep],
  include_directories: include_directories('daemon', 'common'),
  build_by_default: true,
)

test('kqueue_task', task_test)

executable('zfs-guid',
  files(
    'daemon/zfs-guid.c',
    'daemon/zg-exec.c',
    'daemon/zg-policy.c',
    'daemon/zg-server.c',
    'daemon/zg-ops.c',
    'daemon/zg-log.c',
    'daemon/zg-auth.c',
    'daemon/zg-libzfs-exec.c',
    'daemon/zg-frame.c',
    'daemon/zg-helper.c',
    'daemon/zg-progress.c',
  ),
  dependencies: [zgcommon_dep, pam_dep, libzfs_dep],
  c_args: use_libzfs ? ['-DHAVE_LIBZFS', '-Wno-gnu-include-next', '-Wno-variadic-macros'] : [],
  install: true,
  install_dir: get_option('sbindir'),
)

# ---------------------------------------------------------------------------
# 前端：GTK4 图形界面 zfs-gui
# ---------------------------------------------------------------------------
if gtk_dep.found()
  executable('zfs-gui',
    files(
      'src/main.c',
      'src/zg-app.c',
      'src/zg-client.c',
      'src/zg-model.c',
      'src/zg-window.c',
      'src/zg-ui.c',
      'src/zg-page-pools.c',
      'src/zg-page-datasets.c',
      'src/zg-page-snapshots.c',
      'src/zg-page-sendrecv.c',
      'src/zg-page-be.c',
      'src/zg-page-system.c',
      'src/zg-page-ops.c',
      'src/zg-page-backup.c',
      'src/zg-page-help.c',
      'src/zg-dialogs.c',
      'src/zg-dialogs-objects.c',
      'src/zg-dialogs-import.c',
      'src/zg-dialogs-secure.c',
      'src/zg-actions.c',
      'src/zg-libzfs.c',
  'src/zg-table.c',
    ) + gnome.compile_resources('zfs-gui-icons',
        'data/icons/icons.gresource.xml',
        source_dir: 'data/icons',
        c_name: 'zg_icons'),
    dependencies: [zgcommon_dep, gtk_dep, gio_dep, libzfs_dep],
    c_args: use_libzfs ? ['-DHAVE_LIBZFS', '-Wno-gnu-include-next', '-Wno-variadic-macros'] : [],
    install: true,
  )

  install_data('data/org.freebsd.zfsgui.desktop',
    install_dir: get_option('datadir') / 'applications',
  )

  install_data('data/zfs-gui.svg',
    install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
  )
endif

# ---------------------------------------------------------------------------
# 守护进程运行期资源：rc.d 脚本、默认配置、手册页
# ---------------------------------------------------------------------------
install_data('data/rc.d/zfsguid',
  install_dir: get_option('sysconfdir') / 'rc.d',
  install_mode: 'rwxr-xr-x',
)

install_data('data/zfs-gui.conf',
  install_dir: get_option('sysconfdir'),
)

install_man('data/zfs-guid.8')

# PAM 服务配置：危险操作的二次认证
install_data('data/pam.d/zfs-gui',
  install_dir: '/etc/pam.d',
)
install_data('LICENSE',
             install_dir: join_paths(get_option('datadir'), 'doc', meson.project_name()))
install_data('OPEN-SOURCE.md',
             install_dir: join_paths(get_option('datadir'), 'doc', meson.project_name()))
