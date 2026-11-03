#!/bin/sh
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
# See LICENSE.

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: generate_sdk_metadata.sh <stage-dir> [target]" >&2
    exit 2
fi

stage="$1"
target="${2:-}"
version="${LLAM_VERSION:-3.0.0}"
package_major="${version%%.*}"
preempt_tooling="${LLAM_PREEMPT_TOOLING:-off}"

case "$preempt_tooling" in
    off|auto|required) ;;
    *)
        echo "LLAM_PREEMPT_TOOLING must be off, auto, or required" >&2
        exit 2
        ;;
esac

case "$(uname -s)" in
    FreeBSD|OpenBSD|NetBSD|DragonFly)
        echo "BSD support ended in LLAM 3.0.0" >&2; exit 1 ;;
esac

if [ -z "$target" ]; then
    case "$(uname -s)" in
        Darwin) target="macos-$(uname -m)" ;;
        Linux) target="linux-$(uname -m)" ;;
        *) target="$(uname -s)-$(uname -m)" ;;
    esac
fi

case "$target" in
    freebsd-*|openbsd-*|netbsd-*|dragonflybsd-*)
        echo "BSD support ended in LLAM 3.0.0" >&2; exit 1 ;;
    linux-*)
        private_libs="-pthread -luring -lm"
        ;;
    macos-*|darwin-*)
        private_libs="-pthread"
        ;;
    *)
        private_libs="-pthread"
        ;;
esac

mkdir -p "$stage/lib/pkgconfig" "$stage/share/llam/cmake" "$stage/share/llam/preempt"

preempt_plugin="unavailable"
preempt_driver="unavailable"
preempt_plugin_path="$stage/lib/llam/llam_preempt_pass.so"
if [ "$preempt_tooling" = "required" ] && [ "$target" != "linux-x86_64" ]; then
    echo "LLAM_PREEMPT_TOOLING=required currently supports linux-x86_64 only" >&2
    exit 1
fi
if [ "$preempt_tooling" != "off" ] && [ -e "$preempt_plugin_path" ]; then
    if [ -L "$preempt_plugin_path" ] || [ ! -f "$preempt_plugin_path" ]; then
        echo "refusing non-regular staged preemption plugin: $preempt_plugin_path" >&2
        exit 1
    fi
    preempt_plugin="available"
fi
if [ "$preempt_tooling" = "required" ] && [ "$preempt_plugin" != "available" ]; then
    echo "LLAM_PREEMPT_TOOLING=required plugin is absent from the staged SDK" >&2
    exit 1
fi
if [ -x "$stage/bin/llam-clang" ]; then
    preempt_driver="bin/llam-clang"
fi
if [ "$preempt_tooling" = "required" ] &&
   { [ "$preempt_driver" = "unavailable" ] ||
     [ ! -x "$stage/bin/llam_preempt_metadata_inspect" ]; }; then
    echo "LLAM_PREEMPT_TOOLING=required compiler driver/inspector is absent from the staged SDK" >&2
    exit 1
fi

cat > "$stage/share/llam/preempt/tooling.conf" <<EOF
format_version=1
section=.llam.preempt
tooling_mode=$preempt_tooling
compiler_driver=$preempt_driver
metadata_inspector=bin/llam_preempt_metadata_inspect
llvm_plugin=$preempt_plugin
language_bindings=excluded
EOF

cat > "$stage/lib/pkgconfig/llam.pc" <<EOF
prefix=\${pcfiledir}/../..
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: LLAM
Description: Stackful user-thread runtime for C applications
Version: $version
Libs: -L\${libdir} -lllam_runtime
Libs.private: $private_libs
Cflags: -I\${includedir}
EOF

cat > "$stage/share/llam/cmake/llam-config.cmake" <<'EOF'
include(CMakeFindDependencyMacro)

find_dependency(Threads)

get_filename_component(_LLAM_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/llam-targets.cmake")

set(llam_FOUND TRUE)
EOF

cat > "$stage/share/llam/cmake/llam-targets.cmake" <<'EOF'
set(_LLAM_PLATFORM_LIBS Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _LLAM_PLATFORM_LIBS uring m)
endif()

if(NOT TARGET llam::runtime)
    add_library(llam::runtime STATIC IMPORTED)
    set_target_properties(llam::runtime PROPERTIES
        IMPORTED_LOCATION "${_LLAM_PREFIX}/lib/libllam_runtime.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_LLAM_PREFIX}/include"
        INTERFACE_LINK_LIBRARIES "${_LLAM_PLATFORM_LIBS}"
    )
endif()

set(_LLAM_SHARED_LOCATION "")
foreach(_LLAM_CANDIDATE
    "${_LLAM_PREFIX}/lib/libllam_runtime.dylib"
    "${_LLAM_PREFIX}/lib/libllam_runtime.so"
)
    if(EXISTS "${_LLAM_CANDIDATE}")
        set(_LLAM_SHARED_LOCATION "${_LLAM_CANDIDATE}")
        break()
    endif()
endforeach()

if(_LLAM_SHARED_LOCATION AND NOT TARGET llam::runtime_shared)
    add_library(llam::runtime_shared SHARED IMPORTED)
    set_target_properties(llam::runtime_shared PROPERTIES
        IMPORTED_LOCATION "${_LLAM_SHARED_LOCATION}"
        INTERFACE_INCLUDE_DIRECTORIES "${_LLAM_PREFIX}/include"
        INTERFACE_LINK_LIBRARIES "${_LLAM_PLATFORM_LIBS}"
    )
endif()

unset(_LLAM_CANDIDATE)
unset(_LLAM_PLATFORM_LIBS)
unset(_LLAM_SHARED_LOCATION)
EOF

cat > "$stage/share/llam/cmake/llam-config-version.cmake" <<EOF
set(PACKAGE_VERSION "$version")

if(PACKAGE_FIND_VERSION_MAJOR STREQUAL "" OR PACKAGE_FIND_VERSION_MAJOR EQUAL $package_major)
    if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
    endif()
    if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
EOF
