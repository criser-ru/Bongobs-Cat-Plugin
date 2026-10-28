/**
  Created by Weng Y on 2020/05/25.
  Copyright © 2020 Weng Y. Under GNU General Public License v2.0.
*/
#include "VtuberPlugin.hpp"
#include "VtuberFrameWork.hpp"
#include "MouthSync.hpp"
#include <obs-module.h>
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
	};

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
	}

	void update_mouth_settings(Vtuber_data *vtb, obs_data_t *settings)
	{
		bool enabled = obs_data_get_bool(settings, "mouth_sync");
		const char *name = obs_data_get_string(settings, "mouth_audio_source");
		std::string audioName = name ? name : "";

		vtb->thresholdDb.store((float)obs_data_get_double(settings, "mouth_threshold"));
		vtb->rangeDb.store((float)obs_data_get_double(settings, "mouth_range"));

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
	if (gs_texture_map(tex, &ptr, &linesize))
		VtuberFrameWork::ReanderVtuber(spv->modelId,(char*) ptr,width,height);
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
}
