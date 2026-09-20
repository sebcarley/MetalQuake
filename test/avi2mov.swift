// test/avi2mov.swift -- turn a DarkPlaces video capture (a raw I420 AVI, which
// QuickTime will not open: AVFoundation reads it and reports it unplayable) into
// an H.264 .mov, video and PCM audio, with AVFoundation alone. No ffmpeg on
// this machine (SEPTEMBER2 E, 2026-09-11).
//
//   swift test/avi2mov.swift <capture.avi> <out.mov> [fps] [hevc|h264]
//
// HEVC (H.265) by default -- Seb's ask, 2026-09-11 -- at 12 Mbit, which on this
// content is the H.264 picture at rather more than half the size; h264 for a
// player that cannot take HEVC.
//
// Walks the RIFF tree itself (cap_avi.c writes 'movi' lists of 00dc video
// chunks and 01wb PCM chunks, in OpenDML AVIX extensions past 1 GB), feeds the
// video frames through a pixel-buffer adaptor and the audio through a raw
// LPCM sample-buffer stream. fps defaults to cl_capturevideo_fps's 60.
import AVFoundation
import Foundation

setbuf(stdout, nil)
let args = CommandLine.arguments
guard args.count >= 3 else { print("usage: avi2mov <in.avi> <out.mov> [fps]"); exit(2) }
let fps = args.count > 3 ? Int32(args[3])! : 60
let useH264 = args.count > 4 && args[4].lowercased() == "h264"
let data = try! Data(contentsOf: URL(fileURLWithPath: args[1]), options: .mappedIfSafe)

func u32(_ o: Int) -> Int { return Int(data[o]) | Int(data[o+1]) << 8 | Int(data[o+2]) << 16 | Int(data[o+3]) << 24 }
func u16(_ o: Int) -> Int { return Int(data[o]) | Int(data[o+1]) << 8 }
func tag(_ o: Int) -> String { return String(bytes: data[o..<o+4], encoding: .ascii) ?? "????" }

var width = 0, height = 0, rate = 0, channels = 0, bits = 0
var video: [(Int, Int)] = []   // (offset, length)
var audio: [(Int, Int)] = []

func walk(_ start: Int, _ end: Int) {
    var o = start
    while o + 8 <= end {
        let t = tag(o); let len = u32(o + 4); let body = o + 8
        if t == "RIFF" || t == "LIST" {
            walk(body + 4, min(body + len, end))
        } else if t == "strh" {
            // fccType at body, fccHandler +4; scale +20, rate +24
        } else if t == "strf" {
            if width == 0 && len >= 40 && u32(body) == 40 { width = u32(body + 4); height = u32(body + 8) }
            else if rate == 0 && len >= 16 { channels = u16(body + 2); rate = u32(body + 4); bits = u16(body + 14) }
        } else if t == "00dc" { video.append((body, len)) }
        else if t == "01wb" { audio.append((body, len)) }
        o = body + len + (len & 1)
    }
}
walk(0, data.count)
print("avi: \(width)x\(height) \(video.count) frames, audio \(rate) Hz \(channels) ch \(bits) bit, \(audio.count) chunks -> \(useH264 ? "H.264" : "HEVC")")
guard width > 0, !video.isEmpty else { exit(1) }

let out = URL(fileURLWithPath: args[2])
try? FileManager.default.removeItem(at: out)
let writer = try! AVAssetWriter(outputURL: out, fileType: .mov)
let vin = AVAssetWriterInput(mediaType: .video, outputSettings: [
    AVVideoCodecKey: useH264 ? AVVideoCodecType.h264 : AVVideoCodecType.hevc, AVVideoWidthKey: width, AVVideoHeightKey: height,
    AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: useH264 ? 20_000_000 : 12_000_000]])
vin.expectsMediaDataInRealTime = false
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: vin, sourcePixelBufferAttributes: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8Planar,
    kCVPixelBufferWidthKey as String: width, kCVPixelBufferHeightKey as String: height])
writer.add(vin)
var ain: AVAssetWriterInput? = nil
var asbd = AudioStreamBasicDescription(mSampleRate: Double(rate), mFormatID: kAudioFormatLinearPCM,
    mFormatFlags: kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, mBytesPerPacket: UInt32(channels * bits / 8),
    mFramesPerPacket: 1, mBytesPerFrame: UInt32(channels * bits / 8), mChannelsPerFrame: UInt32(channels), mBitsPerChannel: UInt32(bits), mReserved: 0)
