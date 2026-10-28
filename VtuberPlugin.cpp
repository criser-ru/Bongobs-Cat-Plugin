/**
  Created by Weng Y on 2020/05/25.
  Copyright © 2020 Weng Y. Under GNU General Public License v2.0.
*/
#include "VtuberPlugin.hpp"
#include "VtuberFrameWork.hpp"
#include "MouthSync.hpp"
#include <obs-module.h>
#include <util/platform.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <string>

namespace{
	struct Vtuber_data {
		obs_source_t *source = nullptr;
		uint16_t modelId = 0;

		// Mouth sync: the audio source we listen to (usually the microphone)
		std::mutex audioMutex;
		obs_source_t *audioSource = nullptr;
		std::string audioName;
		bool mouthEnabled = false;
		std::atomic<float> thresholdDb{-45.0f};
		std::atomic<float> rangeDb{25.0f};
		float retryTimer = 0.0f;

		// Diagnostics: loudest level over the last few seconds, written to the OBS log
		std::atomic<float> peakDb{-100.0f};
		std::atomic<uint64_t> lastLogNs{0};

		// Open mouth drawn under the cat's own "ω" mouth
		std::atomic<bool> drawMouth{true};
		std::atomic<float> mouthWidth{100.0f};  // % of the default width
		std::atomic<float> mouthDepth{100.0f};  // % of the default max depth
		std::atomic<uint32_t> mouthFill{0xFFFFFFFF}; // OBS colour, 0xAABBGGRR
		float shownLevel = 0.0f;                  // smoothed level, render thread only
		uint64_t lastRenderNs = 0;
	};

	// Blends one pixel of colour (r, g, b) with coverage a (0..1) over the frame.
	inline void blend_pixel(uint8_t *px, uint8_t r, uint8_t g, uint8_t b, float a)
	{
		if (a <= 0.0f)
			return;
		if (a > 1.0f)
			a = 1.0f;
		px[0] = (uint8_t)(r * a + px[0] * (1.0f - a));
		px[1] = (uint8_t)(g * a + px[1] * (1.0f - a));
		px[2] = (uint8_t)(b * a + px[2] * (1.0f - a));
		float dstA = px[3] / 255.0f;
		px[3] = (uint8_t)((a + dstA * (1.0f - a)) * 255.0f);
	}

	inline float clamp01(float v)
	{
		return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	}

	// Draws the open mouth: a "U" that hangs under the cat's "ω" between the two
	// anchor points from the model and gets deeper with a louder voice.
	// The frame comes from glReadPixels: RGBA, rows bottom-up, width*4 bytes per row,
	// and the anchors use the same coordinates (y goes up), so no flipping is needed.
	void draw_mouth(Vtuber_data *vtb, uint8_t *data, int width, int height)
	{
		// Smooth the level here so it does not depend on the model "Speed" setting
		uint64_t now = os_gettime_ns();
		float dt = vtb->lastRenderNs ? (float)((now - vtb->lastRenderNs) / 1e9) : 0.0f;
		vtb->lastRenderNs = now;
		if (dt > 0.2f)
			dt = 0.2f;

		float target = MouthSync::GetLevel();
		if (MouthSync::IsTestMode())
			target = 0.5f + 0.5f * std::sin((float)(now / 1e9) * 6.0f);
		float speed = (target > vtb->shownLevel) ? 25.0f : 12.0f;
		float k = dt * speed;
		if (k > 1.0f)
			k = 1.0f;
		vtb->shownLevel += (target - vtb->shownLevel) * k;

		float level = vtb->shownLevel;
		float ax, ay, bx, by;
		if (level < 0.05f || width <= 0 || height <= 0 ||
		    !MouthSync::GetAnchors(ax, ay, bx, by))
			return;

		// Local frame: t along the "ω", n pointing down out of it
		float tx = bx - ax, ty = by - ay;
		float w0 = std::sqrt(tx * tx + ty * ty);
		if (w0 < 1.0f)
			return;
		tx /= w0;
		ty /= w0;
		float nx = ty, ny = -tx;
		float cx = (ax + bx) * 0.5f, cy = (ay + by) * 0.5f;

		float w = w0 * vtb->mouthWidth.load() / 100.0f;
		float hw = w * 0.5f;
		float depth = w0 * vtb->mouthDepth.load() / 100.0f * level;
		float lw = w0 * 0.22f; // line as thick as the cat's own lines
		if (lw < 1.5f)
			lw = 1.5f;
		float r = depth < hw ? depth : hw; // bottom corners rounded

		uint32_t fill = vtb->mouthFill.load();
		uint8_t fr = fill & 0xFF, fg = (fill >> 8) & 0xFF, fb = (fill >> 16) & 0xFF;

		// Bounding box of the shape in frame pixels
		float ext = hw + lw;
		float cornersS[4] = {-ext, ext, -ext, ext};
		float cornersD[4] = {-1.0f, -1.0f, depth + lw, depth + lw};
		float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
		for (int i = 0; i < 4; i++) {
			float x = cx + cornersS[i] * tx + cornersD[i] * nx;
			float y = cy + cornersS[i] * ty + cornersD[i] * ny;
			minX = std::fmin(minX, x);
			maxX = std::fmax(maxX, x);
			minY = std::fmin(minY, y);
			maxY = std::fmax(maxY, y);
		}
		int x0 = (int)std::floor(minX), x1 = (int)std::ceil(maxX);
		int y0 = (int)std::floor(minY), y1 = (int)std::ceil(maxY);
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > width - 1) x1 = width - 1;
		if (y1 > height - 1) y1 = height - 1;

