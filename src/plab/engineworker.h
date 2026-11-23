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

#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

// owns darktable: every engine call runs on the thread this object lives on,
// so the UI thread never blocks on the pixelpipe
class EngineWorker : public QObject
{
  Q_OBJECT
public:
  using QObject::QObject;

public slots:
  void start(const QStringList &coreArgs, const QString &stylesDir,
             const QString &lutDir, bool importBundledStyles);
  void open(const QString &path);
  void openId(int imgid);
  // edit: the controller's whole edit state, see LabController::editState()
  void render(const QVariantMap &edit, int maxW, int maxH, int generation);
  void renderOriginal(int maxW, int maxH, int generation);
  void applyStyle(const QString &name);
  void importStyles(const QString &path);
  void exportImage(const QString &path, int quality);

  // library
  void importFolder(const QString &folder, bool recursive);
  void listFolders();
  void listImages(int filmId, int minRating, bool rejectedOnly, int generation);
  void setRating(const QList<int> &ids, int rating);
  void toggleLabel(const QList<int> &ids, int color);
  // many images at once: add LUT layers, set a style, or replace their edit
  void addLuts(const QList<int> &ids, const QVariantList &luts);
  void styleImages(const QList<int> &ids, const QString &style);
  void pasteEdit(const QList<int> &ids, const QVariantMap &edit, const QString &style);
  void exportImages(const QList<int> &ids, const QString &folder, int quality);
  void stop();

signals:
  void started(bool ok, const QStringList &styles);
  // edit and style: what this image was left with last time, empty if new
  void opened(int imgid, const QString &path, double asShotTemp,
              const QString &style, const QVariantMap &edit, const QString &error);
  void previewReady(const QImage &image, int generation);
  void styleApplied(const QString &name, int baseEnd);
  void stylesChanged(const QStringList &styles);
  void exported(const QString &path, const QString &error);
  void failed(const QString &message);

  void foldersListed(const QVariantList &folders);
  void imagesListed(const QVariantList &images, int generation);
  // images whose look or labels changed, so their thumbnails reload
  void imagesChanged(const QList<int> &ids);
  void batchDone(const QString &message);

private:
  struct Record
  {
    int autoBase = 0;    // history darktable itself applied on first open
    int base = 0;        // autoBase plus the chosen style
    QString style;
    QVariantMap edit;    // what history holds above base
    bool committed = false;
  };

  QStringList _styles() const;
  Record &_record(int imgid);
  bool _commit(int imgid, Record &rec, const QVariantMap &edit, QString *error);
  int _applyStyle(int imgid, Record &rec, const QString &name);
  void _loadStore();
  void _saveStore();

  QString m_storePath;  // per-image bases and slider state, as JSON
  QHash<int, Record> m_records;
  int m_imgid = -1;
  double m_asShot = 0.0;
  QString m_lutDir;
};
