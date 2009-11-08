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
static const char* g_pipeline_name;

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

int GStreamer_setMedia(const char *uri)
{
	int ret = 0;

	if (!g_initialized) {
		g_error("GStreamer: library not initialized!\n");
		return -1;
	}

	pthread_mutex_lock(&g_mutex);

	g_position = 0;
	g_duration = 0;

	g_print("GStreamer: playing : %s\n", uri);
	g_object_set(G_OBJECT(g_pipeline), "uri", uri, NULL);
	gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_PLAYING);

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

	ret = gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_NULL); 
	if (ret == GST_STATE_CHANGE_FAILURE) {
		g_error("Failed to stop pipeline ret == %d\n", ret);
		pthread_mutex_unlock(&g_mutex);
		return -1;
	}

	/* TODO, hmm */
	trigger_callback(GST_STATE_NULL);
	pthread_mutex_unlock(&g_mutex);
	return 0;
}

static GstCaps *create_color_convert_caps()
{
	GstCaps *caps;
	caps = gst_caps_new_simple ("video/x-raw-rgb",
			"bpp", G_TYPE_INT, 16,
			NULL);

	/* TODO no error checking? */
	return caps;
}

static GstCaps *create_size_convert_caps()
{
	GstCaps *caps;
	caps = gst_caps_new_simple ("video/x-raw-yuv",
			"format", GST_TYPE_FOURCC, GST_MAKE_FOURCC ('I', '4', '2', '0'),
			"width", G_TYPE_INT, 320,
			"height", G_TYPE_INT, 240,
			NULL);

	/* TODO no error checking? */
	return caps;
}

static gboolean link_with_caps(GstElement* elt1, GstElement *elt2,
		GstCaps *caps)
{
	gboolean link_ok;

	if (!caps) {
		g_error("GStreamer: caps is NULL\n");
		return 0;
	}

	g_print("link %s and %s with %s\n", gst_element_get_name(elt1),
			gst_element_get_name(elt2),
			gst_caps_to_string(caps));

	link_ok = gst_element_link_filtered(elt1, elt2, caps);
	gst_caps_unref (caps);

	if (!link_ok) {
		g_warning ("Failed to link elements!");
		return 0;
	}

	return link_ok;
}

GstElement *create_video_sink()
{
	GstElement *sink, *scale, *bin, *convert;
	GstPad *pad;

	bin = gst_bin_new("video_bin");
#ifdef DESKTOP 
	//sink = gst_element_factory_make ("dfbvideosink", "sink");
	// fails: xvideosink ximagesink 
	sink = gst_element_factory_make ("xvimagesink", "sink");
#else
	sink = gst_element_factory_make ("TIDmaiVideoSink", "sink");
#endif
	scale = gst_element_factory_make ("videoscale", "scale");
	convert = gst_element_factory_make ("ffmpegcolorspace", "convert");

	if (!bin || !sink || !scale || !convert) {
		g_error("GStreamer: failed to create video-sink elements\n");
		return NULL;
	}

	/* First add the elements to the bin */
	gst_bin_add_many(GST_BIN(bin), convert, scale, sink, NULL);

	/* add ghostpad */
	pad = gst_element_get_static_pad (scale, "sink");
	gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
	gst_object_unref(GST_OBJECT(pad));

	/* link the elements */
	if (!link_with_caps(scale, convert, create_size_convert_caps())) {
		/* TODO mem leak */
		return NULL;
	}

#ifdef DESKTOP 
	if (!gst_element_link(convert, sink)) {
		g_error("GStreamer: failed to link video queue\n");
		return NULL; /* TODO mem leak */
	}
#else
	if (!link_with_caps(convert, sink, create_color_convert_caps())) {
		/* TODO mem leak */
		return NULL;
	}
#endif

	return bin;
}

