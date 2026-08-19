#pragma once
#include <QString>

// Minimal XMP read/write for the fields the mobile app can edit:
//   xmp:Rating          integer 0–5
//   darktable:colorlabels  first entry in rdf:Bag (0=red … 4=purple, -1=none)
//
// Full darktable history stack (module params, etc.) is passed through
// unmodified — the mobile app only patches these two fields.
namespace XmpIO
{
    // Read rating and colorLabel from an existing .xmp file.
    // Returns false if the file cannot be read.
    bool readFields(const QString &xmpPath, int &rating, int &colorLabel);

    // Load the full XMP text from file.
    QString load(const QString &xmpPath);

    // Return a copy of xmpText with xmp:Rating set to rating (0–5).
    // If the element doesn't exist it is inserted.
    QString setRating(const QString &xmpText, int rating);

    // Return a copy of xmpText with darktable:colorlabels replaced.
    // label == -1 → element is cleared / set to empty Bag.
    QString setColorLabel(const QString &xmpText, int label);

    // Write modified XMP back to file (atomic via temp file + rename).
    bool save(const QString &xmpPath, const QString &content);
}
