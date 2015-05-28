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

struct GstreamerDecoder {
    GstElement *pipeline;
    GstElement *appsource;
    GstElement *appsink;
    GstElement *codec;
    GstElement *convert;
};

static gboolean drain_pipeline(GstreamerDecoder *dec)
{
    GstSample *sample = NULL;
    if (gst_app_sink_is_eos(GST_APP_SINK(dec->appsink)))
        return TRUE;

    do {
        if (sample)
            gst_sample_unref(sample);
        sample = gst_app_sink_pull_sample(GST_APP_SINK(dec->appsink));
    } while(sample);

    return gst_app_sink_is_eos(GST_APP_SINK(dec->appsink));
}

static int construct_pipeline(display_stream *st, GstreamerDecoder *dec)
{
    int ret;
    GstCaps *rgb;
    char *dec_name;

    if (st->codec == SPICE_VIDEO_CODEC_TYPE_VP8)
        dec_name = "vp8dec";
    else if (st->codec == SPICE_VIDEO_CODEC_TYPE_MJPEG)
        dec_name = "jpegdec";
    else {
        SPICE_DEBUG("Uknown codec type %d", st->codec);
        return -1;
    }

    if (dec->pipeline) {
        GstPad *pad = gst_element_get_static_pad(dec->codec, "sink");
        if (! pad) {
            SPICE_DEBUG("Unable to get codec sink pad to flush the pipe");
            return -1;
        }
        gst_pad_send_event(pad, gst_event_new_eos());

        gst_object_unref(pad);
        if (! drain_pipeline(dec))
            return -1;

        gst_bin_remove_many(GST_BIN(dec->pipeline), dec->appsource, dec->codec, dec->appsink, NULL);
        gst_object_unref(dec->pipeline);
    }

    dec->appsource = gst_element_factory_make ("appsrc", NULL);
    dec->appsink = gst_element_factory_make ("appsink", NULL);

    dec->codec = gst_element_factory_make (dec_name, NULL);
    dec->convert = gst_element_factory_make ("videoconvert", NULL);

    rgb = gst_caps_from_string("video/x-raw,format=BGRx");
    if (!rgb) {
        SPICE_DEBUG("Gstreamer error: could not make BGRx caps.");
        return -1;
    }

    if (st->codec == SPICE_VIDEO_CODEC_TYPE_VP8) {
        GstCaps *vp8;
        vp8 = gst_caps_from_string("video/x-vp8");
        if (!vp8) {
            SPICE_DEBUG("Gstreamer error: could not make vp8 caps.");
            return -1;
        }

        gst_app_src_set_caps(GST_APP_SRC(dec->appsource), vp8);
        gst_app_sink_set_caps(GST_APP_SINK(dec->appsink), rgb);
    }

    dec->pipeline = gst_pipeline_new ("pipeline");

    if (!dec->pipeline || !dec->appsource || !dec->appsink || !dec->codec || !dec->convert) {
        SPICE_DEBUG("Gstreamer error: not all elements could be created.");
        return -1;
    }

    gst_bin_add_many (GST_BIN(dec->pipeline), dec->appsource, dec->codec, dec->convert, dec->appsink, NULL);
    if (gst_element_link_many(dec->appsource, dec->codec, dec->convert, dec->appsink, NULL) != TRUE) {
        SPICE_DEBUG("Gstreamer error: could not link all the pieces.");
        gst_object_unref (dec->pipeline);
        return -1;
    }

    ret = gst_element_set_state (dec->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        SPICE_DEBUG("Gstreamer error: Unable to set the pipeline to the playing state.");
        gst_object_unref (dec->pipeline);
        return -1;
    }

    return 0;
}

static GstreamerDecoder *gst_decoder_new(display_stream *st)
{
    GstreamerDecoder *dec;

    gst_init(NULL, NULL);

    dec = g_malloc0(sizeof(*dec));
    if (construct_pipeline(st, dec)) {
        free(dec);
        return NULL;
    }

    return dec;
}


void gst_decoder_destroy(GstreamerDecoder *dec)
{
    gst_object_unref(dec->pipeline);
    // TODO - contemplate calling gst_deinit();
    dec->pipeline = NULL;
    free(dec);
}


gboolean push_frame(display_stream *st)
{
    uint8_t *data;
    uint32_t size;
    gpointer d;
    GstBuffer *buffer;

    size = stream_get_current_frame(st, &data);
    if (size == 0) {
        SPICE_DEBUG("got an empty frame buffer!");
        return false;
    }

    // TODO.  Grr.  Seems like a wasted alloc
    d = g_malloc(size);
    memcpy(d, data, size);

    buffer = gst_buffer_new_wrapped(d, size);

    if (gst_app_src_push_buffer(GST_APP_SRC(st->gst_dec->appsource), buffer) != GST_FLOW_OK) {
        SPICE_DEBUG("Error: unable to push frame of size %d", size);
        return false;
    }

    // TODO.  Unref buffer?

    return true;
}


static void pull_frame(display_stream *st)
{
    int width;
    int height;

    GstSample *sample;
    GstBuffer *buffer = NULL;
    GstCaps *caps;
    GstStructure *structure;
    GstMemory *memory;
    gint caps_width, caps_height;

    sample = gst_app_sink_pull_sample(GST_APP_SINK(st->gst_dec->appsink));
    if (! sample) {
        SPICE_DEBUG("Unable to pull sample");
        return;
    }

    buffer = gst_sample_get_buffer(sample);
    memory = gst_buffer_get_all_memory(buffer);
    caps = gst_sample_get_caps(sample);

    structure = gst_caps_get_structure (caps, 0);
    gst_structure_get_int(structure, "width", &caps_width);
    gst_structure_get_int(structure, "height", &caps_height);

    stream_get_dimensions(st, &width, &height);

    if (width != caps_width || height != caps_height) {
        SPICE_DEBUG("Stream size %dx%x does not match frame size %dx%d",
            width, height, caps_width, caps_height);
    }
    else {
        GstMapInfo mem_info;

        // TODO seems like poor memory management
        if (gst_memory_map(memory, &mem_info, GST_MAP_READ)) {
            g_free(st->out_frame);
            st->out_frame = g_malloc0(mem_info.size);
            memcpy(st->out_frame, mem_info.data, mem_info.size);

            gst_memory_unmap(memory, &mem_info);
        }
    }

    gst_memory_unref(memory);
    gst_sample_unref(sample);
}


G_GNUC_INTERNAL
void stream_gst_init(display_stream *st)
{
    st->gst_dec = gst_decoder_new(st);
}

G_GNUC_INTERNAL
void stream_gst_data(display_stream *st)
{
    if (! push_frame(st))
        return;

    pull_frame(st);
}

G_GNUC_INTERNAL
void stream_gst_cleanup(display_stream *st)
{
    g_free(st->out_frame);
    st->out_frame = NULL;
    if (st->gst_dec) {
        gst_decoder_destroy(st->gst_dec);
        st->gst_dec = NULL;
    }
}
