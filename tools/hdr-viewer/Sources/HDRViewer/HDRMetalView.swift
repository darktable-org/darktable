import AppKit
import Metal
import QuartzCore

/// An NSView subclass that hosts a CAMetalLayer configured for EDR (Extended Dynamic Range).
/// Receives linear BT.2020 float data, uploads it to a texture, and renders it via a
/// Metal render pipeline that converts to Display-P3 linear and outputs RGBA16Float for EDR.
final class HDRMetalView: NSView {

    // MARK: - Metal objects

    private var device: MTLDevice!
    private var commandQueue: MTLCommandQueue!
    private var renderPipeline: MTLRenderPipelineState!
    private var samplerState: MTLSamplerState!

    /// The source texture in RGBA32Float (linear BT.2020, padded to RGBA)
    private var sourceTexture: MTLTexture?

    /// Uniform buffer carrying the EDR headroom value passed to the shader
    private var uniformBuffer: MTLBuffer!

    private struct Uniforms {
        var edrHeadroom: Float
        var _pad0: Float = 0
        var _pad1: Float = 0
        var _pad2: Float = 0
    }

    // MARK: - Metal layer

    override var wantsUpdateLayer: Bool { true }

    private var metalLayer: CAMetalLayer {
        return layer as! CAMetalLayer
    }

