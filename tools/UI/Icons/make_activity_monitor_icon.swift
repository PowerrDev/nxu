#!/usr/bin/env swift
// Renders the Activity Monitor app icon and packs it into an .icns.
//
// Usage (on a macOS host):
//   swift tools/UI/Icons/make_activity_monitor_icon.swift [out.icns] [preview.png]
//
// With no arguments the icon is written over
// tools/DiskRoot/System/Library/Resources/Images/ActivityMonitor.icns, which
// the build copies into "Activity Monitor.app/Contents/Resources".
//
// Everything is drawn as vectors at each output size (no downscaling), on
// the macOS app icon grid: a 1024 canvas holding an 824 continuous-corner
// body with a soft drop shadow. The face is a graphite bezel around a dark
// screen with a faint grid and a glowing green heartbeat trace.

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

// MARK: - Helpers

let srgb = CGColorSpace(name: CGColorSpace.sRGB)!

func rgba(_ r: Double, _ g: Double, _ b: Double, _ a: Double = 1) -> CGColor {
    CGColor(colorSpace: srgb, components: [r, g, b, a])!
}

func hex(_ v: UInt32, _ a: Double = 1) -> CGColor {
    rgba(Double((v >> 16) & 0xff) / 255, Double((v >> 8) & 0xff) / 255, Double(v & 0xff) / 255, a)
}

func gradient(_ stops: [(CGFloat, CGColor)]) -> CGGradient {
    CGGradient(colorsSpace: srgb,
               colors: stops.map { $0.1 } as CFArray,
               locations: stops.map { $0.0 })!
}

/// A continuous-corner rounded rectangle (the macOS "squircle"): straight
/// edges joined by superellipse corners, so the curvature eases in instead
/// of jumping at the tangent points like a circular arc does.
func squircle(_ rect: CGRect, radius r: CGFloat, exponent n: CGFloat = 4.0) -> CGPath {
    let path = CGMutablePath()
    // The corner region spans about 1.7 r along each edge, like Apple's shape.
    let k = min(r * 1.7, min(rect.width, rect.height) / 2)
    let steps = 48
    func corner(_ cx: CGFloat, _ cy: CGFloat, _ sx: CGFloat, _ sy: CGFloat, _ startAngle: CGFloat) {
        // Quarter superellipse centred on (cx, cy), radius k, signs pick the
        // quadrant; angle runs a quarter turn from startAngle.
        for i in 0...steps {
            let t = startAngle + (CGFloat(i) / CGFloat(steps)) * (.pi / 2)
            let c = cos(t), s = sin(t)
            let x = pow(abs(c), 2 / n) * (c < 0 ? -1 : 1)
            let y = pow(abs(s), 2 / n) * (s < 0 ? -1 : 1)
            let p = CGPoint(x: cx + x * k * sx, y: cy + y * k * sy)
            if path.isEmpty { path.move(to: p) } else { path.addLine(to: p) }
        }
    }
    let minX = rect.minX, maxX = rect.maxX, minY = rect.minY, maxY = rect.maxY
    // y grows downward here (the context is flipped to top-left).
    corner(maxX - k, minY + k, 1, 1, -.pi / 2)   // top-right: from top edge to right edge
    corner(maxX - k, maxY - k, 1, 1, 0)          // bottom-right
    corner(minX + k, maxY - k, 1, 1, .pi / 2)    // bottom-left
    corner(minX + k, minY + k, 1, 1, .pi)        // top-left
    path.closeSubpath()
    return path
}

// MARK: - The heartbeat trace (in 1024-canvas units)

