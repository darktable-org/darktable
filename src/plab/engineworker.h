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
  // edit: the controller's whole edit state, see LabController::editState()
  void render(const QVariantMap &edit, int maxW, int maxH, int generation);
  void renderOriginal(int maxW, int maxH, int generation);
  void applyStyle(const QString &name);
  void importStyles(const QString &path);
  void exportImage(const QString &path, int quality);
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

private:
  QStringList _styles() const;
  bool _commit(const QVariantMap &edit, QString *error);
  void _saveRecord();

  QString m_storePath;  // per-image bases and slider state, as JSON
  QString m_style;
  int m_imgid = -1;
  int m_autoBase = 0;   // history darktable itself applied on first open
  int m_base = 0;       // m_autoBase plus the chosen style
  double m_asShot = 0.0;
  QString m_lutDir;
  QVariantMap m_committed; // what history holds now above m_base
  bool m_haveCommitted = false;
};