		for (int y = y0; y <= y1; y++) {
			uint8_t *line = data + (size_t)y * width * 4;
			for (int x = x0; x <= x1; x++) {
				float gx = x + 0.5f - cx, gy = y + 0.5f - cy;
				float s = gx * tx + gy * ty;  // along the "ω"
				float d = gx * nx + gy * ny;  // down from it
				float as = std::fabs(s);

				// Signed distance to the "U" (sides and rounded bottom, open top)
				float dist;
				if (d < depth - r) {
					dist = as - hw;
				} else if (as > hw - r) {
					float qx = as - (hw - r), qy = d - (depth - r);
					dist = std::sqrt(qx * qx + qy * qy) - r;
				} else {
					dist = d - depth;
				}

				float top = clamp01(d + 0.5f); // nothing above the "ω" line
				if (top <= 0.0f)
					continue;
				uint8_t *px = line + x * 4;
				blend_pixel(px, fr, fg, fb, clamp01(-dist - lw * 0.5f + 0.5f) * top);
				blend_pixel(px, 13, 13, 13, clamp01(lw * 0.5f - std::fabs(dist) + 0.5f) * top);
			}
		}
	}

	// Finds an audio source by name, including global ones (Mic/Aux, Desktop Audio).
	// Returns a source with an added reference, or nullptr.
	obs_source_t *find_audio_source(const std::string &name)
	{
		if (name.empty())
			return nullptr;
		obs_source_t *src = obs_get_source_by_name(name.c_str());
		if (src)
			return src;
		for (uint32_t ch = 1; ch <= 6; ch++) {
			obs_source_t *out = obs_get_output_source(ch);
			if (!out)
				continue;
			const char *n = obs_source_get_name(out);
			if (n && name == n)
				return out;
			obs_source_release(out);
		}
		return nullptr;
	}

	// Called by OBS from the audio thread for every audio packet of the source.
	void mouth_audio_callback(void *param, obs_source_t *,
				  const struct audio_data *audio, bool muted)
	{
		Vtuber_data *vtb = (Vtuber_data *)param;
		if (muted || !audio || audio->frames == 0) {
			MouthSync::SetLevel(0.0f);
			return;
		}

		size_t channels = audio_output_get_channels(obs_get_audio());
		if (channels > MAX_AV_PLANES)
			channels = MAX_AV_PLANES;

		double sum = 0.0;
		size_t count = 0;
		for (size_t c = 0; c < channels; c++) {
			const float *samples = (const float *)audio->data[c];
			if (!samples)
				continue;
			for (uint32_t i = 0; i < audio->frames; i++)
				sum += (double)samples[i] * samples[i];
			count += audio->frames;
		}
		if (count == 0) {
			MouthSync::SetLevel(0.0f);
			return;
		}

		double rms = std::sqrt(sum / (double)count);
		float db = rms > 0.0 ? (float)(20.0 * std::log10(rms)) : -100.0f;
		MouthSync::SetLevel(MouthSync::DbToLevel(
			db, vtb->thresholdDb.load(), vtb->rangeDb.load()));

		if (db > vtb->peakDb.load())
			vtb->peakDb.store(db);
		uint64_t now = os_gettime_ns();
		if (now - vtb->lastLogNs.load() > 5000000000ULL) {
			vtb->lastLogNs.store(now);
			float peak = vtb->peakDb.exchange(-100.0f);
			blog(LOG_INFO,
			     "[bongobs-cat] mouth sync: loudest voice in 5 s = %.1f dB (threshold %.1f dB, fully open at %.1f dB)",
			     peak, vtb->thresholdDb.load(),
			     vtb->thresholdDb.load() + vtb->rangeDb.load());
		}
	}

	// Both helpers below must be called with vtb->audioMutex locked.
	void detach_audio_locked(Vtuber_data *vtb)
	{
		if (vtb->audioSource) {
			obs_source_remove_audio_capture_callback(
				vtb->audioSource, mouth_audio_callback, vtb);
			obs_source_release(vtb->audioSource);
			vtb->audioSource = nullptr;
		}
		MouthSync::SetLevel(0.0f);
	}

	void attach_audio_locked(Vtuber_data *vtb)
	{
		if (vtb->audioSource || !vtb->mouthEnabled || vtb->audioName.empty())
			return;
		obs_source_t *src = find_audio_source(vtb->audioName);
		if (!src)
			return; // the source may not be loaded yet, VtuberTick retries
		obs_source_add_audio_capture_callback(src, mouth_audio_callback, vtb);
		vtb->audioSource = src;
		blog(LOG_INFO, "[bongobs-cat] mouth sync: listening to audio source '%s'",
		     vtb->audioName.c_str());
	}

	void update_mouth_settings(Vtuber_data *vtb, obs_data_t *settings)
	{
		bool enabled = obs_data_get_bool(settings, "mouth_sync");
		const char *name = obs_data_get_string(settings, "mouth_audio_source");
		std::string audioName = name ? name : "";

		vtb->thresholdDb.store((float)obs_data_get_double(settings, "mouth_threshold"));
		vtb->rangeDb.store((float)obs_data_get_double(settings, "mouth_range"));
		vtb->drawMouth.store(obs_data_get_bool(settings, "mouth_draw"));
		vtb->mouthWidth.store((float)obs_data_get_double(settings, "mouth_width"));
		vtb->mouthDepth.store((float)obs_data_get_double(settings, "mouth_depth"));
		vtb->mouthFill.store((uint32_t)obs_data_get_int(settings, "mouth_fill"));

		const char *param = obs_data_get_string(settings, "mouth_param");
		MouthSync::SetParameterName(param ? param : "");

		std::lock_guard<std::mutex> lock(vtb->audioMutex);
		if (enabled != vtb->mouthEnabled || audioName != vtb->audioName) {
			detach_audio_locked(vtb);
			vtb->mouthEnabled = enabled;
			vtb->audioName = audioName;
			attach_audio_locked(vtb);
		}
		MouthSync::SetEnabled(enabled);
		MouthSync::SetTestMode(obs_data_get_bool(settings, "mouth_test"));
		blog(LOG_INFO,
		     "[bongobs-cat] mouth sync: %s, audio source '%s' (%s), test %s",
		     enabled ? "ON" : "OFF", vtb->audioName.c_str(),
		     vtb->audioSource ? "connected" : "not connected",
		     obs_data_get_bool(settings, "mouth_test") ? "ON" : "OFF");
	}

	bool add_audio_source_name(void *param, obs_source_t *src)
	{
		std::set<std::string> *names = (std::set<std::string> *)param;
		if (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) {
			const char *n = obs_source_get_name(src);
			if (n && *n)
				names->insert(n);
		}
		return true;
	}

	void fill_audio_source_list(obs_property_t *p)
	{
		std::set<std::string> names;
		obs_enum_sources(add_audio_source_name, &names);
		for (uint32_t ch = 1; ch <= 6; ch++) {
			obs_source_t *out = obs_get_output_source(ch);
			if (!out)
				continue;
			const char *n = obs_source_get_name(out);
			if (n && *n)
				names.insert(n);
			obs_source_release(out);
		}

		obs_property_list_add_string(p, obs_module_text("MouthNoSource"), "");
		for (const std::string &n : names)
			obs_property_list_add_string(p, n.c_str(), n.c_str());
	}

	}

