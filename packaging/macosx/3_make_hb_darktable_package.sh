#!/bin/bash
#
# Script to create an application bundle from build files
#
# Usage note:   Define CODECERT to properly sign app bundle. As example:
#               $ export CODECERT="developer@apple.id"
#               The mail address is the email/id of your developer certificate.
#

# Exit in case of error
set -e -o pipefail
trap 'echo "${BASH_SOURCE[0]}{${FUNCNAME[0]}}:${LINENO}: Error: command \`${BASH_COMMAND}\` failed with exit code $?"' ERR

# Go to directory of script
scriptDir=$(dirname "$0")
cd "$scriptDir"/

# Define base variables
dtPackageDir="package"
dtAppName="darktable"
dtWorkingDir="$dtPackageDir"/"$dtAppName".app
dtResourcesDir="$dtWorkingDir"/Contents/Resources
dtExecDir="$dtWorkingDir"/Contents/MacOS
dtExecutables=$(echo "$dtExecDir"/darktable{,-chart,-cli,-cltest,-generate-cache,-rs-identify,-curve-tool,-noiseprofile})
homebrewHome=$(brew --prefix)

# Install direct and transitive dependencies
function install_dependencies {
    local hbDependencies

    # Get depedencies of current executable
    oToolLDependencies=$(otool -L "$1" 2>/dev/null | grep compatibility | cut -d\( -f1 | sed 's/^[[:blank:]]*//;s/[[:blank:]]*$//' | uniq)

    # Filter for homebrew dependencies
    if [[ "$oToolLDependencies" == *"$homebrewHome"* ]]; then
        hbDependencies=$(echo "$oToolLDependencies" | grep "$homebrewHome")
    fi

    # Check for any homebrew dependencies
    if [[ -n "$hbDependencies" && "$hbDependencies" != "" ]]; then

        # Iterate over homebrew dependencies to install them accordingly
        for hbDependency in $hbDependencies; do
            # Skip dependency if it is a dependency of itself
            if [[ "$hbDependency" != "$1" ]]; then

                # Store file name
                dynDepOrigFile=$(basename "$hbDependency")
                dynDepTargetFile="$dtResourcesDir/lib/$dynDepOrigFile"

                # Install dependency if not yet existant
                if [[ ! -f "$dynDepTargetFile" ]]; then
                    echo "Installing dependency <$hbDependency> of <$1>."

                    # Copy dependency as not yet existant
                    cp -L "$hbDependency" "$dynDepTargetFile"

                    # Handle transitive dependencies
                    install_dependencies "$dynDepTargetFile"
                fi
            fi
        done

    fi
}

# Reset executable path to relative path
# Background: see e.g. http://clarkkromenaker.com/post/library-dynamic-loading-mac/
function reset_exec_path {
    local hbDependencies

    # Get shared libraries used of current executable
    oToolLDependencies=$(otool -L "$1" 2>/dev/null | grep compatibility | cut -d\( -f1 | sed 's/^[[:blank:]]*//;s/[[:blank:]]*$//' | uniq)

    # Handle libdarktable.dylib
    if [[ "$oToolLDependencies" == *"@rpath/libdarktable.dylib"* && "$1" != *"libdarktable.dylib"* ]]; then
        echo "Resetting loader path for libdarktable.dylib of <$1>"
        install_name_tool -rpath @loader_path/../lib/darktable @loader_path/../Resources/lib/darktable "$1"
    fi

    # Filter for any homebrew specific paths
    if [[ "$oToolLDependencies" == *"$homebrewHome"* ]]; then
        hbDependencies=$(echo "$oToolLDependencies" | grep "$homebrewHome")
    fi

    # Check for any homebrew dependencies
    if [[ -n "$hbDependencies" && "$hbDependencies" != "" ]]; then

        # Iterate over homebrew dependencies to reset path accordingly
        for hbDependency in $hbDependencies; do

            # Store file name
            dynDepOrigFile=$(basename "$hbDependency")
            dynDepTargetFile="$dtResourcesDir/lib/$dynDepOrigFile"

            echo "Resetting executable path for dependency <$hbDependency> of <$1>"

            # Set correct executable path
            install_name_tool -change "$hbDependency" "@executable_path/../Resources/lib/$dynDepOrigFile" "$1"
        done

    fi

    # Get shared library id name of current executable
    oToolDDependencies=$(otool -D "$1" 2>/dev/null | sort | uniq)

    # Set correct ID to new destination if required
    if [[ "$oToolDDependencies" == *"$homebrewHome"* ]]; then

        # Store file name
        libraryOrigFile=$(basename "$1")

        echo "Resetting library ID of <$1>"

        # Set correct library id
        install_name_tool -id "@executable_path/../Resources/lib/$libraryOrigFile" "$1"
    fi
}

# Search and install any translation files
function install_translations {

    # Find relevant translation files
    translationFiles=$(find "$homebrewHome"/share/locale -name "$1".mo)

    for srcTranslFile in $translationFiles; do

        # Define target filename
        targetTranslFile=${srcTranslFile//"$homebrewHome"/"$dtResourcesDir"}

        # Create directory if not yet existing
        targetTranslDir=$(dirname "$targetTranslFile")
        if [[ ! -d "$targetTranslDir" ]]; then
            mkdir -p "$targetTranslDir"
        fi
        # Copy translation file
        cp -L "$srcTranslFile" "$targetTranslFile"
    done
}

# Install share directory
function install_share {

    # Define source and target directory
    srcShareDir="$homebrewHome/share/$1"
    targetShareDir="$dtResourcesDir/share/"

    # Copy share directory
    cp -RL "$srcShareDir" "$targetShareDir"
}

# Check for previous attempt and clean
if [[ -d "$dtPackageDir" ]]; then
    echo "Deleting directory $dtPackageDir ... "
    chown -R "$USER" "$dtPackageDir"
    rm -Rf "$dtPackageDir"
fi

# Create basic structure
mkdir -p "$dtExecDir"
mkdir -p "$dtResourcesDir"/share/applications
mkdir -p "$dtResourcesDir"/etc/gtk-3.0

# Add basic elements
cp Info.plist "$dtWorkingDir"/Contents/
echo "APPL$dtAppName" >>"$dtWorkingDir"/Contents/PkgInfo
cp Icons.icns "$dtResourcesDir"/

# Set version information
sed -i '' 's|{VERSION}|'$(git describe --tags --long --match '*[0-9.][0-9.][0-9]' | cut -d- -f2 | sed 's/^\([0-9]*\.[0-9]*\)$/\1.0/')'|' "$dtWorkingDir"/Contents/Info.plist
sed -i '' 's|{COMMITS}|'$(git describe --tags --long --match '*[0-9.][0-9.][0-9]' | cut -d- -f3)'|' "$dtWorkingDir"/Contents/Info.plist

# Generate settings.ini
echo "[Settings]
gtk-icon-theme-name = Adwaita
" >"$dtResourcesDir"/etc/gtk-3.0/settings.ini

# Add darktable executables
cp bin/darktable{,-chart,-cli,-cltest,-generate-cache,-rs-identify} "$dtExecDir"/

# Add darktable tools if existent
if [[ -d libexec/darktable/tools ]]; then
    cp libexec/darktable/tools/* "$dtExecDir"/
fi

# Add darktable directories
cp -R {lib,share} "$dtResourcesDir"/

# Install homebrew dependencies of darktable executables
for dtExecutable in $dtExecutables; do
    if [[ -f "$dtExecutable" ]]; then
        install_dependencies "$dtExecutable"
    fi
done

# Add homebrew shared objects
dtSharedObjDirs="gtk-3.0 libgphoto2 libgphoto2_port gdk-pixbuf-2.0 gio"
for dtSharedObj in $dtSharedObjDirs; do
    cp -LR "$homebrewHome"/lib/"$dtSharedObj" "$dtResourcesDir"/lib/
done

# Add homebrew translations
dtTranslations="gtk30 gtk30-properties gtk-mac-integration iso_639-2 gphoto2"
for dtTranslation in $dtTranslations; do
    install_translations "$dtTranslation"
done

# Add homebrew share directories
dtShares="lensfun icons iso-codes mime"
for dtShare in $dtShares; do
    install_share "$dtShare"
done

# Update icon caches
gtk3-update-icon-cache -f "$dtResourcesDir"/share/icons/Adwaita
gtk3-update-icon-cache -f "$dtResourcesDir"/share/icons/hicolor

# Try updating lensfun
lensfun-update-data || true
lfLatestData="$HOME"/.local/share/lensfun/updates/version_1
if [[ -d "$lfLatestData" ]]; then
    echo "Adding latest lensfun data from $lfLatestData."
    cp -R "$lfLatestData" "$dtResourcesDir"/share/lensfun/
fi

# Add glib gtk settings schemas
glibSchemasDir="$dtResourcesDir"/share/glib-2.0/schemas
if [[ ! -d "$glibSchemasDir" ]]; then
    mkdir -p "$glibSchemasDir"
fi
cp -L "$homebrewHome"/share/glib-2.0/schemas/org.gtk.Settings.*.gschema.xml "$glibSchemasDir"/
# Compile glib schemas
glib-compile-schemas "$dtResourcesDir"/share/glib-2.0/schemas/

# Define gtk-query-immodules-3.0
immodulesCacheFile="$dtResourcesDir"/lib/gtk-3.0/3.0.0/immodules.cache
gtkVersion=$(pkg-config --modversion gtk+-3.0)
sed -i '' "s#$homebrewHome/Cellar/gtk+3/$gtkVersion/lib/gtk-3.0/3.0.0/immodules#@executable_path/../Resources/lib/gtk-3.0/3.0.0/immodules#g" "$immodulesCacheFile"
sed -i '' "s#$homebrewHome/Cellar/gtk+3/$gtkVersion/share/locale#@executable_path/../Resources/share/locale#g" "$immodulesCacheFile"
# Rename and move it to the right place
mv "$immodulesCacheFile" "$dtResourcesDir"/etc/gtk-3.0/gtk.immodules

# Define gdk-pixbuf-query-loaders
loadersCacheFile="$dtResourcesDir"/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache
sed -i '' "s#$homebrewHome/lib/gdk-pixbuf-2.0/2.10.0/loaders#@executable_path/../Resources/lib/gdk-pixbuf-2.0/2.10.0/loaders#g" "$loadersCacheFile"
# Move it to the right place
mv "$loadersCacheFile" "$dtResourcesDir"/etc/gtk-3.0/

# Install homebrew dependencies of lib subdirectories
dtLibFiles=$(find -E "$dtResourcesDir"/lib/*/* -regex '.*\.(so|dylib)')
for dtLibFile in $dtLibFiles; do
    install_dependencies "$dtLibFile"
done

# Reset executable paths to relative path
dtExecFiles="$dtExecutables"
dtExecFiles+=" "
dtExecFiles+=$(find -E "$dtResourcesDir"/lib -regex '.*\.(so|dylib)')
for dtExecFile in $dtExecFiles; do
    if [[ -f "$dtExecFile" ]]; then
        reset_exec_path "$dtExecFile"
    fi
done

# Add gtk files
cp defaults.list "$dtResourcesDir"/share/applications/
cp open.desktop "$dtResourcesDir"/share/applications/

# Sign app bundle
if [ -n "$CODECERT" ]; then
    # Use certificate if one has been provided
    find package/darktable.app/Contents/Resources/lib -type f -exec codesign --verbose --force --options runtime -i "org.darktable" -s "${CODECERT}" \{} \;
    codesign --deep --verbose --force --options runtime -i "org.darktable" -s "${CODECERT}" package/darktable.app
else
    # Use ad-hoc signing and preserve metadata
    find package/darktable.app/Contents/Resources/lib -type f -exec codesign --verbose --force --preserve-metadata=entitlements,requirements,flags,runtime -i "org.darktable" -s - \{} \;
    codesign --deep --verbose --force --preserve-metadata=entitlements,requirements,flags,runtime -i "org.darktable" -s - package/darktable.app
fi
