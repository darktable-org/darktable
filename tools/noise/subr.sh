# --------------------------------------------------------------------
# General purpose functions.
# --------------------------------------------------------------------

color_ok=$'\033[32m'
color_warning=$'\033[33m'
color_error=$'\033[31m'
color_reset=$'\033[0m'

# The functions below can be used to set/get a variable with a variable
# name. This can be useful to have something similar to a hash map.
#
# Example:
#   set_var "hash_$key" "$value"
#   value=$(get_var "hash_$key")

set_var() {
	eval "var_$1=\"$2\""
}

get_var() {
	eval echo \${var_$1}
}

add_to_list() {
	local list item
	list=$1
	item=$2


	case "$list" in
	$item|$item\ *|*\ $item\ *|*\ $item)
		;;
	*)
		if [ -z "$list" ]; then
			list=$item
		else
			list="$list $item"
		fi
		;;
	esac

	echo "$list"
}

# Handle various flavors of sed(1). This function is called at the end
# of this file.

set_sed_cmd() {
	case "$(uname -s)" in
	Linux)
		sed="sed -r"
		;;
	*)
		# For non-Linux systems, try with gsed(1) (common name
		# for GNU sed), otherwise, try "sed -E" which may not
		# exist everywhere.
		if which gsed >/dev/null 2>&1; then
			sed="gsed -r"
		else
			sed="sed -E"
		fi
		;;
	esac
}

# Helper function to check if a given command is available.
#
# Below this function, higher-level helpers which check for sets of
# commands. They are responsible for displaying a message if a tool is
# missing, so the user can understand what's going on.

tool_installed() {
	local tool message checked var
	tool=$1
	message=$2

	# If we already checked for this tool, return. This way, we
	# don't display the message again.
	var="tool_$(echo "$tool" | $sed 's/[^a-zA-Z0-9_]+/_/g')"
	checked=$(get_var $var)
	if [ "$checked" = "found" ]; then
		return 0
	fi
	if [ "$checked" = "not found" ]; then
		return 1
	fi

	if ! which $tool >/dev/null 2>&1; then
		if [ "$message" ]; then
			echo "${color_error}ERROR: $tool not found${color_reset}" 1>&2
			printf "%s\n\n" "$message" 1>&2
		fi

		set_var $var "not found"
		return 1
	fi

	set_var $var "found"
	return 0
}

image_info_tools_installed() {
	local missing_tool
	missing_tool=0

	echo "--> Check for images handling tools availability"

	if ! tool_installed exiv2 "
exiv2 is required to read Exif from images. Please install this package
and re-run this script."; then
		missing_tool=1
	fi

	return $missing_tool
}

image_export_tools_installed() {
	local missing_tool
	missing_tool=0

	echo "--> Check for images export tools availability"

	if ! tool_installed darktable-cli "
darktable-cli (shipped with darktable 1.1 and later) is required to
export RAW images to jpeg and PFM files. Please install this package and
re-run this script."; then
		missing_tool=1
	fi

	if ! tool_installed convert "
ImageMagick or GrahpicsMagick is required to check whether the input images
are suitable for noise calibration.
Please install either packages and re-run this script."; then
		missing_tool=1
	fi

	return $missing_tool
}

tethering_tools_installed() {
	local missing_tool
	missing_tool=0

	echo "--> Check for tethering tools availability"

	if ! tool_installed gphoto2 "
gphoto2 is needed if you want this script to automatically take the
required pictures."; then
		missing_tool=1
	fi

	if ! tool_installed awk "
awk is needed to parse gphot2(1) output."; then
		missing_tool=1
	fi

	return $missing_tool
}

pdf_tools_installed() {
	local missing_tool
	missing_tool=1

	echo "--> Check for pdf tools availability"

	if tool_installed pdftk; then
		pdfcat() {
			local output inputs
			output=$1; shift
			inputs=$@
			pdftk $inputs cat output $output
		}
		missing_tool=0
	elif tool_installed gs; then
		pdfcat() {
			local output inputs
			output=$1; shift
			inputs=$@
			gs -dBATCH -dNOPAUSE -q -sDEVICE=pdfwrite -sOutputFile=$output $inputs
		}
		missing_tool=0
	else
		echo "pdftk or ghostscript are needed if you want one single result pdf."
	fi

	return $missing_tool
}

