import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {

    private var window: NSWindow?
    private var viewController: HDRViewController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        setupMenu()
        setupWindow()
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return true
    }

    // MARK: - Private

    private func setupWindow() {
        let contentRect = NSRect(x: 0, y: 0, width: 800, height: 600)
        let styleMask: NSWindow.StyleMask = [.titled, .closable, .miniaturizable, .resizable]

        let window = NSWindow(
            contentRect: contentRect,
            styleMask: styleMask,
            backing: .buffered,
            defer: false
        )
        window.title = "darktable HDR Preview"
        window.center()
        window.isReleasedWhenClosed = false

        // Allow the window to display HDR content on HDR-capable displays
        // This is set on the view/layer level, but the window must also allow it.
        if #available(macOS 12.0, *) {
            // Nothing extra required at window level; EDR is opt-in per layer.
        }

        let vc = HDRViewController()
        window.contentViewController = vc
        window.makeKeyAndOrderFront(nil)

        self.window = window
        self.viewController = vc
    }

    private func setupMenu() {
        let mainMenu = NSMenu()

        // App menu
        let appMenuItem = NSMenuItem()
        mainMenu.addItem(appMenuItem)
        let appMenu = NSMenu()
        appMenuItem.submenu = appMenu

        let appName = ProcessInfo.processInfo.processName
        appMenu.addItem(
            withTitle: "About \(appName)",
            action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
            keyEquivalent: ""
        )
        appMenu.addItem(.separator())
        appMenu.addItem(
            withTitle: "Quit \(appName)",
            action: #selector(NSApplication.terminate(_:)),
            keyEquivalent: "q"
        )

        // Window menu
        let windowMenuItem = NSMenuItem()
        mainMenu.addItem(windowMenuItem)
        let windowMenu = NSMenu(title: "Window")
        windowMenuItem.submenu = windowMenu
        windowMenu.addItem(
            withTitle: "Minimize",
            action: #selector(NSWindow.miniaturize(_:)),
            keyEquivalent: "m"
        )
        windowMenu.addItem(
            withTitle: "Zoom",
            action: #selector(NSWindow.zoom(_:)),
            keyEquivalent: ""
        )

        NSApp.mainMenu = mainMenu
        NSApp.windowsMenu = windowMenu
    }
}
