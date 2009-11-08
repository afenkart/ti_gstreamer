#ifdef HAVE_CONFIG_H
#include "autoconfig.h"
#endif

#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <stdio.h>
#include <pthread.h>
#include <assert.h>

#include <gst/gst.h>

//#include "libGScontrol.h"

#define GSTREAMER_PARAM_SEEK_REL  "0"
#define DESKTOP

static int g_initialized = 0;

static pthread_mutex_t g_mutex;

static pthread_t g_reader_thread;
static pthread_cond_t g_main_cond;

typedef void (*state_cb_t)(GstState);
static state_cb_t g_state_callback = NULL;

static int g_thread_shutdown_flag = 0;

static long g_duration = 0;
static long g_position = 0;

static GMainLoop *g_main_loop;
static GstElement *g_pipeline;
static GstElement *g_filesrc;
static GstElement *g_httpsrc;
static GstElement *g_videosink;
static GstElement *g_audiosink;
static const char* g_pipeline_name; /* TODO get rid of */

/* call only form bus context */
static void trigger_callback(GstState state)
{
	if (g_state_callback != NULL)
		g_state_callback(state);
}

void GStreamer_regStateCallback(state_cb_t callback)
{
	if (!g_initialized) {
		g_error("GStreamer: library not initialized!\n");
		assert(0);
	}

	g_state_callback = callback;
}

static void *main_thread_proc(void *arg)
{
	if (!g_thread_shutdown_flag) {
		g_print("GStreamer: starting main loop\n");
		g_main_loop_run(g_main_loop);
	}

	g_print("GStreamer: exiting main loop\n");
	return NULL;
}

static const char *gststate_get_name(GstState state)
{
	switch(state) {
	case GST_STATE_VOID_PENDING:
		return "VOID_PENDING";
	case GST_STATE_NULL:
		return "NULL";
	case GST_STATE_READY:
		return "READY";
	case GST_STATE_PAUSED:
		return "PAUSED";
	case GST_STATE_PLAYING:
		return "PLAYING";
	default:
		return "Unknown";
	}
}

/* http://<xxx>/manual/html/section-bus-message-types.html */
static gboolean my_bus_callback(GstBus *bus, GstMessage *msg,
	gpointer user_data)
{
	GstMessageType msgType;
	GstObject *msgSrc;
	gchar *msgSrcName;

	/* used in switch */
	/* error message */
	gchar *debug;
	GError *err;
	GstState oldstate, newstate, pending;

	/* stream status */
	GstElement *owner;

	msgType = GST_MESSAGE_TYPE(msg);
	msgSrc = GST_MESSAGE_SRC(msg);
	msgSrcName = GST_OBJECT_NAME(msgSrc);

	switch (GST_MESSAGE_TYPE(msg)) {
	case GST_MESSAGE_EOS:
		g_print("GStreamer: end-of-stream\n");
		pthread_mutex_lock(&g_mutex);

		gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_NULL);
		trigger_callback(GST_STATE_NULL);

		pthread_mutex_unlock(&g_mutex);
		break;

	case GST_MESSAGE_ERROR:
		gst_message_parse_error(msg, &err, &debug);
		g_free (debug);

		g_error("GStreamer: error: [%d] %s\n", err->code, err->message);
		g_error_free(err);

		/* TODO no sleep in callback */
		pthread_mutex_lock(&g_mutex);

		/* setting state to null flushes pipeline */
		gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_NULL);
		trigger_callback(GST_STATE_NULL);

		pthread_mutex_unlock(&g_mutex);
		break;

	case GST_MESSAGE_STATE_CHANGED:
		gst_message_parse_state_changed(msg, &oldstate, &newstate, &pending);
#if 0   /* noisy */
		g_print("GStreamer: %s: State change: OLD: '%s', NEW: '%s', PENDING: '%s'\n",
				msgSrcName,
				gststate_get_name(oldstate),
				gststate_get_name(newstate),
				gststate_get_name(pending));
