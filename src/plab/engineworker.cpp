/*
    This file is part of PhotographyLAB, a darktable-based photo editor.
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "engineworker.h"
#include "plab_engine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <deque>
#include <vector>

namespace
{
// takes ownership of a g_malloc'd error string
QString takeError(char *err)
{
  const QString s = err ? QString::fromUtf8(err) : QString();
  plab_free(err);
  return s;
}

// the tone mappers darktable's workflows choose between: a display-referred
// LUT belongs after whichever is on
const char *const kToneMappers = "sigmoid,filmicrgb,agx,spektrafilm,basecurve";

// builds plab_layer_t arrays whose strings stay alive as long as this object
class LayerSet
{
public:
  plab_layer_t &add(const char *op, int enabled)
  {
    plab_layer_t l = {};
    l.op = op;
    l.enabled = enabled;
    l.opacity = -1.f;
    m_layers.push_back(l);
    m_fields.emplace_back();
    return m_layers.back();
  }
  void field(const char *name, double value, bool relative = false)
  {
    m_fields.back().push_back({ name, value, nullptr, relative ? 1 : 0 });
  }
  void text(const char *name, const QString &value)
  {
    m_strings.push_back(value.toUtf8());
    m_fields.back().push_back({ name, 0.0, m_strings.back().constData(), 0 });
  }
  const char *keep(const QString &s)
  {
    m_strings.push_back(s.toUtf8());
    return m_strings.back().constData();
  }
  // fields are stored apart and attached last: the vectors may move while adding
  const plab_layer_t *data()
  {
    for(size_t i = 0; i < m_layers.size(); i++)
    {
      m_layers[i].fields = m_fields[i].data();
      m_layers[i].n_fields = int(m_fields[i].size());
    }
    return m_layers.data();
  }
  int size() const { return int(m_layers.size()); }

private:
  std::vector<plab_layer_t> m_layers;
  std::vector<std::vector<plab_field_t>> m_fields;
  std::deque<QByteArray> m_strings; // deque: growing never moves elements
};

double num(const QVariantMap &m, const char *key, double def = 0.0)
{
  const auto it = m.find(QLatin1String(key));
  return it == m.end() ? def : it->toDouble();
}
} // namespace

void EngineWorker::start(const QStringList &coreArgs, const QString &stylesDir,
                         const QString &lutDir, bool importBundledStyles)
{
  std::vector<QByteArray> bytes;
  std::vector<char *> argv;
  for(const QString &a : coreArgs) bytes.push_back(a.toUtf8());
  for(QByteArray &b : bytes) argv.push_back(b.data());

  if(plab_init(int(argv.size()), argv.data()) != 0)
  {
    emit started(false, {});
    return;
  }
  for(int i = 0; i < coreArgs.size() - 1; i++)
    if(coreArgs[i] == QLatin1String("--configdir"))
      m_storePath = QDir(coreArgs[i + 1]).filePath(QStringLiteral("photographylab-edits.json"));

  m_lutDir = lutDir;
  QDir().mkpath(lutDir);
  plab_set_lut_folder(QFile::encodeName(lutDir).constData());
  if(importBundledStyles && QFileInfo(stylesDir).isDir())
    plab_import_styles(QFile::encodeName(stylesDir).constData());
  emit started(true, _styles());
}

QStringList EngineWorker::_styles() const
{
  char *list = plab_list_styles();
  const QString s = QString::fromUtf8(list);
  plab_free(list);
  QStringList out;
  // darktable's own per-camera base styles are not looks, so they stay hidden
  for(const QString &name : s.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
    if(!name.startsWith(QLatin1String("_l10n_darktable|"))) out << name;
  return out;
}

void EngineWorker::open(const QString &path)
{
  char *err = nullptr;
  const int id = plab_import(QFile::encodeName(path).constData(), &err);
  if(id < 0)
  {
    emit opened(-1, path, 0.0, {}, {}, takeError(err));
    return;
  }
  m_imgid = id;
  m_haveCommitted = false;
  m_committed.clear();
  m_style.clear();

  // an image this app edited before: its bases and sliders come back
  QVariantMap saved;
  QJsonObject store;
  QFile f(m_storePath);
  if(f.open(QIODevice::ReadOnly)) store = QJsonDocument::fromJson(f.readAll()).object();
  const QJsonObject rec = store.value(QString::number(id)).toObject();
  if(!rec.isEmpty())
  {
    m_autoBase = rec.value(QStringLiteral("autoBase")).toInt();
    m_base = rec.value(QStringLiteral("base")).toInt();
    m_style = rec.value(QStringLiteral("style")).toString();
    saved = rec.value(QStringLiteral("edit")).toObject().toVariantMap();
    m_committed = saved;
    m_haveCommitted = true;
  }
  else
  {
    m_autoBase = m_base = plab_base_history(id);
  }
  m_asShot = plab_as_shot_temperature(id);
  emit opened(id, path, m_asShot, m_style, saved, takeError(err));
}

void EngineWorker::_saveRecord()
{
  if(m_storePath.isEmpty() || m_imgid < 0) return;
  QJsonObject store;
  QFile f(m_storePath);
  if(f.open(QIODevice::ReadOnly))
  {
    store = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
  }
  QJsonObject rec;
  rec.insert(QStringLiteral("autoBase"), m_autoBase);
  rec.insert(QStringLiteral("base"), m_base);
  rec.insert(QStringLiteral("style"), m_style);
  rec.insert(QStringLiteral("edit"), QJsonObject::fromVariantMap(m_committed));
  store.insert(QString::number(m_imgid), rec);
  if(f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    f.write(QJsonDocument(store).toJson(QJsonDocument::Compact));
}

bool EngineWorker::_commit(const QVariantMap &edit, QString *error)
{
  if(m_haveCommitted && edit == m_committed) return true;

  const QVariantMap a = edit.value(QStringLiteral("adjust")).toMap();
  LayerSet set;

  // white balance: color calibration's custom illuminant, so the CAT stays the
  // one darktable's workflow set up
  const double temp = num(a, "temperature");
  const double tint = num(a, "tint");
  if((temp > 0.0 && qAbs(temp - m_asShot) > 1.0) || tint != 0.0)
  {
    double x = 0.0, y = 0.0;
    plab_white_balance_xy(m_imgid, temp > 0.0 ? temp : m_asShot, tint, &x, &y);
    set.add("channelmixerrgb", 1);
    set.text("illuminant", QStringLiteral("DT_ILLUMINANT_CUSTOM"));
    set.field("x", x);
    set.field("y", y);
  }

  // exposure and blacks add to what darktable's workflow already applied
  const double ev = num(a, "exposure"), blacks = num(a, "blacks");
  if(ev != 0.0 || blacks != 0.0)
  {
    set.add("exposure", 1);
    set.field("exposure", ev, true);
    // lightroom's +blacks lifts them; darktable's black level correction crushes
    set.field("black", -blacks * 0.001, true);
  }

  const double contrast = num(a, "contrast"), hi = num(a, "highlights"),
               sh = num(a, "shadows"), vib = num(a, "vibrance"),
               sat = num(a, "saturation");
  if(contrast != 0.0 || hi != 0.0 || sh != 0.0 || vib != 0.0 || sat != 0.0)
  {
    set.add("colorbalancergb", 1);
    set.field("contrast", contrast / 200.0);
    set.field("brilliance_highlights", hi / 300.0);
    set.field("brilliance_shadows", sh / 150.0);
    set.field("vibrance", vib / 100.0);
    // chroma rather than saturation: at -100 it reaches black and white
    set.field("chroma_global", sat > 0.0 ? sat / 150.0 : sat / 100.0);
  }

  const double clarity = num(a, "clarity");
  if(clarity != 0.0)
  {
    set.add("bilat", 1);
    set.field("detail", clarity / 100.0);
  }

  const double dehaze = num(a, "dehaze");
  if(dehaze != 0.0)
  {
    set.add("hazeremoval", 1);
    set.field("strength", dehaze / 100.0);
  }

  const double noise = num(a, "noise");
  if(noise > 0.0)
  {
    set.add("denoiseprofile", 1);
    set.field("strength", noise / 50.0);
  }

  const double straighten = num(a, "straighten");
  if(straighten != 0.0)
  {
    set.add("ashift", 1);
    set.field("rotation", straighten);
  }

  // lens correction defaults to on: lensfun or the embedded profile, by exif
  set.add("lens", a.value(QStringLiteral("lens"), true).toBool() ? 1 : 0);

  // LUT layers, bottom to top, each its own lut3d instance
  const QVariantList luts = edit.value(QStringLiteral("luts")).toList();
  int n = 0;
  for(const QVariant &v : luts)
  {
    const QVariantMap lut = v.toMap();
    const QString file = lut.value(QStringLiteral("file")).toString();
    if(file.isEmpty()) continue;
    plab_layer_t &l = set.add("lut3d", 1);
    l.fresh = ++n;
    l.after = kToneMappers;
    l.opacity = float(num(lut, "strength", 100.0));
    l.label = set.keep(QFileInfo(file).completeBaseName());
    set.text("filepath", file);
    set.text("colorspace", QStringLiteral("DT_IOP_SRGB"));
  }

  char *err = nullptr;
  const int rc = plab_apply(m_imgid, m_base, set.data(), set.size(), &err);
  const QString msg = takeError(err);
  if(rc != 0)
  {
    if(error) *error = msg;
    // history may hold part of the edit now, so the next commit must rewrite it
    m_haveCommitted = false;
    return false;
  }
  m_committed = edit;
  m_haveCommitted = true;
  _saveRecord();
  return true;
}

void EngineWorker::render(const QVariantMap &edit, int maxW, int maxH, int generation)
{
  if(m_imgid < 0) return;
  QString error;
  if(!_commit(edit, &error)) emit failed(error);

  int w = 0, h = 0;
  char *err = nullptr;
  uint8_t *buf = plab_render(m_imgid, maxW, maxH, -1, &w, &h, &err);
  if(!buf)
  {
    emit failed(takeError(err));
    return;
  }
  plab_free(err);
  // the export hands back BGRx bytes (display byte order), which is RGB32
  QImage img(buf, w, h, w * 4, QImage::Format_RGB32, plab_free, buf);
  emit previewReady(img, generation);
}

void EngineWorker::renderOriginal(int maxW, int maxH, int generation)
{
  if(m_imgid < 0) return;
  int w = 0, h = 0;
  char *err = nullptr;
  uint8_t *buf = plab_render(m_imgid, maxW, maxH, m_autoBase, &w, &h, &err);
  if(!buf)
  {
    emit failed(takeError(err));
    return;
  }
  plab_free(err);
  QImage img(buf, w, h, w * 4, QImage::Format_RGB32, plab_free, buf);
  emit previewReady(img, generation);
}

void EngineWorker::applyStyle(const QString &name)
{
  if(m_imgid < 0) return;
  char *err = nullptr;
  if(name.isEmpty())
  {
    // back to darktable's own starting point
    plab_apply(m_imgid, m_autoBase, nullptr, 0, &err);
    m_base = m_autoBase;
  }
  else
  {
    m_base = plab_apply_style(m_imgid, m_autoBase, name.toUtf8().constData(), &err);
  }
  const QString msg = takeError(err);
  if(!msg.isEmpty()) emit failed(msg);
  m_style = name;
  // the sliders go back on top of the new base at the next render
  m_haveCommitted = false;
  emit styleApplied(name, m_base);
}

void EngineWorker::importStyles(const QString &path)
{
  plab_import_styles(QFile::encodeName(path).constData());
  emit stylesChanged(_styles());
}

void EngineWorker::exportImage(const QString &path, int quality)
{
  if(m_imgid < 0) return;
  char *err = nullptr;
  plab_export(m_imgid, QFile::encodeName(path).constData(), quality, &err);
  emit exported(path, takeError(err));
}

void EngineWorker::stop()
{
  plab_shutdown();
}
