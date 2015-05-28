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

    GstSample *sample;
    GstMapInfo mapinfo;
};

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
}

static gboolean construct_pipeline(display_stream *st, GStreamerDecoder *decoder)
{
    const gchar *src_caps, *gstdec_name;
    switch (st->codec) {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        src_caps = "image/jpeg";
        gstdec_name = "jpegdec";
        break;
    case SPICE_VIDEO_CODEC_TYPE_VP8:
        src_caps = "video/x-vp8";
        gstdec_name = "vp8dec";
        break;
    default:
        spice_warning("Unknown codec type %d", st->codec);
        return -1;
    }

    GError *err = NULL;
    gchar *desc = g_strdup_printf("appsrc name=src format=2 do-timestamp=1 caps=%s ! %s ! videoconvert ! appsink name=sink caps=video/x-raw,format=BGRx", src_caps, gstdec_name);
    SPICE_DEBUG("GStreamer pipeline: %s", desc);
    decoder->pipeline = gst_parse_launch_full(desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &err);
    g_free(desc);
    if (!decoder->pipeline) {
        spice_warning("GStreamer error: %s", err->message);
        g_clear_error(&err);
        return FALSE;
    }

    decoder->appsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(decoder->pipeline), "src"));
    decoder->appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(decoder->pipeline), "sink"));

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

static gboolean push_compressed_buffer(display_stream *st)
{
    uint8_t *data;
    uint32_t size;
    gpointer d;
    GstBuffer *buffer;

    size = stream_get_current_frame(st, &data);
    if (size == 0) {
        SPICE_DEBUG("got an empty frame buffer!");
        return FALSE;
    }

    // TODO.  Grr.  Seems like a wasted alloc
    d = g_malloc(size);
    memcpy(d, data, size);

    buffer = gst_buffer_new_wrapped(d, size);

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
    /* Release the output frame buffer early so the pipeline can reuse it.
     * This also simplifies error handling.
     */
    release_last_frame(st);

    if (push_compressed_buffer(st)) {
        pull_raw_frame(st);
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
