import AppKit

// Ensure we run on the main thread
let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate

// Set activation policy before running
app.setActivationPolicy(.regular)

app.run()
