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
#include <QMutex>
#include <QObject>
#include <QQuickImageProvider>
#include <QThreadPool>
#include <atomic>
#include <QThread>
#include <QUrl>
#include <QVariantMap>

class EngineWorker;

// hands QML the latest rendered preview
class PreviewProvider : public QQuickImageProvider
{
public:
  PreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
  QImage requestImage(const QString &id, QSize *size, const QSize &requested) override;
  void set(const QImage &img);

private:
  QMutex m_lock;
  QImage m_image;
};

// library thumbnails from darktable's own thumbnail cache, loaded off the UI
// thread. darktable's GUI reads that cache from many threads too
class ThumbnailProvider : public QQuickAsyncImageProvider
{
public:
  ThumbnailProvider();
  QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requested) override;
  std::atomic<bool> ready{ false };

private:
  QThreadPool m_pool;
};

// the state QML binds to, and the queue between it and the engine thread
class LabController : public QObject
{
  Q_OBJECT
  Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
  Q_PROPERTY(bool hasImage READ hasImage NOTIFY imageChanged)
  Q_PROPERTY(QString imageName READ imageName NOTIFY imageChanged)
  Q_PROPERTY(QString previewSource READ previewSource NOTIFY previewChanged)
  Q_PROPERTY(double asShotTemperature READ asShotTemperature NOTIFY imageChanged)
  Q_PROPERTY(QVariantMap adjust READ adjust NOTIFY adjustChanged)
  Q_PROPERTY(QVariantList luts READ luts NOTIFY lutsChanged)
  Q_PROPERTY(QStringList styles READ styles NOTIFY stylesChanged)
  Q_PROPERTY(QString currentStyle READ currentStyle NOTIFY currentStyleChanged)
  Q_PROPERTY(QString message READ message NOTIFY messageChanged)
  Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
  Q_PROPERTY(QVariantList folders READ folders NOTIFY foldersChanged)
  Q_PROPERTY(QVariantList images READ images NOTIFY imagesChanged)
  Q_PROPERTY(int currentFolder READ currentFolder NOTIFY filterChanged)
  Q_PROPERTY(int minRating READ minRating NOTIFY filterChanged)
  Q_PROPERTY(bool showRejected READ showRejected NOTIFY filterChanged)
  Q_PROPERTY(QVariantList selection READ selection NOTIFY selectionChanged)
  Q_PROPERTY(int currentImage READ currentImage NOTIFY imageChanged)
  Q_PROPERTY(QVariantMap thumbVersions READ thumbVersions NOTIFY thumbsChanged)
  Q_PROPERTY(bool hasCopiedEdit READ hasCopiedEdit NOTIFY copiedEditChanged)

public:
  LabController(PreviewProvider *provider, ThumbnailProvider *thumbs,
                QObject *parent = nullptr);
  ~LabController() override;

  void start(const QStringList &coreArgs, const QString &stylesDir,
             const QString &lutDir, const QString &dataDir);

  bool ready() const { return m_ready; }
  bool busy() const { return m_inFlight; }
  bool hasImage() const { return m_imgid >= 0; }
  QString imageName() const;
  QString previewSource() const;
  double asShotTemperature() const { return m_asShot; }
  QVariantMap adjust() const { return m_adjust; }
  QVariantList luts() const { return m_luts; }
  QStringList styles() const { return m_styles; }
  QString currentStyle() const { return m_style; }
  QString message() const { return m_message; }
  QString mode() const { return m_mode; }
  void setMode(const QString &mode);
  QVariantList folders() const { return m_folders; }
  QVariantList images() const { return m_images; }
  int currentFolder() const { return m_folder; }
  int minRating() const { return m_minRating; }
  bool showRejected() const { return m_rejected; }
  QVariantList selection() const;
  int currentImage() const { return m_imgid; }
  QVariantMap thumbVersions() const { return m_thumbVersions; }
  bool hasCopiedEdit() const { return !m_copied.isEmpty(); }

