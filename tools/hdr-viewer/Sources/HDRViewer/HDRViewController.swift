import AppKit
import Metal

/// Ties together the Metal view and the IPC server.
/// Receives frames from darktable via Unix socket and forwards them to the Metal view.
final class HDRViewController: NSViewController {

    private var hdrView: HDRMetalView!
    private var ipcServer: IPCServer!

    // Track the current image aspect ratio for window resize constraints
    private var imageAspectRatio: CGFloat = 4.0 / 3.0

    // Status label shown while waiting for the first frame
    private var statusLabel: NSTextField!

    override func loadView() {
        // Create a plain backing view; the HDRMetalView will fill it.
        view = NSView(frame: NSRect(x: 0, y: 0, width: 800, height: 600))
        view.wantsLayer = true
        view.layer?.backgroundColor = NSColor.black.cgColor
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        setupHDRView()
        setupStatusLabel()
        startIPCServer()
    }

    // MARK: - Setup

    private func setupHDRView() {
        hdrView = HDRMetalView(frame: view.bounds)
        hdrView.autoresizingMask = [.width, .height]
        view.addSubview(hdrView)
    }

    private func setupStatusLabel() {
        statusLabel = NSTextField(labelWithString: "Waiting for darktable…\nSocket: /tmp/dt_hdr_viewer.sock")
        statusLabel.alignment = .center
        statusLabel.textColor = NSColor.secondaryLabelColor
        statusLabel.font = NSFont.systemFont(ofSize: 16, weight: .light)
        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        statusLabel.maximumNumberOfLines = 0

        view.addSubview(statusLabel)
        NSLayoutConstraint.activate([
            statusLabel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            statusLabel.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            statusLabel.widthAnchor.constraint(lessThanOrEqualTo: view.widthAnchor, constant: -40)
        ])
    }

    private func startIPCServer() {
        ipcServer = IPCServer(socketPath: IPCServer.defaultSocketPath)
        ipcServer.onFrame = { [weak self] width, height, pixels in
            self?.handleFrame(width: width, height: height, pixels: pixels)
        }
        ipcServer.start()
    }

    // MARK: - Frame handling

    private func handleFrame(width: UInt32, height: UInt32, pixels: [Float]) {
        // Update the Metal view on the main thread
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }

            // Hide the status label once we have a real frame
            if !self.statusLabel.isHidden {
                self.statusLabel.isHidden = true
            }

            let w = Int(width)
            let h = Int(height)

            // Update aspect ratio and resize window if this is the first frame
            // or the image dimensions changed.
            let newAspect = CGFloat(w) / CGFloat(h)
            if abs(newAspect - self.imageAspectRatio) > 0.001 {
                self.imageAspectRatio = newAspect
                self.adjustWindowForAspectRatio()
            }

            self.hdrView.updateTexture(width: w, height: h, pixels: pixels)
        }
    }

    private func adjustWindowForAspectRatio() {
        guard let window = view.window else { return }
        // Keep current width, adjust height to match aspect ratio
        let currentWidth = window.frame.width
        let newHeight = currentWidth / imageAspectRatio
        var frame = window.frame
        frame.size.height = newHeight + window.titlebarHeight
        window.setFrame(frame, display: true, animate: false)

        // Set content aspect ratio so dragging corners maintains it
        window.contentAspectRatio = NSSize(width: imageAspectRatio, height: 1.0)
    }
}

// MARK: - NSWindow titlebar height helper
private extension NSWindow {
    var titlebarHeight: CGFloat {
        frame.height - contentLayoutRect.height
    }
}
