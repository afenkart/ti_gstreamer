#ifdef HAVE_CONFIG_H
#include "autoconfig.h"
#endif

#include <gst/gst.h>
#include <string.h>
#if 0
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#endif

#define DESKTOP

#ifndef DESKTOP
static const char video_pipe_desc[] =  "TIViddec2 genTimeStamps=FALSE \
			    engineName=decode \
			    codecName=h264dec numFrames=-1 \
			! videoscale method=0 \
			! video/x-raw-yuv, format=\(fourcc\)I420, width=320, height=240 \
			! ffmpegcolorspace \
			! video/x-raw-rgb, bpp=16 \
			! TIDmaiVideoSink displayStd=fbdev displayDevice=/dev/fb0 videoStd=QVGA \
			    videoOutput=LCD resizer=FALSE accelFrameCopy=TRUE";
#else
static const char video_pipe_desc[] = "decodebin \
			! videoscale method=0 \
			! video/x-raw-yuv, format=\(fourcc\)I420, width=320, height=240 \
			! xvimagesink";
#endif

static const char audio_pipe_desc[] = "decodebin ! queue ! audioconvert \
			! audioresample \
			! autoaudiosink";


const char filesrc_desc[] = "filesrc location=%s ! ";
const char httpsrc_desc[] = "souphttsrc location=%s ! ";

static GstElement *g_pipeline;

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

static void trigger_callback(GstState state);

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
		gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_NULL);
		trigger_callback(GST_STATE_NULL);
		break;

	case GST_MESSAGE_ERROR:
		gst_message_parse_error(msg, &err, &debug);
		g_free(debug);

		g_error("GStreamer: error: [%d] %s\n", err->code, err->message);
		g_error_free(err);

		/* setting state to null flushes pipeline */
		gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_NULL);
		trigger_callback(GST_STATE_NULL);
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
		if (msgSrc == GST_OBJECT(g_pipeline))
			trigger_callback(newstate);

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


GstElement *create_video_pipe(const char *filename)
{
	GstElement *bin;
	GError* error = NULL;
	int ct, siz;
	char *buf;

	siz = sizeof(video_pipe_desc)  + strlen(filename) + strlen(httpsrc_desc);
	buf = malloc(siz);
	if (!buf) {
		g_error("failed to allocate memory");
		return NULL;
	}

	sprintf(buf, filesrc_desc, filename);
	strcat(buf, video_pipe_desc);
	bin = gst_parse_launch_full(buf, NULL, 0, &error);
	free(buf);

	if (!bin) {
		g_error("GStreamer: failed to parse video sink pipeline\n");
		g_print("%s\n", buf);
		return NULL;
	}              

	gst_object_set_name(GST_OBJECT(bin), "video-pipe");
	return bin;
}

GstElement *create_audio_pipe(const char *filename)
{
	GstElement *bin;
	GError* error = NULL;
	int ct, siz;
	char *buf;

	siz = sizeof(audio_pipe_desc) + strlen(filename) + strlen(httpsrc_desc);
	buf = malloc(siz);
	if (!buf) {
		g_error("failed to allocate memory");
		return NULL;
	}

	sprintf(buf, filesrc_desc, filename);
	strcat(buf, audio_pipe_desc);
	bin = gst_parse_launch_full(buf, NULL, 0, &error);
	free(buf);

	if (!bin) {
		g_error("GStreamer: failed to parse audio sink pipeline\n");
		return NULL;
	}              

	gst_object_set_name(GST_OBJECT(bin), "audio-pipe");
	return bin;
}

GMainLoop *g_main_loop;
static void trigger_callback(GstState state)
{
	static int has_played = 0;
	switch (state) {
	case GST_STATE_READY:
		g_printf("pipe is ready\n");
		gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
		break;
	case GST_STATE_PLAYING:
		g_printf("pipe is playing\n");
		break;
	case GST_STATE_PAUSED:
		g_printf("pipe is paused\n");
		break;
	case GST_STATE_NULL:
		g_printf("pipe is flushing\n");
		g_main_loop_quit(g_main_loop);
		break;
	}
}

int main(int argc, char* argv[])
{
	GError* error;
	GstBus *bus;
	GstElement *videosink, *audiosink;
	int err;

	if (argc < 2) {
		g_error("specify file\n");
		return -1;
	}

	/* init gstreamer library */
	if (!gst_init_check(NULL, NULL, &error)) {
		g_error("GStreamer: failed to initialize gstreamer library: [%d] %s\n",
				error->code, error->message);
		g_error_free(error);
		return -1;
	}

	if (strstr(argv[1], "264"))
		g_pipeline = create_video_pipe(argv[1]);
	else
		g_pipeline = create_audio_pipe(argv[1]);

	if (!g_pipeline) {
		g_printf("GStreamer: failed to initialize pipeline\n");
		return -1;
	}
		
	/* register callback */
	bus = gst_pipeline_get_bus(GST_PIPELINE(g_pipeline));
	gst_bus_add_watch(bus, my_bus_callback, NULL);
	gst_object_unref(bus);

	/* initialize pipeline */
	if (gst_element_set_state(g_pipeline, GST_STATE_READY) ==
		GST_STATE_CHANGE_FAILURE) {
	  g_error("GStreamer: could not set pipeline to ready\n");
	}

	/* start main loop */
	g_print("GStreamer: SUCCESSFULLY INITIALIZED\n");

	g_main_loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(g_main_loop);

	g_print("GStreamer: Main loop terminates\n");
	gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_NULL);
	gst_object_unref(GST_OBJECT(g_pipeline));
	return 0;
}

