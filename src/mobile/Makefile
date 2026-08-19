SCRIPT_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
APK := $(SCRIPT_DIR)build-android/android-build/build/outputs/apk/debug/android-build-debug.apk

build:
	./build-android.sh

install: $(APK)
	adb install -r "$(APK)"
	adb shell am force-stop net.darktable.mobile
	adb shell am start -n "net.darktable.mobile/org.qtproject.qt.android.bindings.QtActivity"

$(APK): build

.PHONY: build install