const char * VtuberPlugin::VtuberPlugin::VtuberGetName(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "bongobs cat";
}

void * VtuberPlugin::VtuberPlugin::VtuberCreate(obs_data_t *settings,
							   obs_source_t *source)
{
	Vtuber_data *vtb = new Vtuber_data();
	vtb->source = source;
	vtb->modelId = 0;

	double x = obs_data_get_double(settings, "x");
	double y = obs_data_get_double(settings, "y");
	int width = obs_data_get_int(settings, "width");
	int height = obs_data_get_int(settings, "height");
	double scale = obs_data_get_double(settings, "scale");
	double delayTime = obs_data_get_double(settings, "delaytime");
	bool random_motion = obs_data_get_bool(settings, "random_motion");
	bool breath = obs_data_get_bool(settings, "breath");
	bool eyeblink = obs_data_get_bool(settings, "eyeblink");
	bool track = obs_data_get_bool(settings, "track");
	const char *mode = obs_data_get_string(settings, "mode");
	bool live2D = obs_data_get_bool(settings, "live2d");
	bool relative_mouse = obs_data_get_bool(settings, "relative_mouse");
	bool mouse_horizontal_flip =obs_data_get_bool(settings, "mouse_horizontal_flip");
	bool mouse_vertical_flip =obs_data_get_bool(settings, "mouse_vertical_flip");
	bool mask = obs_data_get_bool(settings, "mask");

	VtuberFrameWork::InitVtuber(vtb->modelId);

	VtuberFrameWork::UpData(vtb->modelId, x, y, width, height, scale,
				delayTime, random_motion, breath, eyeblink,
				NULL, track, mode, live2D, relative_mouse,
				mouse_horizontal_flip, mouse_vertical_flip,mask);

	update_mouth_settings(vtb, settings);
	return vtb;
}

