#!/usr/bin/env bash
# Builds the release APKs. Runs inside WSL, where the Android SDK and NDK live.
set -euo pipefail
export ANDROID_HOME="${ANDROID_HOME:-/root/android-sdk}"
cd "$(dirname "$0")/../android"
if [ ! -f keystore.properties ]; then
    echo "android/keystore.properties is missing: it names the release key" >&2
    exit 1
fi
./gradlew :app:assembleRelease --console=plain -q
