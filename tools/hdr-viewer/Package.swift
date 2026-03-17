// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "HDRViewer",
    platforms: [
        .macOS(.v12)
    ],
    targets: [
        .executableTarget(
            name: "HDRViewer",
            path: "Sources/HDRViewer",
            exclude: ["Shaders.metal"],
            linkerSettings: [
                .linkedFramework("Metal"),
                .linkedFramework("AppKit"),
                .linkedFramework("CoreGraphics"),
                .linkedFramework("QuartzCore")
            ]
        )
    ]
)
