/*
 * Liu — Metal renderer
 * Instanced glyph rendering + batched rects, same 4-pass architecture as OpenGL version.
 */

import Metal
import MetalKit
import CoreText
import CoreGraphics

// MARK: - Glyph Instance (matches shader struct)
struct GlyphInstance {
    var pos: SIMD2<Float>
    var uv: SIMD4<Float>   // u0, v0, u1, v1
    var fg: SIMD3<Float>
}

// MARK: - Rect Vertex (matches shader struct)
struct RectVertex {
    var pos: SIMD2<Float>
    var color: SIMD4<Float>
}

// MARK: - Uniforms
struct Uniforms {
    var projection: simd_float4x4
    var cellSize: SIMD2<Float>
    var _pad: SIMD2<Float> = .zero
}

// MARK: - Glyph UV Cache
struct GlyphUV {
    var u0: Float, v0: Float, u1: Float, v1: Float
}

// MARK: - LiuRenderer
class LiuRenderer {
    let device: MTLDevice
    let commandQueue: MTLCommandQueue

    // Pipelines
    var glyphPipeline: MTLRenderPipelineState!
    var rectPipeline: MTLRenderPipelineState!
    var sampler: MTLSamplerState!

    // Glyph instancing
    let maxGlyphs = 4096
    var glyphBuffer: MTLBuffer!
    var glyphCount = 0
    var glyphInstances: UnsafeMutablePointer<GlyphInstance>!

    // Quad geometry
    var quadBuffer: MTLBuffer!

    // Rect batching
    let maxRects = 2048
    var rectBuffer: MTLBuffer!
    var rectCount = 0
    var rectVertices: UnsafeMutablePointer<RectVertex>!

    // Font atlas
    var atlasTexture: MTLTexture!
    let atlasSize = 1024
    var glyphCache: [UInt32: GlyphUV] = [:]
    var atlasX = 0, atlasY = 0, atlasRowH = 0

    // Font
    var ctFont: CTFont?
    var fallbackFonts: [CTFont] = []
    var cellWidth: Float = 0
    var cellHeight: Float = 0
    var fontAscent: Float = 0

    // Screen state
    var screenWidth: Int = 0
    var screenHeight: Int = 0
    var dpiScale: Float = 1.0

    // Current frame
    var currentDrawable: CAMetalDrawable?
    var currentCommandBuffer: MTLCommandBuffer?
    var currentEncoder: MTLRenderCommandEncoder?
    var uniforms = Uniforms(projection: .init(1), cellSize: .zero)

    init?(device: MTLDevice) {
        self.device = device
        guard let cq = device.makeCommandQueue() else { return nil }
        self.commandQueue = cq

        setupPipelines()
        setupBuffers()
        setupAtlas()
        setupSampler()
    }

    // MARK: - Setup

    private func setupPipelines() {
        guard let library = try? device.makeLibrary(filepath: Bundle.main.path(forResource: "LiuShaders", ofType: "metallib") ?? "") ??
              device.makeDefaultLibrary() else {
            // Try loading from file next to executable
            let execPath = ProcessInfo.processInfo.arguments[0]
            let dir = (execPath as NSString).deletingLastPathComponent
            let metallib = dir + "/LiuShaders.metallib"
            guard let lib = try? device.makeLibrary(filepath: metallib) else {
                fatalError("Cannot load Metal shaders")
            }
            setupPipelinesFromLibrary(lib)
            return
        }
        setupPipelinesFromLibrary(library)
    }

    private func setupPipelinesFromLibrary(_ library: MTLLibrary) {
        // Glyph pipeline
        let glyphDesc = MTLRenderPipelineDescriptor()
        glyphDesc.vertexFunction = library.makeFunction(name: "glyph_vertex")
        glyphDesc.fragmentFunction = library.makeFunction(name: "glyph_fragment")
        glyphDesc.colorAttachments[0].pixelFormat = .bgra8Unorm
        glyphDesc.colorAttachments[0].isBlendingEnabled = true
        glyphDesc.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
        glyphDesc.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
        glyphDesc.colorAttachments[0].sourceAlphaBlendFactor = .sourceAlpha
        glyphDesc.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
        glyphPipeline = try! device.makeRenderPipelineState(descriptor: glyphDesc)

        // Rect pipeline
        let rectDesc = MTLRenderPipelineDescriptor()
        rectDesc.vertexFunction = library.makeFunction(name: "rect_vertex")
        rectDesc.fragmentFunction = library.makeFunction(name: "rect_fragment")
        rectDesc.colorAttachments[0].pixelFormat = .bgra8Unorm
        rectDesc.colorAttachments[0].isBlendingEnabled = true
        rectDesc.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
        rectDesc.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
        rectDesc.colorAttachments[0].sourceAlphaBlendFactor = .sourceAlpha
        rectDesc.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
        rectPipeline = try! device.makeRenderPipelineState(descriptor: rectDesc)
    }