/// The trace across the screen: flat, a small P wave, the sharp QRS spike,
/// a rounded T wave, a small U ripple, then flat to a glowing tip.
func heartbeat(in screen: CGRect) -> (CGPath, CGPoint) {
    let w = screen.width, h = screen.height
    let x0 = screen.minX, y0 = screen.minY
    func p(_ fx: CGFloat, _ fy: CGFloat) -> CGPoint { CGPoint(x: x0 + fx * w, y: y0 + fy * h) }
    let base: CGFloat = 0.60
    let path = CGMutablePath()
    path.move(to: p(-0.02, base))
    path.addLine(to: p(0.10, base))
    // P wave
    path.addCurve(to: p(0.20, base), control1: p(0.13, base - 0.075), control2: p(0.17, base - 0.075))
    path.addLine(to: p(0.255, base))
    // QRS
    path.addLine(to: p(0.285, base + 0.075))
    path.addLine(to: p(0.345, 0.11))
    path.addLine(to: p(0.405, base + 0.20))
    path.addLine(to: p(0.445, base))
    // ST segment, T wave
    path.addLine(to: p(0.51, base))
    path.addCurve(to: p(0.70, base), control1: p(0.56, base - 0.215), control2: p(0.65, base - 0.215))
    // U ripple
    path.addCurve(to: p(0.80, base), control1: p(0.735, base - 0.05), control2: p(0.765, base - 0.05))
    let tip = p(0.875, base)
    path.addLine(to: tip)
    return (path, tip)
}

// MARK: - Drawing

