// dmg_render_background.swift — render an HTML file to a PNG at an exact pixel
// width using native WebKit (no headless-browser dependency). WKWebView
// re-rasterizes the vector/CSS content at `snapshotWidth`, so a 640pt-wide
// layout captured at snapshotWidth=1280 yields a crisp @2x asset.
//
//   swiftc -O dmg_render_background.swift -o /tmp/dmgrender
//   /tmp/dmgrender <input.html> <output.png> <logicalWidth> <logicalHeight> <pixelWidth>
//
// Used by scripts/dmg_render_background.sh to produce assets/dmg/background.png
// (1x) and background@2x.png (2x) for the styled installer window.

import Cocoa
import WebKit

let args = CommandLine.arguments
guard args.count == 6,
      let lw = Int(args[3]), let lh = Int(args[4]), let pw = Int(args[5]) else {
    FileHandle.standardError.write("usage: dmgrender <html> <png> <logicalW> <logicalH> <pixelW>\n".data(using: .utf8)!)
    exit(64)
}
let htmlPath = args[1]
let outPath  = args[2]

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

final class Renderer: NSObject, WKNavigationDelegate {
    let webView: WKWebView
    let window: NSWindow
    let pixelWidth: Int
    let outPath: String
    init(lw: Int, lh: Int, pixelWidth: Int, outPath: String) {
        self.pixelWidth = pixelWidth
        self.outPath = outPath
        let frame = NSRect(x: 0, y: 0, width: lw, height: lh)
        let cfg = WKWebViewConfiguration()
        webView = WKWebView(frame: frame, configuration: cfg)
        webView.setValue(false, forKey: "drawsBackground")  // honor the page's own bg
        // Off-screen host window so WebKit actually composites the page.
        window = NSWindow(contentRect: frame, styleMask: .borderless,
                          backing: .buffered, defer: false)
        window.contentView = webView
        window.setFrameOrigin(NSPoint(x: -20000, y: -20000))
        window.orderBack(nil)
        super.init()
        webView.navigationDelegate = self
    }
    func load(_ url: URL) {
        webView.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())
    }
    func webView(_ wv: WKWebView, didFinish nav: WKNavigation!) {
        // Let fonts, the radial gradients and the SVG arrow settle.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.7) { self.snapshot() }
    }
    func webView(_ wv: WKWebView, didFail nav: WKNavigation!, withError err: Error) { fail("load failed: \(err)") }
    func webView(_ wv: WKWebView, didFailProvisionalNavigation nav: WKNavigation!, withError err: Error) { fail("provisional load failed: \(err)") }

    func snapshot() {
        let cfg = WKSnapshotConfiguration()
        cfg.rect = webView.bounds
        // snapshotWidth is in POINTS; WebKit multiplies by the backing scale to
        // get pixels. Divide the target pixel width by the scale so the emitted
        // PNG is exactly `pixelWidth` px wide regardless of the host display.
        let scale = max(1.0, window.backingScaleFactor)
        cfg.snapshotWidth = NSNumber(value: Double(pixelWidth) / scale)
        webView.takeSnapshot(with: cfg) { image, error in
            guard let image = image, let tiff = image.tiffRepresentation,
                  let rep = NSBitmapImageRep(data: tiff),
                  let png = rep.representation(using: .png, properties: [:]) else {
                fail("snapshot failed: \(String(describing: error))"); return
            }
            do { try png.write(to: URL(fileURLWithPath: self.outPath)) }
            catch { fail("write failed: \(error)") }
            exit(0)
        }
    }
}

func fail(_ msg: String) {
    FileHandle.standardError.write((msg + "\n").data(using: .utf8)!)
    exit(1)
}

let url = URL(fileURLWithPath: htmlPath)
let r = Renderer(lw: lw, lh: lh, pixelWidth: pw, outPath: outPath)
r.load(url)

// Safety valve: never hang the build.
DispatchQueue.main.asyncAfter(deadline: .now() + 25) { fail("timeout") }
app.run()
