import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

let N = 1024
let cs = CGColorSpace(name: CGColorSpace.sRGB)!
func col(_ hex: UInt32, _ a: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: cs, components: [CGFloat((hex >> 16) & 255) / 255, CGFloat((hex >> 8) & 255) / 255, CGFloat(hex & 255) / 255, a])!
}
func ctx(_ w: Int, _ h: Int) -> CGContext {
    let c = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    c.translateBy(x: 0, y: CGFloat(h)); c.scaleBy(x: 1, y: -1)   // y down, like a design tool
    c.interpolationQuality = .high
    return c
}
func save(_ c: CGContext, _ path: String) {
    let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(d, c.makeImage()!, nil); CGImageDestinationFinalize(d)
}
func grad(_ stops: [(CGFloat, CGColor)]) -> CGGradient {
    CGGradient(colorsSpace: cs, colors: stops.map { $0.1 } as CFArray, locations: stops.map { $0.0 })!
}
let plate = CGRect(x: 100, y: 100, width: 824, height: 824)
let plateR: CGFloat = 186
func platePath() -> CGPath { CGPath(roundedRect: plate, cornerWidth: plateR, cornerHeight: plateR, transform: nil) }

func drawPlate(_ c: CGContext, top: UInt32, bottom: UInt32) {
    c.saveGState()
    c.setShadow(offset: CGSize(width: 0, height: -14), blur: 36, color: col(0x000000, 0.5))
    c.addPath(platePath()); c.setFillColor(col(bottom)); c.fillPath()
    c.restoreGState()
    c.saveGState(); c.addPath(platePath()); c.clip()
    c.drawLinearGradient(grad([(0, col(top)), (1, col(bottom))]), start: CGPoint(x: 0, y: plate.minY), end: CGPoint(x: 0, y: plate.maxY), options: [])
    c.restoreGState()
}
func plateRim(_ c: CGContext) {
    c.saveGState(); c.addPath(platePath()); c.clip()
    c.addPath(CGPath(roundedRect: plate.insetBy(dx: 3, dy: 3), cornerWidth: plateR - 3, cornerHeight: plateR - 3, transform: nil))
    c.replacePathWithStrokedPath(); c.clip()
    c.drawLinearGradient(grad([(0, col(0xffffff, 0.22)), (0.5, col(0xffffff, 0.03)), (1, col(0x000000, 0.25))]),
                         start: CGPoint(x: 0, y: plate.minY), end: CGPoint(x: 0, y: plate.maxY), options: [])
    c.restoreGState()
}
func glow(_ c: CGContext, at p: CGPoint, r: CGFloat, _ hex: UInt32, _ a: CGFloat) {
    c.saveGState(); c.addPath(platePath()); c.clip()
    c.drawRadialGradient(grad([(0, col(hex, a)), (0.55, col(hex, a * 0.28)), (1, col(hex, 0))]),
                         startCenter: p, startRadius: 0, endCenter: p, endRadius: r, options: [])
    c.restoreGState()
}
func fillAmber(_ c: CGContext, _ path: CGPath, from y0: CGFloat, to y1: CGFloat) {
    c.saveGState()
    c.setShadow(offset: CGSize(width: 0, height: -10), blur: 24, color: col(0x000000, 0.45))
    c.addPath(path); c.setFillColor(col(0xf59a2c)); c.fillPath()
    c.restoreGState()
    c.saveGState(); c.addPath(path); c.clip()
    c.drawLinearGradient(grad([(0, col(0xffd27a)), (0.45, col(0xf7a033)), (1, col(0xd9701a))]),
                         start: CGPoint(x: 0, y: y0), end: CGPoint(x: 0, y: y1), options: [])
    c.restoreGState()
}
func fillSilver(_ c: CGContext, _ path: CGPath, from y0: CGFloat, to y1: CGFloat) {
    c.saveGState()
    c.setShadow(offset: CGSize(width: 0, height: -12), blur: 28, color: col(0x000000, 0.55))
    c.addPath(path); c.setFillColor(col(0xb9c0c8)); c.fillPath()
    c.restoreGState()
    c.saveGState(); c.addPath(path); c.clip()
    // brushed silver: bright at the top, a darker band through the middle, a lift at the feet
    c.drawLinearGradient(grad([(0, col(0xf4f6f8)), (0.30, col(0xc3cad2)), (0.62, col(0x8a929c)), (0.80, col(0xa9b1ba)), (1, col(0x6d757f))]),
                         start: CGPoint(x: 0, y: y0), end: CGPoint(x: 0, y: y1), options: [])
    c.restoreGState()
    // a hairline highlight along the top edges
    c.saveGState(); c.addPath(path); c.clip()
    c.addPath(path); c.setStrokeColor(col(0xffffff, 0.55)); c.setLineWidth(5); c.strokePath()
    c.restoreGState()
}
func poly(_ pts: [(CGFloat, CGFloat)]) -> CGPath {
    let p = CGMutablePath(); p.move(to: CGPoint(x: pts[0].0, y: pts[0].1))
    for q in pts.dropFirst() { p.addLine(to: CGPoint(x: q.0, y: q.1)) }
    p.closeSubpath(); return p
}