profiling_tools_installed() {
	local missing_tool
	missing_tool=0

	echo "--> Check for profiling tools availability"

	if ! tool_installed darktable-cli "
darktable-cli (shipped with darktable 1.1 and later) is required to
export RAW images to jpeg and PFM files. Please install this package and
re-run this script."; then
		missing_tool=1
	fi

	if ! tool_installed gnuplot "
gnuplot is required to generate the graphs used to estimate the quality
of the presets. Please install this command and re-run this script."; then
		missing_tool=1
	fi

	return $missing_tool
}

get_darktable_version() {
	local version

	version=$(darktable --version | head -n 1 | cut -d' ' -f 4)

	echo "$version"
}

normalize_darktable_version() {
	local version
	version=$1

	version=${version%+*}
	version=${version%~*}

	case "$version" in
	*.*.*) ;;
	*)     version="$version.0" ;;
	esac

	IFS='.'
	for i in $version; do
		normalized="${normalized}$(printf "%03d" $i)"
	done

	echo "$normalized"
}
cmp_darktable_version() {
	local v1 v2 cmp
	v1=$1
	cmp=$2
	v2=$3

	v1=$(normalize_darktable_version "$v1")
	v2=$(normalize_darktable_version "$v2")

	test "$v1" "$cmp" "$v2"
}

# --------------------------------------------------------------------
# Input image file handling.
# --------------------------------------------------------------------

get_exif_key() {
	local file key
	file=$1
	key=$2

	exiv2 -K "$key" -Pv -b "$file" 2>/dev/null | sed 's/ *$//g' || :
}

get_image_iso() {
	local file iso
	file=$1

	tool_installed exiv2

	iso=$(get_exif_key "$file" Exif.Photo.ISOSpeedRatings | grep -o '[[:digit:]]*')

	if [ -z "$iso" -o "$iso" = "65535" ]; then
		iso=$(get_exif_key "$file" Exif.Photo.RecommendedExposureIndex)
	fi
	if [ -z "$iso" -o "$iso" = "65535" ]; then
		iso=$(get_exif_key "$file" Exif.Photo.StandardOutputSensitivity)
	fi
	if [ -z "$iso" -o "$iso" = "65535" ]; then
		iso=$(get_exif_key "$file" Exif.Image.ISOSpeedRatings)
	fi

	# Then try some brand specific values if still not found.

	# TODO: Try to use exiv2's "fixiso" option for reading only,
	# possibly talk with exiv2 developers to get an option that only
	# displays the correct iso

	if [ -z "$iso" -o "$iso" = "0" ]; then
		case "$(get_image_camera_maker "$1")" in
		[Nn][Ii][Kk][Oo][Nn]*)
			# Read "Exif.Nikon3.*" before "Exif.NikonIi.*":
			#     1. "Exif.NikonIi.*" are bytes, not even
			#        shorts, so they're smaller than other
			#        keys.
			#     2. That looks like versioned nodes:
			#        "Nikon2" vs. "Nikon3".
			iso=$(get_exif_key "$file" Exif.Nikon3.ISOSpeed)
			if [ -z "$iso" -o "$iso" = "0" ]; then
				iso=$(get_exif_key "$file" Exif.Nikon3.ISOSettings)
			fi
			if [ -z "$iso" -o "$iso" = "0" ]; then
				iso=$(get_exif_key "$file" Exif.NikonIi.ISO)
				# read hi/low iso setting
				ciso=$(echo $iso | cut -d' ' -f2)
				if [ "$ciso" = "Hi" -o "$ciso" = "Lo" ]; then
					iso=$(echo $iso  | cut -d' '  -f1 )
				fi
			fi
			;;
    [Cc][Aa][Nn][Oo][Nn]*)
			if [ -z "$iso" -o "$iso" = "0" ]; then
				iso=$(get_exif_key "$file" Exif.CanonSi.ISOSpeed)
			fi
      ;;
		esac
	fi

	echo $iso
}

