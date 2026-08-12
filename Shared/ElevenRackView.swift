/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
The SwiftUI view that provides the driver-loading UI.
*/

import SwiftUI
import Combine

struct ElevenRackView: View {
	@StateObject var viewModel = ElevenRackViewModel()

	var body: some View {
#if os(macOS)
		VStack(alignment: .leading, spacing: 16) {
			Text("Eleven Rack Driver")
				.font(.title)
			Text("Connect and power on your Eleven Rack, then activate the native Apple Silicon audio driver.")
				.fixedSize(horizontal: false, vertical: true)

			GroupBox(label: Text("Driver")) {
				VStack(alignment: .leading, spacing: 10) {
					Text(self.viewModel.dextLoadingState)
						.multilineTextAlignment(.leading)
					HStack {
						Button("Activate Driver") {
							self.viewModel.activateMyDext()
						}
						Button("Connect / Refresh") {
							self.viewModel.connectAudioControls()
						}
					}
				}
				.frame(maxWidth: .infinity, alignment: .leading)
			}

			GroupBox(label: Text("Audio Clocking")) {
				VStack(alignment: .leading, spacing: 14) {
					HStack {
						Text("Sample Rate")
							.frame(width: 100, alignment: .leading)
						Picker("", selection: $viewModel.selectedSampleRate) {
							ForEach(viewModel.supportedSampleRates, id: \.self) { rate in
								Text(viewModel.sampleRateLabel(rate)).tag(rate)
							}
						}
						.labelsHidden()
						.pickerStyle(.segmented)
						.disabled(viewModel.selectedClockSource != 1)
						Button("Apply") { viewModel.applySampleRate() }
							.disabled(viewModel.selectedClockSource != 1)
					}

					HStack {
						Text("Clock Source")
							.frame(width: 100, alignment: .leading)
						Picker("", selection: $viewModel.selectedClockSource) {
							Text("Internal").tag(1)
							Text("AES/EBU").tag(2)
							Text("S/PDIF").tag(3)
						}
						.labelsHidden()
						.pickerStyle(.segmented)
						Button("Apply") { viewModel.applyClockSource() }
					}

					Divider()
					HStack {
						Text("Hardware")
							.fontWeight(.semibold)
						Spacer()
						Text(viewModel.clockSourceLabel)
						Circle()
							.fill(viewModel.clockValidity == 1 ? Color.green :
								(viewModel.clockValidity == 0 ? Color.red : Color.gray))
							.frame(width: 9, height: 9)
						Text(viewModel.clockLockLabel)
						Text(viewModel.hardwareSampleRate == 0 ? "Rate unavailable" :
							viewModel.sampleRateLabel(Double(viewModel.hardwareSampleRate)))
							.monospacedDigit()
					}
					Text(viewModel.selectedClockSource == 1 ?
						"Internal clock: this app controls sample rate." :
						"External clock: the connected digital source determines sample rate. Stop streaming before changing source.")
						.font(.caption)
						.foregroundColor(.secondary)
				}
				.frame(maxWidth: .infinity, alignment: .leading)
			}

			GroupBox(label: Text("Core Audio Transport")) {
				VStack(alignment: .leading, spacing: 8) {
					HStack {
						Text(viewModel.transportLabel).fontWeight(.semibold)
						Spacer()
						Text("Buffer \(viewModel.bufferFrames) frames")
						Text(viewModel.latencySummary).monospacedDigit()
					}
					HStack {
						Text("Input")
						Spacer()
						Text("latency \(viewModel.inputLatencyFrames) + safety \(viewModel.inputSafetyFrames) frames")
					}
					HStack {
						Text("Output")
						Spacer()
						Text("latency \(viewModel.outputLatencyFrames) + safety \(viewModel.outputSafetyFrames) frames")
					}
					Text("Round trip is estimated from Core Audio's buffer, latency, and safety-offset properties; it is not a physical loopback measurement.")
						.font(.caption)
						.foregroundColor(.secondary)
				}
				.frame(maxWidth: .infinity, alignment: .leading)
			}

			HStack(alignment: .top) {
				Text(viewModel.audioControlStatus)
					.font(.callout)
					.fixedSize(horizontal: false, vertical: true)
				Spacer()
				Button("Export Diagnostics…") { viewModel.exportDiagnostics() }
			}
		}
		.padding(24)
		.frame(width: 720, height: 650, alignment: .topLeading)
		.onAppear { viewModel.connectAudioControls() }
		.onReceive(Timer.publish(every: 2, on: .main, in: .common).autoconnect()) { _ in
			viewModel.pollAudioStatus()
		}
#endif
	}
}

struct ElevenRackDriverView_Previews: PreviewProvider {
    static var previews: some View {
		ElevenRackView()
    }
}
