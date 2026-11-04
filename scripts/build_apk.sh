#!/usr/bin/env bash
# Builds the release APK. Runs inside WSL, where the Android SDK and NDK live.
set -euo pipefail
export ANDROID_HOME="${ANDROID_HOME:-/root/android-sdk}"
cd "$(dirname "$0")/../android"
./gradlew :app:assembleRelease --console=plain -q
