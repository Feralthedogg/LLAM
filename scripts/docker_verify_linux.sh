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
# See LICENSES/OLD-LICENSE/Apache-2.0.txt.

set -eu

DOCKERFILE="${DOCKERFILE:-docker/linux/Dockerfile.ubuntu24}"
IMAGE="${IMAGE:-llam-linux-ubuntu24}"
VERIFY_CMD="${VERIFY_CMD:-make verify-linux CC=gcc}"

docker build -f "$DOCKERFILE" -t "$IMAGE" .
if [ -n "${DOCKER_RUN_FLAGS:-}" ]; then
    # Intentionally split trusted local runner flags such as "-v path:path -e K=V".
    # shellcheck disable=SC2086
    set -- $DOCKER_RUN_FLAGS
elif [ "${ALLOW_PRIVILEGED_DOCKER:-0}" = "1" ]; then
    set -- --privileged
else
    set -- --security-opt=no-new-privileges --cap-drop=ALL
fi
docker run --rm "$@" "$IMAGE" sh -lc "$VERIFY_CMD"