func draw(_ ctx: CGContext, pixels: Int) {
    let px = CGFloat(pixels)
    let s = px / 1024                    // canvas units -> pixels
    let small = pixels <= 32             // 16 and 32: simplify
    let tiny = pixels <= 16

    ctx.translateBy(x: 0, y: px)
    ctx.scaleBy(x: s, y: -s)             // 1024 canvas, origin top-left
    ctx.setShouldAntialias(true)
    ctx.setAllowsAntialiasing(true)
    ctx.interpolationQuality = .high

    // Body on the Big Sur grid: 824 square, 100 margin.
    let body = CGRect(x: 100, y: 100, width: 824, height: 824)
    let bodyPath = squircle(body, radius: 185)

    // Drop shadow (the system icons' soft shadow, offset down).
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10 * s), blur: 28 * s, color: rgba(0, 0, 0, 0.42))
    ctx.addPath(bodyPath)
    ctx.setFillColor(hex(0x1C1C1E))
    ctx.fillPath()
    ctx.restoreGState()
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -2 * s), blur: 4 * s, color: rgba(0, 0, 0, 0.30))
    ctx.addPath(bodyPath)
    ctx.setFillColor(hex(0x1C1C1E))
    ctx.fillPath()
    ctx.restoreGState()

    // Graphite bezel: a brushed vertical gradient.
    ctx.saveGState()
    ctx.addPath(bodyPath)
    ctx.clip()
    ctx.drawLinearGradient(gradient([
        (0.00, hex(0x5A5A5F)),
        (0.08, hex(0x48484C)),
        (0.55, hex(0x2E2E31)),
        (1.00, hex(0x1D1D1F)),
    ]), start: CGPoint(x: 0, y: 100), end: CGPoint(x: 0, y: 924), options: [])
    // A soft sheen across the top of the bezel.
    ctx.drawRadialGradient(gradient([
        (0, rgba(1, 1, 1, 0.16)),
        (1, rgba(1, 1, 1, 0)),
    ]), startCenter: CGPoint(x: 512, y: 60), startRadius: 0,
       endCenter: CGPoint(x: 512, y: 60), endRadius: 560, options: [])
    ctx.restoreGState()

    // Rim: a thin light edge on top, fading to a dark one at the bottom.
    ctx.saveGState()
    ctx.addPath(bodyPath)
    ctx.clip()
    ctx.addPath(bodyPath)
    ctx.setLineWidth(small ? 24 : 7)
    ctx.replacePathWithStrokedPath()
    ctx.clip()
    ctx.drawLinearGradient(gradient([
        (0.00, rgba(1, 1, 1, 0.42)),
        (0.30, rgba(1, 1, 1, 0.10)),
        (0.70, rgba(1, 1, 1, 0.03)),
        (1.00, rgba(0, 0, 0, 0.35)),
    ]), start: CGPoint(x: 0, y: 100), end: CGPoint(x: 0, y: 924), options: [])
    ctx.restoreGState()

    // Screen, inset into the bezel.
    let inset: CGFloat = tiny ? 76 : (small ? 70 : 58)
    let screen = body.insetBy(dx: inset, dy: inset)
    let screenPath = squircle(screen, radius: 185 - inset * 0.9)

    // Recess: a dark lip above the screen and a light catch below it.
    ctx.saveGState()
    ctx.addPath(squircle(screen.insetBy(dx: -6, dy: -6), radius: 185 - inset * 0.9 + 6))
    ctx.clip()
    ctx.drawLinearGradient(gradient([
        (0, rgba(0, 0, 0, 0.55)),
        (1, rgba(1, 1, 1, 0.16)),
    ]), start: CGPoint(x: 0, y: screen.minY - 6), end: CGPoint(x: 0, y: screen.maxY + 6), options: [])
    ctx.restoreGState()

    ctx.saveGState()
    ctx.addPath(screenPath)
    ctx.clip()
    // Near-black face with a soft vertical gradient and a faint green cast.
    ctx.drawLinearGradient(gradient([
        (0.0, hex(0x1A1F1C)),
        (0.5, hex(0x0E1210)),
        (1.0, hex(0x060807)),
    ]), start: CGPoint(x: 0, y: screen.minY), end: CGPoint(x: 0, y: screen.maxY), options: [])
    // Phosphor bloom behind the trace.
    ctx.drawRadialGradient(gradient([
        (0, hex(0x32D74B, 0.16)),
        (1, hex(0x32D74B, 0.0)),
    ]), startCenter: CGPoint(x: screen.midX, y: screen.minY + screen.height * 0.55), startRadius: 0,
       endCenter: CGPoint(x: screen.midX, y: screen.minY + screen.height * 0.55), endRadius: screen.width * 0.62,
       options: [])

    // Grid.
    if !tiny {
        let divisions = small ? 4 : 6
        let step = screen.width / CGFloat(divisions)
        ctx.setLineWidth(small ? 10 : 3)
        ctx.setStrokeColor(hex(0x32D74B, small ? 0.10 : 0.13))
        for i in 1..<divisions {
            let x = screen.minX + CGFloat(i) * step
            let y = screen.minY + CGFloat(i) * step
            ctx.move(to: CGPoint(x: x, y: screen.minY))
            ctx.addLine(to: CGPoint(x: x, y: screen.maxY))
            ctx.move(to: CGPoint(x: screen.minX, y: y))
            ctx.addLine(to: CGPoint(x: screen.maxX, y: y))
        }
        ctx.strokePath()
        if !small {
            // Finer minor ticks along the centre lines, like a scope graticule.
            ctx.setLineWidth(2)
            ctx.setStrokeColor(hex(0x32D74B, 0.10))
            let minor = step / 5
            let midX = screen.minX + step * 3, midY = screen.minY + step * 3
            var t = screen.minX + minor
            while t < screen.maxX - 1 {
                ctx.move(to: CGPoint(x: t, y: midY - 7)); ctx.addLine(to: CGPoint(x: t, y: midY + 7))
                t += minor
            }
            t = screen.minY + minor
            while t < screen.maxY - 1 {
                ctx.move(to: CGPoint(x: midX - 7, y: t)); ctx.addLine(to: CGPoint(x: midX + 7, y: t))
                t += minor
            }
            ctx.strokePath()
        }
    }

    // The trace: wide soft glow, tighter glow, green line, bright core.
    let (trace, tip) = heartbeat(in: screen)
    let green = hex(0x32D74B)
    let lineW: CGFloat = tiny ? 44 : (small ? 38 : 17)
    ctx.setLineCap(.round)
    ctx.setLineJoin(.round)

    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: 46 * s, color: hex(0x32D74B, 0.95))
    ctx.addPath(trace)
    ctx.setLineWidth(lineW)
    ctx.setStrokeColor(hex(0x32D74B, 0.55))
    ctx.strokePath()
    ctx.restoreGState()

    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: 14 * s, color: hex(0x5CFF74, 0.9))
    ctx.addPath(trace)
    ctx.setLineWidth(lineW)
    ctx.setStrokeColor(green)
    ctx.strokePath()
    ctx.restoreGState()

    if !tiny {
        ctx.addPath(trace)
        ctx.setLineWidth(lineW * 0.42)
        ctx.setStrokeColor(hex(0xB8FFC4, 0.85))
        ctx.strokePath()
    }

    // Glowing tip.
    if !small {
        ctx.drawRadialGradient(gradient([
            (0.0, hex(0x32D74B, 0.55)),
            (1.0, hex(0x32D74B, 0.0)),
        ]), startCenter: tip, startRadius: 0, endCenter: tip, endRadius: 90, options: [])
        ctx.saveGState()
        ctx.setShadow(offset: .zero, blur: 20 * s, color: hex(0x5CFF74, 1))
        ctx.setFillColor(hex(0xE6FFE9))
        ctx.fillEllipse(in: CGRect(x: tip.x - 17, y: tip.y - 17, width: 34, height: 34))
        ctx.restoreGState()
    }

    // Glass: a faint highlight across the top of the screen.
    ctx.drawLinearGradient(gradient([
        (0.0, rgba(1, 1, 1, 0.09)),
        (0.42, rgba(1, 1, 1, 0.02)),
        (0.43, rgba(1, 1, 1, 0.0)),
    ]), start: CGPoint(x: 0, y: screen.minY), end: CGPoint(x: 0, y: screen.maxY), options: [])
    // Vignette toward the screen's corners.
    ctx.drawRadialGradient(gradient([
        (0.0, rgba(0, 0, 0, 0)),
        (0.7, rgba(0, 0, 0, 0)),
        (1.0, rgba(0, 0, 0, 0.45)),
    ]), startCenter: CGPoint(x: screen.midX, y: screen.midY), startRadius: 0,
       endCenter: CGPoint(x: screen.midX, y: screen.midY), endRadius: screen.width * 0.74, options: [])
    ctx.restoreGState()

    // Inner edge of the screen glass.
    ctx.addPath(screenPath)
    ctx.setLineWidth(small ? 8 : 3)
    ctx.setStrokeColor(rgba(0, 0, 0, 0.6))
    ctx.strokePath()
}