var afmt: CMAudioFormatDescription? = nil
if rate > 0 && !audio.isEmpty {
    CMAudioFormatDescriptionCreate(allocator: nil, asbd: &asbd, layoutSize: 0, layout: nil, magicCookieSize: 0, magicCookie: nil, extensions: nil, formatDescriptionOut: &afmt)
    ain = AVAssetWriterInput(mediaType: .audio, outputSettings: nil, sourceFormatHint: afmt)
    ain!.expectsMediaDataInRealTime = false
    writer.add(ain!)
}
guard writer.startWriting() else { print("startWriting failed: \(writer.error.map { "\($0)" } ?? "?")"); exit(1) }
writer.startSession(atSourceTime: .zero)
func waitReady(_ input: AVAssetWriterInput) -> Bool {
    while !input.isReadyForMoreMediaData {
        if writer.status != .writing { print("writer failed: \(writer.error.map { "\($0)" } ?? "?")"); return false }
        usleep(2000)
    }
    return true
}

// WHICHEVER INPUT IS READY GETS THE NEXT SAMPLE. AVAssetWriter interleaves its
// inputs and decides for itself how far ahead it wants each; a loop that feeds
// one input on its own schedule and waits on the other deadlocks (two cuts of
// this file did: all-video-then-audio, and audio fed only up to the video's
// time -- the writer wanted a deeper audio lead and parked both). So: append a
// video frame when the video input is ready, an audio chunk when the audio
// input is, and sleep only when neither is.
let ysize = width * height, csize = (width / 2) * (height / 2)
let bpf = max(Int(asbd.mBytesPerFrame), 1)
var vidx = 0, aidx = 0
var aframes: Int64 = 0
func appendVideo() -> Bool {
    let (off, len) = video[vidx]; let i = vidx; vidx += 1
    guard len >= ysize + 2 * csize else { return true }
    var pb: CVPixelBuffer? = nil
    CVPixelBufferPoolCreatePixelBuffer(nil, adaptor.pixelBufferPool!, &pb)
    guard let buf = pb else { return false }
    CVPixelBufferLockBaseAddress(buf, [])
    let planes = [(0, ysize, width, height), (1, csize, width / 2, height / 2), (2, csize, width / 2, height / 2)]
    var src = off
    for (p, size, w, h) in planes {
        let dst = CVPixelBufferGetBaseAddressOfPlane(buf, p)!
        let stride = CVPixelBufferGetBytesPerRowOfPlane(buf, p)
        data.withUnsafeBytes { raw in
            for row in 0..<h { memcpy(dst + row * stride, raw.baseAddress! + src + row * w, w) }
        }
        src += size
    }
    CVPixelBufferUnlockBaseAddress(buf, [])
    if !adaptor.append(buf, withPresentationTime: CMTime(value: CMTimeValue(i), timescale: fps)) { print("append failed at frame \(i): \(writer.error.map { "\($0)" } ?? "?")"); return false }
    if i % 120 == 0 { print("frame \(i)/\(video.count)") }
    return true
}
func appendAudio() -> Bool {
    guard let ain = ain, let fmt = afmt else { aidx = audio.count; return true }
    let (off, len) = audio[aidx]; aidx += 1
    let n = len / bpf
    var block: CMBlockBuffer? = nil
    CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: nil, blockLength: len, blockAllocator: nil, customBlockSource: nil, offsetToData: 0, dataLength: len, flags: 0, blockBufferOut: &block)
    data.withUnsafeBytes { raw in _ = CMBlockBufferReplaceDataBytes(with: raw.baseAddress! + off, blockBuffer: block!, offsetIntoDestination: 0, dataLength: len) }
    var sb: CMSampleBuffer? = nil
    CMAudioSampleBufferCreateReadyWithPacketDescriptions(allocator: nil, dataBuffer: block!, formatDescription: fmt, sampleCount: n, presentationTimeStamp: CMTime(value: aframes, timescale: Int32(rate)), packetDescriptions: nil, sampleBufferOut: &sb)
    if let sb = sb { if !ain.append(sb) { print("audio append failed: \(writer.error.map { "\($0)" } ?? "?")"); return false } }
    aframes += Int64(n)
    return true
}
if ain == nil { aidx = audio.count }
while vidx < video.count || aidx < audio.count {
    if writer.status != .writing { print("writer failed: \(writer.error.map { "\($0)" } ?? "?")"); exit(1) }
    var did = false
    if vidx < video.count && vin.isReadyForMoreMediaData { if !appendVideo() { exit(1) }; did = true }
    if aidx < audio.count, let ain = ain, ain.isReadyForMoreMediaData { if !appendAudio() { exit(1) }; did = true }
    if vidx >= video.count { vin.markAsFinished() }
    if aidx >= audio.count { ain?.markAsFinished() }
    if !did { usleep(2000) }
}
if vidx >= video.count { vin.markAsFinished() }
ain?.markAsFinished()
let sem = DispatchSemaphore(value: 0)
writer.finishWriting { sem.signal() }
sem.wait()
print("wrote \(out.path) status \(writer.status.rawValue) \(writer.error.map { "\($0)" } ?? "")")
