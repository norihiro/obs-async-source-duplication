#include <inttypes.h>
#include <obs-module.h>
#include <util/threading.h>
#include "plugin-macros.generated.h"

static const char *signals[] = {
	"void output_video(ptr frame)",
	"void output_audio(ptr audio)",
	NULL,
};

struct filter_s
{
	obs_source_t *context;

	pthread_mutex_t video_mutex;
	pthread_mutex_t audio_mutex;
	bool in_video;
	bool in_audio;

	volatile long show_refs;
	volatile long active_refs;

	gs_texrender_t *texrender;
};

static const char *get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Asynchronous Source Duplication Filter");
}

static void async_filter_video_internal(struct filter_s *s, struct obs_source_frame *frame)
{
	struct calldata data;
	uint8_t stack[128];
	calldata_init_fixed(&data, stack, sizeof(stack));
	calldata_set_ptr(&data, "frame", frame);

	signal_handler_signal(obs_source_get_signal_handler(s->context), "output_video", &data);
}

static void async_filter_audio_internal(struct filter_s *s, struct obs_audio_data *audio)
{
	const struct audio_output_info *obs_info = audio_output_get_info(obs_get_audio());
	if (!obs_info)
		return;

	struct obs_source_audio srcaudio = {
		.frames = audio->frames,
		.timestamp = audio->timestamp,
		.speakers = obs_info->speakers,
		.format = obs_info->format,
		.samples_per_sec = obs_info->samples_per_sec,
	};
	for (int i = 0; i < MAX_AV_PLANES; i++)
		srcaudio.data[i] = audio->data[i];

	struct calldata data;
	uint8_t stack[128];
	calldata_init_fixed(&data, stack, sizeof(stack));
	calldata_set_ptr(&data, "audio", &srcaudio);

	signal_handler_signal(obs_source_get_signal_handler(s->context), "output_audio", &data);
}

static struct obs_source_frame *async_filter_video(void *data, struct obs_source_frame *frame)
{
	struct filter_s *s = data;

	pthread_mutex_lock(&s->video_mutex);
	if (!s->in_video) {
		/* `in_video` will be true if a loop exists. */
		s->in_video = true;
		async_filter_video_internal(s, frame);
		s->in_video = false;
	}
	pthread_mutex_unlock(&s->video_mutex);

	return frame;
}

static struct obs_audio_data *async_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct filter_s *s = data;

	pthread_mutex_lock(&s->audio_mutex);
	if (!s->in_audio) {
		/* `in_audio` will be true if a loop exists. */
		s->in_audio = true;
		async_filter_audio_internal(s, audio);
		s->in_audio = false;
	}
	pthread_mutex_unlock(&s->audio_mutex);

	return audio;
}

#if LIBOBS_API_VER <= MAKE_SEMANTIC_VERSION(27, 0, 1)
static inline int pthread_mutex_init_recursive(pthread_mutex_t *mutex)
{
	pthread_mutexattr_t attr;
	int ret = pthread_mutexattr_init(&attr);
	if (ret == 0) {
		ret = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		if (ret == 0) {
			ret = pthread_mutex_init(mutex, &attr);
		}

		pthread_mutexattr_destroy(&attr);
	}

	return ret;
}
#endif

static void inc_showing(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct filter_s *s = data;

	if (os_atomic_inc_long(&s->show_refs) == 1)
		obs_source_inc_showing(obs_filter_get_parent(s->context));
}

static void dec_showing(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct filter_s *s = data;

	if (os_atomic_dec_long(&s->show_refs) == 0)
		obs_source_dec_showing(obs_filter_get_parent(s->context));
}

static void inc_active(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct filter_s *s = data;

	if (os_atomic_inc_long(&s->active_refs) == 1)
		obs_source_inc_active(obs_filter_get_parent(s->context));
}

static void dec_active(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct filter_s *s = data;

	if (os_atomic_dec_long(&s->active_refs) == 0)
		obs_source_dec_active(obs_filter_get_parent(s->context));
}

static void offscreen_render_cb(void *data, uint32_t cx, uint32_t cy)
{
	struct filter_s *s = data;

	if (os_atomic_load_long(&s->show_refs) <= 0)
		return;

	obs_source_t *parent = obs_filter_get_parent(s->context);
	if (!parent)
		return;

	gs_texrender_reset(s->texrender);
	if (!gs_texrender_begin(s->texrender, 1, 1))
		return;

	obs_source_video_render(parent);

	gs_texrender_end(s->texrender);
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	struct filter_s *s = bzalloc(sizeof(struct filter_s));
	s->context = source;

	pthread_mutex_init_recursive(&s->video_mutex);
	pthread_mutex_init_recursive(&s->audio_mutex);

	s->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);

	// update(s, settings); // no properties in this filter.

	signal_handler_add_array(obs_source_get_signal_handler(source), signals);
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void inc_showing()", inc_showing, s);
	proc_handler_add(ph, "void dec_showing()", dec_showing, s);
	proc_handler_add(ph, "void inc_active()", inc_active, s);
	proc_handler_add(ph, "void dec_active()", dec_active, s);

	obs_add_main_render_callback(offscreen_render_cb, s);

	return s;
}

static void filter_remove(void *data, obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	struct filter_s *s = data;

	while (os_atomic_load_long(&s->active_refs) > 0)
		dec_active(s, NULL);
	while (os_atomic_load_long(&s->show_refs) > 0)
		dec_showing(s, NULL);
}

static void destroy(void *data)
{
	struct filter_s *s = data;

	obs_remove_main_render_callback(offscreen_render_cb, s);

	pthread_mutex_destroy(&s->audio_mutex);
	pthread_mutex_destroy(&s->video_mutex);

	obs_enter_graphics();
	gs_texrender_destroy(s->texrender);
	obs_leave_graphics();

	bfree(s);
}

const struct obs_source_info async_srcdup_filter = {
	.id = ID_PREFIX "filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = get_name,
	.create = create,
	.destroy = destroy,
	.filter_remove = filter_remove,
	.filter_video = async_filter_video,
	.filter_audio = async_filter_audio,
};
