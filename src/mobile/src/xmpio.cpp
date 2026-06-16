#include "xmpio.h"
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QXmlStreamReader>

// ── read ──────────────────────────────────────────────────────────────────────

bool XmpIO::readFields(const QString &xmpPath, int &rating, int &colorLabel)
{
    QFile f(xmpPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    rating     = 0;
    colorLabel = -1;

    QXmlStreamReader xml(&f);
    bool inColorLabels = false;
    bool inBagLi      = false;
    bool firstLi      = true;

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == u"Rating")
                inColorLabels = false;
            if (name == u"colorlabels")
                inColorLabels = true;
            if (inColorLabels && name == u"li") {
                inBagLi = true;
                firstLi = (colorLabel == -1);
            }
        }

        if (xml.isCharacters() && !xml.isWhitespace()) {
            const QString text = xml.text().toString().trimmed();
            // xmp:Rating
            if (xml.tokenType() == QXmlStreamReader::Characters) {
                // Walk up via name of the parent element
                // (QXmlStreamReader doesn't track parent, so we key on context flags)
                if (inBagLi && firstLi) {
                    colorLabel = text.toInt();
                    firstLi    = false;
                }
            }
        }

        if (xml.isEndElement()) {
            if (xml.name() == u"colorlabels") inColorLabels = false;
            if (xml.name() == u"li")          inBagLi       = false;
        }
    }

    // Simpler fallback for xmp:Rating — use QRegularExpression since
    // QXmlStreamReader doesn't surface parent element names directly.
    f.seek(0);
    const QString text = QString::fromUtf8(f.readAll());
    static const QRegularExpression re(R"(<xmp:Rating>\s*(\d+)\s*</xmp:Rating>)");
    const auto m = re.match(text);
    if (m.hasMatch())
        rating = m.captured(1).toInt();

    return true;
}

QString XmpIO::load(const QString &xmpPath)
{
    QFile f(xmpPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

// ── write helpers ─────────────────────────────────────────────────────────────

static QString replaceOrInsert(const QString &xml,
                               const QRegularExpression &pattern,
                               const QString &replacement,
                               const QString &insertBefore)
{
    if (pattern.match(xml).hasMatch())
        return xml.size() ? xml : xml;  // handled below

    QString result = xml;
    const auto m = pattern.match(result);
    if (m.hasMatch())
        return result.replace(m.capturedStart(), m.capturedLength(), replacement);

    // Insert before the closing tag of rdf:Description
    const int pos = result.indexOf(insertBefore);
    if (pos >= 0)
        return result.insert(pos, replacement + "\n");
    return result;
}

QString XmpIO::setRating(const QString &xmpText, int rating)
{
    static const QRegularExpression re(
        R"(<xmp:Rating>\s*\d+\s*</xmp:Rating>)");
    const QString replacement = QString("<xmp:Rating>%1</xmp:Rating>").arg(rating);
    QString result = xmpText;
    const auto m = re.match(result);
    if (m.hasMatch())
        return result.replace(m.capturedStart(), m.capturedLength(), replacement);
    // Insert before </rdf:Description>
    const int pos = result.indexOf("</rdf:Description>");
    if (pos >= 0)
        return result.insert(pos, "  " + replacement + "\n");
    return result;
}

QString XmpIO::setColorLabel(const QString &xmpText, int label)
{
    // Replace the entire <darktable:colorlabels>…</darktable:colorlabels> block
    static const QRegularExpression re(
        R"(<darktable:colorlabels>.*?</darktable:colorlabels>)",
        QRegularExpression::DotMatchesEverythingOption);

    const QString replacement = label < 0
        ? "<darktable:colorlabels><rdf:Bag/></darktable:colorlabels>"
        : QString("<darktable:colorlabels>"
                  "<rdf:Bag><rdf:li>%1</rdf:li></rdf:Bag>"
                  "</darktable:colorlabels>").arg(label);

    QString result = xmpText;
    const auto m = re.match(result);
    if (m.hasMatch())
        return result.replace(m.capturedStart(), m.capturedLength(), replacement);
    // Insert before </rdf:Description>
    const int pos = result.indexOf("</rdf:Description>");
    if (pos >= 0)
        return result.insert(pos, "  " + replacement + "\n");
    return result;
}

bool XmpIO::save(const QString &xmpPath, const QString &content)
{
    QSaveFile f(xmpPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    f.write(content.toUtf8());
    return f.commit();
}
