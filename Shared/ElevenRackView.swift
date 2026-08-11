/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
The SwiftUI view that provides the driver-loading UI.
*/

import SwiftUI

struct ElevenRackView: View {
	@ObservedObject var viewModel = ElevenRackViewModel()
	var userClient = ElevenRackUserClient()
	@State var userClientText = ""

	var body: some View {
#if os(macOS)
		VStack(alignment: .leading, spacing: 14) {
			Text("Eleven Rack Driver")
				.padding()
				.font(.title)
			Text("Connect and power on your Eleven Rack, then activate the native Apple Silicon audio driver.")
				.fixedSize(horizontal: false, vertical: true)
			Text(self.viewModel.dextLoadingState)
				.multilineTextAlignment(.leading)
			HStack {
				Button(
					action: {
						self.viewModel.activateMyDext()
					}, label: {
						Text("Activate Driver")
					}
				)
			}
		}
		.padding(24)
		.frame(width: 520, height: 240, alignment: .leading)
#endif
	}
}

struct ElevenRackDriverView_Previews: PreviewProvider {
    static var previews: some View {
		ElevenRackView()
    }
}