void VtuberPlugin::VtuberPlugin::VtuberDestroy(void *data)
{
	
	Vtuber_data *spv = (Vtuber_data *)data;

	{
		std::lock_guard<std::mutex> lock(spv->audioMutex);
		detach_audio_locked(spv);
	}
	MouthSync::SetEnabled(false);

	VtuberFrameWork::UinitVtuber(0);

	delete spv;
}

void VtuberPlugin::VtuberPlugin::VtuberRender(
	void *data,
	gs_effect_t *effect)
{
	Vtuber_data *spv = (Vtuber_data *)data;

	int width, height;
	width = VtuberFrameWork::GetWidth(spv->modelId);
	height = VtuberFrameWork::GetHeight(spv->modelId);

	obs_enter_graphics();

	gs_texture_t *tex =gs_texture_create(width, height, GS_RGBA, 1, NULL, GS_DYNAMIC);

	uint8_t *ptr;
	uint32_t linesize;
	if (gs_texture_map(tex, &ptr, &linesize)) {
		MouthSync::BeginFrame();
		VtuberFrameWork::ReanderVtuber(spv->modelId,(char*) ptr,width,height);
		if (MouthSync::IsEnabled() && spv->drawMouth.load())
			draw_mouth(spv, ptr, width, height);
	}
	gs_texture_unmap(tex);
	
	obs_source_draw(tex, 0, 0, 0, 0, true);

	gs_texture_destroy(tex);

	obs_leave_graphics();
}

uint32_t VtuberPlugin::VtuberPlugin::VtuberWidth(void *data)
{
	Vtuber_data *vtb = (Vtuber_data *)data;
	return VtuberFrameWork::GetWidth(vtb->modelId);
}

uint32_t VtuberPlugin::VtuberPlugin::VtuberHeight(void *data)
{
	Vtuber_data *vtb = (Vtuber_data *)data;
	return VtuberFrameWork::GetHeight(vtb->modelId);
}

