// Swift binding over FORGE's C ABI (forge.h). HARDWARE-UNTESTED (written without a Mac).
import CForge
import Foundation

public struct ForgeError: Error, CustomStringConvertible {
    public let description: String
    static func last(_ what: String) -> ForgeError { ForgeError(description: "\(what): \(String(cString: forge_last_error()))") }
}

public struct Sampling {
    public var temperature: Float = 0, topK: Int32 = 0, topP: Float = 1, minP: Float = 0, repetitionPenalty: Float = 1, seed: UInt64 = 0
    public init(temperature: Float = 0, topK: Int32 = 0, topP: Float = 1, minP: Float = 0, repetitionPenalty: Float = 1, seed: UInt64 = 0) {
        self.temperature = temperature; self.topK = topK; self.topP = topP; self.minP = minP; self.repetitionPenalty = repetitionPenalty; self.seed = seed
    }
    var c: forge_sampling { forge_sampling(temperature: temperature, top_k: topK, top_p: topP, min_p: minP, repetition_penalty: repetitionPenalty, seed: seed) }
}

/// One model + KV state. Not thread-safe; confine to one thread/actor.
public final class ForgeModel {
    private let m: OpaquePointer
    public static var version: String { String(cString: forge_version()) }
    public static var abi: UInt32 { forge_abi_version() }

    /// `device`: "metal" (default on Apple GPUs), "cpu", or anything FORGE_DEVICE accepts.
    public init(path: String, context: Int64 = 2048, verify: Bool = false, device: String = "metal") throws {
        setenv("FORGE_DEVICE", device, 1)
        guard let h = forge_open(path, context, verify ? 1 : 0) else { throw ForgeError.last("forge_open") }
        m = h
    }
    deinit { forge_close(m) }

    public var vocabSize: Int { Int(forge_vocab_size(m)) }
    public var position: Int64 { forge_position(m) }
    public func reset() { forge_reset(m) }
    public func rewind(to p: Int64) throws { if forge_rewind(m, p) != 0 { throw ForgeError.last("forge_rewind") } }

    /// addBOS nil: the model's default (no BOS for Qwen-family models).
    public func tokenize(_ text: String, addBOS: Bool? = nil) throws -> [Int32] {
        var out = [Int32](repeating: 0, count: text.utf8.count + 8)
        let n = forge_tokenize(m, text, addBOS.map { $0 ? 1 : 0 } ?? -1, &out, Int64(out.count))
        if n < 0 { throw ForgeError.last("forge_tokenize") }
        return Array(out.prefix(Int(n)))
    }
    public func detokenize(_ ids: [Int32]) throws -> String {
        var buf = [CChar](repeating: 0, count: ids.count * 32 + 16)
        let n = forge_detokenize(m, ids, Int64(ids.count), &buf, Int64(buf.count))
        if n < 0 { throw ForgeError.last("forge_detokenize") }
        return String(decoding: buf.prefix(Int(n)).map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }
    public func forward(_ ids: [Int32]) throws -> [Float] {
        var lg = [Float](repeating: 0, count: vocabSize)
        if forge_forward(m, ids, Int64(ids.count), &lg, Int64(lg.count)) != 0 { throw ForgeError.last("forge_forward") }
        return lg
    }

    private final class Box { let f: (Int32, String) -> Bool; init(_ f: @escaping (Int32, String) -> Bool) { self.f = f } }
    /// `onToken` returns true to stop.
    public func generate(_ prompt: [Int32], maxNew: Int32, sampling: Sampling = Sampling(), onToken: ((Int32, String) -> Bool)? = nil) throws -> [Int32] {
        var out = [Int32](repeating: 0, count: Int(max(maxNew, 1)))
        var s = sampling.c
        let box = onToken.map(Box.init)
        let user = box.map { Unmanaged.passUnretained($0).toOpaque() }
        let cb: forge_token_cb? = box == nil ? nil : { tok, text, u in
            let b = Unmanaged<Box>.fromOpaque(u!).takeUnretainedValue()
            return b.f(tok, text.map { String(cString: $0) } ?? "") ? 1 : 0
        }
        let n = withExtendedLifetime(box) { forge_generate(m, prompt, Int64(prompt.count), maxNew, &s, &out, Int64(out.count), cb, user) }
        if n < 0 { throw ForgeError.last("forge_generate") }
        return Array(out.prefix(Int(n)))
    }
    public func generate(_ prompt: String, maxNew: Int32, sampling: Sampling = Sampling(), onToken: ((Int32, String) -> Bool)? = nil) throws -> String {
        try detokenize(generate(tokenize(prompt), maxNew: maxNew, sampling: sampling, onToken: onToken))
    }
}
