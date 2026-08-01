#!/bin/sh
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

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
