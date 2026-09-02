#!/usr/bin/env bash

# Setup Discord SDK for build
# Downloads the Discord Social SDK from the private release using a token.

set -euo pipefail

SDK_DIR="lib/discord_social_sdk"
RELEASE_LIB_DIR="$SDK_DIR/lib/release"
DEBUG_LIB_DIR="$SDK_DIR/lib/debug"
TMP_DIR="$(mktemp -d)"

ASSET_NAME="${SDK_ASSET_NAME:?SDK_ASSET_NAME is not set}"
DOWNLOAD_URL="${SDK_DOWNLOAD_URL:?SDK_DOWNLOAD_URL is not set}"
DOWNLOAD_TOKEN="${SDK_DOWNLOAD_TOKEN:?SDK_DOWNLOAD_TOKEN is not set}"

# SHA-256 of the official Discord Social SDK zip as downloaded from the
# Discord Developer Portal. Used as both the cache key in CI and to verify
# the downloaded archive has not been tampered with.
# ASSET_NAME:0:-4 deletes the last 4 characters from the string ".zip"
SDK_SHA256=$(tr -d '[:space:]' < "$(dirname "$0")/${ASSET_NAME%????}-sha256.txt")

# Parse a GitHub release asset browser URL into an API assets URL.
# Input:  https://github.com/{owner}/{repo}/releases/download/{tag}/{filename}
# Output: https://api.github.com/repos/{owner}/{repo}/releases/assets/{asset_id}
resolve_github_asset_url() {
    local browser_url="$1"
    local token="$2"

    # Extract owner, repo, tag from the browser download URL
    local owner repo tag
    owner=$(echo "$browser_url" | sed -E 's|https://github.com/([^/]+)/.*|\1|')
    repo=$(echo "$browser_url"  | sed -E 's|https://github.com/[^/]+/([^/]+)/.*|\1|')
    tag=$(echo "$browser_url"   | sed -E 's|.*/releases/download/([^/]+)/.*|\1|')

    local api_release="https://api.github.com/repos/$owner/$repo/releases/tags/$tag"
    echo "🔍 Looking up release via API: $api_release" >&2

    if ! command -v curl >/dev/null 2>&1; then
        echo "❌ curl is required to resolve the GitHub API asset URL" >&2
        exit 1
    fi

    local release_json
    release_json=$(curl -sL --fail \
        -H "Authorization: Bearer $token" \
        -H "Accept: application/vnd.github+json" \
        "$api_release")

    local asset_url
    if command -v jq >/dev/null 2>&1; then
        asset_url=$(echo "$release_json" | jq -r ".assets[] | select(.name == \"$ASSET_NAME\") | .url")
    else
        # Fallback: grep for the asset URL by scanning for the name then backtracking to its url field
        asset_url=$(echo "$release_json" \
            | grep -B5 "\"name\": *\"$ASSET_NAME\"" \
            | grep '"url"' \
            | tail -1 \
            | sed -E 's/.*"url": *"([^"]+)".*/\1/')
    fi

    if [ -z "$asset_url" ]; then
        echo "❌ Could not find asset '$ASSET_NAME' in release '$tag'" >&2
        exit 1
    fi

    echo "$asset_url"
}

download_file() {
    local dest="$1"
    local url="$2"
    local token="$3"

    echo "🌐 Requesting: $url"

    if command -v curl >/dev/null 2>&1; then
        curl -L --fail \
            -H "Authorization: Bearer $token" \
            -H "Accept: application/octet-stream" \
            -o "$dest" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget \
            --header="Authorization: Bearer $token" \
            --header="Accept: application/octet-stream" \
            -O "$dest" "$url"
    elif command -v pwsh >/dev/null 2>&1; then
        pwsh -Command "Invoke-WebRequest -Uri '$url' -OutFile '$dest' -UseBasicParsing -Headers @{'Authorization'='Bearer $token';'Accept'='application/octet-stream'}"
    elif command -v powershell >/dev/null 2>&1; then
        powershell -Command "Invoke-WebRequest -Uri '$url' -OutFile '$dest' -UseBasicParsing -Headers @{'Authorization'='Bearer $token';'Accept'='application/octet-stream'}"
    else
        echo "❌ No download tool available (curl, wget, or powershell required)"
        exit 1
    fi
}

verify_sha256() {
    local file="$1"
    local expected="$2"
    local actual

    if command -v sha256sum >/dev/null 2>&1; then
        actual=$(sha256sum "$file" | awk '{print $1}')
    elif command -v shasum >/dev/null 2>&1; then
        actual=$(shasum -a 256 "$file" | awk '{print $1}')
    else
        echo "❌ No SHA-256 tool available (sha256sum or shasum required)"
        exit 1
    fi

    if [ "$actual" != "$expected" ]; then
        echo "❌ SHA-256 mismatch — file may have been tampered with"
        echo "   Expected: $expected"
        echo "   Got:      $actual"
        exit 1
    fi

    echo "✓ SHA-256 verified"
}

echo "Setting up Discord Social SDK..."

if [ -d "$SDK_DIR" ] && [ -f "$RELEASE_LIB_DIR/libdiscord_partner_sdk.so" ]; then
    echo "✓ Discord SDK already present"
    exit 0
fi

mkdir -p "$SDK_DIR"
mkdir -p "$RELEASE_LIB_DIR"
mkdir -p "$DEBUG_LIB_DIR"

echo "📥 Downloading Discord SDK..."
ASSET_API_URL=$(resolve_github_asset_url "$DOWNLOAD_URL" "$DOWNLOAD_TOKEN")
FILE_PATH="$TMP_DIR/$ASSET_NAME"
download_file "$FILE_PATH" "$ASSET_API_URL" "$DOWNLOAD_TOKEN"

echo "🔍 Verifying integrity..."
verify_sha256 "$FILE_PATH" "$SDK_SHA256"

echo "📦 Extracting SDK..."
unzip -q "$FILE_PATH" -d "$TMP_DIR"

if [ -d "$TMP_DIR/discord-sdk/discord_social_sdk" ]; then
    rm -rf "$SDK_DIR"
    mv "$TMP_DIR/discord-sdk/discord_social_sdk" "$SDK_DIR"
elif [ -d "$TMP_DIR/discord_social_sdk" ]; then
    rm -rf "$SDK_DIR"
    mv "$TMP_DIR/discord_social_sdk" "$SDK_DIR"
else
    echo "❌ Extracted archive does not contain discord_social_sdk in the expected location"
    echo "Contents of $TMP_DIR:"
    find "$TMP_DIR" -maxdepth 3 | sed "s#^$TMP_DIR##"
    exit 1
fi

rm -rf "$TMP_DIR"

echo "✓ Discord SDK setup complete"
exit 0
