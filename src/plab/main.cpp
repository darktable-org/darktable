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

#include "labcontroller.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <memory>

int main(int argc, char *argv[])
{
  QGuiApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("photographylab"));
  app.setApplicationDisplayName(QStringLiteral("PhotographyLAB"));
  QQuickStyle::setStyle(QStringLiteral("Material"));

  QCommandLineParser cli;
  cli.setApplicationDescription(QStringLiteral("PhotographyLAB photo editor, on darktable's engine"));
  cli.addHelpOption();
  cli.addPositionalArgument(QStringLiteral("image"), QStringLiteral("a raw or other image to open"));
  QCommandLineOption screenshot(QStringLiteral("screenshot"),
                                QStringLiteral("save the window to <file> once the preview is up, then quit"),
                                QStringLiteral("file"));
  QCommandLineOption tab(QStringLiteral("tab"), QStringLiteral("start on panel tab <n>"),
                         QStringLiteral("n"), QStringLiteral("0"));
  QCommandLineOption set(QStringLiteral("set"),
                         QStringLiteral("set an adjustment, key=value, before the first render"),
                         QStringLiteral("key=value"));
  QCommandLineOption size(QStringLiteral("size"), QStringLiteral("window size, WxH"),
                          QStringLiteral("WxH"));
  QCommandLineOption exportOpt(QStringLiteral("export"),
                               QStringLiteral("export the image to <file> once the preview is up, then quit"),
                               QStringLiteral("file"));
  QCommandLineOption importOpt(QStringLiteral("import"),
                               QStringLiteral("import a folder into the library at start"),
                               QStringLiteral("folder"));
  cli.addOption(importOpt);
  cli.addOption(size);
  cli.addOption(exportOpt);
  cli.addOption(screenshot);
  cli.addOption(tab);
  cli.addOption(set);
  cli.process(app);

  // separate from darktable's own folders, so a darktable install is untouched
  const QString config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
  const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir().mkpath(config);
  QDir().mkpath(cache);
  QDir().mkpath(data);

  const QString styles = QDir(QCoreApplication::applicationDirPath())
                           .filePath(QStringLiteral("../share/photographylab/styles"));
  const QStringList core = {
    QStringLiteral("--configdir"), config,
    QStringLiteral("--cachedir"), cache,
  };

  // the QML engine takes ownership of both providers
  auto *provider = new PreviewProvider;
  auto *thumbs = new ThumbnailProvider;
  LabController lab(provider, thumbs);

  QQmlApplicationEngine engine;
  engine.addImageProvider(QStringLiteral("preview"), provider);
  engine.addImageProvider(QStringLiteral("thumb"), thumbs);
  engine.rootContext()->setContextProperty(QStringLiteral("lab"), &lab);
  engine.rootContext()->setContextProperty(QStringLiteral("startTab"),
                                           cli.value(tab).toInt());
  engine.load(QUrl(QStringLiteral("qrc:/PhotographyLab/Main.qml")));
  if(engine.rootObjects().isEmpty()) return 1;
  auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
  if(cli.isSet(size))
  {
    const QStringList wh = cli.value(size).split(QLatin1Char('x'));
    if(wh.size() == 2) window->resize(wh[0].toInt(), wh[1].toInt());
  }

  lab.start(core, QDir::cleanPath(styles), QDir(data).filePath(QStringLiteral("luts")), data);

  if(cli.isSet(importOpt))
  {
    const QString folder = QFileInfo(cli.value(importOpt)).absoluteFilePath();
    QObject::connect(&lab, &LabController::readyChanged, &lab, [&lab, folder] {
      if(lab.ready()) lab.importFolder(QUrl::fromLocalFile(folder), true);
    });
  }

  const QStringList files = cli.positionalArguments();
  const QStringList sets = cli.values(set);
  if(!files.isEmpty())
  {
    const QString first = QFileInfo(files.first()).absoluteFilePath();
    QObject::connect(&lab, &LabController::readyChanged, &lab, [&lab, first] {
      if(lab.ready()) lab.openFile(QUrl::fromLocalFile(first));
    });
    // --set values go on top of whatever the image was left with
    auto applied = std::make_shared<bool>(false);
    QObject::connect(&lab, &LabController::imageChanged, &lab, [&lab, sets, applied] {
      if(*applied) return;
      *applied = true;
      for(const QString &s : sets)
      {
        const int eq = s.indexOf(QLatin1Char('='));
        if(eq <= 0) continue;
        const QString key = s.left(eq), val = s.mid(eq + 1);
        if(key == QLatin1String("lut"))
          lab.addLut(QUrl::fromLocalFile(val));
        else if(key == QLatin1String("style"))
          lab.applyStyle(val);
        else
          lab.setValue(key, val == QLatin1String("false") ? QVariant(false)
                                                          : QVariant(val.toDouble()));
      }
    });
  }

  // for checking a build without a person at the screen
  if(cli.isSet(screenshot))
  {
    const QString out = cli.value(screenshot);
    auto shoot = [window, out](int delay) {
      QTimer::singleShot(delay, window, [window, out] {
        window->grabWindow().save(out);
        QCoreApplication::quit();
      });
    };
    if(!files.isEmpty())
      QObject::connect(&lab, &LabController::previewChanged, &lab, [shoot, &lab] {
        if(!lab.busy()) shoot(800);
      });
    else
    {
      // the library: once the grid is filled, give the thumbnails time to load
      auto armed = std::make_shared<bool>(false);
      QObject::connect(&lab, &LabController::imagesChanged, &lab, [shoot, &lab, armed] {
        if(lab.busy() || lab.images().isEmpty() || *armed) return;
        *armed = true;
        // batch actions on everything shown, for checking them without a person
        const QString lut = qEnvironmentVariable("PLAB_BATCH_LUT");
        const QString style = qEnvironmentVariable("PLAB_BATCH_STYLE");
        const QString rate = qEnvironmentVariable("PLAB_BATCH_RATE");
        if(qEnvironmentVariableIsSet("PLAB_SELECT_ALL") || !lut.isEmpty() || !style.isEmpty()
           || !rate.isEmpty())
          lab.selectAll();
        if(!rate.isEmpty()) lab.rateSelected(rate.toInt());
        if(!lut.isEmpty()) lab.addLutToSelected(QUrl::fromLocalFile(lut));
        if(!style.isEmpty()) lab.styleSelected(style);
        shoot(lut.isEmpty() && style.isEmpty() ? 6000 : 25000);
      });
    }
  }

  if(cli.isSet(exportOpt))
  {
    const QString out = QFileInfo(cli.value(exportOpt)).absoluteFilePath();
    auto done = std::make_shared<bool>(false);
    QObject::connect(&lab, &LabController::previewChanged, &lab, [&lab, out, done] {
      if(lab.busy() || *done) return;
      *done = true;
      lab.exportTo(QUrl::fromLocalFile(out), 92);
    });
    QObject::connect(&lab, &LabController::messageChanged, &lab, [&lab, done] {
      if(*done && !lab.busy())
      {
        fprintf(stderr, "photographylab: %s\n", qPrintable(lab.message()));
        QCoreApplication::quit();
      }
    });
  }

  return app.exec();
}
