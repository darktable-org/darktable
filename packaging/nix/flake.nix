{
  description = "build darktable from source";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      utils,
      ...
    }:
    utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        packages = rec {
          darktable-git = pkgs.stdenv.mkDerivation rec {
            pname = "darktable";
            version = "git";

            #src = pkgs.lib.cleanSource ../..;
            src = ../..;

            nativeBuildInputs = with pkgs; [
              cmake
              desktop-file-utils
              intltool
              llvmPackages.llvm
              ninja
              perl
              pkg-config
              wrapGAppsHook3
              saxon # Use Saxon instead of libxslt to fix XSLT generate-id() consistency issues
              potrace
            ];

            buildInputs =
              with pkgs;
              [
                SDL2
                adwaita-icon-theme
                cairo
                curl
                exiv2
                glib
                glib-networking
                gmic
                graphicsmagick
                gtk3
                icu
                ilmbase
                isocodes
                jasper
                json-glib
                lcms2
                lensfun
                lerc
                libaom
                #libavif # TODO re-enable once cmake files are fixed (#425306)
                libdatrie
                libepoxy
                libexif
                libgcrypt
                libgpg-error
                libgphoto2
                libheif
                libjpeg
                libjxl
                libpng
                librsvg
                libsecret
                libsysprof-capture
                libthai
                libtiff
                libwebp
                libxml2
                lua
                openexr
                openjpeg
                osm-gps-map
                pcre2
                portmidi
                pugixml
                sqlite
              ]
              ++ pkgs.lib.optionals stdenv.hostPlatform.isLinux [
                alsa-lib
                colord
                colord-gtk
                libselinux
                libsepol
                xorg.libX11
                xorg.libXdmcp
                libxkbcommon
                xorg.libXtst
                ocl-icd
                util-linux
              ]
              ++ pkgs.lib.optional pkgs.stdenv.hostPlatform.isDarwin gtk-mac-integration
              ++ pkgs.lib.optional pkgs.stdenv.cc.isClang llvmPackages.openmp;

            cmakeFlags = [
              "-DBUILD_USERMANUAL=False"
            ]
            ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isDarwin [
              "-DUSE_COLORD=OFF"
              "-DUSE_KWALLET=OFF"
            ];

            # darktable changed its rpath handling in commit
            # 83c70b876af6484506901e6b381304ae0d073d3c and as a result the
            # binaries can't find libdarktable.so, so change LD_LIBRARY_PATH in
            # the wrappers:
            preFixup =
              let
                libPathEnvVar =
                  if pkgs.stdenv.hostPlatform.isDarwin then "DYLD_LIBRARY_PATH" else "LD_LIBRARY_PATH";
                libPathPrefix =
                  "$out/lib/darktable"
                  + pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isLinux ":${pkgs.ocl-icd}/lib";
              in
              ''
                for f in $out/share/darktable/kernels/*.cl; do
                  sed -r "s|#include \"(.*)\"|#include \"$out/share/darktable/kernels/\1\"|g" -i "$f"
                done

                gappsWrapperArgs+=(
                  --prefix ${libPathEnvVar} ":" "${libPathPrefix}"
                )
              '';

            postPatch = ''
              patchShebangs ./tools/generate_styles_string.sh
            '';

          }; # END

          default = darktable-git;
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.darktable-git ];
        };
      }
    );
}
