#include <obs-module.h>
#include <util/threading.h>
#include <util/dstr.h>
#include "plugin-macros.generated.h"

struct source_s
{
	obs_source_t *context;

	// properties
	char *target_source_name;

	pthread_mutex_t target_update_mutex;
	obs_weak_source_t *target_weak;
	float target_check;

	bool shown;
	bool activated;
};

static const char *get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Asynchronous Source Duplicator");
}

struct target_prop_info
{
	obs_property_t *prop;
	struct source_s *s;
};

static bool add_target_sources_cb(void *data, obs_source_t *source)
{
	struct target_prop_info *info = data;

	if (info->s && source == info->s->context)
		return true;

	uint32_t caps = obs_source_get_output_flags(source);
	bool is_async_video = (caps & OBS_SOURCE_ASYNC_VIDEO) == OBS_SOURCE_ASYNC_VIDEO;
	bool is_audio = !!(caps & OBS_SOURCE_AUDIO);
	if (!is_async_video && !is_audio)
		return true;

	const char *name = obs_source_get_name(source);
	obs_property_list_add_string(info->prop, name, name);

	return true;
}

static inline obs_source_t *source_to_filter(obs_source_t *src);

static bool target_source_modified_cb(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	bool ret = false;

	bool no_filter = false;
	const char *target_source_name = obs_data_get_string(settings, "target_source_name");
	obs_source_t *src = obs_get_source_by_name(target_source_name);
	if (src) {
		obs_source_t *target = source_to_filter(src);
		if (!target)
			no_filter = true;
		obs_source_release(target);
	}
	obs_source_release(src);

	obs_property_t *add_filter_button = obs_properties_get(props, "target_source_add_filter");
	if (obs_property_visible(add_filter_button) != no_filter) {
		obs_property_set_visible(add_filter_button, no_filter);
		ret = true;
	}

	return ret;
}

static void add_filter(obs_source_t *src)
{
	const char *display_name = obs_source_get_display_name(ID_PREFIX "filter");
	struct dstr name;
	dstr_init_copy(&name, display_name);
	obs_source_t *found;
	int ix = 0;
	while ((found = obs_source_get_filter_by_name(src, name.array))) {
		obs_source_release(found);
		dstr_printf(&name, "%s (%d)", display_name, ++ix);
	}

	obs_source_t *filter = obs_source_create_private(ID_PREFIX "filter", name.array, NULL);
	obs_source_filter_add(src, filter);
	blog(LOG_INFO, "added filter '%s' (%p) to source '%s'", name.array, filter, obs_source_get_name(src));
	obs_source_release(filter);
	dstr_free(&name);
}

static bool add_filter_cb(obs_properties_t *props, obs_property_t *property, void *data)
{
	if (!data)
		return false;
	struct source_s *s = data;

	obs_source_t *src = obs_get_source_by_name(s->target_source_name);
	if (src) {
		add_filter(src);
		s->target_check = 0.0f;
		obs_property_t *add_filter_button = obs_properties_get(props, "target_source_add_filter");
		obs_property_set_visible(add_filter_button, false);
		return true;
	}

	return false;
}

static obs_properties_t *get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop;
	obs_property_t *target_source_name;

	target_source_name = obs_properties_add_list(props, "target_source_name", obs_module_text("Source Name"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	prop = obs_properties_add_button2(props, "target_source_add_filter",
					  obs_module_text("Insert Filter to the Source"), add_filter_cb, data);
	obs_property_set_visible(prop, false);

	obs_property_set_modified_callback(target_source_name, target_source_modified_cb);
	struct target_prop_info info = {target_source_name, data};
	obs_enum_sources(add_target_sources_cb, &info);

	obs_properties_add_bool(props, "buffered", obs_module_text("Enable Buffering"));

	return props;
}

static void output_video(void *data, calldata_t *cd)
{
	struct source_s *s = data;

	struct obs_source_frame *frame = calldata_ptr(cd, "frame");
	obs_source_output_video(s->context, frame);
}

static void output_audio(void *data, calldata_t *cd)
{
	struct source_s *s = data;

	struct obs_source_audio *audio = calldata_ptr(cd, "audio");
	obs_source_output_audio(s->context, audio);
}

static void target_inc_showing(obs_source_t *target)
{
	proc_handler_t *ph = obs_source_get_proc_handler(target);
	proc_handler_call(ph, "inc_showing", NULL);
}

static void target_dec_showing(obs_source_t *target)
{
	proc_handler_t *ph = obs_source_get_proc_handler(target);
	proc_handler_call(ph, "dec_showing", NULL);
}

static void target_inc_active(obs_source_t *target)
{
	proc_handler_t *ph = obs_source_get_proc_handler(target);
	proc_handler_call(ph, "inc_active", NULL);
}

static void target_dec_active(obs_source_t *target)
{
	proc_handler_t *ph = obs_source_get_proc_handler(target);
	proc_handler_call(ph, "dec_active", NULL);
}

static void release_weak_target(struct source_s *s)
{
	if (!s->target_weak)
		return;

	obs_source_t *target = obs_weak_source_get_source(s->target_weak);
	if (target) {
		signal_handler_t *sh = obs_source_get_signal_handler(target);
		signal_handler_disconnect(sh, "output_video", output_video, s);
		signal_handler_disconnect(sh, "output_audio", output_audio, s);

		if (s->shown)
			target_dec_showing(target);
		if (s->activated)
			target_dec_active(target);

		obs_source_release(target);
	}

	obs_weak_source_release(s->target_weak);
	s->target_weak = NULL;
}

static void set_weak_target(struct source_s *s, obs_source_t *target)
{
	if (s->target_weak)
		release_weak_target(s);
	s->target_weak = obs_source_get_weak_source(target);
	s->target_check = 3.0f;

	signal_handler_t *sh = obs_source_get_signal_handler(target);
	signal_handler_connect(sh, "output_video", output_video, s);
	signal_handler_connect(sh, "output_audio", output_audio, s);

	if (s->shown)
		target_inc_showing(target);
	if (s->activated)
		target_inc_active(target);
}

static void find_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	obs_source_t **target = param;
	if (*target)
		return;
	const char *id = obs_source_get_id(child);
	if (strcmp(id, ID_PREFIX "filter") == 0)
		*target = obs_source_get_ref(child);
}

