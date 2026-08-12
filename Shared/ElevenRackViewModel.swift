/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
The view model that indicates the state of driver-loading.
*/

import Foundation
import os.log
#if os(macOS)
import AppKit
import SystemExtensions
#endif

class ElevenRackDriverLoadingStateMachine {

	enum State {
		case unloaded
		case activating
		case needsApproval
		case activated
		case activationError
	}

	enum Event {
		case activationStarted
		case promptForApproval
		case activationFinished
		case activationFailed
	}

	static func process(_ state: State, _ event: Event) -> State {

		switch state {
		case .unloaded:
			switch event {
			case .activationStarted:
				return .activating
			case .promptForApproval, .activationFinished, .activationFailed:
				return .activationError
			}

		case .activating, .needsApproval:
			switch event {
			case .activationStarted:
				return .activating
			case .promptForApproval:
				return .needsApproval
			case .activationFinished:
				return .activated
			case .activationFailed:
				return .activationError
			}

		case .activated:
			switch event {
			case .activationStarted:
				return .activating
			case .promptForApproval, .activationFailed:
				return .activationError
			case .activationFinished:
				return .activated
			}

		case .activationError:
			switch event {
			case .activationStarted:
				return .activating
			case .promptForApproval, .activationFinished, .activationFailed:
				return .activationError
			}
		}
	}
}

class ElevenRackViewModel: NSObject {

	// Your dext may not start in unloaded state every time. Add logic or states to check this.
	@Published private var state: ElevenRackDriverLoadingStateMachine.State = .unloaded

	private let dextIdentifier: String = Bundle.main.bundleIdentifier! + ".Driver"
	private let userClient = ElevenRackUserClient()

	let supportedSampleRates: [Double] = [44_100, 48_000, 88_200, 96_000]
	@Published var selectedSampleRate: Double = 48_000
	@Published var selectedClockSource: Int = 1
	@Published var hardwareClockSource: Int = 0
	@Published var hardwareSampleRate: UInt32 = 0
	@Published var clockValidity: Int = 2
	@Published var isStreaming = false
	@Published var bufferFrames: UInt32 = 0
	@Published var inputSafetyFrames: UInt32 = 0
	@Published var outputSafetyFrames: UInt32 = 0
	@Published var inputLatencyFrames: UInt32 = 0
	@Published var outputLatencyFrames: UInt32 = 0
	@Published var estimatedRoundTripMilliseconds = 0.0
	@Published var audioControlStatus = "Connect the driver to read hardware clock status."
	private var audioControlsConnected = false

	public var dextLoadingState: String {
		switch state {
		case .unloaded:
			return "ElevenRackDriver isn't loaded."
		case .activating:
			return "Activating ElevenRackDriver, please wait."
		case .needsApproval:
			return "Please follow the prompt to approve ElevenRackDriver."
		case .activated:
			return "ElevenRackDriver has been activated and is ready to use."
		case .activationError:
			return "ElevenRackDriver has experienced an error during activation.\nPlease check the logs to find the error."
		}
	}

	func connectAudioControls() {
		audioControlStatus = userClient.open()
		audioControlsConnected = refreshAudioStatus()
	}

