// Minimal SwiftUI app: bundle a model.fpk as a resource; streams a completion on Metal.
import Forge
import SwiftUI

@main struct ForgeExampleApp: App { var body: some Scene { WindowGroup { ContentView() } } }

struct ContentView: View {
    @State private var text = "FORGE \(ForgeModel.version)\n"
    var body: some View {
        ScrollView { Text(text).font(.body.monospaced()).padding() }
            .task.detached {
                guard let url = Bundle.main.url(forResource: "model", withExtension: "fpk") else {
                    await MainActor.run { text += "add model.fpk to the app bundle\n" }; return }
                do {
                    let m = try ForgeModel(path: url.path)
                    _ = try m.generate("Once upon a time", maxNew: 64, sampling: Sampling(temperature: 0.7, topP: 0.9)) { _, t in
                        Task { @MainActor in text += t }; return false }
                } catch { await MainActor.run { text += "\(error)\n" } }
            }
    }
}
