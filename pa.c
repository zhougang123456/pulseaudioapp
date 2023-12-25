#include <string.h>
#include <pulse/pulseaudio.h>
#include <pulse/thread-mainloop.h>
#include "pa.h"

struct _PaState{
    pa_threaded_mainloop *mainloop;
    pa_context *context;
    pa_stream *stream;
    pa_stream_direction_t direction;

    const void *read_data;
    size_t read_index, read_length;
};

static const pa_sample_spec ss = {
	.format = PA_SAMPLE_S16LE,
	.rate = 48000,
	.channels = 2
};

#define CHECK_DEAD_GOTO(p, label)                               \
    do {                                                                \
        if (!(p)->context || !PA_CONTEXT_IS_GOOD(pa_context_get_state((p)->context)) || \
            !(p)->stream || !PA_STREAM_IS_GOOD(pa_stream_get_state((p)->stream))) { \
            if (((p)->context && pa_context_get_state((p)->context) == PA_CONTEXT_FAILED) || \
                ((p)->stream && pa_stream_get_state((p)->stream) == PA_STREAM_FAILED)) { \
                    printf("error msg %s\n", pa_context_errno((p)->context));         \
            } else                                                      \
                printf("error msg PA_ERR_BADSTATE\n");                  \
            goto label;                                                 \
        }                                                               \
    } while(0);

static void context_state_cb(pa_context *c, void* userdata){

    PaState *p = (PaState*)userdata;
	switch(pa_context_get_state(c)) {
		case PA_CONTEXT_READY:
		case PA_CONTEXT_TERMINATED:
		case PA_CONTEXT_FAILED:
			pa_threaded_mainloop_signal(p->mainloop, 0);
			break;
		case PA_CONTEXT_UNCONNECTED:
		case PA_CONTEXT_CONNECTING:
		case PA_CONTEXT_AUTHORIZING:
		case PA_CONTEXT_SETTING_NAME:
			break;
	}
}

static void stream_state_cb(pa_stream *s, void *userdata) {
    PaState *p = (PaState*)userdata;
	switch (pa_stream_get_state(s)) {
		case PA_STREAM_READY:
		case PA_STREAM_FAILED:
		case PA_STREAM_TERMINATED:
			pa_threaded_mainloop_signal(p->mainloop, 0);
			break;
		case PA_STREAM_UNCONNECTED:
		case PA_STREAM_CREATING:
			break;
	}
}

static void stream_request_cb(pa_stream *stream, size_t length, void *userdata)
{
    PaState *p = (PaState*)userdata;
	pa_threaded_mainloop_signal(p->mainloop, 0);
}

static void stream_latency_update_cb(pa_stream *stream, void *userdata)
{
    PaState *p = (PaState*)userdata;
	pa_threaded_mainloop_signal(p->mainloop, 0);
}

