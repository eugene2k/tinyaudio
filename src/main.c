/*
 * MIT License
 *
 * Copyright (c) 2025
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <assert.h>
#include <libavutil/dict.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <dbus/dbus-protocol.h>
#include <dbus/dbus-shared.h>
#include <dbus/dbus.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/codec_desc.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <pulse/simple.h>

#define SAMPLE_RATE 44100
#define CHANNELS 2

#define APP_NAME "tinyaudio"
#define BUS_NAME "org.mpris.MediaPlayer2.tinyaudio"
#define IFACE_ROOT "org.mpris.MediaPlayer2"
#define IFACE_PLAYER "org.mpris.MediaPlayer2.Player"
#define OBJ_PATH "/org/mpris/MediaPlayer2"
#define NO_TRACK "/TrackList/NoTrack"
#define XML_DATA                                                                                                       \
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "                                \
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\"><node "                                          \
    "name=\"/org/mpris/MediaPlayer2\"><interface name=\"org.mpris.MediaPlayer2\"><method "                             \
    "name=\"Raise\"/><method name=\"Quit\"/><property name=\"CanQuit\" type=\"b\" access=\"read\"/><property "         \
    "name=\"CanRaise\" type=\"b\" access=\"read\"/><property name=\"HasTrackList\" type=\"b\" "                        \
    "access=\"read\"/><property name=\"Identity\" type=\"s\" access=\"read\"/><property "                              \
    "name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/><property name=\"SupportedMimeTypes\" "                 \
    "type=\"as\" access=\"read\"/><property name=\"CanSetFullscreen\" type=\"b\" access=\"read\"/><property "          \
    "name=\"Fullscreen\" type=\"b\" access=\"read\"/></interface><interface "                                          \
    "name=\"org.mpris.MediaPlayer2.Player\"><method name=\"Play\"/><method name=\"Pause\"/><method "                   \
    "name=\"Stop\"/><method name=\"PlayPause\"/><method name=\"Next\"/><method name=\"Previous\"/><method "            \
    "name=\"Seek\"><arg name=\"offset\" type=\"x\" direction=\"in\"/></method><method "                                \
    "name=\"SetPosition\"><arg name=\"track_id\" type=\"x\" direction=\"in\"/><arg name=\"position\" "                 \
    "type=\"Seeked\" direction=\"in\"/></method><method name=\"OpenUri\"><arg name=\"uri\" type=\"Rate\" "             \
    "direction=\"in\"/></method><property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/><property "              \
    "name=\"Rate\" type=\"d\" access=\"readwrite\"/><property name=\"Shuffle\" type=\"b\" "                            \
    "access=\"readwrite\"/><property name=\"LoopStatus\" type=\"s\" access=\"readwrite\"/><property "                  \
    "name=\"Position\" type=\"x\" access=\"readwrite\"/><property name=\"MinimumRate\" type=\"d\" "                    \
    "access=\"read\"/><property name=\"MaximumRate\" type=\"d\" access=\"read\"/><property name=\"CanGoNext\" "        \
    "type=\"b\" access=\"read\"/><property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/><property "              \
    "name=\"CanPlay\" type=\"b\" access=\"read\"/><property name=\"CanPause\" type=\"b\" "                             \
    "access=\"read\"/><property name=\"CanSeek\" type=\"b\" access=\"read\"/><property name=\"CanControl\" "           \
    "type=\"b\" access=\"read\"/><signal name=\"Seeked\"><arg name=\"Position\" "                                      \
    "type=\"x\"/></signal></interface><interface name=\"org.freedesktop.DBus.Properties\"><method "                    \
    "name=\"Get\"/><method name=\"Set\"/><method name=\"GetAll\"/></interface><interface "                             \
    "name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\"/></interface></node>";

#define ADD_CONTAINER(iter, container_type, contents_type, expr)                                                       \
    ({                                                                                                                 \
        DBusMessageIter container;                                                                                     \
        assert(dbus_message_iter_open_container(iter, container_type, contents_type, &container));                     \
        expr;                                                                                                          \
        dbus_message_iter_close_container(iter, &container);                                                           \
    })

#define ADD_DICT_ENTRY(iter, name, expr)                                                                               \
    {                                                                                                                  \
        DBusMessageIter dict;                                                                                          \
        dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict);                                     \
        const char *key = name;                                                                                        \
        dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key);                                                 \
        {                                                                                                              \
            expr;                                                                                                      \
        }                                                                                                              \
        dbus_message_iter_close_container(iter, &dict);                                                                \
    }

#define STRING_PLAYING "Playing";
#define STRING_PAUSED "Paused";
#define STRING_STOPPED "Stopped";

typedef struct {
    AVFormatContext *fmt;
    int astream;
    AVCodecContext *cc;
    SwrContext *swr;
} ffmpegparams_t;

typedef struct {
    const char *name;
    char type;
    void *value;
} Property;

typedef struct {
    char type;
    void *value;
} PropertyValue;

static struct RootPropertyValues {
    dbus_bool_t can_quit;
    dbus_bool_t can_raise;
    dbus_bool_t has_track_list;
    const char *identity;
    dbus_bool_t can_set_fullscreen;
    dbus_bool_t fullscreen;
} root_values = {.can_quit = TRUE,
                 .can_raise = FALSE,
                 .has_track_list = FALSE,
                 .identity = APP_NAME,
                 .can_set_fullscreen = FALSE,
                 .fullscreen = FALSE};

const char *rootprop_names[] = {"CanQuit",  "CanRaise",     "CanSetFullscreen",   "Fullscreen",         "HasTrackList",
                                "Identity", "DesktopEntry", "SupportedMimeTypes", "SupportedUriSchemes"};
#define SUPPORTED_MIME_TYPES_INDEX 7
#define SUPPORTED_URI_SCHEMES_INDEX 8
PropertyValue rootprop_values[] = {{DBUS_TYPE_BOOLEAN, &root_values.can_quit},
                                   {DBUS_TYPE_BOOLEAN, &root_values.can_raise},
                                   {DBUS_TYPE_BOOLEAN, &root_values.can_set_fullscreen},
                                   {DBUS_TYPE_BOOLEAN, &root_values.fullscreen},
                                   {DBUS_TYPE_BOOLEAN, &root_values.has_track_list},
                                   {DBUS_TYPE_STRING, &root_values.identity},
                                   {DBUS_TYPE_STRING, &root_values.identity}};

struct PlayerPropertyValues {
    const char *playback_status;
    double rate;
    dbus_bool_t shuffle;
    const char *loop_status;
    double minimum_rate;
    double maximum_rate;
    dbus_bool_t can_go_next;
    dbus_bool_t can_go_previous;
    dbus_bool_t can_play;
    dbus_bool_t can_pause;
    dbus_bool_t can_seek;
    dbus_bool_t can_control;
} player_values = {.playback_status = "Stopped",
                   .rate = 1.0,
                   .shuffle = 0,
                   .loop_status = "None",
                   .minimum_rate = 1.0,
                   .maximum_rate = 1.0,
                   .can_go_next = FALSE,
                   .can_go_previous = FALSE,
                   .can_play = TRUE,
                   .can_pause = TRUE,
                   .can_seek = 0,
                   .can_control = TRUE};
const char *playerprop_names[] = {"CanControl",     "CanGoNext",  "CanGoPrevious", "CanPause", "CanPlay",
                                  "CanSeek",        "LoopStatus", "MaximumRate",   "Metadata", "MinimumRate",
                                  "PlaybackStatus", "Rate",       "Shuffle"};
PropertyValue playerprop_values[] = {{DBUS_TYPE_BOOLEAN, &player_values.can_control},
                                     {DBUS_TYPE_BOOLEAN, &player_values.can_go_next},
                                     {DBUS_TYPE_BOOLEAN, &player_values.can_go_previous},
                                     {DBUS_TYPE_BOOLEAN, &player_values.can_pause},
                                     {DBUS_TYPE_BOOLEAN, &player_values.can_play},
                                     {DBUS_TYPE_BOOLEAN, &player_values.can_seek},
                                     {DBUS_TYPE_STRING, &player_values.loop_status},
                                     {DBUS_TYPE_DOUBLE, &player_values.maximum_rate},
                                     {DBUS_TYPE_DOUBLE, &player_values.minimum_rate},
                                     {DBUS_TYPE_STRING, &player_values.playback_status},
                                     {DBUS_TYPE_DOUBLE, &player_values.rate},
                                     {DBUS_TYPE_BOOLEAN, &player_values.shuffle}};
#define METADATA_INDEX 8
char *uri = NULL;
int64_t position = 0;
ffmpegparams_t ffmpegparams;
enum status_t { PLAYING, PAUSED, STOPPED, QUITTING };
enum status_t status = STOPPED;

static inline void set_playing() {
    status = PLAYING;
    player_values.playback_status = STRING_PLAYING;
}

static inline void set_paused() {
    status = PAUSED;
    player_values.playback_status = STRING_PAUSED;
}

static inline void set_stopped() {
    status = STOPPED;
    player_values.playback_status = STRING_STOPPED;
}

int binsearch(const char *target, const char *array[], int nelements) {
    int first = 0;
    int last = nelements - 1;

    while (first <= last) {
        int mid = (first + last) / 2;
        int cmpresult = strcmp(target, array[mid]);

        if (cmpresult == 0) {
            return mid;
        } else if (cmpresult < 0) {
            last = mid - 1;
        } else {
            first = mid + 1;
        }
    }
    return -1;
}

const char *gettag(char *tag[2]) { return tag[1]; }
const char *tag2xesam(const char *tagname) {
    static const char *tagmap_keys[] = {"album", "album_artist", "artist", "comment", "composer", "date",
                                        "disc",  "genre",        "title",  "track",   "url"};
    static const char *tagmap_values[] = {"xesam:album",    "xesam:albumArtist",    "xesam:artist",     "xesam:comment",
                                          "xesam:composer", "xesam:contentCreated", "xesam:discNumber", "xesam:genre",
                                          "xesam:title",    "xesam:trackNumber",    "xesam:url"};

    int index = binsearch(tagname, tagmap_keys, sizeof(tagmap_keys) / sizeof(tagmap_keys[0]));
    if (index >= 0) {
        return tagmap_values[index];
    }

    return NULL;
}

typedef pa_simple audio_t;
audio_t *initaudio() {
    pa_simple *s;
    pa_sample_spec ss;

    ss.format = PA_SAMPLE_S16NE;
    ss.channels = 2;
    ss.rate = 44100;

    s = pa_simple_new(NULL,
                      APP_NAME, // Our application's name.
                      PA_STREAM_PLAYBACK,
                      NULL,    // Use the default device.
                      "Music", // Description of our stream.
                      &ss,     // Our sample format.
                      NULL,    // Use default channel map
                      NULL,    // Use default buffering attributes.
                      NULL     // Ignore error code.
    );
    return s;
}

void writeaudio(audio_t *audio, const uint8_t *outbuf, int frames) {
    int error;
    int bytes = 2 * 2 * frames; // sizeof frame (16bits = 2bytes) * number of
                                // channels (2) * frames
    pa_simple_write(audio, outbuf, bytes, &error);
}

void finishaudio(audio_t *audio) { pa_simple_free(audio); }

int openuri(const char *uri, ffmpegparams_t *ffmpegparams) {
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, uri, NULL, NULL) < 0) {
        syslog(LOG_ERR, "Failed to open URI\n");
        return 1;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        syslog(LOG_ERR, "Failed to read stream info\n");
        return 1;
    }

    const AVCodec *codec = NULL;
    int astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (astream < 0) {
        syslog(LOG_ERR, "No audio stream present\n");
        return 1;
    }

    if (!codec) {
        syslog(LOG_ERR, "No decoder\n");
        return 1;
    }

    AVCodecContext *cc = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(cc, fmt->streams[astream]->codecpar);
    cc->pkt_timebase = fmt->streams[astream]->time_base;
    if (avcodec_open2(cc, codec, NULL) < 0) {
        syslog(LOG_ERR, "Failed to open decoder\n");
        return 1;
    }

    AVChannelLayout out_layout, in_layout;
    av_channel_layout_default(&out_layout, CHANNELS);
    av_channel_layout_default(&in_layout, cc->ch_layout.nb_channels);
    SwrContext *swr = swr_alloc();
    swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, SAMPLE_RATE, &in_layout, cc->sample_fmt, cc->sample_rate,
                        0, NULL);
    swr_init(swr);
    ffmpegparams->fmt = fmt;
    ffmpegparams->astream = astream;
    ffmpegparams->cc = cc;
    ffmpegparams->swr = swr;

    return 0;
}

void ffmpegparams_free(ffmpegparams_t *ffmpegparams) {
    avcodec_free_context(&ffmpegparams->cc);
    avformat_close_input(&ffmpegparams->fmt);
    swr_free(&ffmpegparams->swr);
}

void add_basic_variant(DBusMessageIter *iter, int type, const void *value) {
    DBusMessageIter sub;
    char typestr[2];
    typestr[0] = type;
    typestr[1] = 0;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, typestr, &sub);
    dbus_message_iter_append_basic(&sub, type, value);
    dbus_message_iter_close_container(iter, &sub);
}

void add_dict_entry(DBusMessageIter *iter, const char *key, int type, const void *value) {
    DBusMessageIter sub;
    dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &sub);
    dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &key);

    add_basic_variant(&sub, type, value);

    dbus_message_iter_close_container(iter, &sub);
}

static inline void add_metadata_entries(DBusMessageIter *iter, AVDictionary *metadata) {
    const char *path = OBJ_PATH NO_TRACK;

    add_dict_entry(iter, "mpris:trackId", DBUS_TYPE_OBJECT_PATH, &path);

    const AVDictionaryEntry *tag = NULL;
    while ((tag = av_dict_iterate(metadata, tag))) {
        const char *key = NULL;
        if (strcmp(tag->key, "StreamTitle") == 0) {
            key = "xesam:title";
        } else if (strcmp(tag->key, "icy-genre") == 0) {
            key = "xesam:genre";
        } else if (strcmp(tag->key, "icy-logo") == 0) {
            key = "mpris:artUrl";
        } else if (strcmp(tag->key, "icy-stream-url") == 0) {
            key = "xesam:url";
        } else {
            key = tag2xesam(tag->key);
        };
        if (key) {
            if (dbus_validate_utf8(tag->value, NULL)) {
                add_dict_entry(iter, key, DBUS_TYPE_STRING, &tag->value);
            } else {
                syslog(LOG_ERR, "Tag %s value is not valid utf8: %s", tag->key, tag->value);
            }
        }
    }
}

void add_protocols(DBusMessageIter *iter) {
    void *protoiter = NULL;
    const char *proto = avio_enum_protocols(&protoiter, 0);
    while (proto != NULL) {
        dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &proto);
        proto = avio_enum_protocols(&protoiter, 0);
    };
}

void add_mimetypes(DBusMessageIter *iter) {
    for (const AVCodecDescriptor *desc = NULL; desc != NULL; desc = avcodec_descriptor_next(desc)) {
        for (int i = 0; desc->mime_types[i] != NULL; i++) {
            // NOTE: if there are cases where two codecs support the same mimetype, then this
            // needs to use a hash map
            dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &desc->mime_types[i]);
        }
    }
}

void add_metadata_variant(DBusMessageIter *iter, AVDictionary *metadata) {
    DBusMessageIter sub, map;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "a{sv}", &sub);
    dbus_message_iter_open_container(&sub, DBUS_TYPE_ARRAY, "{sv}", &map);

    add_metadata_entries(&map, metadata);

    dbus_message_iter_close_container(&sub, &map);
    dbus_message_iter_close_container(iter, &sub);
}

void add_metadata_dict_entry(DBusMessageIter *iter, AVDictionary *metadata) {
    DBusMessageIter entry;
    dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *val = "Metadata";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &val);
    add_metadata_variant(&entry, metadata);
    dbus_message_iter_close_container(iter, &entry);
}

void notify_metadata_changed(DBusConnection *connection, AVDictionary *metadata) {
    DBusMessageIter iter, sub;
    DBusMessage *signal = dbus_message_new_signal(OBJ_PATH, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged");
    dbus_message_iter_init_append(signal, &iter);
    const char *interface = IFACE_PLAYER;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);

    DBusMessageIter array;
    assert(dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &array));
    add_metadata_dict_entry(&array, metadata);
    dbus_message_iter_close_container(&iter, &array);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &sub);
    dbus_message_iter_close_container(&iter, &sub);
    dbus_connection_send(connection, signal, NULL);
    dbus_message_unref(signal);
}

void notify_playback_status_changed(DBusConnection *connection, const char *new_status) {
    DBusMessageIter iter, sub;
    DBusMessage *signal = dbus_message_new_signal(OBJ_PATH, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged");
    dbus_message_iter_init_append(signal, &iter);
    const char *interface = IFACE_PLAYER;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);

    DBusMessageIter array;
    assert(dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &array));
    add_dict_entry(&array, "PlaybackStatus", DBUS_TYPE_STRING, &new_status);
    dbus_message_iter_close_container(&iter, &array);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &sub);
    dbus_message_iter_close_container(&iter, &sub);
    dbus_connection_send(connection, signal, NULL);
    dbus_message_unref(signal);
}

static inline dbus_bool_t get_relevant_args(DBusMessage *msg, const char **interface, const char **property) {
    DBusMessageIter iter;
    dbus_message_iter_init(msg, &iter);
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        return FALSE;
    }
    dbus_message_iter_get_basic(&iter, interface);
    if (dbus_message_iter_has_next(&iter)) {
        dbus_message_iter_next(&iter);
    } else {
        return FALSE;
    }
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        return FALSE;
    }
    dbus_message_iter_get_basic(&iter, property);
    return TRUE;
}

static inline DBusMessage *openuri_handler(DBusMessage *msg, ffmpegparams_t *ffmpegparams) {
    DBusMessageIter args;
    if (!dbus_message_iter_init(msg, &args))
        return dbus_message_new_error(msg, "Message has no arguments!\n", "");
    if (DBUS_TYPE_STRING != dbus_message_iter_get_arg_type(&args))
        return dbus_message_new_error(msg, "Argument is not string!\n", "");

    dbus_message_iter_get_basic(&args, &uri);
    openuri(uri, ffmpegparams);
    set_playing();
    return dbus_message_new_method_return(msg);
}

static inline DBusMessage *play_handler(DBusMessage *msg, ffmpegparams_t *ffmpegparams) {
    switch (status) {
        case PAUSED:
            av_read_play(ffmpegparams->fmt);
            set_playing();
            break;
        case STOPPED:
            if (uri != NULL && !openuri(uri, ffmpegparams))
                set_playing();
            break;
        default:
            break;
    }
    return dbus_message_new_method_return(msg);
}

static inline DBusMessage *playpause_handler(DBusMessage *msg, ffmpegparams_t *ffmpegparams) {
    switch (status) {
        case PLAYING:
            av_read_pause(ffmpegparams->fmt);
            set_paused();
            break;
        case PAUSED:
            av_read_play(ffmpegparams->fmt);
            set_playing();
            break;
        case STOPPED:
            if (uri != NULL && !openuri(uri, ffmpegparams))
                set_playing();
            break;
        default:
            break;
    }
    return dbus_message_new_method_return(msg);
}

static inline DBusMessage *pause_handler(DBusMessage *msg, ffmpegparams_t *ffmpegparams) {
    if (status == PLAYING) {
        av_read_pause(ffmpegparams->fmt);
        set_paused();
    }
    return dbus_message_new_method_return(msg);
}

static inline DBusMessage *stop_handler(DBusMessage *msg, ffmpegparams_t *ffmpegparams) {
    if (status != STOPPED) {
        avcodec_free_context(&ffmpegparams->cc);
        avformat_close_input(&ffmpegparams->fmt);
        set_stopped();
    }
    return dbus_message_new_method_return(msg);
}

static inline DBusMessage *get_handler(DBusMessage *msg, ffmpegparams_t *ffmpegparams) {
    const char *interface = NULL, *property = NULL;
    DBusMessage *reply;
    if (get_relevant_args(msg, &interface, &property)) {
        DBusMessageIter iter;
        reply = dbus_message_new_method_return(msg);
        dbus_message_iter_init_append(reply, &iter);
        if (strcmp(interface, IFACE_ROOT) == 0) {
            int index = binsearch(property, rootprop_names, sizeof(rootprop_names) / sizeof(rootprop_names[0]));
            if (index >= 0) {
                int cmp = index < (int)sizeof(rootprop_values) / (int)sizeof(rootprop_values[0]);
                DBusMessageIter dict, variant;
                dbus_message_iter_open_container(&iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
                dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &rootprop_names[index]);
                if (cmp) {
                    PropertyValue *pv = &rootprop_values[index];
                    add_basic_variant(&dict, pv->type, pv->value);
                } else {
                    DBusMessageIter array;
                    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &array);
                    if (index == SUPPORTED_MIME_TYPES_INDEX)
                        add_mimetypes(&dict);
                    else
                        add_protocols(&dict);
                    dbus_message_iter_close_container(&variant, &array);
                }
                dbus_message_iter_close_container(&dict, &variant);
                dbus_message_iter_close_container(&iter, &dict);
            } else {
                dbus_message_unref(reply);
                reply = dbus_message_new_error(msg, "org.freedesktop.DBus.Properties.Get.Error", "No such property");
            }
        } else if (strcmp(interface, IFACE_PLAYER) == 0) {
            int index = binsearch(property, playerprop_names, sizeof(playerprop_names) / sizeof(playerprop_names[0]));
            if (index >= 0) {
                if (index == METADATA_INDEX) {
                    add_metadata_variant(&iter, ffmpegparams->fmt->metadata);
                } else {
                    if (index > METADATA_INDEX)
                        index--;
                    PropertyValue *pv = &playerprop_values[index];
                    add_basic_variant(&iter, pv->type, pv->value);
                }
            } else {
                dbus_message_unref(reply);
                reply = dbus_message_new_error(msg, "org.freedesktop.Properties.Get.Error", "No such property");
            }
        } else {
            dbus_message_unref(reply);
            reply = dbus_message_new_error(msg, "org.freedesktop.Properties.Get.Error", "No such interface");
        }
    } else {
        reply = dbus_message_new_error(msg, "org.mpris.MediaPlayer2.tinyaudio.Error",
                                       "Expected interface and property arguments");
    }
    return reply;
}

static inline DBusMessage *set_handler(DBusMessage *msg) {
    DBusMessage *reply;
    const char *interface, *property;
    if (!get_relevant_args(msg, &interface, &property)) {
        reply = dbus_message_new_error(msg, "org.mpris.MediaPlayer2.tinyaudio.Error",
                                       "Expected interface and property arguments");
    } else {
        reply = dbus_message_new_method_return(msg);
    }
    if (strcmp(interface, IFACE_PLAYER) == 0) {
        if (strcmp(property, "LoopStatus") == 0) {
            reply = dbus_message_new_method_return(msg);
        } else if (strcmp(property, "Rate") == 0) {
            reply = dbus_message_new_method_return(msg);
        } else if (strcmp(property, "Shuffle") == 0) {
            reply = dbus_message_new_method_return(msg);
        } else if (strcmp(property, "Volume") == 0) {
            reply = dbus_message_new_method_return(msg);
        } else {
            reply = dbus_message_new_error(msg, "org.freedesktop.DBus.Properties.Set.Error", "No such property");
        }
    } else {
        reply = dbus_message_new_error(msg, "org.freedesktop.DBus.Properties.Set.Error", "No such interface");
    }
    return reply;
}

static inline DBusMessage *getall_handler(DBusMessage *msg, ffmpegparams_t *ffmpegparams) {
    DBusMessage *reply;
    const char *interface = NULL, *property = NULL;
    get_relevant_args(msg, &interface, &property);
    if (strcmp(interface, IFACE_ROOT) == 0) {
        reply = dbus_message_new_method_return(msg);
        DBusMessageIter iter;
        dbus_message_iter_init_append(reply, &iter);
        ADD_CONTAINER(&iter, DBUS_TYPE_ARRAY, "{sv}", ({
            for (unsigned int i = 0; i < sizeof(rootprop_values) / sizeof(rootprop_values[0]); i++) {
                PropertyValue *pv = &rootprop_values[i];
                add_dict_entry(&container, rootprop_names[i], pv->type, pv->value);
            }

            for (unsigned int i = SUPPORTED_MIME_TYPES_INDEX; i < sizeof(rootprop_names) / sizeof(rootprop_names[0]); i++) {
                DBusMessageIter dict;
                dbus_message_iter_open_container(&container, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
                dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &rootprop_names[i]);

                DBusMessageIter variant, array;
                dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "as", &variant);
                dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &array);

                (i == SUPPORTED_URI_SCHEMES_INDEX) ? add_protocols(&array) : add_mimetypes(&array);

                dbus_message_iter_close_container(&variant, &array);
                dbus_message_iter_close_container(&dict, &variant);

                dbus_message_iter_close_container(&container, &dict);
            }
        }));
    } else if (strcmp(interface, IFACE_PLAYER) == 0) {
        reply = dbus_message_new_method_return(msg);
        DBusMessageIter iter, sub[2];
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub[0]);
        for (int i = 0; i < METADATA_INDEX; i++) {
            PropertyValue *pv = &playerprop_values[i];
            add_dict_entry(&sub[0], playerprop_names[i], pv->type, pv->value);
        }
        if (ffmpegparams->fmt)
            add_metadata_dict_entry(&sub[0], ffmpegparams->fmt->metadata);
        for (unsigned int i = METADATA_INDEX; i < sizeof(playerprop_values) / sizeof(playerprop_values[0]); i++) {
            PropertyValue *pv = &playerprop_values[i];
            add_dict_entry(&sub[0], playerprop_names[i + 1], pv->type, pv->value);
        }
        dbus_message_iter_close_container(&iter, &sub[0]);
    } else {
        reply = dbus_message_new_error(msg, "org.freedesktop.DBus.Properties.GetAll.Error", "No such interface");
    }
    return reply;
}

static inline DBusMessage *properties_handler(DBusMessage *msg, const char *member, ffmpegparams_t *ffmpegparams) {
    int cmp = strcmp(member, "GetAll");
    if (cmp < 0 && strcmp(member, "Get") == 0)
        return get_handler(msg, ffmpegparams);
    else if (cmp > 0 && strcmp(member, "Set") == 0)
        return set_handler(msg);
    else
        return getall_handler(msg, ffmpegparams);
    return NULL;
}

static inline DBusMessage *root_handler(DBusMessage *msg, const char *member) {
    if (strcmp(member, "Quit") == 0) {
        status = QUITTING;
        return dbus_message_new_method_return(msg);
    } else
        return NULL;
}

static inline DBusMessage *player_handler(DBusMessage *msg, const char *member, ffmpegparams_t *ffmpegparams) {
    // NOTE: binary search -based dispatch. This produces less code than calling binsearch and using switch aferwards,
    // and is probably faster too.
    int cmp = strcmp("Play", member);
    if (cmp > 0) {
        int cmp = strcmp("OpenUri", member);
        if (cmp == 0) {
            return openuri_handler(msg, ffmpegparams);
        } else if (cmp < 0 && strcmp("Pause", member) == 0) {
            return pause_handler(msg, ffmpegparams);
        }
    } else if (cmp < 0) {
        int cmp = strcmp("Stop", member);
        if (cmp == 0) {
            return stop_handler(msg, ffmpegparams);
        } else if (cmp > 0 && strcmp("PlayPause", member) == 0) {
            return playpause_handler(msg, ffmpegparams);
        }
    } else {
        return play_handler(msg, ffmpegparams);
    }
    return NULL;
}

static inline void handle_message(DBusConnection *conn, DBusMessage *msg, ffmpegparams_t *ffmpegparams) {
    DBusMessage *reply = NULL;

    if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
        const char *iface = dbus_message_get_interface(msg);
        const char *member = dbus_message_get_member(msg);

        if (strcmp(DBUS_INTERFACE_PROPERTIES, iface) == 0)
            reply = properties_handler(msg, member, ffmpegparams);
        else if (strcmp(IFACE_PLAYER, iface) == 0) {
            enum status_t old_status = status;
            reply = player_handler(msg, member, ffmpegparams);
            if (old_status != status) {
                notify_playback_status_changed(conn, player_values.playback_status);
            }
        } else if (strcmp(IFACE_ROOT, iface) == 0)
            reply = root_handler(msg, member);
        else if (strcmp(DBUS_INTERFACE_INTROSPECTABLE, iface) == 0 && strcmp("Introspect", member) == 0) {
            reply = dbus_message_new_method_return(msg);
            const char *val = XML_DATA;
            dbus_message_append_args(reply, DBUS_TYPE_STRING, &val, DBUS_TYPE_INVALID);
        }
        if (!reply) {
            reply =
                dbus_message_new_error(msg, "org.mpris.MediaPlayer2.tinyaudio.Error", "Invalid interface or method");
        }
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        dbus_connection_flush(conn);
    }
}

static inline dbus_bool_t handle_dbus_error(DBusError *e, const char *msg) {
    if (dbus_error_is_set(e)) {
        syslog(LOG_ERR, "%s: %s\n", msg, e->message);
        dbus_error_free(e);
        return TRUE;
    } else {
        return FALSE;
    }
}

const char *process_command_line(int argc, char *argv[]) {
    if (argc > 1) {
        int cmp = strcmp("play", argv[1]);
        if (cmp > 0 && strcmp("pause", argv[1]) == 0) {
            return "Pause";
        } else if (cmp < 0) {
            if (strcmp("stop", argv[1]) == 0) {
                return "Stop";
            } else if (strcmp("quit", argv[1]) == 0) {
                return "Quit";
            };
        } else {
            if (argc == 3) {
                return "OpenUri";
            }
            return "Play";
        }
    }
    printf("USAGE: %s (play [uri] | pause | stop | quit)\nStart playback of an internet audio stream, music file or "
           "playlist or control the player running in the background.",
           argv[0]);
    return NULL;
}

void ffmpeg_log_handler(void *avcl, int av_level, const char *fmt, va_list vl) {
    (void)avcl; // suppress unused parameter warning

    int level = LOG_DEBUG;
    switch (av_level) {
        case AV_LOG_PANIC:
            level = LOG_CRIT;
            break;
        case AV_LOG_FATAL:
            level = LOG_CRIT;
            break;
        case AV_LOG_ERROR:
            level = LOG_ERR;
            break;
        case AV_LOG_WARNING:
            level = LOG_WARNING;
            break;
        case AV_LOG_INFO:
            level = LOG_NOTICE;
            break;
        case AV_LOG_VERBOSE:
            level = LOG_INFO;
            break;
        case AV_LOG_DEBUG:
            level = LOG_DEBUG;
            break;
    }
    vsyslog(level, fmt, vl);
}

int main(int argc, char **argv) {
    const char *method = process_command_line(argc, argv);

    if (!method)
        return 0;

    openlog(APP_NAME, LOG_CONS, 0);
    av_log_set_callback(ffmpeg_log_handler);

    DBusError err;
    dbus_error_init(&err);

    DBusConnection *dbus_conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!dbus_conn) {
        const char *msg = "Failed to connect to session bus";
        if (!handle_dbus_error(&err, msg)) {
            syslog(LOG_ERR, "%s\n", msg);
        }
        return 1;
    }

    int has_owner = dbus_bus_name_has_owner(dbus_conn, BUS_NAME, &err);
    if (handle_dbus_error(&err, "NameHasOwner failed")) {
        return 1;
    }

    if (has_owner) {
        const char *iface = IFACE_PLAYER;
        const char *quit_method = "Quit";
        if (method == quit_method) {
            iface = IFACE_ROOT;
        }
        /* Call OpenUri on the existing owner and exit */
        DBusMessage *msg = dbus_message_new_method_call(BUS_NAME, OBJ_PATH, iface, method);
        if (!msg) {
            syslog(LOG_ERR, "Failed to create DBus message\n");
            return 1;
        }

        const char *openuri_method = "OpenUri";
        if (method == openuri_method) {
            DBusMessageIter it;
            dbus_message_iter_init_append(msg, &it);
            uri = argv[2];
            const char *s = uri;
            if (!dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &s)) {
                syslog(LOG_ERR, "Failed to append argument\n");
                return 1;
            }
        }

        DBusMessage *reply = dbus_connection_send_with_reply_and_block(dbus_conn, msg, -1, &err);
        dbus_message_unref(msg);
        if (!reply) {
            const char *msg = "OpenUri failed";
            if (handle_dbus_error(&err, msg)) {
                syslog(LOG_ERR, "%s\n", msg);
            }
            return 1;
        }
        dbus_message_unref(reply);
    } else {
        const char *openuri_method = "OpenUri";
        if (method != openuri_method) {
            printf("Player is not running\n");
            return 0;
        }
        uri = argv[2];

        int ret = dbus_bus_request_name(dbus_conn, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
        if (handle_dbus_error(&err, "RequestName failed")) {
            return 1;
        }
        if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
            syslog(LOG_ERR, "Could not become primary owner\n");
            return 1;
        }

        __pid_t pid = fork();
        switch (pid) {
            case -1:;
                syslog(LOG_ERR, "Failed to fork\n");
                return 1;
            case 0:;
                audio_t *audio = initaudio();
                if (audio == NULL)
                    return 1;
                openuri(uri, &ffmpegparams);
                set_playing();

                AVFrame *frm = av_frame_alloc();
                AVPacket *pkt = av_packet_alloc();
                int error_count = 0;
                // TODO: log when playback started
                while (1) {
                    if (!dbus_connection_read_write(dbus_conn, 0)) {
                        syslog(LOG_ERR, "DBus connection closed");
                        return 1;
                    }
                    DBusMessage *msg;
                    while ((msg = dbus_connection_pop_message(dbus_conn)) != NULL) {
                        handle_message(dbus_conn, msg, &ffmpegparams);
                        dbus_message_unref(msg);
                    }
                    if (status == QUITTING)
                        break;
                    if (status != PLAYING) {
                        // TODO: sleep value should depend on how long dbus method
                        // handling took
                        usleep(100000);
                        continue;
                    }
                    int read_result = av_read_frame(ffmpegparams.fmt, pkt);
                    if (read_result >= 0) {
                        if (ffmpegparams.fmt->event_flags & AVFMT_EVENT_FLAG_METADATA_UPDATED) {
                            AVDictionary *metadata = ffmpegparams.fmt->metadata;
                            notify_metadata_changed(dbus_conn, metadata);
                            ffmpegparams.fmt->event_flags ^= AVFMT_EVENT_FLAG_METADATA_UPDATED;
                        }
                        if (pkt->stream_index == ffmpegparams.astream) {
                            if (avcodec_send_packet(ffmpegparams.cc, pkt) == 0) {
                                while (avcodec_receive_frame(ffmpegparams.cc, frm) == 0) {
                                    position = frm->best_effort_timestamp * frm->time_base.num / frm->time_base.den;
                                    uint8_t *outbuf = NULL;
                                    int out_samples = av_rescale_rnd(
                                        swr_get_delay(ffmpegparams.swr, ffmpegparams.cc->sample_rate) + frm->nb_samples,
                                        ffmpegparams.cc->sample_rate, ffmpegparams.cc->sample_rate, AV_ROUND_UP);
                                    av_samples_alloc(&outbuf, NULL, CHANNELS, out_samples, AV_SAMPLE_FMT_S16, 0);
                                    int n = swr_convert(ffmpegparams.swr, &outbuf, out_samples,
                                                        (const uint8_t **)frm->data, frm->nb_samples);
                                    int frames = n;
                                    writeaudio(audio, outbuf, frames);
                                    av_freep(&outbuf);
                                }
                            }
                        }
                        av_packet_unref(pkt);
                        error_count = 0;
                    } else {
                        if (read_result != AVERROR(EOF)) {
                            syslog(LOG_WARNING, "Unexpected stream error!");
                            error_count++;
                            if (error_count < 5) {
                                continue;
                            }
                        } else {
                            // TODO: if not at the end of playlist, or looping, call openuri with a new uri and continue
                        }
                        ffmpegparams_free(&ffmpegparams);
                        set_stopped();
                    }
                }
                // TODO: log an error if one occured, log when playback finished
                finishaudio(audio);
        }
    }
    return 0;
}