#endif
		if (!strcmp(msgSrcName, g_pipeline_name))
			trigger_callback(newstate); /* TODO GstState != GStreamer_state */

		break;

	case GST_MESSAGE_WARNING:
	case GST_MESSAGE_INFO:
		/* TODO */
		break;
	case GST_MESSAGE_APPLICATION:  /* marshal information into the main thread */
	case GST_MESSAGE_ASYNC_START:
	case GST_MESSAGE_ASYNC_DONE:
	case GST_MESSAGE_BUFFERING: /* caching of network streams */
	case GST_MESSAGE_CLOCK_LOST:
	case GST_MESSAGE_CLOCK_PROVIDE:
	case GST_MESSAGE_ELEMENT:  /* custom message, e.g. qtdemux redirect */
	case GST_MESSAGE_LATENCY:
	case GST_MESSAGE_NEW_CLOCK:
	case GST_MESSAGE_REQUEST_STATE:
	case GST_MESSAGE_SEGMENT_DONE:
	case GST_MESSAGE_SEGMENT_START:
	case GST_MESSAGE_STATE_DIRTY:
	case GST_MESSAGE_STEP_DONE:
	case GST_MESSAGE_STRUCTURE_CHANGE:
	case GST_MESSAGE_TAG: /* meta data: artist, title */
		/* ignore */
		break;
	case GST_MESSAGE_DURATION:
	default:
		g_print("GStreamer: BUS_CALL %s %d\n",
				gst_message_type_get_name(GST_MESSAGE_TYPE(msg)),
				GST_MESSAGE_TYPE(msg));
		break;
	}

	return 1;
}

static int cleanup_pipe()
{
	GError *error = NULL;
	GstIterator *iter;
	GstIteratorResult res;
	gpointer element = NULL;

	iter = gst_bin_iterate_elements(GST_BIN(g_pipeline));
	res = gst_iterator_next(iter, &element);

	while (res == GST_ITERATOR_OK) {
		gchar *name;

		name = gst_object_get_name(GST_OBJECT(element));
		if (name) {
			g_printf("GS: runing pipe elements: %s \n", name);
			g_free (name);
		}

		gst_object_unref(element);
		element = NULL;

		res = gst_iterator_next(iter, &element);
	}
	gst_iterator_free (iter);
}

int GStreamer_setMedia(const char *uri)
{
	GstElement *src, *sink;
	int is_video, is_http = 1;
	int ret = 0;

	if (!g_initialized) {
		g_error("GStreamer: library not initialized!\n");
		return -1;
	}

	cleanup_pipe();

	pthread_mutex_lock(&g_mutex);

	g_position = 0;
	g_duration = 0;

	g_print("GStreamer: playing : %s\n", uri);

	is_video = strstr(uri, "264") != NULL;
	is_http = !strncmp(uri, "http://", strlen("http://"));

	g_printf("GStreamer: playing %s via %s\n", is_video ? "video" : "audio",
			is_http ? "http" : "filesrc");

	if (is_http)
		g_object_set(G_OBJECT(g_httpsrc), "location", uri, NULL);
	else
		g_object_set(G_OBJECT(g_filesrc), "location", uri, NULL);

	sink = (is_video) ? g_videosink : g_audiosink;
	src = (is_http) ? g_httpsrc : g_filesrc;

	gst_bin_add_many(GST_BIN(g_pipeline), src, sink, NULL);

	if (!gst_element_link(src, sink)) {
		g_error("GStreamer: failed to link %s with %s\n",
				gst_element_get_name(src),
				gst_element_get_name(sink));
		return -1;
	}

	gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_PLAYING);
	//gst_element_get_state(GST_ELEMENT(g_pipeline), ...);

	/* TODO what is signalled? */
	pthread_cond_signal(&g_main_cond);
	pthread_mutex_unlock(&g_mutex);

	return ret;
}

int GStreamer_stop()
{
	GstStateChangeReturn ret; 

	g_print("GStreamer: stop\n");
	if (!g_initialized) {
		g_error("GStreamer: library not initialized!\n");
		return -1;
	}

	pthread_mutex_lock(&g_mutex);

	g_duration = 0;
	g_position = 0;

	ret = gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_READY); 
	if (ret == GST_STATE_CHANGE_FAILURE) {
		g_error("Failed to stop pipeline ret == %d\n", ret);
		pthread_mutex_unlock(&g_mutex);
		return -1;
	}

#if 0
	/* TODO, hmm */
	trigger_callback(GST_STATE_NULL);
#endif
	pthread_mutex_unlock(&g_mutex);
	return 0;
}

