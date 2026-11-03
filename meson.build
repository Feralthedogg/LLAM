# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# See LICENSES/Apache-2.0.txt.

project('llam-preempt-tooling',
  'c',
  version: '1.0.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=true',
  ])

preempt_mode = get_option('preempt_tooling')
preempt_target_supported = host_machine.system() == 'linux' and host_machine.cpu_family() == 'x86_64'
plugin_available = false
llvm_major = -1
python3 = import('python').find_installation(required: false)
bash_program = find_program('bash', required: preempt_mode == 'required' and preempt_target_supported)

if preempt_mode == 'required' and not preempt_target_supported
  error('preempt_tooling=required currently supports Linux x86-64 only')
endif

preempt_include = include_directories('tools/preempt')
metadata = static_library('llam_preempt_metadata',
  'tools/preempt/metadata.c',
  include_directories: preempt_include,
  install: false)
metadata_inspector = executable('llam_preempt_metadata_inspect',
  'tools/preempt/metadata_inspect.c',
  include_directories: preempt_include,
  link_with: metadata,
  install: true)
metadata_test = executable('test_preempt_metadata',
  'tests/test_preempt_metadata.c',
  include_directories: preempt_include,
  link_with: metadata)
test('preempt-metadata-format', metadata_test)

if python3.found()
  test('preempt-sdk-metadata', python3,
    args: files('scripts/test_preempt_sdk_metadata.py'))
  if host_machine.system() != 'windows'
    test('preempt-release-package', python3,
      args: files('scripts/test_preempt_package.py'))
  endif
endif
if python3.found() and bash_program.found() and host_machine.system() != 'windows'
  test('llam-clang-wrapper', python3,
    args: files('scripts/test_llam_clang.py'))
endif

install_data('tools/preempt/metadata.h',
  install_dir: join_paths(get_option('datadir'), 'llam', 'preempt'))

install_data('LICENSE', 'NOTICE', 'THIRD_PARTY_NOTICES.md',
  install_dir: join_paths(get_option('datadir'), 'llam'))
install_data('LICENSES/Apache-2.0.txt',
  install_dir: join_paths(get_option('datadir'), 'llam', 'LICENSES'))

if preempt_mode != 'off'
  if not preempt_target_supported
    message('LLVM preemption plugin unsupported on this target; auto mode is off')
  else
    cpp_available = add_languages('cpp', required: preempt_mode == 'required')
    if cpp_available
      llvm_dependency = dependency('llvm',
        modules: ['core', 'analysis', 'passes', 'support'],
        required: preempt_mode == 'required')
      if llvm_dependency.found()
        llvm_major = llvm_dependency.version().split('.')[0].to_int()
        llvm_config_program = find_program(
          'llvm-config-@0@'.format(llvm_major), 'llvm-config', required: true)
        llvm_include_path = run_command(llvm_config_program, '--includedir', check: true).stdout().strip()
        llvm_system_include = include_directories(llvm_include_path, is_system: true)
        llvm_link_dependency = llvm_dependency.partial_dependency(includes: false, sources: false)
        plugin_target = shared_module('llam_preempt_pass',
          'tools/preempt/llam_preempt_pass.cpp',
          include_directories: [preempt_include, llvm_system_include],
          dependencies: llvm_link_dependency,
          override_options: ['cpp_std=c++17'],
          name_prefix: '',
          install: true,
          install_dir: join_paths(get_option('libdir'), 'llam'))
        plugin_available = true
      elif preempt_mode == 'required'
        error('preempt_tooling=required needs LLVM development files')
      else
        message('LLVM development files not found; optional preemption plugin is off')
      endif
    elif preempt_mode == 'required'
      error('preempt_tooling=required needs a C++ compiler')
    else
      message('C++ compiler not found; optional preemption plugin is off')
    endif
  endif
endif

tooling_metadata = configuration_data()
tooling_metadata.set('CMAKE_INSTALL_BINDIR', get_option('bindir'))
tooling_metadata.set('LLAM_PREEMPT_TOOLING_MODE', preempt_mode)
tooling_metadata.set('LLAM_PREEMPT_PLUGIN_STATUS', plugin_available ? 'available' : 'unavailable')
driver_status = 'unavailable'
if preempt_target_supported and bash_program.found()
  driver_status = join_paths(get_option('bindir'), 'llam-clang')
  install_data('tools/llam-clang',
    install_dir: get_option('bindir'),
    install_mode: 'rwxr-xr-x')
endif
if preempt_mode == 'required' and not plugin_available
  error('preempt_tooling=required needs the LLVM preemption plugin')
endif
tooling_metadata.set('LLAM_PREEMPT_DRIVER_STATUS', driver_status)
configure_file(
  input: 'cmake/llam-preempt-tooling.conf.in',
  output: 'tooling.conf',
  configuration: tooling_metadata,
  install_dir: join_paths(get_option('datadir'), 'llam', 'preempt'))

if preempt_mode != 'off' and plugin_available
  clang_program = find_program(
    'clang-@0@'.format(llvm_major), 'clang', required: preempt_mode == 'required')
  objcopy_program = find_program('objcopy', required: preempt_mode == 'required')
  clang_major_matches = false
  if clang_program.found()
    clang_version = run_command(clang_program, '-dumpversion', check: preempt_mode == 'required')
    clang_version_fields = clang_version.stdout().strip().split('.')
    clang_major_matches = clang_version.returncode() == 0 and \
      clang_version_fields.length() > 0 and \
      clang_version_fields[0] == '@0@'.format(llvm_major)
  endif
  if preempt_mode == 'required' and not clang_major_matches
    error('preempt_tooling=required needs clang matching LLVM @0@'.format(llvm_major))
  endif
  if python3.found() and clang_program.found() and objcopy_program.found() and clang_major_matches
    test('llam-preempt-llvm-plugin', python3,
      args: files('scripts/test_preempt_llvm_plugin.py') + [
        '--plugin', plugin_target.full_path(),
        '--inspector', metadata_inspector.full_path(),
        '--driver', join_paths(meson.current_source_dir(), 'tools', 'llam-clang'),
        '--clang', clang_program.full_path(),
        '--llvm-major', '@0@'.format(llvm_major),
      ],
      depends: [plugin_target, metadata_inspector])
  elif python3.found()
    message('matching clang/objcopy unavailable; LLVM plugin smoke test is not registered')
  endif
endif
