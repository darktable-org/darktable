#! /bin/sh

DT=$HOME/.config/darktable

if [ ! -f $DT/lr.db ]; then
    echo please, create lr.db using get_lr_data.sh first
    exit 1
fi

# select only images with flags=1 (this is the default value when imported)

cat<<EOF | sqlite3 $DT/library.db > /tmp/data
select images.id, folder || '/' || filename from images, film_rolls where flags&7=1 and film_rolls.id=images.film_id;
CREATE UNIQUE INDEX IF NOT EXISTS color_labels_idx ON color_labels(imgid,color);
EOF

cat<<EOF | sqlite3 $DT/lr.db
DROP TABLE IF EXISTS files;

CREATE TABLE files (
   id INTEGER,
   dt_filename VARCHAR);

.import /tmp/data files
EOF

inject_data()
{
    echo "$1;" >> $DT/lrsync.log
    echo "$1;" | sqlite3 $DT/library.db
}

parse_data()
{
    id=$1
    rating=$2
    pick=$3
    color=$4
    gpslat=$5
    gpslon=$6
    hasgps=$7

    if [ $pick = 1 ]; then
        if [ $rating != "''" ]; then
            inject_data "UPDATE images SET flags=flags|$rating WHERE images.id=$id"
        fi

    elif [ $pick = -1 ]; then
        # rejected
        inject_data "UPDATE images SET flags=flags|6 WHERE images.id=$id"

    else
        # pick=0 (not selected), set to 0 star
        inject_data "UPDATE images SET flags=flags&~7 WHERE images.id=$id"
    fi

    case "$color" in
        Rouge|Red)
            inject_data "INSERT OR IGNORE INTO color_labels VALUES ($id,0)"
            ;;
        Jaune|Yellow)
            inject_data "INSERT OR IGNORE INTO color_labels VALUES ($id,1)"
            ;;
        Vert|Green)
            inject_data "INSERT OR IGNORE INTO color_labels VALUES ($id,2)"
            ;;
        Bleu|Blue)
            inject_data "INSERT OR IGNORE INTO color_labels VALUES ($id,3)"
            ;;
        \'\')
            ;;
        *)
            inject_data "INSERT OR IGNORE INTO color_labels VALUES ($id,4)"
            ;;
    esac

    if [ $hasgps != 0 ]; then
        inject_data "UPDATE images SET longitude=$gpslon, latitude=$gpslat WHERE images.id=$id"
    fi
}

cat<<EOF | sqlite3 -separator ' ' $DT/lr.db | while read id rating pick color gpslat gpslon hgps; do parse_data $id "$rating" "$pick" "$color" "$gpslat" "$gpslon" "$hgps"; done
SELECT id, QUOTE(rating), pick, QUOTE(colorLabels), QUOTE(gpsLatitude), QUOTE(gpsLongitude), hasGPS FROM data, files WHERE files.dt_filename like '%'||data.filename;
EOF