GstElement *create_video_sink()
{
	GstElement *bin, *decoder = NULL;
	GstIterator *iter;
	GstIteratorResult res;
	GError *error = NULL;
	GstPad *pad;
	gpointer element = NULL;
	const char* decoder_name;

#ifndef DESKTOP 
	/* create pipeline */                                                                                 
	decoder_name = "tividdec20";
	bin = gst_parse_launch_full("TIViddec2 genTimeStamps=FALSE \
			    engineName=decode \
			    codecName=h264dec numFrames=-1 \
			! videoscale method=0 \
			! video/x-raw-yuv, format=(fourcc)I420, width=320, height=240 \
			! ffmpegcolorspace \
			! video/x-raw-rgb, bpp=16 \
			! TIDmaiVideoSink displayStd=fbdev displayDevice=/dev/fb0 videoStd=QVGA \
			    videoOutput=LCD resizer=FALSE accelFrameCopy=TRUE",
			NULL, 0, &error);                                      
#else
	decoder_name = "decodebin";
	bin = gst_parse_launch_full("decodebin \
			! videoscale method=0 \
			! video/x-raw-yuv, format=(fourcc)I420, width=320, height=240 \
			! xvimagesink",
			NULL, 0, &error);                                      
#endif

	if (!bin) {
		g_error("GStreamer: failed to parse video sink pipeline\n");
		return NULL;
	}              

	gst_object_set_name(GST_OBJECT(bin), "video-sink");

	iter = gst_bin_iterate_elements(GST_BIN(bin));
	res = gst_iterator_next (iter, &element);
	while (res == GST_ITERATOR_OK) {
		gchar *name;

		name = gst_object_get_name(GST_OBJECT (element));
		if (name) {
			if (!strncmp(name, decoder_name, strlen(decoder_name))) {
				decoder = GST_ELEMENT(element); 
			}
			g_printf("GS: video sink element: %s \n", name);
			g_free (name);
		}

		gst_object_unref (element);
		element = NULL;

		res = gst_iterator_next (iter, &element);
	}
	gst_iterator_free (iter);

	if (!decoder) {
		/* mem leak */
		g_printf("decoder element not found\n");
		return NULL;
	}

	/* add ghostpad */
	pad = gst_element_get_static_pad (decoder, "sink");
	gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
	gst_object_unref(GST_OBJECT(pad));

	return bin;
}

GstElement *create_audio_sink()
{
	GstElement *bin, *decoder = NULL;
	GstIterator *iter;
	GstIteratorResult res;
	GError *error = NULL;
	GstPad *pad;
	gpointer element = NULL;

	bin = gst_parse_launch_full("decodebin ! queue ! audioconvert \
			! audioresample \
			! autoaudiosink",
			NULL, 0, &error);                                      

	if (!bin) {
		g_error("GStreamer: failed to parse audio sink pipeline\n");
		return NULL;
	}              

	gst_object_set_name(GST_OBJECT(bin), "audio-sink");

	iter = gst_bin_iterate_elements(GST_BIN(bin));
	res = gst_iterator_next (iter, &element);
	while (res == GST_ITERATOR_OK) {
		gchar *name;

		name = gst_object_get_name(GST_OBJECT (element));
		if (name) {
			if (!strncmp(name, "decodebin", strlen("decodebin"))) {
				decoder = GST_ELEMENT(element); 
			}
			g_printf("GS: audio sink element: %s \n", name);
			g_free (name);
		}

		gst_object_unref (element);
		element = NULL;

		res = gst_iterator_next (iter, &element);
	}
	gst_iterator_free (iter);

	if (!decoder) {
		/* mem leak */
		g_printf("decoder element not found\n");
		return NULL;
	}

	/* add ghostpad */
	pad = gst_element_get_static_pad(decoder, "sink");
	gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
	gst_object_unref(GST_OBJECT(pad));

	return bin;
}

int GStreamer_init(const char *mplayer)
{
	GError* error;
	GstBus *bus;
	GstElement *videosink, *audiosink;
	int err;

	if (g_initialized)
		g_error("GStreamer: already initialized, call destroy first!\n");

	g_state_callback = NULL;
	g_duration = 0;
	g_position = 0;

	/* pthread synchronization */
	pthread_mutex_init(&g_mutex, NULL);
	err = pthread_cond_init(&g_main_cond, NULL);
	if (err) {
		g_error("GStreamer: failed to initialize main condition %s\n",
				strerror(errno));
		return -1;
	}

	/* init gstreamer library */
	if (!gst_init_check(NULL, NULL, &error)) {
		g_error("GStreamer: failed to initialize gstreamer library: [%d] %s\n",
				error->code, error->message);
		g_error_free(error);
		return -1;
	}

	/* create pipeline */
	g_pipeline = gst_pipeline_new("pipeline");
	g_pipeline_name = gst_element_get_name(GST_ELEMENT(g_pipeline));

	/* register callback */
	bus = gst_pipeline_get_bus(GST_PIPELINE(g_pipeline));
	gst_bus_add_watch(bus, my_bus_callback, NULL);
	gst_object_unref(bus);

	/* hardcode audio/video sink */
	g_videosink = create_video_sink();
	g_audiosink = create_audio_sink();

	if (!g_videosink || !g_audiosink) {
		/* TODO memory leak */
		g_error("GStreamer: failed to create sink elements\n");
		return -1;
	}

	/* prepare http/file src */
	g_filesrc = gst_element_factory_make ("filesrc", "filesrc");
	g_httpsrc = gst_element_factory_make ("souphttpsrc", "httpsrc");

	if (!g_filesrc || !g_httpsrc) {
		/* TODO memory leak */
		g_error("GStreamer: failed to create src elements %x %x\n", g_filesrc, g_httpsrc);
		return -1;
	}

	/* initialize pipeline */
	/* TODO do for audio/video pipe separately */

	if (gst_element_set_state(g_pipeline, GST_STATE_READY) ==
		GST_STATE_CHANGE_FAILURE) {
	  g_error("GStreamer: could not set pipeline to ready\n");
	}

	/* start main loop */
	g_main_loop = g_main_loop_new(NULL, FALSE);

	err = pthread_create(&g_reader_thread, NULL, main_thread_proc, NULL);
	if (err) {
		g_error("GStreamer: failed to launch gstreamer main thread %s\n",
				strerror(errno));
		goto err_pthread;
	}

	g_print("GStreamer: SUCCESSFULLY INITIALIZED\n");
	g_initialized = 1;

	return 0;

err_pthread:
	pthread_cond_destroy(&g_main_cond);
	pthread_mutex_destroy(&g_mutex);
	
	return err;
}