func render(pixels: Int) -> CGImage {
    let ctx = CGContext(data: nil, width: pixels, height: pixels, bitsPerComponent: 8,
                        bytesPerRow: 0, space: srgb,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    draw(ctx, pixels: pixels)
    return ctx.makeImage()!
}

func writePNG(_ image: CGImage, to url: URL) {
    let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, image, nil)
    guard CGImageDestinationFinalize(dest) else { fatalError("cannot write \(url.path)") }
}

// MARK: - Main

let scriptDir = URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent()
let repoRoot = scriptDir.appendingPathComponent("../../..").standardizedFileURL
let args = Array(CommandLine.arguments.dropFirst())
let outICNS = args.count > 0
    ? URL(fileURLWithPath: args[0])
    : repoRoot.appendingPathComponent("tools/DiskRoot/System/Library/Resources/Images/ActivityMonitor.icns")

let work = FileManager.default.temporaryDirectory
    .appendingPathComponent("ActivityMonitor-\(ProcessInfo.processInfo.processIdentifier).iconset")
try? FileManager.default.removeItem(at: work)
try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)

for base in [16, 32, 128, 256, 512] {
    writePNG(render(pixels: base), to: work.appendingPathComponent("icon_\(base)x\(base).png"))
    writePNG(render(pixels: base * 2), to: work.appendingPathComponent("icon_\(base)x\(base)@2x.png"))
}
if args.count > 1 {
    writePNG(render(pixels: 1024), to: URL(fileURLWithPath: args[1]))
}

let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", "-o", outICNS.path, work.path]
try iconutil.run()
iconutil.waitUntilExit()
try? FileManager.default.removeItem(at: work)
guard iconutil.terminationStatus == 0 else { fatalError("iconutil failed") }
print("wrote \(outICNS.path)")
