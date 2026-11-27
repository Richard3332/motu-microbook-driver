import Cocoa
import SystemExtensions

// Minimal installer: activates or removes the embedded MicroBook dext.
// Usage (from Terminal):  MicroBookInstaller.app/Contents/MacOS/MicroBookInstaller activate|deactivate
let dextID = (Bundle.main.bundleIdentifier ?? "") + ".driver"

final class Delegate: NSObject, OSSystemExtensionRequestDelegate {
    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        print("replacing \(existing.bundleVersion) with \(ext.bundleVersion)")
        return .replace
    }
    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        print("NEEDS USER APPROVAL: open System Settings > General > Login Items & Extensions > Driver Extensions and allow it")
    }
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        print("finished: \(result == .completed ? "completed" : "will complete after reboot")")
        exit(0)
    }
    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        print("FAILED: \(error)")
        exit(1)
    }
}

let mode = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "activate"
let delegate = Delegate()
let request: OSSystemExtensionRequest = mode == "deactivate"
    ? OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: dextID, queue: .main)
    : OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: dextID, queue: .main)
request.delegate = delegate
print("submitting \(mode) request for \(dextID)")
OSSystemExtensionManager.shared.submitRequest(request)
RunLoop.main.run()
