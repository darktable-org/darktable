import Foundation
import Darwin

/// Listens on a Unix domain socket and decodes pixel frames sent by darktable.
///
/// Packet format (little-endian):
///   [4 bytes] width  – UInt32
///   [4 bytes] height – UInt32
///   [width * height * 3 * 4 bytes] – Float32 RGB, linear BT.2020, row-major top-to-bottom
final class IPCServer {

    static let defaultSocketPath = "/tmp/dt_hdr_viewer.sock"

    /// Called on a background thread with each decoded frame.
    var onFrame: ((_ width: UInt32, _ height: UInt32, _ pixels: [Float]) -> Void)?

    private let socketPath: String
    private var serverFD: Int32 = -1
    private var isRunning = false
    private let queue = DispatchQueue(label: "com.darktable.hdr-viewer.ipc", qos: .userInteractive)

    init(socketPath: String = IPCServer.defaultSocketPath) {
        self.socketPath = socketPath
    }

    deinit {
        stop()
    }

    // MARK: - Start / Stop

    func start() {
        guard !isRunning else { return }
        isRunning = true
        queue.async { [weak self] in
            self?.runAcceptLoop()
        }
    }

    func stop() {
        isRunning = false
        if serverFD >= 0 {
            Darwin.close(serverFD)
            serverFD = -1
        }
        unlink(socketPath)
    }

    // MARK: - Accept loop

    private func runAcceptLoop() {
        // Remove stale socket file
        unlink(socketPath)

        // Create UNIX domain socket
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else {
            printErr("IPCServer: socket() failed: \(String(cString: strerror(errno)))")
            return
        }
        serverFD = fd

        // Set SO_REUSEADDR
        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))

        // Bind
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        withUnsafeMutableBytes(of: &addr.sun_path) { ptr in
            socketPath.withCString { cstr in
                _ = strncpy(ptr.baseAddress!.assumingMemoryBound(to: CChar.self),
                            cstr,
                            ptr.count - 1)
            }
        }

        let bindResult = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard bindResult == 0 else {
            printErr("IPCServer: bind() failed: \(String(cString: strerror(errno)))")
            Darwin.close(fd)
            return
        }

        // Listen (backlog = 4; darktable typically sends one frame at a time)
        guard listen(fd, 4) == 0 else {
            printErr("IPCServer: listen() failed: \(String(cString: strerror(errno)))")
            Darwin.close(fd)
            return
        }

        print("IPCServer: listening on \(socketPath)")

        while isRunning {
            var clientAddr = sockaddr_un()
            var clientAddrLen = socklen_t(MemoryLayout<sockaddr_un>.size)

            let clientFD = withUnsafeMutablePointer(to: &clientAddr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    accept(fd, $0, &clientAddrLen)
                }
            }

            guard clientFD >= 0 else {
                if errno == EINTR || errno == EBADF { break }
                printErr("IPCServer: accept() failed: \(String(cString: strerror(errno)))")
                continue
            }

            // Handle each client on the same serial queue (darktable sends one frame per connection)
            handleClient(clientFD)
        }

        Darwin.close(fd)
        unlink(socketPath)
        print("IPCServer: stopped.")
    }

    // MARK: - Client handling

    private func handleClient(_ fd: Int32) {
        defer { Darwin.close(fd) }

        // Read header: 4+4 bytes
        var width:  UInt32 = 0
        var height: UInt32 = 0

        guard readExact(fd: fd, into: &width,  count: 4),
              readExact(fd: fd, into: &height, count: 4)
        else {
            printErr("IPCServer: failed to read header")
            return
        }

        // Ensure little-endian (darktable sends LE)
        width  = UInt32(littleEndian: width)
        height = UInt32(littleEndian: height)

        guard width > 0, height > 0, width <= 32768, height <= 32768 else {
            printErr("IPCServer: invalid dimensions \(width)x\(height)")
            return
        }

        let floatCount = Int(width) * Int(height) * 3
        let byteCount  = floatCount * MemoryLayout<Float>.size

        var pixels = [Float](repeating: 0, count: floatCount)
        guard readExactBytes(fd: fd, buffer: &pixels, byteCount: byteCount) else {
            printErr("IPCServer: failed to read pixel data (\(byteCount) bytes)")
            return
        }

        // On little-endian hosts (all modern Macs) Float byte order is native,
        // so no byte-swapping is needed.

        onFrame?(width, height, pixels)
    }

    // MARK: - Low-level I/O helpers

    /// Read exactly `count` bytes into a value via its raw pointer.
    private func readExact<T>(fd: Int32, into value: inout T, count: Int) -> Bool {
        return withUnsafeMutableBytes(of: &value) { ptr in
            readExactBytes(fd: fd, buffer: ptr.baseAddress!, byteCount: count)
        }
    }

    private func readExactBytes(fd: Int32, buffer: UnsafeMutableRawPointer, byteCount: Int) -> Bool {
        var remaining = byteCount
        var offset    = 0
        while remaining > 0 {
            let n = recv(fd, buffer.advanced(by: offset), remaining, 0)
            if n <= 0 {
                if n == 0 { return false }  // connection closed
                if errno == EINTR { continue }
                return false
            }
            offset    += n
            remaining -= n
        }
        return true
    }

    private func readExactBytes(fd: Int32, buffer: inout [Float], byteCount: Int) -> Bool {
        buffer.withUnsafeMutableBytes { ptr in
            readExactBytes(fd: fd, buffer: ptr.baseAddress!, byteCount: byteCount)
        }
    }
}

// MARK: - Helpers

private func printErr(_ msg: String) {
    let data = ((msg + "\n").data(using: .utf8)) ?? Data()
    FileHandle.standardError.write(data)
}