static void fill_vtuber_model_list(obs_property_t *p, void *data)
{
	Vtuber_data *vtb = (Vtuber_data *)data;
	int _size;
	const char **_modeNames= VtuberFrameWork::GetModeDefine(_size);

	for (int index = 0; index < _size; index++) {
		obs_property_list_add_string(p, _modeNames[index],_modeNames[index]);
	}
	

}

obs_properties_t * VtuberPlugin::VtuberPlugin::VtuberGetProperties(void *data)
{

	Vtuber_data *vtb = (Vtuber_data *)data;

	obs_properties_t *ppts = obs_properties_create();
	obs_properties_set_param(ppts, vtb, NULL);

	obs_property_t *p;


        //obs_properties_add_path(ppts, "models_path",obs_module_text("ModelsPath"), OBS_PATH_FILE,"*.model3.json", "Resources/");

	
	p = obs_properties_add_list(ppts, "mode", obs_module_text("Mode"),
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	fill_vtuber_model_list(p, data);

	//obs_property_set_modified_callback(p, vtuber_model_callback);
	//obs_properties_add_int(ppts, "width", obs_module_text("Width"), 32, 1900, 32);
	//obs_properties_add_int(ppts, "height", obs_module_text("Height"), 32, 1050, 32);
	//obs_properties_add_float_slider(ppts,"scale",obs_module_text("Scale"),0.1,10.0,0.1);
	//obs_properties_add_float_slider(ppts, "x", obs_module_text("X"), -3.0, 3.0, 0.1);
	//obs_properties_add_float_slider(ppts, "y", obs_module_text("Y"), -3.0, 3.0, 0.1);
	obs_properties_add_bool(ppts, "relative_mouse", obs_module_text("Relative Mouse Movement"));
	obs_properties_add_bool(ppts, "mouse_horizontal_flip", obs_module_text("Mouse Horizontal Flip"));
	obs_properties_add_bool(ppts, "mouse_vertical_flip", obs_module_text("Mouse Vertical Flip"));
	obs_properties_add_bool(ppts, "mask", obs_module_text("Use Mask"));
	obs_properties_add_bool(ppts, "live2d", obs_module_text("Live2D"));
	obs_properties_add_float_slider(ppts, "delay", obs_module_text("Speed"), 0.0, 10.0, 0.1);
	obs_properties_add_bool(ppts,"random_motion",obs_module_text("RandomMotion"));
	obs_properties_add_bool(ppts, "breath", obs_module_text("Breath"));
	obs_properties_add_bool(ppts, "eyeblink", obs_module_text("EyeBlink"));
	obs_properties_add_bool(ppts, "track", obs_module_text("Mouse Tracktion"));

	// Mouth sync
	obs_properties_add_bool(ppts, "mouth_sync", obs_module_text("MouthSync"));
	p = obs_properties_add_list(ppts, "mouth_audio_source",
				    obs_module_text("MouthAudioSource"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_audio_source_list(p);
	p = obs_properties_add_float_slider(ppts, "mouth_threshold",
					    obs_module_text("MouthThreshold"),
					    -80.0, 0.0, 1.0);
	obs_property_float_set_suffix(p, " dB");
	p = obs_properties_add_float_slider(ppts, "mouth_range",
					    obs_module_text("MouthRange"),
					    5.0, 60.0, 1.0);
	obs_property_float_set_suffix(p, " dB");
	obs_properties_add_bool(ppts, "mouth_test", obs_module_text("MouthTest"));
	obs_properties_add_bool(ppts, "mouth_draw", obs_module_text("MouthDraw"));
	p = obs_properties_add_float_slider(ppts, "mouth_width", obs_module_text("MouthWidth"), 40.0, 200.0, 5.0);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(ppts, "mouth_depth", obs_module_text("MouthDepth"), 20.0, 250.0, 5.0);
	obs_property_float_set_suffix(p, " %");
	obs_properties_add_color(ppts, "mouth_fill", obs_module_text("MouthFill"));
	obs_properties_add_text(ppts, "mouth_param",
				obs_module_text("MouthParam"), OBS_TEXT_DEFAULT);
	

	return ppts;
}

void VtuberPlugin::VtuberPlugin::Vtuber_update(
	void *data, obs_data_t *settings)
{
	Vtuber_data *vtb = (Vtuber_data *)data;

	double x = obs_data_get_double(settings, "x");
	double y = obs_data_get_double(settings, "y");
	int width = obs_data_get_int(settings,"width");
	int height = obs_data_get_int(settings, "height");
	double vscale = obs_data_get_double(settings, "scale");
	double delayTime = obs_data_get_double(settings, "delay");
	bool random_motion = obs_data_get_bool(settings, "random_motion");
	bool breath = obs_data_get_bool(settings, "breath");
	bool eyeblink = obs_data_get_bool(settings, "eyeblink");
	bool track = obs_data_get_bool(settings, "track");
	const char *mode = obs_data_get_string(settings, "mode");
	bool live2D = obs_data_get_bool(settings, "live2d");
	bool relative_mouse = obs_data_get_bool(settings, "relative_mouse");
	bool mouse_horizontal_flip =obs_data_get_bool(settings, "mouse_horizontal_flip");
	bool mouse_vertical_flip =obs_data_get_bool(settings, "mouse_vertical_flip");
	bool mask = obs_data_get_bool(settings, "mask");

	const char *vtb_str = NULL; //obs_data_get_string(settings, "models_path");

	VtuberFrameWork::UpData(vtb->modelId, x, y, width, height, vscale,
				delayTime, random_motion, breath, eyeblink,
				vtb_str, track, mode, live2D, relative_mouse,
				mouse_horizontal_flip, mouse_vertical_flip,
				mask);

	update_mouth_settings(vtb, settings);
}

void VtuberPlugin::VtuberPlugin::VtuberTick(void *data, float seconds)
{
	Vtuber_data *vtb = (Vtuber_data *)data;

	std::lock_guard<std::mutex> lock(vtb->audioMutex);
	if (!vtb->mouthEnabled || vtb->audioName.empty())
		return;

	// The microphone source was deleted in OBS: let go of it
	if (vtb->audioSource && obs_source_removed(vtb->audioSource))
		detach_audio_locked(vtb);

	// Not attached yet (OBS is still loading or the source was renamed):
	// try again once a second
	if (!vtb->audioSource) {
		vtb->retryTimer += seconds;
		if (vtb->retryTimer >= 1.0f) {
			vtb->retryTimer = 0.0f;
			attach_audio_locked(vtb);
		}
	}
}

void VtuberPlugin::VtuberPlugin::Vtuber_defaults(
	obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 1280);
	obs_data_set_default_int(settings, "height", 768);
	obs_data_set_default_double(settings, "x", 0);
	obs_data_set_default_double(settings, "y", 0.02);
	obs_data_set_default_double(settings, "scale", 1.83);
	obs_data_set_default_double(settings, "delaytime", 1.0);
	obs_data_set_default_bool(settings, "random_motion", true);
	obs_data_set_default_bool(settings, "breath",true);
	obs_data_set_default_bool(settings, "eyeblink",true);
	obs_data_set_default_bool(settings, "track", true);
	obs_data_set_default_string(settings, "Mode", "standard");
	obs_data_set_default_bool(settings, "live2d", true);
	obs_data_set_default_bool(settings, "relative_mouse", false);
	obs_data_set_default_bool(settings, "mouse_horizontal_flip",true);
	obs_data_set_default_bool(settings, "mouse_vertical_flip",true);
	obs_data_set_default_bool(settings, "mask", false);
	obs_data_set_default_bool(settings, "mouth_sync", false);
	obs_data_set_default_string(settings, "mouth_audio_source", "");
	obs_data_set_default_double(settings, "mouth_threshold", -45.0);
	obs_data_set_default_double(settings, "mouth_range", 25.0);
	obs_data_set_default_string(settings, "mouth_param", "ParamMouthOpenY");
	obs_data_set_default_bool(settings, "mouth_test", false);
	obs_data_set_default_bool(settings, "mouth_draw", true);
	obs_data_set_default_double(settings, "mouth_width", 100.0);
	obs_data_set_default_double(settings, "mouth_depth", 100.0);
	obs_data_set_default_int(settings, "mouth_fill", 0xFFFFFFFF);
}