void GStreamer_destroy()
{
	g_print("GStreamer: destroy\n");
	if (!g_initialized) {
		g_error("GStreamer: not initialized!\n");
		return;
	}

	g_state_callback = NULL;
	g_thread_shutdown_flag = 1;

	g_main_loop_quit(g_main_loop);
	pthread_cond_signal(&g_main_cond);

	g_printf("GStreamer: wait main loop thread\n");
	pthread_join(g_reader_thread, NULL);
	g_printf("GStreamer: wait main loop joined\n");

	pthread_mutex_lock(&g_mutex);
	pthread_cond_destroy(&g_main_cond);

	gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_NULL);
	gst_object_unref(GST_OBJECT(g_pipeline));

	sleep(1);
	pthread_mutex_unlock(&g_mutex);
	pthread_mutex_destroy(&g_mutex);
}

static pthread_mutex_t g_cb_mut = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cb_cond = PTHREAD_COND_INITIALIZER;

typedef enum {
	MY_EOS,
	MY_NULL,
	MY_PAUSE,
	MY_PLAY,
	MY_READY,
} my_state_t;

my_state_t my_state;

static void my_state_callback(GstState state)
{
	pthread_mutex_lock(&g_cb_mut);

	g_print("%s, state: %s\n", __func__, gststate_get_name(state));
	switch (state) {
	case GST_STATE_PLAYING: 
		my_state = MY_PLAY;
		break;
	case GST_STATE_PAUSED:
		my_state = MY_PAUSE;
		break;
	case GST_STATE_READY:
		my_state = MY_READY;
		break;
	case GST_STATE_NULL:
		my_state = MY_NULL;
		break;
	default:
		assert(0);
	}

	pthread_cond_signal(&g_cb_cond);
	pthread_mutex_unlock(&g_cb_mut);
}

struct timespec calc_delay(int sec)
{
	struct timeval now;
	struct timespec timeout;

	g_print("sleep for %d sec\n", sec);
	gettimeofday(&now, NULL);
	timeout.tv_sec = now.tv_sec + 5;
	timeout.tv_nsec = now.tv_usec * 1000;
	return timeout;
}

int main(int argc, char* argv[])
{
	struct timespec delay;
	int i, ret;

	if (argc < 2) {
		g_error("specify file [ file2 file3 .. ]\n");
		return -1;
	}

	pthread_cond_init(&g_cb_cond, NULL);
	pthread_mutex_init(&g_cb_mut, NULL);

	GStreamer_init(NULL);
	GStreamer_regStateCallback(&my_state_callback);

	for (i = 1; i < argc; i++) { 
		ret = GStreamer_setMedia(argv[i]);
		assert(ret == 0);

		do {
			ret = pthread_cond_wait(&g_cb_cond, &g_cb_mut);

			if (my_state == MY_PLAY) {
				delay = calc_delay(3);
				ret = pthread_cond_timedwait(&g_cb_cond, &g_cb_mut, &delay);
				if (ret == ETIMEDOUT)
					break;
			}
		} while (my_state != MY_EOS && my_state != MY_NULL);
		g_printf("GStreamer: asset done\n");

		if (ret == ETIMEDOUT) {
			GStreamer_stop();
			pthread_cond_wait(&g_cb_cond, &g_cb_mut);
		}
	}

	GStreamer_destroy();
	pthread_cond_destroy(&g_cb_cond);
	pthread_mutex_destroy(&g_cb_mut);

	g_printf("GStreamer: exit normally\n");
	return 0;
}
