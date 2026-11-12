import XCTest
@testable import Forge

/// Skips (XCTSkip) without FORGE_TEST_FPK; on a Mac/iPhone with a model it checks the round trip on Metal.
final class ForgeTests: XCTestCase {
    func testABI() { XCTAssertGreaterThanOrEqual(ForgeModel.abi, 3) }
    func testRoundTrip() throws {
        guard let p = ProcessInfo.processInfo.environment["FORGE_TEST_FPK"], FileManager.default.fileExists(atPath: p) else {
            throw XCTSkip("FORGE_TEST_FPK not set") }
        let m = try ForgeModel(path: p)
        XCTAssertEqual(try m.detokenize(try m.tokenize("hello", addBOS: false)), "hello")
        let out = try m.generate(try m.tokenize("hi"), maxNew: 4)
        XCTAssertTrue((1...4).contains(out.count))
        XCTAssertEqual(try m.forward([out[0]]).count, m.vocabSize)
    }
}
