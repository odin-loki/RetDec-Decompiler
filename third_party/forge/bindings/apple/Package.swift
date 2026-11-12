// swift-tools-version:5.9
// FORGE for iOS / macOS. FORGE.xcframework (libforge + forge.h) is produced by tools/build_xcframework.sh;
// the Swift API (Sources/Forge) wraps the C ABI. Metal is the default device on Apple GPUs.
import PackageDescription

let package = Package(
    name: "Forge",
    platforms: [.iOS("16.3"), .macOS("13.3")],   // std::to_chars(float) availability
    products: [.library(name: "Forge", targets: ["Forge"])],
    targets: [
        .binaryTarget(name: "FORGEBinary", path: "FORGE.xcframework"),
        .target(name: "CForge", dependencies: ["FORGEBinary"], path: "Sources/CForge"),
        .target(name: "Forge", dependencies: ["CForge"], path: "Sources/Forge",
                linkerSettings: [.linkedFramework("Metal"), .linkedFramework("Foundation"), .linkedLibrary("c++")]),
        .testTarget(name: "ForgeTests", dependencies: ["Forge"], path: "Tests/ForgeTests"),
    ]
)