    private func setupBuffers() {
        // Unit quad (6 vertices, 2 triangles)
        let quad: [SIMD2<Float>] = [
            SIMD2(0, 0), SIMD2(1, 0), SIMD2(1, 1),
            SIMD2(0, 0), SIMD2(1, 1), SIMD2(0, 1)
        ]
        quadBuffer = device.makeBuffer(bytes: quad, length: MemoryLayout<SIMD2<Float>>.stride * 6, options: .storageModeShared)

        // Instance buffer
        glyphBuffer = device.makeBuffer(length: MemoryLayout<GlyphInstance>.stride * maxGlyphs, options: .storageModeShared)
        glyphInstances = glyphBuffer.contents().bindMemory(to: GlyphInstance.self, capacity: maxGlyphs)

        // Rect buffer
        rectBuffer = device.makeBuffer(length: MemoryLayout<RectVertex>.stride * maxRects * 6, options: .storageModeShared)
        rectVertices = rectBuffer.contents().bindMemory(to: RectVertex.self, capacity: maxRects * 6)
    }

    private func setupAtlas() {
        let desc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .r8Unorm,
            width: atlasSize, height: atlasSize,
            mipmapped: false
        )
        desc.usage = [.shaderRead]
        atlasTexture = device.makeTexture(descriptor: desc)
    }

    private func setupSampler() {
        let desc = MTLSamplerDescriptor()
        desc.minFilter = .linear
        desc.magFilter = .linear
        desc.sAddressMode = .clampToEdge
        desc.tAddressMode = .clampToEdge
        sampler = device.makeSamplerState(descriptor: desc)
    }

    // MARK: - Font Loading

    func loadFont(path: String, size: Float, dpi: Float) {
        self.dpiScale = dpi
        let pixelH = size * dpi

        // Create CTFont from file
        if let url = CFURLCreateWithFileSystemPath(nil, path as CFString, .cfurlposixPathStyle, false),
           let provider = CGDataProvider(url: url),
           let cgFont = CGFont(provider) {
            ctFont = CTFontCreateWithGraphicsFont(cgFont, CGFloat(pixelH), nil, nil)
        }

        guard let font = ctFont else { return }

        // Metrics from Core Text
        let ascent = CTFontGetAscent(font)
        let descent = CTFontGetDescent(font)
        let leading = CTFontGetLeading(font)
        fontAscent = ceil(Float(ascent))
        cellHeight = ceil(Float(ascent + descent + leading))

        // Cell width from 'M' advance
        var glyph: CGGlyph = 0
        var mChar: UniChar = 0x4D // 'M'
        CTFontGetGlyphsForCharacters(font, &mChar, &glyph, 1)
        var advance = CGSize.zero
        CTFontGetAdvancesForGlyphs(font, .horizontal, &glyph, &advance, 1)
        cellWidth = ceil(Float(advance.width))

        // Clear cache
        glyphCache.removeAll()
        atlasX = 0; atlasY = 0; atlasRowH = 0

        // Pre-rasterize ASCII + box drawing
        for cp: UInt32 in 32..<127 { _ = rasterizeGlyph(cp) }
        for cp: UInt32 in 0x2500...0x257F { _ = rasterizeGlyph(cp) }
        for cp: UInt32 in 0x2580...0x259F { _ = rasterizeGlyph(cp) }
    }

    // MARK: - Glyph Rasterization (Core Text)

    func rasterizeGlyph(_ cp: UInt32) -> GlyphUV? {
        let cw = Int(cellWidth)
        let ch = Int(cellHeight)
        guard cw > 0 && ch > 0 else { return nil }

        // Atlas packing
        if atlasX + cw + 2 > atlasSize {
            atlasX = 0; atlasY += atlasRowH + 2; atlasRowH = 0
        }
        guard atlasY + ch + 2 <= atlasSize else { return nil }

        // Rasterize with Core Text
        var bitmap = [UInt8](repeating: 0, count: cw * ch)

        if let font = ctFont {
            var uc = UniChar(cp)
            var glyph: CGGlyph = 0
            let found = CTFontGetGlyphsForCharacters(font, &uc, &glyph, 1)

            if found || cp == 32 {
                let cs = CGColorSpace(name: CGColorSpace.linearGray)!
                if let ctx = CGContext(data: &bitmap, width: cw, height: ch,
                                        bitsPerComponent: 8, bytesPerRow: cw,
                                        space: cs, bitmapInfo: CGImageAlphaInfo.none.rawValue) {
                    ctx.setAllowsAntialiasing(true)
                    ctx.setShouldAntialias(true)
                    ctx.setAllowsFontSmoothing(true)
                    ctx.setShouldSmoothFonts(true)
                    ctx.setGrayFillColor(1.0, alpha: 1.0)

                    let pos = CGPoint(x: 0, y: CGFloat(ch) - CGFloat(fontAscent))
                    CTFontDrawGlyphs(font, &glyph, &[pos], 1, ctx)
                }
            }
        }

        // Upload to atlas
        let ax = atlasX + 1
        let ay = atlasY + 1
        let region = MTLRegion(origin: MTLOrigin(x: ax, y: ay, z: 0),
                                size: MTLSize(width: cw, height: ch, depth: 1))
        atlasTexture.replace(region: region, mipmapLevel: 0, withBytes: bitmap, bytesPerRow: cw)

        // UV
        let u0 = Float(ax) / Float(atlasSize)
        let v0 = Float(ay) / Float(atlasSize)
        let u1 = Float(ax + cw) / Float(atlasSize)
        let v1 = Float(ay + ch) / Float(atlasSize)

        atlasX += cw + 2
        if ch + 2 > atlasRowH { atlasRowH = ch + 2 }

        let uv = GlyphUV(u0: u0, v0: v0, u1: u1, v1: v1)
        glyphCache[cp] = uv
        return uv
    }

    func getGlyphUV(_ cp: UInt32) -> GlyphUV? {
        if let cached = glyphCache[cp] { return cached }
        return rasterizeGlyph(cp)
    }

    // MARK: - Frame Rendering

    func beginFrame(view: MTKView, clearColor: MTLClearColor) {
        screenWidth = Int(view.drawableSize.width)
        screenHeight = Int(view.drawableSize.height)
        glyphCount = 0
        rectCount = 0

        currentCommandBuffer = commandQueue.makeCommandBuffer()
        currentDrawable = view.currentDrawable

        guard let rpd = view.currentRenderPassDescriptor else { return }
        rpd.colorAttachments[0].clearColor = clearColor
        rpd.colorAttachments[0].loadAction = .clear
        rpd.colorAttachments[0].storeAction = .store

        currentEncoder = currentCommandBuffer?.makeRenderCommandEncoder(descriptor: rpd)

        // Orthographic projection (Y flipped: top=0)
        let w = Float(screenWidth), h = Float(screenHeight)
        uniforms.projection = simd_float4x4(columns: (
            SIMD4( 2/w,    0,  0, 0),
            SIMD4(   0, -2/h,  0, 0),
            SIMD4(   0,    0, -1, 0),
            SIMD4(  -1,    1,  0, 1)
        ))
        uniforms.cellSize = SIMD2(cellWidth, cellHeight)
    }

    func pushGlyph(x: Float, y: Float, codepoint: UInt32, fg: SIMD3<Float>) {
        guard codepoint > 32, glyphCount < maxGlyphs else { return }
        guard let uv = getGlyphUV(codepoint) else { return }

        glyphInstances[glyphCount] = GlyphInstance(
            pos: SIMD2(x, y),
            uv: SIMD4(uv.u0, uv.v0, uv.u1, uv.v1),
            fg: fg
        )
        glyphCount += 1
    }

    func drawRect(x: Float, y: Float, w: Float, h: Float, color: SIMD4<Float>) {
        guard rectCount < maxRects else { return }
        let base = rectCount * 6
        let v = [
            RectVertex(pos: SIMD2(x, y), color: color),
            RectVertex(pos: SIMD2(x+w, y), color: color),
            RectVertex(pos: SIMD2(x+w, y+h), color: color),
            RectVertex(pos: SIMD2(x, y), color: color),
            RectVertex(pos: SIMD2(x+w, y+h), color: color),
            RectVertex(pos: SIMD2(x, y+h), color: color),
        ]
        for i in 0..<6 { rectVertices[base + i] = v[i] }
        rectCount += 1
    }

    func flushRects() {
        guard rectCount > 0, let encoder = currentEncoder else { return }
        encoder.setRenderPipelineState(rectPipeline)
        encoder.setVertexBuffer(rectBuffer, offset: 0, index: 0)
        encoder.setVertexBytes(&uniforms, length: MemoryLayout<Uniforms>.size, index: 1)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: rectCount * 6)
        rectCount = 0
    }

    func flushGlyphs() {
        guard glyphCount > 0, let encoder = currentEncoder else { return }
        encoder.setRenderPipelineState(glyphPipeline)
        encoder.setVertexBuffer(quadBuffer, offset: 0, index: 0)
        encoder.setVertexBuffer(glyphBuffer, offset: 0, index: 1)
        encoder.setVertexBytes(&uniforms, length: MemoryLayout<Uniforms>.size, index: 2)
        encoder.setFragmentTexture(atlasTexture, index: 0)
        encoder.setFragmentSamplerState(sampler, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 6, instanceCount: glyphCount)
        glyphCount = 0
    }

    func endFrame() {
        flushRects()
        flushGlyphs()
        currentEncoder?.endEncoding()
        if let drawable = currentDrawable {
            currentCommandBuffer?.present(drawable)
        }
        currentCommandBuffer?.commit()
        currentEncoder = nil
        currentCommandBuffer = nil
        currentDrawable = nil
    }

    // MARK: - UI Scale

    func setUIScale(cw: Float, ch: Float) {
        flushGlyphs()
        uniforms.cellSize = SIMD2(cw, ch)
    }

    func resetUIScale() {
        flushGlyphs()
        uniforms.cellSize = SIMD2(cellWidth, cellHeight)
    }
}