	@discardableResult
	func refreshAudioStatus() -> Bool {
		let hardware = userClient.readAudioStatus() ?? [:]
		let coreAudio = userClient.readCoreAudioStatus() ?? [:]
		let coreAudioRate = coreAudio["sampleRate"]?.doubleValue ?? 0
		let clockSource = hardware["clockSource"]?.intValue ?? 0
		let deviceRate = hardware["hardwareSampleRate"]?.uint32Value ?? 0
		if coreAudioRate > 0 {
			selectedSampleRate = coreAudioRate
		}
		if clockSource >= 1 && clockSource <= 3 {
			selectedClockSource = clockSource
			hardwareClockSource = clockSource
		}
		hardwareSampleRate = deviceRate
		clockValidity = hardware["clockValid"]?.intValue ?? 2
		isStreaming = hardware["streaming"]?.boolValue ?? false
		bufferFrames = coreAudio["bufferFrames"]?.uint32Value ?? 0
		inputSafetyFrames = coreAudio["inputSafetyFrames"]?.uint32Value ?? 0
		outputSafetyFrames = coreAudio["outputSafetyFrames"]?.uint32Value ?? 0
		inputLatencyFrames = coreAudio["inputLatencyFrames"]?.uint32Value ?? 0
		outputLatencyFrames = coreAudio["outputLatencyFrames"]?.uint32Value ?? 0
		estimatedRoundTripMilliseconds = coreAudio["estimatedRoundTripMilliseconds"]?.doubleValue ?? 0
		return !hardware.isEmpty && coreAudioRate > 0
	}

	func pollAudioStatus() {
		if refreshAudioStatus() {
			if selectedClockSource != 1 && !isStreaming && hardwareSampleRate > 0 &&
				UInt32(selectedSampleRate) != hardwareSampleRate {
				_ = userClient.followExternalSampleRate(Double(hardwareSampleRate))
				_ = refreshAudioStatus()
			}
			if !audioControlsConnected {
				audioControlsConnected = true
				audioControlStatus = "Eleven Rack reconnected."
			}
			return
		}
		if audioControlsConnected {
			audioControlsConnected = false
			audioControlStatus = "Eleven Rack disconnected; waiting to reconnect."
		}
		userClient.close()
		_ = userClient.open()
		if refreshAudioStatus() {
			audioControlsConnected = true
			audioControlStatus = "Eleven Rack reconnected."
		}
	}

	func applySampleRate() {
		audioControlStatus = userClient.setSampleRate(selectedSampleRate)
		refreshAudioStatusAfterSuccess()
	}

	func applyClockSource() {
		audioControlStatus = userClient.setClockSource(selectedClockSource)
		refreshAudioStatusAfterSuccess()
	}

	private func refreshAudioStatusAfterSuccess() {
		let message = audioControlStatus
		audioControlsConnected = refreshAudioStatus()
		audioControlStatus = message
	}

	func sampleRateLabel(_ rate: Double) -> String {
		String(format: rate.truncatingRemainder(dividingBy: 1_000) == 0 ?
			"%.0f kHz" : "%.1f kHz", rate / 1_000)
	}

	var clockSourceLabel: String {
		switch hardwareClockSource {
		case 1: return "Internal"
		case 2: return "AES/EBU"
		case 3: return "S/PDIF"
		default: return "Unknown"
		}
	}

	var clockLockLabel: String {
		switch clockValidity {
		case 0: return "Unlocked"
		case 1: return "Locked"
		default: return "Lock unknown"
		}
	}

	var transportLabel: String { isStreaming ? "Streaming" : "Idle" }

