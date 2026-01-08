#!/bin/bash
# Bash script to get git commit hash and create version header
# This can be run before building to inject the git commit hash

GIT_HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")

cat > version.h << EOF
// Auto-generated version header - do not edit manually
// Generated from git commit hash: $GIT_HASH
#ifndef FIRMWARE_VERSION_H
#define FIRMWARE_VERSION_H
#define FIRMWARE_VERSION "$GIT_HASH"
#endif
EOF

echo "Generated version.h with commit hash: $GIT_HASH"
