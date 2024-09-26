#!/bin/bash
#
# Script to generate DMG image from application bundle
#

# Exit in case of error
set -e -o pipefail
trap 'echo "${BASH_SOURCE[0]}{${FUNCNAME[0]}}:${LINENO}: Error: command \`${BASH_COMMAND}\` failed with exit code $?"' ERR

# Define application name
PROGN=darktable

# Go to directory of script
scriptDir=$(dirname "$0")
cd "$scriptDir"/
scriptDir=$(pwd)
buildDir="${scriptDir}/../../build/macosx"

cd "$buildDir"/

# Generate symlink to applications folder for easier drag & drop within dmg
ln -s /Applications ./package || true

# copy macOS install background image
mkdir ./package/.background
cp "$scriptDir"/macos_install_background.png ./package/.background

# When building on github runner, 'hdiutil create' occasionally fails (resource busy)
# so we make several retries
try_count=0
hdiutil_success=0

while [ $hdiutil_success -ne 1 -a $try_count -lt 8 ]; do
    # Create temporary rw image
    if hdiutil create -srcfolder package -volname "${PROGN}" -fs HFS+ \
        -fsargs "-c c=64,a=16,e=16" -format UDRW pack.temp.dmg
    then
        hdiutil_success=1
        break
    fi
    try_count=$(( $try_count + 1 ))
    echo "'hdiutil create' failed (attempt ${try_count}). Retrying..."
    sleep 1
done

if [ $hdiutil_success -ne 1 -a -n "${GITHUB_RUN_ID}" ]; then
    # Still no success after 8 attempts.
    # If we are on github runner, kill the Xprotect service and make one
    # final attempt.
    # see https://github.com/actions/runner-images/issues/7522
    echo "Killing XProtect..."
    sudo pkill -9 XProtect >/dev/null || true;
    sleep 3

    if hdiutil create -srcfolder package -volname "${PROGN}" -fs HFS+ \
        -fsargs "-c c=64,a=16,e=16" -format UDRW pack.temp.dmg
    then
        hdiutil_success=1
    fi
fi

if [ $hdiutil_success -ne 1 ]; then
    echo "FATAL: 'hdiutil create' FAILED!"
    exit 1
fi 

# Mount image without autoopen to create window style params
device=$(hdiutil attach -readwrite -noverify -autoopen "pack.temp.dmg" |
    egrep '^/dev/' | sed 1q | awk '{print $1}')

echo '
 set {product_name} to words of (do shell script "printf \"%s\", '${PROGN}'")
 set background to alias ("Volumes:"&product_name&":.background:macos_install_background.png")

 tell application "Finder"
    tell disk "'${PROGN}'"
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set the bounds of container window to {400, 100, 900, 430}
        set theViewOptions to the icon view options of container window
        set arrangement of theViewOptions to not arranged
        set icon size of theViewOptions to 72
        set background picture of theViewOptions to background
        set position of item "'${PROGN}'" of container window to {100, 150}
        set position of item "Applications" of container window to {400, 150}
        update without registering applications
    end tell
 end tell
' | osascript

# Finalizing creation
chmod -Rf go-w /Volumes/"${PROGN}"
sync
hdiutil detach ${device}
DMG="${PROGN}-$(git describe --tags --match release-* | sed 's/^release-//;s/-/+/;s/-/~/;s/rc/~rc/')-$(uname -m)"
hdiutil convert "pack.temp.dmg" -format UDZO -imagekey zlib-level=9 -o "${DMG}"

#cleanup
rm -f pack.temp.dmg
rm -f package/Applications
rm -rf package/.background

# Sign dmg image when a certificate has been provided
if [ -n "$CODECERT" ]; then
    codesign --deep --verbose --force --options runtime -i "org.darktable" -s "${CODECERT}" "${DMG}".dmg
fi