	var latencySummary: String {
		guard estimatedRoundTripMilliseconds > 0 else { return "Unavailable" }
		return String(format: "%.1f ms estimated", estimatedRoundTripMilliseconds)
	}

#if os(macOS)
	func exportDiagnostics() {
		_ = refreshAudioStatus()
		let appVersion = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "unknown"
		let payload: [String: Any] = [
			"schemaVersion": 1,
			"generatedAt": ISO8601DateFormatter().string(from: Date()),
			"privacy": "Contains driver/audio configuration only; no serial number, rig data, MIDI, or audio content.",
			"application": ["version": appVersion],
			"system": ["operatingSystem": ProcessInfo.processInfo.operatingSystemVersionString],
			"device": [
				"model": "Eleven Rack",
				"driverConnected": audioControlsConnected,
				"clockSource": clockSourceLabel,
				"clockValidity": clockLockLabel,
				"hardwareSampleRate": hardwareSampleRate,
				"streaming": isStreaming,
				"inputChannels": 8,
				"outputChannels": 6
			],
			"coreAudio": [
				"nominalSampleRate": selectedSampleRate,
				"bufferFrames": bufferFrames,
				"inputSafetyFrames": inputSafetyFrames,
				"outputSafetyFrames": outputSafetyFrames,
				"inputLatencyFrames": inputLatencyFrames,
				"outputLatencyFrames": outputLatencyFrames,
				"estimatedRoundTripMilliseconds": estimatedRoundTripMilliseconds
			],
			"supportedSampleRates": supportedSampleRates
		]
		do {
			let data = try JSONSerialization.data(withJSONObject: payload,
				options: [.prettyPrinted, .sortedKeys])
			let panel = NSSavePanel()
			panel.allowedContentTypes = [.json]
			panel.nameFieldStringValue = "ElevenRack-Diagnostics.json"
			guard panel.runModal() == .OK, let url = panel.url else { return }
			try data.write(to: url, options: .atomic)
			audioControlStatus = "Diagnostics exported without serial, rig, MIDI, or audio data."
		} catch {
			audioControlStatus = "Diagnostics export failed: \(error.localizedDescription)"
		}
	}
#endif
}

extension ElevenRackViewModel: ObservableObject {

}

extension ElevenRackViewModel {

#if os(macOS)
	func activateMyDext() {
		activateExtension(dextIdentifier)
	}

	/// - Tag: ActivateExtension
	func activateExtension(_ dextIdentifier: String) {

		let request = OSSystemExtensionRequest
			.activationRequest(forExtensionWithIdentifier: dextIdentifier,
							   queue: .main)
		request.delegate = self
		OSSystemExtensionManager.shared.submitRequest(request)

		self.state = ElevenRackDriverLoadingStateMachine.process(self.state, .activationStarted)
	}

	// The sample doesn't use this method, but provides it for completeness.
	func deactivateExtension(_ dextIdentifier: String) {

		let request = OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
		request.delegate = self
		OSSystemExtensionManager.shared.submitRequest(request)

		// Update your state machine with deactivation states and process that change here
	}
#endif
}

#if os(macOS)
extension ElevenRackViewModel: OSSystemExtensionRequestDelegate {

	func request(
		_ request: OSSystemExtensionRequest,
		actionForReplacingExtension existing: OSSystemExtensionProperties,
		withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {

		var replacementAction: OSSystemExtensionRequest.ReplacementAction

		os_log("sysex actionForReplacingExtension: %@ %@", existing, ext)

		// Add appropriate logic here to determine whether to replace the extension
		// with the new extension. Common things to check for include
		// testing whether the new extension's version number is newer than
		// the current version number, or whether the bundleIdentifier is different.
		// For simplicity, this sample always replaces the current extension
		// with the new one.
		replacementAction = .replace

		// The upgrade case may require a separate set of states.
		self.state = ElevenRackDriverLoadingStateMachine.process(self.state, .activationStarted)

		return replacementAction
	}

	func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {

		os_log("sysex requestNeedsUserApproval")

		self.state = ElevenRackDriverLoadingStateMachine.process(self.state, .promptForApproval)
	}

	func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {

		os_log("sysex didFinishWithResult: %d", result.rawValue)

		// The "result" may be "willCompleteAfterReboot", which requires another state.
		// This sample ignores this state for simplicity, but a production app needs to check for it.

		self.state = ElevenRackDriverLoadingStateMachine.process(self.state, .activationFinished)
	}

	func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {

		os_log("sysex didFailWithError: %@", error.localizedDescription)

		// Some possible errors to check for:
		// Error 4: The dext identifier string in the code needs to match the one in the project settings.
		// Error 8: Indicates a signing problem. During development, set signing to "automatic" and "sign to run locally".
		// See README.md for more information.

		// This app only logs errors. Production apps need to provide feedback to customers about any errors they encounter while loading the dext.

		self.state = ElevenRackDriverLoadingStateMachine.process(self.state, .activationFailed)
	}
}
#endif
