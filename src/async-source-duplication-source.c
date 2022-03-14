#include <obs-module.h>
#include "plugin-macros.generated.h"

struct source_s
{
	obs_source_t *context;

	// properties
	char *target_source_name;

	obs_weak_source_t *target_weak;
};

static const char *get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Asynchronous Source Duplicator");
}

static obs_properties_t *get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop;

	obs_properties_add_text(props, "target_source_name", obs_module_text("Source Name"), OBS_TEXT_DEFAULT);

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

static void release_weak_target(struct source_s *s)
{
	if (!s->target_weak)
		return;

	obs_source_t *target = obs_weak_source_get_source(s->target_weak);
	if (target) {
		signal_handler_t *sh = obs_source_get_signal_handler(target);
		signal_handler_disconnect(sh, "output_video", output_video, s);
		signal_handler_disconnect(sh, "output_audio", output_audio, s);
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

	signal_handler_t *sh = obs_source_get_signal_handler(target);
	signal_handler_connect(sh, "output_video", output_video, s);
	signal_handler_connect(sh, "output_audio", output_audio, s);
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

	if (target)
		blog(LOG_INFO, "found target filter \"%s\" in source \"%s\"", obs_source_get_name(target),
		     obs_source_get_name(src));
	else
		blog(LOG_INFO, "not found the target filter in \"%s\"", obs_source_get_name(src));

	return target;
}

static inline void set_weak_target_by_name(struct source_s *s, const char *target_source_name)
{
	obs_source_t *src = obs_get_source_by_name(target_source_name);
	if (!src)
		return;
	obs_source_t *target = source_to_filter(src);
	if (target) {
		set_weak_target(s, target);
		obs_source_release(target);
	}
	obs_source_release(src);
}

static void update(void *data, obs_data_t *settings)
{
	struct source_s *s = data;

	const char *target_source_name = obs_data_get_string(settings, "target_source_name");
	if (target_source_name && (!s->target_source_name || strcmp(target_source_name, s->target_source_name))) {
		bfree(s->target_source_name);
		s->target_source_name = bstrdup(target_source_name);
		release_weak_target(s);
		set_weak_target_by_name(s, target_source_name);
	}
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	struct source_s *s = bzalloc(sizeof(struct source_s));
	s->context = source;

	update(s, settings);

	return s;
}

static void destroy(void *data)
{
	struct source_s *s = data;

	release_weak_target(s);
	bfree(s->target_source_name);

	bfree(s);
}

const struct obs_source_info async_srcdup_source = {
	.id = ID_PREFIX "source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = get_name,
	.create = create,
	.destroy = destroy,
	.update = update,
	.get_properties = get_properties,
};
