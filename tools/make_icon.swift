// Draws the H89 app icon: a Heathkit H89 seen from the front, with the
// MTR-90 prompt on its screen. Writes a 1024x1024 PNG.
//
//   swift tools/make_icon.swift out.png
//
// tools/make_icons.sh turns it into platform/mac/H89.icns and
// platform/glfw/H89.ico.

import AppKit
import CoreGraphics

let size = 1024
let args = CommandLine.arguments
guard args.count == 2 else {
    FileHandle.standardError.write("usage: make_icon.swift out.png\n".data(using: .utf8)!)
    exit(1)
}

let space = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!

func color(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}

func roundedRect(_ r: CGRect, _ radius: CGFloat) -> CGPath {
    CGPath(roundedRect: r, cornerWidth: radius, cornerHeight: radius, transform: nil)
}

func fill(_ path: CGPath, _ c: CGColor) {
    ctx.addPath(path)
    ctx.setFillColor(c)
    ctx.fillPath()
}

func gradient(_ path: CGPath, _ top: CGColor, _ bottom: CGColor, _ r: CGRect) {
    ctx.saveGState()
    ctx.addPath(path)
    ctx.clip()
    let g = CGGradient(colorsSpace: space, colors: [top, bottom] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(g, start: CGPoint(x: r.midX, y: r.maxY), end: CGPoint(x: r.midX, y: r.minY), options: [])
    ctx.restoreGState()
}

// The case: putty-coloured, inside the standard macOS icon margin.
let body = CGRect(x: 100, y: 130, width: 824, height: 770)
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -14), blur: 28, color: color(0x000000, 0.35))
fill(roundedRect(body, 90), color(0xD9D0BA))
ctx.restoreGState()
gradient(roundedRect(body, 90), color(0xE8E1CF), color(0xC9BFA6), body)

// Keyboard ledge along the bottom.
let ledge = CGRect(x: 100, y: 130, width: 824, height: 190)
ctx.saveGState()
ctx.addPath(roundedRect(body, 90))
ctx.clip()
gradient(CGPath(rect: ledge, transform: nil), color(0x4A4540), color(0x2E2A27), ledge)
// Key rows.
for row in 0..<3 {
    let y = 160 + CGFloat(row) * 48
    let inset = CGFloat(row) * 20
    var x = 150 + inset
    while x < 874 - inset - 40 {
        fill(roundedRect(CGRect(x: x, y: y, width: 40, height: 34), 6), color(0x6B645C))
        x += 50
    }
}
ctx.restoreGState()
// Seam between the case and the keyboard.
fill(CGPath(rect: CGRect(x: 100, y: 318, width: 824, height: 6), transform: nil), color(0x8F866F))

// Screen bezel and tube.
let bezel = CGRect(x: 160, y: 380, width: 500, height: 450)
fill(roundedRect(bezel, 40), color(0x2B2824))
let tube = bezel.insetBy(dx: 34, dy: 34)
let tubePath = roundedRect(tube, 46)
gradient(tubePath, color(0x0E1A10), color(0x050A06), tube)

// The monitor prompt, glowing green.
ctx.saveGState()
ctx.addPath(tubePath)
ctx.clip()
let font = NSFont.monospacedSystemFont(ofSize: 120, weight: .bold)
let green = NSColor(srgbRed: 0.35, green: 1.0, blue: 0.45, alpha: 1)
let text = NSAttributedString(string: "H:", attributes: [.font: font, .foregroundColor: green])
let line = CTLineCreateWithAttributedString(text)
let textPos = CGPoint(x: tube.minX + 46, y: tube.maxY - 170)
ctx.setShadow(offset: .zero, blur: 30, color: color(0x40FF60, 0.9))
ctx.textPosition = textPos
CTLineDraw(line, ctx)
let width = CTLineGetTypographicBounds(line, nil, nil, nil)
fill(CGPath(rect: CGRect(x: textPos.x + width + 22, y: textPos.y - 6, width: 60, height: 16), transform: nil),
     color(0x5AFF73))
ctx.restoreGState()
// Glass highlight.
ctx.saveGState()
ctx.addPath(tubePath)
ctx.clip()
let shine = CGGradient(colorsSpace: space, colors: [color(0xFFFFFF, 0.16), color(0xFFFFFF, 0)] as CFArray,
                       locations: [0, 1])!
ctx.drawLinearGradient(shine, start: CGPoint(x: tube.minX, y: tube.maxY),
                       end: CGPoint(x: tube.midX, y: tube.midY), options: [])
ctx.restoreGState()

// Floppy drive to the right of the screen: door, slot and latch.
let drive = CGRect(x: 700, y: 430, width: 170, height: 350)
fill(roundedRect(drive, 18), color(0xB8AE95))
fill(roundedRect(drive.insetBy(dx: 10, dy: 10), 12), color(0x26231F))
fill(roundedRect(CGRect(x: drive.midX - 8, y: drive.minY + 40, width: 16, height: 270), 8), color(0x0B0A09))
fill(roundedRect(CGRect(x: drive.minX + 30, y: drive.maxY - 90, width: 110, height: 40), 10), color(0x9A917C))

// Power light.
ctx.saveGState()
ctx.setShadow(offset: .zero, blur: 16, color: color(0xFF3B30, 0.9))
ctx.addEllipse(in: CGRect(x: 772, y: 382, width: 26, height: 26))
ctx.setFillColor(color(0xFF4D40))
ctx.fillPath()
ctx.restoreGState()

let image = ctx.makeImage()!
let rep = NSBitmapImageRep(cgImage: image)
let png = rep.representation(using: .png, properties: [:])!
try! png.write(to: URL(fileURLWithPath: args[1]))
