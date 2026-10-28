/**
  Mouth sync (lip sync by microphone volume) for Bongobs Cat.
  Under GNU General Public License v2.0, same as the rest of the plugin.

  The OBS audio thread writes the current voice level (0..1) here,
  and the render thread reads it in Model::Update to open the mouth.
*/
#pragma once
#include <string>

namespace MouthSync {

	// Turns the feature on/off. When off, the model behaves exactly as before.
	void SetEnabled(bool enabled);
	bool IsEnabled();

	// Current target mouth opening, 0 = closed, 1 = fully open.
	void SetLevel(float level);
	float GetLevel();

	// Live2D parameter that opens the mouth (ParamMouthOpenY by default).
	void SetParameterName(const std::string &name);
	// Returns a counter that changes every time the parameter name changes,
	// so the model knows when to look the parameter up again.
	unsigned int GetParameterVersion();
	std::string GetParameterName();

	// Test mode: the mouth opens and closes by itself, without audio.
	// Shows whether the model can open its mouth at all.
	void SetTestMode(bool test);
	bool IsTestMode();

	// Converts an audio level in dB to a mouth opening 0..1:
	// below thresholdDb -> 0, at thresholdDb + rangeDb and above -> 1.
	float DbToLevel(float db, float thresholdDb, float rangeDb);
}
