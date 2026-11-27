#!/bin/sh
# Generate the Xcode project with YOUR identity and build the installer app.
#   DEVELOPMENT_TEAM=XXXXXXXXXX BUNDLE_PREFIX=com.yourname ./build.sh
# Add CHECK=1 to only check that it compiles (no signing, no Apple account needed).
set -e
cd "$(dirname "$0")"
if [ "${CHECK:-0}" = 1 ]; then
    : "${DEVELOPMENT_TEAM:=0000000000}"
    : "${BUNDLE_PREFIX:=com.example}"
fi
: "${DEVELOPMENT_TEAM:?set DEVELOPMENT_TEAM to your Apple Developer team ID}"
: "${BUNDLE_PREFIX:?set BUNDLE_PREFIX, for example com.yourname}"
export DEVELOPMENT_TEAM BUNDLE_PREFIX
command -v xcodegen >/dev/null || { echo "Install XcodeGen first: brew install xcodegen"; exit 1; }
xcodegen generate
if [ "${CHECK:-0}" = 1 ]; then
    set -- CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
else
    set -- -allowProvisioningUpdates -allowProvisioningDeviceRegistration
fi
xcodebuild -project MicroBook.xcodeproj -scheme MicroBook -configuration Debug \
    -derivedDataPath build-xcode "$@" build
echo "Built: $PWD/build-xcode/Build/Products/Debug/MicroBookInstaller.app"
