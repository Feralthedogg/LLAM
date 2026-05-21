#!/bin/sh
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

set -eu

DOCKERFILE="${DOCKERFILE:-docker/linux/Dockerfile.ubuntu24}"
IMAGE="${IMAGE:-llam-linux-ubuntu24}"
VERIFY_CMD="${VERIFY_CMD:-make verify-linux CC=gcc}"

docker build -f "$DOCKERFILE" -t "$IMAGE" .
docker run --rm ${DOCKER_RUN_FLAGS:---privileged} "$IMAGE" sh -lc "$VERIFY_CMD"
