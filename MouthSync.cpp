/**
  Mouth sync (lip sync by microphone volume) for Bongobs Cat.
  Under GNU General Public License v2.0, same as the rest of the plugin.
*/
#include "MouthSync.hpp"
#include <atomic>
#include <mutex>

namespace {
	std::atomic<bool> g_enabled{false};
	std::atomic<bool> g_test{false};
	std::atomic<float> g_level{0.0f};
	std::atomic<unsigned int> g_paramVersion{1};
	std::mutex g_nameMutex;
	std::string g_paramName = "ParamMouthOpenY";

	// Render thread only: written by the model, read by the plugin in the same frame
	bool g_anchorsValid = false;
	float g_anchors[4] = {0, 0, 0, 0};
}

void MouthSync::BeginFrame()
{
	g_anchorsValid = false;
}

void MouthSync::SetAnchors(float ax, float ay, float bx, float by)
{
	g_anchors[0] = ax;
	g_anchors[1] = ay;
	g_anchors[2] = bx;
	g_anchors[3] = by;
	g_anchorsValid = true;
}

bool MouthSync::GetAnchors(float &ax, float &ay, float &bx, float &by)
{
	if (!g_anchorsValid)
		return false;
	ax = g_anchors[0];
	ay = g_anchors[1];
	bx = g_anchors[2];
	by = g_anchors[3];
	return true;
}

void MouthSync::SetEnabled(bool enabled)
{
	g_enabled.store(enabled);
	if (!enabled)
		g_level.store(0.0f);
}

bool MouthSync::IsEnabled()
{
	return g_enabled.load();
}

void MouthSync::SetLevel(float level)
{
	if (level < 0.0f)
		level = 0.0f;
	if (level > 1.0f)
		level = 1.0f;
	g_level.store(level);
}

float MouthSync::GetLevel()
{
	return g_level.load();
}

void MouthSync::SetParameterName(const std::string &name)
{
	std::string value = name.empty() ? std::string("ParamMouthOpenY") : name;
	std::lock_guard<std::mutex> lock(g_nameMutex);
	if (value != g_paramName) {
		g_paramName = value;
		g_paramVersion.fetch_add(1);
	}
}

unsigned int MouthSync::GetParameterVersion()
{
	return g_paramVersion.load();
}

std::string MouthSync::GetParameterName()
{
	std::lock_guard<std::mutex> lock(g_nameMutex);
	return g_paramName;
}

void MouthSync::SetTestMode(bool test)
{
	g_test.store(test);
}

bool MouthSync::IsTestMode()
{
	return g_test.load();
}

float MouthSync::DbToLevel(float db, float thresholdDb, float rangeDb)
{
	if (rangeDb < 1.0f)
		rangeDb = 1.0f;
	float level = (db - thresholdDb) / rangeDb;
	if (level < 0.0f)
		level = 0.0f;
	if (level > 1.0f)
		level = 1.0f;
	return level;
}
