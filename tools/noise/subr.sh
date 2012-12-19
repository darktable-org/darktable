# --------------------------------------------------------------------
# Internal functions.
# --------------------------------------------------------------------

camera_is_plugged() {
	gphoto2 -a >/dev/null 2>&1
}

get_camera_name() {
	if camera_is_plugged; then
		camera=$(gphoto2 -a | head -n 1 | sed -r s'/^[^:]+: //')
		echo $camera
	fi
}

get_camera_raw_setting() {
	raw_setting=$(gphoto2 --get-config /main/imgsettings/imageformat | awk '
/^Choice: [0-9]+ RAW$/ {
	id = $0;
	sub(/^Choice: /, "", id);
	sub(/ RAW$/, "", id);
	print id;
	exit;
}
')

	echo $raw_setting
}

get_camera_iso_settings() {
	iso_settings=$(gphoto2 --get-config /main/imgsettings/iso | awk '
/^Choice: [0-9]+ [0-9]+$/ {
	iso = $0;
	sub(/^Choice: [0-9]+ /, "", iso);
	print iso;
}
')

	echo $iso_settings
}

get_image_iso() {
	iso=$(exiv2 -g Exif.Photo.ISOSpeedRatings -Pt "$1" 2>/dev/null || :)

	if [ -z "$iso" -o "$iso" = "65535" ]; then
		iso=$(exiv2 -g Exif.Photo.RecommendedExposureIndex -Pt "$1" 2>/dev/null || :)
	fi

        # Then try some brand specific values if still not found.

        if [ "$iso" = "" ]; then
            case "$(get_image_camera_maker "$1")" in
                NIKON*)
                    iso=$(exiv2 -g Exif.NikonIi.ISO -Pt "$1" 2>/dev/null || :)
                    ;;
            esac
        fi

	echo $iso
}

get_image_camera_maker() {
	maker=$(exiv2 -g Exif.Image.Make -Pt "$1" 2>/dev/null || :)
	echo $maker
}

get_image_camera_model() {
	model=$(exiv2 -g Exif.Image.Model -Pt "$1" 2>/dev/null || :)
	echo $model
}

set_var() {
	eval "var_$1=\"$2\""
}

get_var() {
	eval echo \${var_$1}
}