get_image_camera_maker() {
	local file first_model maker
	file=$1

	tool_installed exiv2

	first_model=$(echo $(get_exif_key "$file" Exif.Image.Model) | cut -d " " -f 1)
	if [ "$first_model" = "PENTAX" ] || [ "$first_model" = "RICOH" ]; then
		maker=$first_model
	else
		maker=$(get_exif_key "$file" Exif.Image.Make)
	fi
	if [ "$maker" != "DJI" ] && [ "$maker" != "LGE" ]; then
		# ensure name is capitalized
		maker=$(echo $maker | cut -c 1 | tr "[a-z]" "[A-Z]")$(echo $maker | cut -c 2- | cut -d " " -f 1 | tr "[A-Z]" "[a-z]")
	fi
	echo $maker
}

get_image_camera_model() {
	local file first_maker model first_model
	file=$1

	tool_installed exiv2

	first_maker=$(echo $(get_exif_key "$file" Exif.Image.Make) | cut -d " " -f 1)
	model=$(get_exif_key "$file" Exif.Image.Model)
	first_model=$(echo $model | cut -d " " -f 1)
	if [ "$first_maker" = "$first_model" ] || [ "$first_model" = "PENTAX" ]; then
		model=$(echo $model | cut -d " " -f 2-)
	fi
	echo $model
}

sort_iso_list() {
	local iso_list

	iso_list="$@"
	echo $(for iso in $iso_list; do echo $iso; done | sort -n)
}

# CAUTION: This function uses the following global variables:
#     o  profiling_dir

auto_set_profiling_dir() {
	local flag camera subdir
	flag=$1

	echo
	echo "===> Check profiling directory"

	if [ "$profiling_dir" ]; then
		if [ -d "$profiling_dir" ]; then
			profiling_dir=${profiling_dir%/}
			return 0
		else
			cat <<EOF
${color_error}ERROR: Profiling directory doesn't exist:
$profiling_dir${color_reset}
EOF
			return 1
		fi
	fi

	if ! camera_is_plugged; then
		cat <<EOF
${color_error}ERROR: Please specify a directory to read or write profiling RAW images
(using the "$flag" flag) or plug your camera and turn it on.${color_reset}
EOF
		return 1
	fi

	camera=$(get_camera_name)
	subdir=$(echo $camera | $sed 's/[^a-zA-Z0-9_]+/-/g')
	profiling_dir="/var/tmp/darktable-noise-profiling/$subdir/profiling"
	test -d "$profiling_dir" || mkdir -p "$profiling_dir"
}

# CAUTION: This function uses the following global variables:
#     o  profiling_dir
#     o  images_$iso
#     o  images_for_iso_settings

