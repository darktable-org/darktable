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
buildDir="../../build/macosx"
dtPackageDir="$buildDir"/package
dtAppName="darktable"
dtWorkingDir="$dtPackageDir"/"$dtAppName".app
dtResourcesDir="$dtWorkingDir"/Contents/Resources
dtExecDir="$dtWorkingDir"/Contents/MacOS
dtExecutables=$(echo "$dtExecDir"/darktable{,-chart,-cli,-cltest,-generate-cache,-rs-identify,-curve-tool,-noiseprofile})
homebrewHome=$(brew --prefix)


# Install direct and transitive dependencies
function install_dependencies {
    local hbDependencies

    absolutePath=$(dirname $(grealpath "$1"))

    # Get dependencies of current executable
    oToolLDependencies=$(otool -L "$1" 2>/dev/null | grep compatibility | cut -d\( -f1 | sed 's/^[[:blank:]]*//;s/[[:blank:]]*$//' | uniq)

    # Handle library relative paths
    oToolLDependencies=$(echo "$oToolLDependencies" | sed "s#@loader_path#${absolutePath}#")
    oToolLDependencies=$(echo "$oToolLDependencies" | sed "s#@rpath#${absolutePath}#")

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

                # Install dependency if not yet existent
                if [[ ! -f "$dynDepTargetFile" ]]; then
                    echo "Installing dependency $hbDependency of $1"

                    # Copy dependency as not yet existent
                    cp -L "$hbDependency" "$dynDepTargetFile"

                    # Handle transitive dependencies
                    install_dependencies "$hbDependency"
                fi
            fi
        done

    fi
}

# Reset executable path to relative path
# Background: see e.g. http://clarkkromenaker.com/post/library-dynamic-loading-mac/
function reset_exec_path {
    local hbDependencies

    # Store file name
    libraryOrigFile=$(basename "$1")

    # Get shared libraries used of current executable
    oToolLDependencies=$(otool -L "$1" 2>/dev/null | grep compatibility | cut -d\( -f1 | sed 's/^[[:blank:]]*//;s/[[:blank:]]*$//' | uniq)

    # Handle libdarktable.dylib
    if [[ "$oToolLDependencies" == *"@rpath/libdarktable.dylib"* && "$1" != *"libdarktable.dylib"* ]]; then
        # Only need to reset binaries that live outside of lib/darktable
        oToolLoader=$(otool -l "$1" 2>/dev/null | grep '@loader_path' | cut -d\( -f1 | sed 's/^[[:blank:]]*path[[:blank:]]*//;s/[[:blank:]]*$//' )
        if [[ "$oToolLoader" == "@loader_path/../lib/darktable" ]]; then
            echo "Resetting loader path for libdarktable.dylib of $libraryOrigFile"
            install_name_tool -rpath @loader_path/../lib/darktable @loader_path/../Resources/lib/darktable "$1" || true
        fi
    fi

    # Handle library relative paths
    oToolLDependencies=$(echo "$oToolLDependencies" | sed "s#@loader_path/[../]*opt/#${homebrewHome}/opt/#")

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

            echo "Resetting executable path for $dynDepOrigFile of $libraryOrigFile"

            # Set correct executable path
            install_name_tool -change "$hbDependency" "@executable_path/../Resources/lib/$dynDepOrigFile" "$1"  || true

            # Check for loader path
            oToolLoader=$(otool -L "$1" 2>/dev/null | grep '@loader_path' | grep $dynDepOrigFile | cut -d\( -f1 | sed 's/^[[:blank:]]*//;s/[[:blank:]]*$//' ) || true
            if [[ -n "$oToolLoader" ]]; then
                echo "Resetting loader path for $hbDependency of $libraryOrigFile"
                oToolLoaderNew=$(echo $oToolLoader | sed "s#@loader_path/##" | sed "s#../../../../opt/.*##")
                install_name_tool -change "$oToolLoader" "@loader_path/${oToolLoaderNew}${dynDepOrigFile}" "$1"  || true
            fi
        done

    fi

    # Get shared library id name of current executable
    oToolDDependencies=$(otool -D "$1" 2>/dev/null | sort | uniq)

    # Set correct ID to new destination if required
    if [[ "$oToolDDependencies" == *"$homebrewHome"* ]]; then
        echo "Resetting library ID of $libraryOrigFile"

        # Set correct library id
        install_name_tool -id "@executable_path/../Resources/lib/$libraryOrigFile" "$1"  || true
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
if [[ -d "$dtWorkingDir" ]]; then
    echo "Deleting directory $dtWorkingDir ... "
    chown -R "$USER" "$dtWorkingDir"
    rm -Rf "$dtWorkingDir"
fi

# Create basic structure
mkdir -p "$dtExecDir"
mkdir -p "$dtResourcesDir"/share/applications
mkdir -p "$dtResourcesDir"/etc/gtk-3.0

# exiv2 expects the localization files in '../share/locale'
ln -s "Resources/share" "$dtWorkingDir"/Contents/share

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
cp "$buildDir"/bin/darktable{,-chart,-cli,-cltest,-generate-cache,-rs-identify} "$dtExecDir"/

# Add darktable tools if existent
if [[ -d "$buildDir"/libexec/darktable/tools ]]; then
    cp "$buildDir"/libexec/darktable/tools/* "$dtExecDir"/
fi

# Add darktable directories
cp -R "$buildDir"/{lib,share} "$dtResourcesDir"/

# Install homebrew dependencies of darktable executables
for dtExecutable in $dtExecutables; do
    if [[ -f "$dtExecutable" ]]; then
        install_dependencies "$dtExecutable"
    fi
done

# Add homebrew shared objects
dtSharedObjDirs="ImageMagick gtk-3.0 gdk-pixbuf-2.0 gio"
for dtSharedObj in $dtSharedObjDirs; do
    mkdir "$dtResourcesDir"/lib/"$dtSharedObj"
    cp -LR "$homebrewHome"/lib/"$dtSharedObj"/* "$dtResourcesDir"/lib/"$dtSharedObj"/
done

dtSharedObjDirs="libgphoto2 libgphoto2_port"
for dtSharedObj in $dtSharedObjDirs; do
    mkdir "$dtResourcesDir"/lib/"$dtSharedObj"
    dtSharedObjVersion=$(pkg-config --modversion "$dtSharedObj")
    cp -LR "$homebrewHome"/lib/"$dtSharedObj"/"$dtSharedObjVersion"/* "$dtResourcesDir"/lib/"$dtSharedObj"
done

# Add homebrew translations
dtTranslations="gtk30 gtk30-properties gtk-mac-integration iso_639-2 gphoto2 exiv2"
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

# ImageMagick config files
cp -R $homebrewHome/Cellar/imagemagick/*/etc $dtResourcesDir

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

# Add gtk Mac theme (to enable default macos keyboard shortcuts)
if [[ ! -d "$dtResourcesDir"/share/themes/Mac/gtk-3.0 ]]; then
    mkdir -p "$dtResourcesDir"/share/themes/Mac/gtk-3.0
fi
cp -L "$homebrewHome"/share/themes/Mac/gtk-3.0/gtk-keys.css "$dtResourcesDir"/share/themes/Mac/gtk-3.0/

# Sign app bundle
if [ -n "$CODECERT" ]; then
    # Use certificate if one has been provided
    find ${dtWorkingDir}/Contents/Resources/lib -type f -exec codesign --verbose --force --options runtime -i "org.darktable" -s "${CODECERT}" \{} \;
    codesign --deep --verbose --force --options runtime -i "org.darktable" -s "${CODECERT}" ${dtWorkingDir}
else
    # Use ad-hoc signing and preserve metadata
    find ${dtWorkingDir}/Contents/Resources/lib -type f -exec codesign --verbose --force --preserve-metadata=entitlements,requirements,flags,runtime -i "org.darktable" -s - \{} \;
    codesign --deep --verbose --force --preserve-metadata=entitlements,requirements,flags,runtime -i "org.darktable" -s - ${dtWorkingDir}
fi
