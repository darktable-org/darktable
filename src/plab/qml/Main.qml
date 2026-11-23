import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1440
    height: 900
    visible: true
    title: lab.hasImage ? lab.imageName + " - PhotographyLAB" : "PhotographyLAB"
    color: Theme.window

    Material.theme: Material.Dark
    Material.accent: Theme.accent
    Material.background: Theme.panel

    // unfolded or desktop: side panel. folded phone, portrait: panel below
    readonly property bool wide: width > height * 1.05 && width > 720

    function adj(key) { return lab.adjust[key] }

    header: ToolBar {
        Material.background: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.gutter
            anchors.rightMargin: 8
            spacing: 4
            Text {
                text: "PhotographyLAB"
                color: Theme.text
                font.pixelSize: 18
                font.bold: true
                visible: win.wide
            }
            TabBar {
                id: modeBar
                Material.background: Theme.panel
                currentIndex: lab.mode === "library" ? 0 : 1
                TabButton { text: "Library"; width: implicitWidth; onClicked: lab.mode = "library" }
                TabButton {
                    text: "Develop"
                    width: implicitWidth
                    enabled: lab.hasImage
                    onClicked: lab.mode = "develop"
                }
            }
            Text {
                text: lab.hasImage && win.wide ? "  " + lab.imageName : ""
                color: Theme.textDim
                font.pixelSize: 14
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
            ToolButton {
                text: "Copy edit"
                visible: lab.mode === "develop"
                onClicked: lab.copyEdit()
                ToolTip.visible: hovered
                ToolTip.text: "copy this photo's sliders, LUTs and style, to paste onto others in the Library"
            }
            ToolButton {
                text: "Before"
                visible: lab.mode === "develop"
                enabled: lab.hasImage
                onPressed: lab.showOriginal(true)
                onReleased: lab.showOriginal(false)
                onCanceled: lab.showOriginal(false)
                ToolTip.visible: hovered
                ToolTip.text: "hold to see the photo without your edits"
            }
            ToolButton { text: "Open"; onClicked: openDialog.open() }
            ToolButton {
                text: "Export"
                visible: lab.mode === "develop"
                enabled: lab.hasImage
                onClicked: exportDialog.open()
            }
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: lab.mode === "library" ? 0 : 1

    LibraryView {
        wide: win.wide
    }

    GridLayout {
        columns: win.wide ? 2 : 1
        rowSpacing: 0
        columnSpacing: 0

        // ---------------------------------------------------------------- photo
        Rectangle {
            id: stage
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: win.wide ? -1 : win.height * 0.5
            color: Theme.window
            clip: true

            onWidthChanged: sizeTimer.restart()
            onHeightChanged: sizeTimer.restart()
            Timer {
                id: sizeTimer
                interval: 250
                onTriggered: lab.setViewSize(stage.width * Screen.devicePixelRatio,
                                             stage.height * Screen.devicePixelRatio)
            }
            Component.onCompleted: sizeTimer.start()

            ListView {
                id: filmstrip
                visible: win.wide && lab.images.length > 1
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: visible ? 96 : 0
                orientation: ListView.Horizontal
                clip: true
                spacing: 4
                model: lab.images
                currentIndex: {
                    for(let i = 0; i < lab.images.length; i++)
                        if(lab.images[i].id === lab.currentImage) return i
                    return -1
                }
                onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
                delegate: Rectangle {
                    required property var modelData
                    width: 120
                    height: filmstrip.height
                    color: modelData.id === lab.currentImage ? Theme.panelRaised : Theme.window
                    border.width: modelData.id === lab.currentImage ? 2 : 0
                    border.color: Theme.accent
                    Image {
                        anchors.fill: parent
                        anchors.margins: 4
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                        sourceSize.width: 240
                        sourceSize.height: 240
                        source: "image://thumb/" + parent.modelData.id + "/"
                                + (lab.thumbVersions[String(parent.modelData.id)] || 0)
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: lab.openImage(parent.modelData.id)
                    }
                }
            }

            Image {
                id: photo
                anchors.fill: parent
                anchors.margins: 12
                anchors.bottomMargin: 12 + filmstrip.height
                source: lab.previewSource
                fillMode: Image.PreserveAspectFit
                cache: false
                asynchronous: false
                smooth: true
                mipmap: true
            }

            BusyIndicator {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 16
                width: 36
                height: 36
                running: lab.busy
            }

            ColumnLayout {
                anchors.centerIn: parent
                visible: !lab.hasImage
                spacing: 12
                Text {
                    text: lab.ready ? "Open a photo to start editing" : "Starting the darktable engine..."
                    color: Theme.text
                    font.pixelSize: 20
                    Layout.alignment: Qt.AlignHCenter
                }
                Text {
                    text: "raw files from your camera, or JPEG, TIFF, PNG. You can also drop a file here."
                    color: Theme.textDim
                    font.pixelSize: 14
                    Layout.alignment: Qt.AlignHCenter
                }
                Button {
                    text: "Open photo"
                    highlighted: true
                    enabled: lab.ready
                    Layout.alignment: Qt.AlignHCenter
                    onClicked: openDialog.open()
                }
            }

            DropArea {
                anchors.fill: parent
                onDropped: (drop) => { if(drop.hasUrls) lab.openFile(drop.urls[0]) }
            }

            Label {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 16
                text: lab.message
                visible: text.length > 0
                color: Theme.text
                background: Rectangle { color: "#cc000000"; radius: 4 }
                padding: 8
            }
        }

        // ---------------------------------------------------------------- panel
        Rectangle {
            Layout.fillHeight: true
            Layout.fillWidth: !win.wide
            Layout.preferredWidth: win.wide ? 360 : -1
            Layout.preferredHeight: win.wide ? -1 : win.height * 0.42
            color: Theme.panel

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                TabBar {
                    id: tabs
                    Layout.fillWidth: true
                    currentIndex: startTab
                    Material.background: Theme.panel
                    TabButton { text: "Basic" }
                    TabButton { text: "Looks" }
                    TabButton { text: "Detail" }
                    TabButton { text: "Optics" }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: tabs.currentIndex
                    enabled: lab.hasImage

                    // ---------------------------------------------------- basic
                    ScrollView {
                        id: basicScroll
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: basicScroll.availableWidth - 2 * Theme.gutter
                            x: Theme.gutter
                            spacing: 4

                            SectionHeader { title: "White balance" }
                            AdjustSlider {
                                label: "Temperature"
                                from: 2000; to: 12000; stepSize: 50
                                unit: " K"
                                value: adj("temperature") > 0 ? adj("temperature") : lab.asShotTemperature
                                neutral: lab.asShotTemperature
                                gradient: ["#3d7bd9", "#e0b341"]
                                onMoved: (v) => lab.setValue("temperature", Math.abs(v - lab.asShotTemperature) < 25 ? 0 : v)
                            }
                            AdjustSlider {
                                label: "Tint"
                                from: -100; to: 100
                                value: adj("tint")
                                gradient: ["#4fae55", "#c04fbf"]
                                onMoved: (v) => lab.setValue("tint", v)
                            }

                            SectionHeader { title: "Tone" }
                            AdjustSlider {
                                label: "Exposure"
                                from: -5; to: 5; stepSize: 0.05; decimals: 2; unit: " EV"
                                value: adj("exposure")
                                onMoved: (v) => lab.setValue("exposure", v)
                            }
                            AdjustSlider { label: "Contrast"; value: adj("contrast"); onMoved: (v) => lab.setValue("contrast", v) }
                            AdjustSlider { label: "Highlights"; value: adj("highlights"); onMoved: (v) => lab.setValue("highlights", v) }
                            AdjustSlider { label: "Shadows"; value: adj("shadows"); onMoved: (v) => lab.setValue("shadows", v) }
                            AdjustSlider { label: "Blacks"; value: adj("blacks"); onMoved: (v) => lab.setValue("blacks", v) }

                            SectionHeader { title: "Presence" }
                            AdjustSlider { label: "Clarity"; value: adj("clarity"); onMoved: (v) => lab.setValue("clarity", v) }
                            AdjustSlider { label: "Dehaze"; value: adj("dehaze"); onMoved: (v) => lab.setValue("dehaze", v) }
                            AdjustSlider { label: "Vibrance"; value: adj("vibrance"); onMoved: (v) => lab.setValue("vibrance", v) }
                            AdjustSlider { label: "Saturation"; value: adj("saturation"); onMoved: (v) => lab.setValue("saturation", v) }

                            Button {
                                text: "Reset all"
                                flat: true
                                Layout.alignment: Qt.AlignRight
                                onClicked: lab.resetAdjustments()
                            }
                            Item { implicitHeight: 12 }
                        }
                    }

                    // ---------------------------------------------------- looks
                    ScrollView {
                        id: looksScroll
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: looksScroll.availableWidth - 2 * Theme.gutter
                            x: Theme.gutter
                            spacing: 6

                            SectionHeader { title: "LUT stack" }
                            Text {
                                visible: lab.luts.length === 0
                                text: "No LUTs yet. Add .cube, .3dl or .png LUTs; each one stacks on top of the last."
                                color: Theme.textDim
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                            Repeater {
                                model: lab.luts
                                delegate: Rectangle {
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    implicitHeight: lutCol.implicitHeight + 16
                                    radius: 6
                                    color: Theme.panelRaised
                                    ColumnLayout {
                                        id: lutCol
                                        anchors.fill: parent
                                        anchors.margins: 8
                                        spacing: 0
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                text: (index + 1) + ". " + modelData.file.replace(/\.[^.]+$/, "")
                                                color: Theme.text
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            ToolButton {
                                                text: "✕"
                                                onClicked: lab.removeLut(index)
                                            }
                                        }
                                        AdjustSlider {
                                            label: "Strength"
                                            from: 0; to: 100; neutral: 100; unit: " %"
                                            value: modelData.strength
                                            onMoved: (v) => lab.setLutStrength(index, v)
                                        }
                                    }
                                }
                            }
                            Button {
                                text: "+ Add LUT"
                                highlighted: true
                                onClicked: lutDialog.open()
                            }

                            SectionHeader { title: "Styles" }
                            RowLayout {
                                Layout.fillWidth: true
                                TextField {
                                    id: styleFilter
                                    placeholderText: "search styles"
                                    Layout.fillWidth: true
                                }
                                ToolButton {
                                    text: "Import"
                                    onClicked: styleDialog.open()
                                }
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: 6
                                Repeater {
                                    model: [""].concat(lab.styles.filter(s => s.toLowerCase().indexOf(styleFilter.text.toLowerCase()) >= 0))
                                    delegate: Button {
                                        required property string modelData
                                        text: modelData === "" ? "none" : modelData.replace(/^.*\|/, "")
                                        checkable: true
                                        checked: lab.currentStyle === modelData
                                        highlighted: checked
                                        font.capitalization: Font.MixedCase
                                        onClicked: lab.applyStyle(modelData)
                                    }
                                }
                            }
                            Item { implicitHeight: 12 }
                        }
                    }

                    // --------------------------------------------------- detail
                    ScrollView {
                        id: detailScroll
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: detailScroll.availableWidth - 2 * Theme.gutter
                            x: Theme.gutter
                            spacing: 4
                            SectionHeader { title: "Noise reduction" }
                            AdjustSlider {
                                label: "Noise reduction"
                                from: 0; to: 100
                                value: adj("noise")
                                onMoved: (v) => lab.setValue("noise", v)
                            }
                            Text {
                                text: "Uses darktable's profiled denoise for your camera model. AI denoise comes in a later build."
                                color: Theme.textDim
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                    }

                    // --------------------------------------------------- optics
                    ScrollView {
                        id: opticsScroll
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: opticsScroll.availableWidth - 2 * Theme.gutter
                            x: Theme.gutter
                            spacing: 4
                            SectionHeader { title: "Lens" }
                            Switch {
                                text: "Lens correction (automatic, from the camera and lens data)"
                                checked: adj("lens")
                                onToggled: lab.setValue("lens", checked)
                                Layout.fillWidth: true
                            }
                            SectionHeader { title: "Geometry" }
                            AdjustSlider {
                                label: "Straighten"
                                from: -10; to: 10; stepSize: 0.1; decimals: 1; unit: "°"
                                value: adj("straighten")
                                onMoved: (v) => lab.setValue("straighten", v)
                            }
                        }
                    }
                }
            }
        }
    }

    }

    Shortcut { sequences: ["Right"]; enabled: lab.mode === "develop"; onActivated: lab.openNeighbour(1) }
    Shortcut { sequences: ["Left"]; enabled: lab.mode === "develop"; onActivated: lab.openNeighbour(-1) }
    Shortcut { sequences: [StandardKey.Copy]; enabled: lab.mode === "develop"; onActivated: lab.copyEdit() }

    FileDialog {
        id: openDialog
        title: "Open a photo"
        nameFilters: ["Photos (*.cr2 *.cr3 *.nef *.arw *.raf *.orf *.rw2 *.dng *.pef *.srw *.jpg *.jpeg *.tif *.tiff *.png *.heic *.avif)", "All files (*)"]
        onAccepted: lab.openFile(selectedFile)
    }
    FileDialog {
        id: exportDialog
        title: "Export"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "jpg"
        nameFilters: ["JPEG (*.jpg)", "TIFF (*.tif)", "PNG (*.png)", "WebP (*.webp)"]
        onAccepted: lab.exportTo(selectedFile, 92)
    }
    FileDialog {
        id: lutDialog
        title: "Add a LUT"
        nameFilters: ["LUTs (*.cube *.3dl *.png)", "All files (*)"]
        onAccepted: lab.addLut(selectedFile)
    }
    FileDialog {
        id: styleDialog
        title: "Import darktable styles"
        nameFilters: ["darktable styles (*.dtstyle)"]
        fileMode: FileDialog.OpenFiles
        onAccepted: selectedFiles.forEach(f => lab.importStyles(f))
    }
}