list_input_images() {
	local iso image images

	echo
	echo "===> List profiling input RAW images"
	for image in "$profiling_dir"/*; do
		if [ "$image" = "$profiling_dir/*" ]; then
			# Directory empty.
			break
		fi

		case "$image" in
		*.[Jj][Pp][Gg]|*.[Jj][Pp][Ee][Gg])
			# Skip jpeg files, if any. Other files don't
			# have Exif and will be skipped automatically.
			continue
			;;
		esac

		iso=$(get_image_iso "$image")
		if [ -z "$iso" ]; then
			# Not an image.
			continue
		fi

		echo "--> Found ISO $iso image: $image"

		# Record filename for this ISO setting.
		images=$(get_var "images_$iso")
		if [ -z "$image" ]; then
			images=$image
		else
			images="$images $image"
		fi
		set_var "images_$iso" "$images"

		# Add ISO setting to a list.
		images_for_iso_settings=$(add_to_list "$images_for_iso_settings" $iso)
	done

	images_for_iso_settings=$(sort_iso_list "$images_for_iso_settings")
}

export_large_jpeg() {
	local input output xmp
	input=$1
	output=$2
	xmp="$input.xmp"
	xmp_profiling="$scriptdir/profiling-shot.xmp"

	tool_installed darktable-cli

	rm -f "$output" "$xmp"
	darktable-cli "$input" "$xmp_profiling" "$output" --apply-custom-presets false --core --conf plugins/lighttable/export/iccprofile=image --conf plugins/lighttable/export/style=none
	rm -f "$xmp"
}

export_thumbnail() {
	local input output xmp
	input=$1
	output=$2

	tool_installed convert

	convert "$input" -resize 1024x1024 "$output"
}

check_exposure() {
	local orig input inputdir over under ret convert_flags
	orig=$1
	input=$2
	inputdir=$(dirname $input)

	ret=0

	# This is a discussion about how to check which percentile of pixels falls within 
	# a certain luminosity histogram range with GrahpicsImage:
	# https://sourceforge.net/p/graphicsmagick/discussion/250738/thread/f64160afbd
	pixel_percentile=80 # range: [0; 65536] in Image/GraphicsMagick
	convert_flags_im="-process analyze= -format %[mean] info:-"
	convert_flags_gm="-process analyze= -format %[BrightnessMean] info:-"

	if convert -version | grep ImageMagick &>/dev/null; then
		over=$(convert -threshold 99% "$input" $convert_flags_im | awk '{ print int($1) }')
		under=$(convert -negate -threshold 99% "$input" $convert_flags_im | awk '{ print int($1) }')
	else
		over=$(convert -threshold 99% "$input" $convert_flags_gm | awk '{ print int($1) }')
		under=$(convert -negate -threshold 99% "$input" $convert_flags_gm | awk '{ print int($1) }')
	fi

	if [ "$over" ] && [ "$over" -lt $pixel_percentile ]; then
		# Image does not contain sufficient over-exposed pixels.
		echo "${color_error}\"$orig\" not sufficiently over-exposed ($over / $pixel_percentile) ${color_reset}"
		ret=1
	fi

	if [ "$under" ] && [ "$under" -lt $pixel_percentile ]; then
		# Image does not contain sufficient under-exposed pixels.
		echo "${color_error}\"$orig\" not sufficiently under-exposed ($under / $pixel_percentile) ${color_reset}"
		ret=1
	fi

	return $ret
}

# --------------------------------------------------------------------
# Camera tethering.
# --------------------------------------------------------------------

camera_is_plugged() {
	tool_installed gphoto2 && gphoto2 -a >/dev/null 2>&1
}

get_camera_name() {
	local camera
	if camera_is_plugged; then
		camera=$(gphoto2 -a | head -n 1 | $sed 's/^[^:]+: //')
		echo $camera
	fi
}

get_camera_raw_setting() {
	local key raw_setting

	# Try know configuration keys one after another, because cameras
	# don't support the same keys.

	# This one seems supported by most cameras.
	key="/main/imgsettings/imageformat"
	raw_setting=$(gphoto2 --get-config "$key" | awk "
/^Choice: [0-9]+ RAW$/ {
	id = \$0;
	sub(/^Choice: /, \"\", id);
	sub(/ RAW$/, \"\", id);
	print \"$key=\" id;
	exit;
}
")
	if [ "$raw_setting" ]; then
		echo "$raw_setting"
	fi

	# This one is used by Nikon cameras (at least, some).
	key="/main/capturesettings/imagequality"
	raw_setting=$(gphoto2 --get-config "$key" | awk "
/^Choice: [0-9]+ NEF \(Raw\)$/ {
	id = \$0;
	sub(/^Choice: /, \"\", id);
	sub(/ NEF \(Raw\)$/, \"\", id);
	print \"$key=\" id;
	exit;
}
")
	if [ "$raw_setting" ]; then
		echo "$raw_setting"
	fi
}

get_camera_iso_settings() {
	local iso_settings
	iso_settings=$(gphoto2 --get-config /main/imgsettings/iso | awk '
/^Choice: [0-9]+ [0-9]+$/ {
	iso = $0;
	sub(/^Choice: [0-9]+ /, "", iso);
	print iso;
}
')

	echo $iso_settings
}

# CAUTION: This function uses the following global variables:
#     o  profiling_dir
#     o  force_profiling_shots
#     o  pause_between_shots
#     o  iso_settings
#     o  images_$iso
#     o  images_for_iso_settings

auto_capture_images() {
	local do_profiling_shots iso image images			\
	 profiling_note_displayed profiling_note answer camera		\
	 shots_per_iso shots_seq files raw_id i not_first_round

	tool_installed exiv2

	do_profiling_shots=0
	if [ -z "$images_for_iso_settings" -o "$force_profiling_shots" = "1" ]; then
		do_profiling_shots=1
	fi
	if [ "$iso_settings" ]; then
		for iso in $iso_settings; do
			images=$(get_var "images_$iso")
			if [ -z "$images" ]; then
				do_profiling_shots=1
			fi
		done
	else
		iso_settings=$images_for_iso_settings
	fi

	if [ "$do_profiling_shots" = "0" ]; then
		cat <<EOF

The script will use existing input RAW images for the profiling. No more
shot will be taken.
EOF
		return 0
	fi

	# Check for the camera presence, and if no camera is found, ask
	# the user to plug in his camera or point to an appropriate
	# directory.

	profiling_note_displayed=0
	profiling_note="Important note about the required images:

    o  The subject must contain both under-exposed AND over-exposed
       areas. A possible subject could be a sunny window (or an in-door
       light) on half of the picture and a dark/shadowed in-door object on
       the other half.

    o  Disable auto-focus and put everything out of focus."

	if ! camera_is_plugged; then
		profiling_note_displayed=1
		cat <<EOF

Noise profiling requires at least a RAW image per required or supported
ISO setting.

Either:

    o  Plug your camera to this computer and, when detected, hit Return.
       This script will query the camera for supported ISO settings and
       take the appropriate images.

    o  Type Ctrl+C, take at least one image per supported ISO setting and
       put them in a dedicated directory. Then, re-run this script and be
       sure to indicate this directory by using the "-d" flag.

$profiling_note
EOF
		read answer
	fi
	while ! camera_is_plugged; do
		cat <<EOF
${color_error}ERROR: No camera found by gphoto2(1)!

Retry or check gphoto2 documentation.${color_reset}
EOF
		read answer
	done

	# If we reach this part, a camera is plugged in and the user
	# wants us to take the pictures for him.

	if [ -z "$raw_config" ]; then
		raw_config=$(get_camera_raw_setting)
	fi

	# If he didn't specify any ISO settings, query the camera.
	if [ -z "$iso_settings" ]; then
		iso_settings=$(get_camera_iso_settings)
	fi
	iso_settings=$(sort_iso_list $iso_settings)

	camera=$(get_camera_name)

	# We are now ready to take pictures for each supported/wanted
	# ISO settings.

	# TODO: For now, we take one image per ISO setting. When
	# benchmark is automated, this value can be raised to 3 for
	# instance, and the benchmark script will choose the best
	# preset.
	shots_per_iso=1
	shots_seq="$shots_per_iso"

	# gphoto2(1) writes images to the current directory, so cd to
	# the profiling directory.
	cd "$profiling_dir"
	for iso in $iso_settings; do
		if [ "$force_profiling_shots" = 1 ]; then
			# Remove existing shots for this ISO setting.
			echo "--> (remove ISO $iso existing shots)"
			files=$(get_var "images_$iso")
			for file in $files; do
				rm -v $file
			done
			set_var "images_$iso" ""
		fi

		images=$(get_var "images_$iso")
		if [ "$images" ]; then
			# We already have images for this ISO setting,
			# continue with the next one.
			continue
		fi

		echo
		echo "===> Taking $shots_per_iso profiling shot(s) for \"$camera - ISO $iso\""
		if [ "$profiling_note_displayed" = "0" ]; then
			profiling_note_displayed=1
			cat <<EOF

$profiling_note

Press Enter when ready.
EOF
			read answer
		fi

		# This script will do $shots_seq shots for each ISO setting.
		for i in $shots_seq; do
			if [ "$pause_between_shots" -a "$not_first_round" ]; then
				echo "(waiting $pause_between_shots seconds before shooting)"
				sleep "$pause_between_shots"
			fi
			not_first_round=1

			gphoto2						\
			 --set-config "$raw_config"			\
			 --set-config /main/imgsettings/iso=$iso	\
			 --filename="$iso-$i.%C"			\
			 --capture-image-and-download

			image=$(ls -t "$profiling_dir/$iso-$i".* | head -n 1)

			images=$(get_var "images_$iso")
			if [ -z "$image" ]; then
				images=$image
			else
				images="$images $image"
			fi
			set_var "images_$iso" "$images"
		done
	done
	cd -
}

set_sed_cmd
