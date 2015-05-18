/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2015 CodeWeavers, Inc

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include "config.h"

#include "spice-client.h"
#include "spice-common.h"
#include "spice-channel-priv.h"

#include "channel-display-priv.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>


/* GStreamer decoder implementation */

struct GStreamerDecoder {
    GstElement *pipeline;
    GstAppSrc *appsrc;
    GstAppSink *appsink;

    GMutex pipeline_mutex;
    GCond pipeline_cond;
    int pipeline_wait;
    uint32_t samples_count;

    GstSample *sample;
    GstMapInfo mapinfo;
};


/* Signals that the pipeline is done processing the last buffer we gave it.
 *
 * @decoder:   The video decoder object.
 * @samples:   How many samples to add to the available samples count.
 */
static void signal_pipeline(GStreamerDecoder *decoder, uint32_t samples)
{
    g_mutex_lock(&decoder->pipeline_mutex);
    decoder->pipeline_wait = 0;
    decoder->samples_count += samples;
    g_cond_signal(&decoder->pipeline_cond);
    g_mutex_unlock(&decoder->pipeline_mutex);
}

static void appsrc_need_data_cb(GstAppSrc *src, guint length, gpointer user_data)
{
    GStreamerDecoder *decoder = (GStreamerDecoder*)user_data;
    signal_pipeline(decoder, 0);
}

static GstFlowReturn appsink_new_sample_cb(GstAppSink *appsrc, gpointer user_data)
{
    GStreamerDecoder *decoder = (GStreamerDecoder*)user_data;
    signal_pipeline(decoder, 1);
    return GST_FLOW_OK;
}

static void reset_pipeline(GStreamerDecoder *decoder)
{
    if (!decoder->pipeline) {
        return;
    }

    gst_element_set_state(decoder->pipeline, GST_STATE_NULL);
    gst_object_unref(decoder->appsrc);
    gst_object_unref(decoder->appsink);
    gst_object_unref(decoder->pipeline);
    decoder->pipeline = NULL;

    g_mutex_clear(&decoder->pipeline_mutex);
    g_cond_clear(&decoder->pipeline_cond);
}

static gboolean construct_pipeline(display_stream *st, GStreamerDecoder *decoder)
{
    g_mutex_init(&decoder->pipeline_mutex);
    g_cond_init(&decoder->pipeline_cond);
    decoder->pipeline_wait = 1;
    decoder->samples_count = 0;

    const gchar *src_caps = NULL;
    const gchar *gstdec_name = NULL;
    switch (st->codec) {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        src_caps = "caps=image/jpeg";
        gstdec_name = "jpegdec";
        break;
    case SPICE_VIDEO_CODEC_TYPE_VP8:
        src_caps = "caps=video/x-vp8";
        gstdec_name = "vp8dec";
        break;
    case SPICE_VIDEO_CODEC_TYPE_H264:
        src_caps = "caps=video/x-h264";
        gstdec_name = "h264parse ! avdec_h264";
        break;
    default:
        SPICE_DEBUG("Unknown codec type %d", st->codec);
        break;
    }
    const gchar *gst_auto = getenv("SPICE_GST_AUTO");
    if (!src_caps || (gst_auto && strcmp(gst_auto, "decodebin"))) {
        src_caps = "typefind=true"; /* Misidentifies VP8 */
    }
    if (!gstdec_name || gst_auto) {
        gstdec_name = "decodebin";  /* vaapi is assert-happy */
    }

    GError *err = NULL;
    gchar *desc = g_strdup_printf("appsrc name=src format=2 do-timestamp=1 %s ! %s ! videoconvert ! appsink name=sink caps=video/x-raw,format=BGRx", src_caps, gstdec_name);
    SPICE_DEBUG("GStreamer pipeline: %s", desc);
    decoder->pipeline = gst_parse_launch_full(desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &err);
    g_free(desc);
    if (!decoder->pipeline) {
        spice_warning("GStreamer error: %s", err->message);
        g_clear_error(&err);
        return FALSE;
    }

    decoder->appsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(decoder->pipeline), "src"));
    GstAppSrcCallbacks appsrc_cbs = {&appsrc_need_data_cb, NULL, NULL};
    gst_app_src_set_callbacks(decoder->appsrc, &appsrc_cbs, decoder, NULL);

    decoder->appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(decoder->pipeline), "sink"));
    GstAppSinkCallbacks appsink_cbs = {NULL, NULL, &appsink_new_sample_cb};
    gst_app_sink_set_callbacks(decoder->appsink, &appsink_cbs, decoder, NULL);

    if (gst_element_set_state(decoder->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        SPICE_DEBUG("GStreamer error: Unable to set the pipeline to the playing state.");
        reset_pipeline(decoder);
        return FALSE;
    }

    return TRUE;
}

