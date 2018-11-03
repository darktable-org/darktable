-- This script takes a Capture One Pro catalogue (tested with version 11) 
-- and recreates the Albums in the catalog in Darktable as tags while 
-- preserving the album hierarchy. For example, a hierarchy like this:
-- - Level1
--   - Level2_1
--     - Level3
--   - Level2_2
-- Will result in the following tags:
-- - Level1
-- - Level1|Level2_1
-- - Level1|Level2_1|Level3
-- - Level1|Level2_2
-- If images are present in the Darktable catalogue, the relevant tags
-- will also be added. Images that are not in the catalogue will be 
-- ignored.
-- 
-- To use, you must either:
-- - Ensure that Darktable's data.db, library.db are present in the 
--   current directory and Capture One Pro's catalog is in the current 
--   directory and named "C1ProCat.cocatalogdb"
-- - Update the "ATTACH" statements below so that they reflect the 
--   correct name/path of the aforementioned files.
-- Then simply run sqlite3 migrate_capture_one_pro.sql.
-- It is strongly recommended that you back up your Darktable databases 
-- prior to attempting this.

ATTACH 'data.db' AS data;
ATTACH 'library.db' AS library;
ATTACH 'C1ProCat.cocatalogdb' AS c1cat;

-- Temporary table to store the mapping from Capture One Pro albums and Darktable tags to be looked up when bringing in images
CREATE TEMPORARY TABLE c1collections (c1_pk INTEGER PRIMARY KEY, dt_pk INTEGER, path VARCHAR, level INTEGER);
-- This query will produce rows of the form id, Darktable tag, level/depth in tree
WITH RECURSIVE
    under_collection(parent, path, level) AS (
        VALUES(1, 'Capture One Pro Import', 0)
        UNION ALL
        SELECT c.Z_PK, under_collection.path || '|' || c.ZNAME, level + 1
        FROM c1cat.ZCOLLECTION AS c JOIN under_collection ON c.ZPARENT=under_collection.parent
        ORDER BY 2
    )
INSERT INTO c1collections (c1_pk, path, level) SELECT parent, path, level FROM under_collection;
-- Create any new Darktable tags as necessary
INSERT INTO data.tags (name) 
    SELECT path 
    FROM c1collections 
        LEFT JOIN data.tags ON data.tags.name=c1collections.path 
    WHERE data.tags.id IS NULL;
-- Look up and store the Darktable tags that correspond to the C1Pro albums
UPDATE c1collections SET dt_pk=(SELECT t.id FROM data.tags t WHERE t.name=path);
-- Sync the used_tags table with the tags table
INSERT INTO library.used_tags (id, name) 
    SELECT dt_pk, path 
    FROM c1collections 
        LEFT JOIN library.used_tags ON library.used_tags.name=c1collections.path 
    WHERE library.used_tags.id IS NULL;
-- Insert image/tag relations
INSERT INTO library.tagged_images (imgid, tagid) 
    SELECT 
        dti.id,
        c1c.dt_pk
    FROM 
        c1cat.ZIMAGE i 
            INNER JOIN c1cat.ZPATHLOCATION pl ON i.ZIMAGELOCATION=pl.Z_PK
            INNER JOIN c1cat.ZSTACKIMAGELINK sil ON sil.ZIMAGE=i.Z_PK
            INNER JOIN c1cat.ZSTACK s ON s.Z_PK=sil.ZSTACK
            INNER JOIN c1cat.ZCOLLECTION c ON c.Z_PK=s.ZCOLLECTION
            INNER JOIN c1collections c1c ON c1c.c1_pk=c.Z_PK
            INNER JOIN library.film_rolls fr ON fr.folder='<path to C1 root>' || replace(pl.ZRELATIVEPATH, '\', '/')
            INNER JOIN library.images dti ON dti.film_id=fr.id AND dti.filename=i.ZIMAGEFILENAME
    WHERE NOT EXISTS (SELECT 1 FROM library.tagged_images WHERE imgid=dti.id AND tagid=c1c.dt_pk);