PaState* pa_state_new(int pa_type)
{
    int ret;
    PaState *p = (PaState*)malloc(sizeof(PaState));
	memset(p, 0, sizeof(PaState));
    if (pa_type == PA_TYPE_PLAYBACK) {
        p->direction = PA_STREAM_PLAYBACK;
    } else {
        p->direction = PA_STREAM_RECORD;
    }
    pa_threaded_mainloop *mainloop = pa_threaded_mainloop_new();
    p->mainloop = mainloop;
	if (!p->mainloop) {
        printf("new pa mainloop fail!\n");
		goto fail;
	}
    
	p->context = pa_context_new(pa_threaded_mainloop_get_api(mainloop), 
         p->direction==PA_STREAM_PLAYBACK? "paplaytest" : "parectest");
	if (!p->context) {
		printf("new pa context fail!\n");
		goto fail;
	}
     
	pa_context_set_state_callback(p->context, context_state_cb, p);
	if (pa_context_connect(p->context, NULL, 0, NULL) < 0) {
		printf("pa connect fail!\n");
		goto fail;
	}

	pa_threaded_mainloop_lock(p->mainloop);

	if (pa_threaded_mainloop_start(p->mainloop) < 0) { 
		printf("pa start fail!\n");
		goto unlock_fail;
	}
	for (;;) {
		pa_context_state_t state;
		state = pa_context_get_state(p->context);
		if (state == PA_CONTEXT_READY) {
			break;
		}
		if (!PA_CONTEXT_IS_GOOD(state)) {
			goto unlock_fail;
		}
		pa_threaded_mainloop_wait(p->mainloop);
	}
	if (!(p->stream = pa_stream_new(p->context, 
            p->direction==PA_STREAM_PLAYBACK? "paplaytest" : "parectest", &ss, NULL))) {
		printf("new pa record stream fail!\n");
		goto unlock_fail;
	}
	pa_stream_set_state_callback(p->stream, stream_state_cb, p);
	pa_stream_set_read_callback(p->stream, stream_request_cb, p);
	pa_stream_set_write_callback(p->stream, stream_request_cb, p);
	pa_stream_set_latency_update_callback(p->stream, stream_latency_update_cb, p);
	if (p->direction == PA_STREAM_PLAYBACK) {
		ret = pa_stream_connect_playback(p->stream, NULL, NULL, 
						                 PA_STREAM_INTERPOLATE_TIMING
						                 |PA_STREAM_ADJUST_LATENCY
					                     |PA_STREAM_AUTO_TIMING_UPDATE,
					                     NULL, NULL);
	} else {
		ret = pa_stream_connect_record(p->stream, NULL, NULL,
					                   PA_STREAM_INTERPOLATE_TIMING
					                   |PA_STREAM_ADJUST_LATENCY
					                   |PA_STREAM_AUTO_TIMING_UPDATE);
	}
	if (ret < 0) {
		goto unlock_fail;
	}
	for (;;){
		pa_stream_state_t state;
		state = pa_stream_get_state(p->stream);
		if (state == PA_STREAM_READY){
			break;
		}
		if (!PA_STREAM_IS_GOOD(state)) {
			printf("pa stream is bad!\n");
			goto unlock_fail;
		}
		pa_threaded_mainloop_wait(p->mainloop);
	}
	pa_threaded_mainloop_unlock(p->mainloop);

    return p;

unlock_fail:
	pa_threaded_mainloop_unlock(p->mainloop);
fail:
	
	return NULL;
}

int pa_state_write(PaState *p, const void* data, int length)
{
    pa_threaded_mainloop_lock(p->mainloop);
    while (length > 0) {
        size_t l;
        int r;

        while (!(l = pa_stream_writable_size(p->stream))) {
            pa_threaded_mainloop_wait(p->mainloop);
            if (!PA_STREAM_IS_GOOD(pa_stream_get_state(p->stream))) {
                goto unlock_fail;
            }
        }

        if (l == -1) {
            goto unlock_fail;
        }

        if (l > length)
            l = length;

        r = pa_stream_write(p->stream, data, l, NULL, 0LL, PA_SEEK_RELATIVE);

        if (r < 0) {
            goto unlock_fail;
        }

        data = (const uint8_t*) data + l;
        length -= l;
    }

    pa_threaded_mainloop_unlock(p->mainloop);
    return 0;

unlock_fail:
    pa_threaded_mainloop_unlock(p->mainloop);
    return -1;
}

int pa_state_read(PaState *p, void* data, int length)
{
    int ret;
	pa_threaded_mainloop_lock(p->mainloop);
	while (length > 0) {
		size_t l;
		while (!p->read_data) {
			ret = pa_stream_peek(p->stream, &p->read_data, &p->read_length);
			if (ret != 0) {
				goto unlock_fail;
			}
			if (p->read_length <= 0) {
				pa_threaded_mainloop_wait(p->mainloop);
				CHECK_DEAD_GOTO(p, unlock_fail);
			} else if (!p->read_data) {
				ret = pa_stream_drop(p->stream);
				if (ret != 0) {
					goto unlock_fail;
				}
			} else {
				p->read_index = 0;
			}
		}
		l = p->read_length < length ? p->read_length : length;
		memcpy(data, (const uint8_t*) p->read_data + p->read_index, l);

		data = (uint8_t*) data + l;
		length -= l;

		p->read_index += l;
		p->read_length -= l;

		if (!p->read_length) {
			ret = pa_stream_drop(p->stream);
			p->read_data = NULL;
			p->read_length = 0;
			p->read_index = 0;
			if (ret != 0) {
				goto unlock_fail;
			}
		}
	}
    pa_threaded_mainloop_unlock(p->mainloop);
    return 0;
unlock_fail:
	pa_threaded_mainloop_unlock(p->mainloop);	
    return -1;
}

void pa_state_free(PaState* p)
{
    if (p->mainloop)
        pa_threaded_mainloop_stop(p->mainloop);

    if (p->stream)
        pa_stream_unref(p->stream);

    if (p->context) {
        pa_context_disconnect(p->context);
        pa_context_unref(p->context);
    }

    if (p->mainloop) {
        pa_threaded_mainloop_free(p->mainloop);
    }
    free(p);
}
