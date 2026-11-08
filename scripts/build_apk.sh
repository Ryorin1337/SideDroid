#!/usr/bin/env bash
# Builds the release APKs. Runs inside WSL, where the Android SDK and NDK live.
set -euo pipefail
export ANDROID_HOME="${ANDROID_HOME:-/root/android-sdk}"
cd "$(dirname "$0")/../android"
signing_properties="${SIDEDROID_SIGNING_PROPERTIES:-$PWD/keystore.properties}"
if [ ! -f "$signing_properties" ]; then
    echo "Signing properties not found: $signing_properties" >&2
    exit 1
fi
./gradlew :app:assembleRelease --console=plain -q -PsigningProperties="$signing_properties"
