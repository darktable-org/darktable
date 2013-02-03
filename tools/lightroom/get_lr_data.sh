#! /bin/sh

if [ "$1" = "" ]; then
    echo usage: $0 lr_catalogue_name
    exit 1
fi

DT=$HOME/.config/darktable

cat<<EOF | sqlite3 "$1" > /tmp/extract
SELECT rating, pick, colorLabels, pathFromRoot||originalFilename, gpsLatitude, gpsLongitude, hasGPS FROM Adobe_images, AgLibraryFile, AgLibraryFolder, AgHarvestedExifMetadata WHERE AgLibraryFile.id_local=rootFile AND AgLibraryFile.folder=AgLibraryFolder.id_local AND AgHarvestedExifMetadata.image=Adobe_images.id_local;
EOF

rm -f $DT/lr.db

cat<<EOF | sqlite3 $DT/lr.db
CREATE TABLE data (
   rating  INTEGER,
   pick    INTEGER,
   colorLabels VARCHAR,
   filename VARCHAR,
   gpsLatitude, gpsLongitude, hasGPS);

.import /tmp/extract data
EOF