  // library
  Q_INVOKABLE void importFolder(const QUrl &url, bool recursive);
  Q_INVOKABLE void showFolder(int filmId);
  Q_INVOKABLE void setMinRating(int stars);
  Q_INVOKABLE void setShowRejected(bool on);
  // how: 0 select only this, 1 toggle it (ctrl), 2 extend from the last (shift)
  Q_INVOKABLE void select(int imgid, int how);
  Q_INVOKABLE void selectAll();
  Q_INVOKABLE void clearSelection();
  Q_INVOKABLE void rateSelected(int stars);
  Q_INVOKABLE void rejectSelected();
  Q_INVOKABLE void toggleLabelSelected(int color);
  Q_INVOKABLE void addLutToSelected(const QUrl &url);
  Q_INVOKABLE void styleSelected(const QString &name);
  Q_INVOKABLE void copyEdit();
  Q_INVOKABLE void pasteEditToSelected();
  Q_INVOKABLE void exportSelected(const QUrl &folder, int quality);
  Q_INVOKABLE void openImage(int imgid);
  Q_INVOKABLE void openNeighbour(int step);

  Q_INVOKABLE void openFile(const QUrl &url);
  Q_INVOKABLE void setValue(const QString &key, const QVariant &value);
  Q_INVOKABLE void resetAdjustments();
  Q_INVOKABLE void applyStyle(const QString &name);
  Q_INVOKABLE void importStyles(const QUrl &url);
  Q_INVOKABLE void addLut(const QUrl &url);
  Q_INVOKABLE void removeLut(int index);
  Q_INVOKABLE void setLutStrength(int index, double strength);
  Q_INVOKABLE void exportTo(const QUrl &url, int quality);
  Q_INVOKABLE void setViewSize(int width, int height);
  Q_INVOKABLE void showOriginal(bool on);

signals:
  void readyChanged();
  void busyChanged();
  void imageChanged();
  void previewChanged();
  void adjustChanged();
  void lutsChanged();
  void stylesChanged();
  void currentStyleChanged();
  void messageChanged();
  void modeChanged();
  void foldersChanged();
  void imagesChanged();
  void filterChanged();
  void selectionChanged();
  void thumbsChanged();
  void copiedEditChanged();

  // to the engine thread
  void requestOpen(const QString &path);
  void requestRender(const QVariantMap &edit, int maxW, int maxH, int generation);
  void requestOriginal(int maxW, int maxH, int generation);
  void requestStyle(const QString &name);
  void requestImportStyles(const QString &path);
  void requestExport(const QString &path, int quality);
  void requestOpenId(int imgid);
  void requestImportFolder(const QString &folder, bool recursive);
  void requestFolders();
  void requestImages(int filmId, int minRating, bool rejectedOnly, int generation);
  void requestRating(const QList<int> &ids, int rating);
  void requestLabel(const QList<int> &ids, int color);
  void requestAddLuts(const QList<int> &ids, const QVariantList &luts);
  void requestStyleImages(const QList<int> &ids, const QString &style);
  void requestPaste(const QList<int> &ids, const QVariantMap &edit, const QString &style);
  void requestExportImages(const QList<int> &ids, const QString &folder, int quality);

private:
  static QVariantMap defaultAdjust();
  QVariantMap editState() const;
  void scheduleRender();
  void kick();
  void setMessage(const QString &m);
  void setInFlight(bool on);
  void refreshImages();
  QList<int> selectedOrCurrent() const;
  QString copyLut(const QUrl &url);

  PreviewProvider *m_provider;
  QThread m_thread;
  EngineWorker *m_worker = nullptr;

  bool m_ready = false;
  int m_imgid = -1;
  QString m_path;
  QString m_lutDir;
  double m_asShot = 0.0;
  QVariantMap m_adjust;
  QVariantList m_luts;
  QStringList m_styles;
  QString m_style;
  QString m_message;

  int m_viewW = 1600, m_viewH = 1200;
  bool m_original = false;
  bool m_inFlight = false;
  bool m_dirty = false;    // a render was asked for while one was running
  int m_generation = 0;
  int m_shown = 0;

  ThumbnailProvider *m_thumbs;
  QString m_mode = QStringLiteral("library");
  QVariantList m_folders;
  QVariantList m_images;
  int m_folder = -1;
  int m_minRating = 0;
  bool m_rejected = false;
  int m_listGeneration = 0;
  QList<int> m_selected;
  int m_anchor = -1;
  QVariantMap m_thumbVersions;
  QVariantMap m_copied;
  QString m_copiedStyle;
};