static GStreamerDecoder *gst_decoder_new(display_stream *st)
{
    gst_init(NULL, NULL);

    GStreamerDecoder *decoder = g_malloc0(sizeof(*decoder));
    if (!construct_pipeline(st, decoder)) {
        g_free(decoder);
        return NULL;
    }

    return decoder;
}

static void gst_decoder_destroy(GStreamerDecoder *decoder)
{
    reset_pipeline(decoder);
    g_free(decoder);
    /* Don't call gst_deinit() as other parts may still be using GStreamer */
}

static void release_msg_in(gpointer data)
{
    spice_msg_in_unref((SpiceMsgIn*)data);
}

static gboolean push_compressed_buffer(display_stream *st)
{
    uint8_t *data;
    uint32_t size;
    GstBuffer *buffer;

    size = stream_get_current_frame(st, &data);
    if (size == 0) {
        SPICE_DEBUG("got an empty frame buffer!");
        return FALSE;
    }

    /* Reference msg_data so it stays around until our 'deallocator' releases it */
    spice_msg_in_ref(st->msg_data);
    buffer = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,
                                         data, size, 0, size,
                                         st->msg_data, &release_msg_in);

    if (gst_app_src_push_buffer(st->gst_dec->appsrc, buffer) != GST_FLOW_OK) {
        SPICE_DEBUG("GStreamer error: unable to push frame of size %d", size);
        return FALSE;
    }

    return TRUE;
}

static void release_last_frame(display_stream *st)
{
    GStreamerDecoder* decoder = st->gst_dec;

    if (decoder->mapinfo.memory) {
        gst_memory_unmap(decoder->mapinfo.memory, &decoder->mapinfo);
        gst_memory_unref(decoder->mapinfo.memory);
        decoder->mapinfo.memory = NULL;
    }
    if (decoder->sample) {
        gst_sample_unref(decoder->sample);
        decoder->sample = NULL;
    }
    st->out_frame = NULL;
}

static void pull_raw_frame(display_stream *st)
{
    GStreamerDecoder* decoder = st->gst_dec;

    decoder->sample = gst_app_sink_pull_sample(decoder->appsink);
    if (!decoder->sample) {
        SPICE_DEBUG("GStreamer error: could not pull sample");
        return;
    }

    GstBuffer *buffer = gst_sample_get_buffer(decoder->sample);
    if (gst_buffer_map(buffer, &decoder->mapinfo, GST_MAP_READ)) {
        st->out_frame = decoder->mapinfo.data;
    }
}


/* Video decoder API implementation */

G_GNUC_INTERNAL
void stream_gst_init(display_stream *st)
{
    st->gst_dec = gst_decoder_new(st);
}

G_GNUC_INTERNAL
void stream_gst_data(display_stream *st)
{
    GStreamerDecoder* decoder = st->gst_dec;

    /* Release the output frame buffer early so the pipeline can reuse it.
     * This also simplifies error handling.
     */
    release_last_frame(st);

    /* The pipeline may have called appsrc_need_data_cb() after we got the last
     * output frame. This would cause us to return prematurely so reset
     * pipeline_wait so we do wait for it to process this buffer.
     */
    g_mutex_lock(&decoder->pipeline_mutex);
    decoder->pipeline_wait = 1;
    g_mutex_unlock(&decoder->pipeline_mutex);
    /* Note that it's possible for appsrc_need_data_cb() to get called between
     * now and the pipeline wait. But this will at most cause a one frame delay.
     */

    if (push_compressed_buffer(st)) {
        /* Wait for the pipeline to either produce a decoded frame, or ask
         * for more data which means an error happened.
         */
        g_mutex_lock(&decoder->pipeline_mutex);
        while (decoder->pipeline_wait) {
            g_cond_wait(&decoder->pipeline_cond, &decoder->pipeline_mutex);
        }
        decoder->pipeline_wait = 1;
        uint32_t samples = decoder->samples_count;
        if (samples) {
            decoder->samples_count--;
        }
        g_mutex_unlock(&decoder->pipeline_mutex);

        /* If a decoded frame waits for us, return it */
        if (samples) {
            pull_raw_frame(st);
        }
    }
}

G_GNUC_INTERNAL
void stream_gst_cleanup(display_stream *st)
{
    release_last_frame(st);
    if (st->gst_dec) {
        gst_decoder_destroy(st->gst_dec);
        st->gst_dec = NULL;
    }
}