    // MARK: - Init

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        commonInit()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        commonInit()
    }

    private func commonInit() {
        wantsLayer = true

        guard let device = MTLCreateSystemDefaultDevice() else {
            fatalError("No Metal-capable GPU found.")
        }
        self.device = device

        setupLayer()
        setupMetal()
    }

    // MARK: - Layer setup

    override func makeBackingLayer() -> CALayer {
        return CAMetalLayer()
    }

    private func setupLayer() {
        let ml = metalLayer
        ml.device = device
        // RGBA16Float is required for EDR values above 1.0
        ml.pixelFormat = .rgba16Float
        ml.framebufferOnly = true

        // Extended Dynamic Range: allow values above 1.0 to reach the display
        ml.wantsExtendedDynamicRangeContent = true

        // Use the extended linear Display-P3 colorspace so Metal values map correctly
        // to physical display output without OS-level tone mapping.
        if let cs = CGColorSpace(name: CGColorSpace.extendedLinearDisplayP3) {
            ml.colorspace = cs
        }

        ml.contentsScale = NSScreen.main?.backingScaleFactor ?? 2.0
        ml.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
    }

    // MARK: - Metal setup

    private func setupMetal() {
        commandQueue = device.makeCommandQueue()!

        // Compile shaders from the embedded source string at runtime.
        // This avoids SPM resource bundle complexities and works in all contexts.
        let library: MTLLibrary
        do {
            library = try device.makeLibrary(source: metalShaderSource, options: nil)
        } catch {
            fatalError("Failed to compile Metal shaders: \(error)")
        }

        guard
            let vertexFn   = library.makeFunction(name: "vertexPassthrough"),
            let fragmentFn = library.makeFunction(name: "fragmentHDR")
        else {
            fatalError("Metal shader functions not found. Ensure Shaders.metal is included in the target.")
        }

        let pipelineDesc = MTLRenderPipelineDescriptor()
        pipelineDesc.vertexFunction   = vertexFn
        pipelineDesc.fragmentFunction = fragmentFn
        // Output pixel format must match the CAMetalLayer
        pipelineDesc.colorAttachments[0].pixelFormat = .rgba16Float

        do {
            renderPipeline = try device.makeRenderPipelineState(descriptor: pipelineDesc)
        } catch {
            fatalError("Failed to create render pipeline: \(error)")
        }

        // Bilinear sampler – good quality for scaling the image to window size
        let samplerDesc = MTLSamplerDescriptor()
        samplerDesc.minFilter             = .linear
        samplerDesc.magFilter             = .linear
        samplerDesc.mipFilter             = .notMipmapped
        samplerDesc.sAddressMode          = .clampToEdge
        samplerDesc.tAddressMode          = .clampToEdge
        samplerState = device.makeSamplerState(descriptor: samplerDesc)!

        // Uniform buffer (single struct, reused every frame)
        uniformBuffer = device.makeBuffer(
            length: MemoryLayout<Uniforms>.stride,
            options: .storageModeShared
        )!
    }

    // MARK: - Texture upload

    /// Called from the main thread with new pixel data from darktable.
    /// `pixels` is interleaved RGB float32 in linear BT.2020, row-major, top-to-bottom.
    func updateTexture(width: Int, height: Int, pixels: [Float]) {
        guard width > 0, height > 0 else { return }

        // (Re)create the texture if dimensions changed
        if sourceTexture == nil
            || sourceTexture!.width  != width
            || sourceTexture!.height != height
        {
            let desc = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba32Float,   // Metal does not support RGB32Float natively
                width:  width,
                height: height,
                mipmapped: false
            )
            desc.usage = [.shaderRead]
            desc.storageMode = .shared
            sourceTexture = device.makeTexture(descriptor: desc)!
        }

        guard let tex = sourceTexture else { return }

        // Expand RGB → RGBA (Metal has no native RGB32Float texture format)
        let pixelCount = width * height
        var rgba = [Float](repeating: 1.0, count: pixelCount * 4)
        for i in 0 ..< pixelCount {
            rgba[i * 4 + 0] = pixels[i * 3 + 0]
            rgba[i * 4 + 1] = pixels[i * 3 + 1]
            rgba[i * 4 + 2] = pixels[i * 3 + 2]
            rgba[i * 4 + 3] = 1.0
        }

        rgba.withUnsafeBytes { ptr in
            tex.replace(
                region: MTLRegionMake2D(0, 0, width, height),
                mipmapLevel: 0,
                withBytes: ptr.baseAddress!,
                bytesPerRow: width * 4 * MemoryLayout<Float>.size
            )
        }

        render()
    }

    // MARK: - Rendering

    private func render() {
        guard
            let drawable = metalLayer.nextDrawable(),
            let texture  = sourceTexture
        else { return }

        // Read current EDR headroom from the screen
        let headroom = Float(
            window?.screen?.maximumExtendedDynamicRangeColorComponentValue ?? 1.0
        )

        // Write uniforms
        var uniforms = Uniforms(edrHeadroom: headroom)
        memcpy(uniformBuffer.contents(), &uniforms, MemoryLayout<Uniforms>.stride)

        let rpDesc = MTLRenderPassDescriptor()
        rpDesc.colorAttachments[0].texture    = drawable.texture
        rpDesc.colorAttachments[0].loadAction  = .clear
        rpDesc.colorAttachments[0].storeAction = .store
        rpDesc.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1)

        guard
            let cmdBuf  = commandQueue.makeCommandBuffer(),
            let encoder = cmdBuf.makeRenderCommandEncoder(descriptor: rpDesc)
        else { return }

        encoder.setRenderPipelineState(renderPipeline)
        encoder.setFragmentTexture(texture, index: 0)
        encoder.setFragmentSamplerState(samplerState, index: 0)
        encoder.setFragmentBuffer(uniformBuffer, offset: 0, index: 0)

        // Full-screen triangle (no vertex buffer needed; positions generated in vertex shader)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)

        encoder.endEncoding()
        cmdBuf.present(drawable)
        cmdBuf.commit()
    }

    // MARK: - Layout

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        let scale = window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2.0
        metalLayer.drawableSize = CGSize(
            width:  newSize.width  * scale,
            height: newSize.height * scale
        )
        // Defer render until the next run loop pass so the CAMetalLayer drawable
        // pool has time to resize before we request a new drawable.
        if sourceTexture != nil {
            DispatchQueue.main.async { [weak self] in self?.render() }
        }
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if let scale = window?.backingScaleFactor {
            metalLayer.contentsScale = scale
        }
    }
}