static inline obs_source_t *source_to_filter(obs_source_t *src)
{
	obs_source_t *target = NULL;
	obs_source_enum_filters(src, find_filter, &target);
	return target;
}

static obs_source_t *get_filter_by_target_source_name(const char *target_source_name)
{
	obs_source_t *src = obs_get_source_by_name(target_source_name);
	if (!src)
		return NULL;
	obs_source_t *target = source_to_filter(src);
	obs_source_release(src);
	return target;
}

static inline void set_weak_target_by_name(struct source_s *s, const char *target_source_name)
{
	obs_source_t *target = get_filter_by_target_source_name(target_source_name);
	if (target) {
		set_weak_target(s, target);
		obs_source_release(target);
	}
}

static void update(void *data, obs_data_t *settings)
{
	struct source_s *s = data;

	const char *target_source_name = obs_data_get_string(settings, "target_source_name");
	pthread_mutex_lock(&s->target_update_mutex);
	if (target_source_name && (!s->target_source_name || strcmp(target_source_name, s->target_source_name))) {
		bfree(s->target_source_name);
		s->target_source_name = bstrdup(target_source_name);
		release_weak_target(s);
		set_weak_target_by_name(s, target_source_name);
	}
	pthread_mutex_unlock(&s->target_update_mutex);

	bool buffered = obs_data_get_bool(settings, "buffered");
	obs_source_set_async_unbuffered(s->context, !buffered);
}

static void tick(void *data, float seconds)
{
	struct source_s *s = data;

	pthread_mutex_lock(&s->target_update_mutex);
	if ((s->target_check -= seconds) < 0.0f) {
		obs_source_t *target_by_name = get_filter_by_target_source_name(s->target_source_name);
		obs_source_t *target_by_weak = obs_weak_source_get_source(s->target_weak);
		if (target_by_name != target_by_weak) {
			blog(LOG_INFO, "updating target from %p to %p", target_by_weak, target_by_name);
			set_weak_target(s, target_by_name);
		}
		obs_source_release(target_by_weak);
		obs_source_release(target_by_name);
		s->target_check = 3.0f;
	}
	pthread_mutex_unlock(&s->target_update_mutex);
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	struct source_s *s = bzalloc(sizeof(struct source_s));
	s->context = source;
	pthread_mutex_init(&s->target_update_mutex, NULL);

	update(s, settings);

	return s;
}

static void destroy(void *data)
{
	struct source_s *s = data;

	release_weak_target(s);
	pthread_mutex_destroy(&s->target_update_mutex);
	bfree(s->target_source_name);

	bfree(s);
}

static void show(void *data)
{
	struct source_s *s = data;

	pthread_mutex_lock(&s->target_update_mutex);
	obs_source_t *target = obs_weak_source_get_source(s->target_weak);
	if (target && !s->shown)
		target_inc_showing(target);
	s->shown = true;
	pthread_mutex_unlock(&s->target_update_mutex);
}

static void hide(void *data)
{
	struct source_s *s = data;

	pthread_mutex_lock(&s->target_update_mutex);
	obs_source_t *target = obs_weak_source_get_source(s->target_weak);
	if (target && s->shown)
		target_dec_showing(target);
	s->shown = false;
	pthread_mutex_unlock(&s->target_update_mutex);
}

static void activate(void *data)
{
	struct source_s *s = data;

	pthread_mutex_lock(&s->target_update_mutex);
	obs_source_t *target = obs_weak_source_get_source(s->target_weak);
	if (target && !s->activated)
		target_inc_active(target);
	s->activated = true;
	pthread_mutex_unlock(&s->target_update_mutex);
}

static void deactivate(void *data)
{
	struct source_s *s = data;

	pthread_mutex_lock(&s->target_update_mutex);
	obs_source_t *target = obs_weak_source_get_source(s->target_weak);
	if (target && s->activated)
		target_dec_active(target);
	s->activated = false;
	pthread_mutex_unlock(&s->target_update_mutex);
}

const struct obs_source_info async_srcdup_source = {
	.id = ID_PREFIX "source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = get_name,
	.create = create,
	.destroy = destroy,
	.update = update,
	.video_tick = tick,
	.get_properties = get_properties,
	.show = show,
	.hide = hide,
	.activate = activate,
	.deactivate = deactivate,
};