GstElement *create_audio_sink()
{
	GstElement *sink, *resample, *bin, *convert;
	GstPad *pad;

	bin = gst_bin_new("audio_bin");
	convert = gst_element_factory_make ("audioconvert", "convert");
	resample = gst_element_factory_make ("audioresample", "resample");
	sink = gst_element_factory_make ("alsasink", "sink");

	if (!bin || !sink || !resample || !convert) {
		g_error("GStreamer: failed to create audio-sink elements\n");
		return NULL; /* TODO mem leak */
	}

	/* First add the elements to the bin */
	gst_bin_add_many(GST_BIN(bin), convert, resample, sink, NULL);

	/* add ghostpad */
	pad = gst_element_get_static_pad(resample, "sink");
	gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
	gst_object_unref(GST_OBJECT(pad));

	/* link the elements */
	if (!gst_element_link_many(resample, convert, sink, NULL)) {
		g_error("GStreamer: failed to link audio queue\n");
		return NULL; /* TODO mem leak */
	}

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
	g_pipeline = gst_element_factory_make("playbin", "player");
	if (!g_pipeline) {
		g_error("GStreamer: playbin plugin not found\n");
		return -1;
	}
	g_pipeline_name = gst_element_get_name(GST_ELEMENT(g_pipeline));

	/* register callback */
	bus = gst_pipeline_get_bus(GST_PIPELINE(g_pipeline));
	gst_bus_add_watch(bus, my_bus_callback, NULL);
	gst_object_unref(bus);

	/* hardcode audio/video sink */
	if (!(videosink = create_video_sink())) {
		/* TODO memory leak */
		return -1;
	}

	if (!(audiosink = create_audio_sink())) {
		/* TODO memory leak */
		return -1;
	}

	g_object_set(G_OBJECT(g_pipeline), "video-sink", videosink, NULL);
	g_object_set(G_OBJECT(g_pipeline), "audio-sink", audiosink, NULL);

	/* initialize pipeline */
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

char g_buf[256];
const char* create_uri(const char *rel_name)
{
	int ret;

	/* assume it's local file system */
	ret = (*rel_name == '/') ? snprintf(g_buf, sizeof(g_buf),  "file://%s", rel_name)
		: snprintf(g_buf, sizeof(g_buf),  "file://%s/%s", getenv("PWD"), rel_name);
	if (ret > sizeof(g_buf)) {
		g_error("filename too long\n");
		return NULL;
	}
	
	return g_buf;
}

static pthread_mutex_t g_cb_mut = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_eos_cond = PTHREAD_COND_INITIALIZER;

static void my_state_callback(GstState state)
{
	pthread_mutex_lock(&g_cb_mut);

	g_print("%s, state: %s\n", __func__, gststate_get_name(state));
#if 0
	if (state == GST_STATE_PLAYING) {
		gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_PAUSED);
	} else if (state == GST_STATE_PAUSED) {
		gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_PLAYING);
	}
	sleep(5);
	g_printf("wake-up\n");
#if 0
	if (state == GST_STATE_READY)
		gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_NULL);
#endif
#endif

	if (state == GST_STATE_NULL)
		pthread_cond_signal(&g_eos_cond);

	pthread_mutex_unlock(&g_cb_mut);
}

int main(int argc, char* argv[])
{
	int i, ret;

	if (argc < 2) {
		g_error("specify file [ file2 file3 .. ]\n");
		return -1;
	}

	pthread_cond_init(&g_eos_cond, NULL);
	pthread_mutex_init(&g_cb_mut, NULL);

	GStreamer_init(NULL);
	GStreamer_regStateCallback(&my_state_callback);

	for (i = 1; i < argc; i++) { 
		const char *uri_name = create_uri(argv[i]); 
		if (!uri_name)
			continue;

		ret = GStreamer_setMedia(uri_name);
		assert(ret == 0);
		
		pthread_cond_wait(&g_eos_cond, &g_cb_mut);
		g_printf("GStreamer: asset done\n");
	}

	pthread_cond_destroy(&g_eos_cond);
	pthread_mutex_destroy(&g_cb_mut);

	g_printf("GStreamer: exit normally\n");
	return 0;
}
