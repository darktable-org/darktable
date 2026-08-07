#include "sharehelper.h"
#include <QFileInfo>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#endif

ShareHelper::ShareHelper(QObject *parent) : QObject(parent) {}

void ShareHelper::shareRawPaths(const QStringList &rawPaths)
{
    // Resolve raw paths → best available JPEG preview (full > thumb).
    QStringList jpegPaths;
    for (const QString &raw : rawPaths) {
        const QString full  = raw + QLatin1String(".preview-full.jpg");
        const QString thumb = raw + QLatin1String(".preview-thumb.jpg");
        if (QFileInfo::exists(full))
            jpegPaths << full;
        else if (QFileInfo::exists(thumb))
            jpegPaths << thumb;
    }
    if (jpegPaths.isEmpty())
        return;

#ifdef Q_OS_ANDROID
    QJniEnvironment env;
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray jPaths = env->NewObjectArray(
        static_cast<jsize>(jpegPaths.size()), stringClass, nullptr);

    for (int i = 0; i < jpegPaths.size(); ++i) {
        jstring js = env->NewStringUTF(jpegPaths.at(i).toUtf8().constData());
        env->SetObjectArrayElement(jPaths, static_cast<jsize>(i), js);
        env->DeleteLocalRef(js);
    }

    // Retrieve the Qt activity as an Android Context.
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative",
        "activity",
        "()Landroid/app/Activity;");

    QJniObject::callStaticMethod<void>(
        "net/darktable/mobile/ShareHelper",
        "shareImages",
        "(Landroid/content/Context;[Ljava/lang/String;)V",
        activity.object<jobject>(),
        jPaths);

    env->DeleteLocalRef(jPaths);
#else
    Q_UNUSED(jpegPaths)
#endif
}
