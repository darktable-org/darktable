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
            // darktable writes xmp:Rating as an attribute on rdf:Description.
            if (name == u"Description") {
                const QString attr = xml.attributes().value(u"xmp:Rating").toString();
                if (!attr.isEmpty())
                    rating = attr.toInt();
            }
        }

        if (xml.isCharacters() && !xml.isWhitespace()) {
            const QString text = xml.text().toString().trimmed();
            if (xml.tokenType() == QXmlStreamReader::Characters) {
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

    // Also check element form <xmp:Rating>N</xmp:Rating> (written by mobile).
    // If we already got a non-zero rating from the attribute form, keep that
    // unless the element form has a higher value (element form is the edit).
    f.seek(0);
    const QString text = QString::fromUtf8(f.readAll());
    static const QRegularExpression reElem(R"(<xmp:Rating>\s*(\d+)\s*</xmp:Rating>)");
    const auto m = reElem.match(text);
    if (m.hasMatch()) {
        const int elemRating = m.captured(1).toInt();
        if (elemRating != 0 || rating == 0)
            rating = elemRating;
    }

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

QString XmpIO::setRating(const QString &xmpText, int rating)
{
    QString result = xmpText;

    // darktable writes Rating as an attribute on rdf:Description; update in-place
    // to avoid creating a duplicate element that conflicts with the attribute.
    static const QRegularExpression reAttr(R"(\bxmp:Rating\s*=\s*"[^"]*")");
    if (reAttr.match(result).hasMatch())
        return result.replace(reAttr, QString("xmp:Rating=\"%1\"").arg(rating));

    // Mobile-written element form <xmp:Rating>N</xmp:Rating>.
    static const QRegularExpression reElem(R"(<xmp:Rating>\s*\d+\s*</xmp:Rating>)");
    const QString elemReplacement = QString("<xmp:Rating>%1</xmp:Rating>").arg(rating);
    const auto m = reElem.match(result);
    if (m.hasMatch())
        return result.replace(m.capturedStart(), m.capturedLength(), elemReplacement);

    // No rating yet — insert element form before </rdf:Description>.
    const int pos = result.indexOf("</rdf:Description>");
    if (pos >= 0)
        return result.insert(pos, "  " + elemReplacement + "\n");
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