// A -- the nail and the bar
func variantA() -> CGContext {
    let c = ctx(N, N)
    drawPlate(c, top: 0x323b45, bottom: 0x12161a)
    glow(c, at: CGPoint(x: 512, y: 470), r: 430, 0xff8a1e, 0.42)
    let p = CGMutablePath()
    p.addPath(poly([(462, 236), (562, 236), (548, 330), (512, 800), (476, 330)]))          // the nail: head, then a long taper
    p.addPath(CGPath(roundedRect: CGRect(x: 322, y: 372, width: 380, height: 64), cornerWidth: 10, cornerHeight: 10, transform: nil))
    fillAmber(c, p, from: 236, to: 800)
    plateRim(c); return c
}
// B -- light through a doorway, into fog
func variantB() -> CGContext {
    let c = ctx(N, N)
    drawPlate(c, top: 0x27313b, bottom: 0x0d1013)
    glow(c, at: CGPoint(x: 512, y: 420), r: 470, 0xff8f26, 0.38)
    // the shaft on the floor, widening towards us and dying out
    c.saveGState(); c.addPath(platePath()); c.clip()
    c.addPath(poly([(432, 560), (592, 560), (792, 924), (232, 924)])); c.clip()
    c.drawLinearGradient(grad([(0, col(0xffb04a, 0.85)), (0.55, col(0xf08a24, 0.30)), (1, col(0xf08a24, 0))]),
                         start: CGPoint(x: 0, y: 560), end: CGPoint(x: 0, y: 900), options: [])
    c.restoreGState()
    // the doorway: a tall opening with an arched head
    let d = CGMutablePath()
    d.move(to: CGPoint(x: 432, y: 560)); d.addLine(to: CGPoint(x: 432, y: 300))
    d.addArc(center: CGPoint(x: 512, y: 300), radius: 80, startAngle: .pi, endAngle: 0, clockwise: false)
    d.addLine(to: CGPoint(x: 592, y: 560)); d.closeSubpath()
    c.saveGState(); c.addPath(d); c.clip()
    c.drawLinearGradient(grad([(0, col(0xfff0c8)), (0.5, col(0xffc565)), (1, col(0xf59a2c))]),
                         start: CGPoint(x: 0, y: 220), end: CGPoint(x: 0, y: 560), options: [])
    c.restoreGState()
    plateRim(c); return c
}
// C -- an M of three nails
func variantC() -> CGContext {
    let c = ctx(N, N)
    drawPlate(c, top: 0x323b45, bottom: 0x12161a)
    glow(c, at: CGPoint(x: 512, y: 500), r: 440, 0xff8a1e, 0.40)
    let x0: CGFloat = 262, w: CGFloat = 500, y0: CGFloat = 270, h: CGFloat = 500
    let u: [(CGFloat, CGFloat)] = [(0, 0), (0.23, 0), (0.5, 0.40), (0.77, 0), (1, 0), (0.885, 1), (0.77, 0.34), (0.5, 0.76), (0.23, 0.34), (0.115, 1)]
    fillAmber(c, poly(u.map { (x0 + $0.0 * w, y0 + $0.1 * h) }), from: y0, to: y0 + h)
    plateRim(c); return c
}

func variantS() -> CGContext {
    let c = ctx(N, N)
    drawPlate(c, top: 0x3a444f, bottom: 0x14181d)
    glow(c, at: CGPoint(x: 512, y: 470), r: 460, 0x9fb4c8, 0.22)
    let x0: CGFloat = 252, w: CGFloat = 520, y0: CGFloat = 262, h: CGFloat = 510
    let u: [(CGFloat, CGFloat)] = [(0, 0), (0.23, 0), (0.5, 0.40), (0.77, 0), (1, 0), (0.885, 1), (0.77, 0.34), (0.5, 0.76), (0.23, 0.34), (0.115, 1)]
    fillSilver(c, poly(u.map { (x0 + $0.0 * w, y0 + $0.1 * h) }), from: y0, to: y0 + h)
    plateRim(c); return c
}
let out = CommandLine.arguments[1]
save(variantS(), "\(out)/icon_silver_m.png")
let vs: [(String, CGContext)] = [("a_nail", variantA()), ("b_doorway", variantB()), ("c_nail_m", variantC())]
for (n, c) in vs { save(c, "\(out)/icon_\(n).png") }

// the contact sheet: each variant at 512 / 128 / 64 / 32, on a light and a dark desktop
let sizes: [CGFloat] = [512, 128, 64, 32]
let sheet = ctx(3 * 600, 2 * 560)
for (row, bg) in [UInt32(0xd9dde2), UInt32(0x1c2026)].enumerated() {
    sheet.setFillColor(col(bg)); sheet.fill(CGRect(x: 0, y: row * 560, width: 1800, height: 560))
}
// images are drawn with the context's y flipped, so un-flip each one as it is placed
func place(_ img: CGImage, _ r: CGRect) {
    sheet.saveGState(); sheet.translateBy(x: r.minX, y: r.maxY); sheet.scaleBy(x: 1, y: -1)
    sheet.draw(img, in: CGRect(x: 0, y: 0, width: r.width, height: r.height)); sheet.restoreGState()
}
for (i, (_, c)) in vs.enumerated() {
    let img = c.makeImage()!
    // row 0: the big one on light.  row 1: 128/64/32 on dark, and again small on light above
    place(img, CGRect(x: CGFloat(i) * 600 + 44, y: 24, width: 512, height: 512))
    var x = CGFloat(i) * 600 + 60
    for s in sizes.dropFirst() { place(img, CGRect(x: x, y: 560 + 280 - s / 2 - 60, width: s, height: s)); x += s + 40 }
    x = CGFloat(i) * 600 + 60
    sheet.setFillColor(col(0xd9dde2)); sheet.fill(CGRect(x: CGFloat(i) * 600 + 30, y: 560 + 330, width: 540, height: 200))
    for s in sizes.dropFirst() { place(img, CGRect(x: x, y: 560 + 430 - s / 2, width: s, height: s)); x += s + 40 }
}
save(sheet, "\(out)/sheet.png")
