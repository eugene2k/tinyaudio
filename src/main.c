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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
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
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <pulse/simple.h>

#define SAMPLE_RATE 44100
#define CHANNELS 2

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

typedef struct {
    AVFormatContext *fmt;
    int astream;
    AVCodecContext *cc;
    SwrContext *swr;
} ffmpegparams_t;
typedef struct Property {
    const char *name;
    char type;
    void *value;
} Property;

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
                 .identity = "tinyaudio",
                 .can_set_fullscreen = FALSE,
                 .fullscreen = FALSE};

struct Property rootprops[] = {{"CanQuit", DBUS_TYPE_BOOLEAN, &root_values.can_quit},
                               {"CanRaise", DBUS_TYPE_BOOLEAN, &root_values.can_raise},
                               {"CanSetFullscreen", DBUS_TYPE_BOOLEAN, &root_values.can_set_fullscreen},
                               {"Fullscreen", DBUS_TYPE_BOOLEAN, &root_values.fullscreen},
                               {"HasTrackList", DBUS_TYPE_BOOLEAN, &root_values.has_track_list},
                               {"Identity", DBUS_TYPE_STRING, &root_values.identity}};
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
struct Property playerprops[] = {{"CanControl", DBUS_TYPE_BOOLEAN, &player_values.can_control},
                                 {"CanGoNext", DBUS_TYPE_BOOLEAN, &player_values.can_go_next},
                                 {"CanGoPrevious", DBUS_TYPE_BOOLEAN, &player_values.can_go_previous},
                                 {"CanPause", DBUS_TYPE_BOOLEAN, &player_values.can_pause},
                                 {"CanPlay", DBUS_TYPE_BOOLEAN, &player_values.can_play},
                                 {"CanSeek", DBUS_TYPE_BOOLEAN, &player_values.can_seek},
                                 {"LoopStatus", DBUS_TYPE_STRING, &player_values.loop_status},
                                 {"MinimumRate", DBUS_TYPE_DOUBLE, &player_values.minimum_rate},
                                 {"MaximumRate", DBUS_TYPE_DOUBLE, &player_values.maximum_rate},
                                 {"PlaybackStatus", DBUS_TYPE_STRING, &player_values.playback_status},
                                 {"Rate", DBUS_TYPE_DOUBLE, &player_values.rate},
                                 {"Shuffle", DBUS_TYPE_BOOLEAN, &player_values.shuffle}};

char *uri = NULL;
int64_t position = 0;
ffmpegparams_t ffmpegparams;
enum status_t { PLAYING, PAUSED, STOPPED, QUITTING };
enum status_t status = STOPPED;

static inline void set_playing() {
    status = PLAYING;
    player_values.playback_status = "Playing";
}
static inline void set_paused() {
    status = PAUSED;
    player_values.playback_status = "Paused";
}
static inline void set_stopped() {
    status = STOPPED;
    player_values.playback_status = "Stopped";
}

typedef const char *(__get_element_t)(const void *);
void *binsearch(const char *target, const void *array, int nelements, int size, __get_element_t func) {
    int first = 0;
    int last = nelements - 1;

    while (first <= last) {
        int mid = (first + last) / 2;
        int cmpresult = strcmp(target, func(array + mid * size));

        if (cmpresult == 0) {
            return (void *)array + mid * size;
        } else if (cmpresult < 0) {
            last = mid - 1;
        } else {
            first = mid + 1;
        }
    }
    return NULL;
}
#define BINSEARCH(needle, haystack, get_element_cb)                                                                    \
    binsearch(needle, haystack, sizeof(haystack) / sizeof(haystack[0]), sizeof(haystack[0]), get_element_cb)

const char *gettag(char *tag[2]) { return tag[1]; }
const char *tag2xesam(const char *tagname) {
    static const char *tagmap[][2] = {{"album", "xesam:album"},
                                      {"album_artist", "xesam:albumArtist"},
                                      {"artist", "xesam:artist"},
                                      {"comment", "xesam:comment"},
                                      {"composer", "xesam:composer"},
                                      {"date", "xesam:contentCreated"},
                                      {"disc", "xesam:discNumber"},
                                      {"genre", "xesam:genre"},
                                      {"title", "xesam:title"},
                                      {"track", "xesam:trackNumber"},
                                      {"url", "xesam:url"}};

    const char **tag = BINSEARCH(tagname, tagmap, (__get_element_t *)gettag);
    if (tag != NULL) {
        return tag[1];
    }

    return NULL;
}

int propcmp(Property *a, Property *b) { return strcmp(a->name, b->name); }

const char *getprop(Property *p) { return p->name; }

typedef pa_simple audio_t;
audio_t *initaudio() {
    pa_simple *s;
    pa_sample_spec ss;

    ss.format = PA_SAMPLE_S16NE;
    ss.channels = 2;
    ss.rate = 44100;

    s = pa_simple_new(NULL,
                      "tinyaudio", // Our application's name.
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
        fprintf(stderr, "Failed to open URI\n");
        return 1;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        fprintf(stderr, "Failed to read stream info\n");
        return 1;
    }

    const AVCodec *codec = NULL;
    int astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (astream < 0) {
        fprintf(stderr, "No audio stream present\n");
        return 1;
    }

    if (!codec) {
        fprintf(stderr, "No decoder\n");
        return 1;
    }

    AVCodecContext *cc = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(cc, fmt->streams[astream]->codecpar);
    cc->pkt_timebase = fmt->streams[astream]->time_base;
    if (avcodec_open2(cc, codec, NULL) < 0) {
        fprintf(stderr, "Failed to open decoder\n");
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

typedef void (*__add_elements_t)(DBusMessageIter *, void *);

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
        } else {
            key = tag2xesam(tag->key);
        };
        if (key) {
            add_dict_entry(iter, key, DBUS_TYPE_STRING, &tag->value);
        }
    }
}
#define ADD_CONTAINER(iter, container_type, contents_type, expr)                                                       \
    ({                                                                                                                 \
        DBusMessageIter container;                                                                                     \
        assert(dbus_message_iter_open_container(iter, container_type, contents_type, &container));                     \
        expr;                                                                                                          \
        dbus_message_iter_close_container(iter, &container);                                                           \
    })
#define ADD_ARRAY_VARIANT(iter, content_type, expr)                                                                    \
    {                                                                                                                  \
        DBusMessageIter sub, array;                                                                                    \
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "a" content_type, &sub);                             \
        dbus_message_iter_open_container(&sub, DBUS_TYPE_ARRAY, content_type, &array);                                 \
        {                                                                                                              \
            expr;                                                                                                      \
        }                                                                                                              \
        dbus_message_iter_close_container(&sub, &array);                                                               \
        dbus_message_iter_close_container(iter, &sub);                                                                 \
    }
#define ADD_MAP_VARIANT(iter, expr)                                                                                    \
    {                                                                                                                  \
        DBusMessageIter sub, map;                                                                                      \
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "a{sv}", &sub);                                      \
        dbus_message_iter_open_container(&sub, DBUS_TYPE_ARRAY, "{sv}", &map);                                         \
        {                                                                                                              \
            expr;                                                                                                      \
        }                                                                                                              \
        dbus_message_iter_close_container(&sub, &map);                                                                 \
        dbus_message_iter_close_container(iter, &sub);                                                                 \
    }
#define ADD_DICT_ENTRY(iter, name, expr)                                                                               \
    {                                                                                                                  \
        const char *key = name;                                                                                        \
        dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &key);                                                  \
        {                                                                                                              \
            expr;                                                                                                      \
        }                                                                                                              \
    }
void add_protocols_variant(DBusMessageIter *iter) {
    ADD_ARRAY_VARIANT(iter, "s", {
        void *protoiter = NULL;
        const char *proto = avio_enum_protocols(&protoiter, 0);
        while (proto != NULL) {
            dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &proto);
            proto = avio_enum_protocols(&protoiter, 0);
        };
    });
}
void add_mimetypes_variant(DBusMessageIter *iter) {
    ADD_ARRAY_VARIANT(iter, "s", {
        for (const AVCodecDescriptor *desc = NULL; desc != NULL; desc = avcodec_descriptor_next(desc)) {
            for (int i = 0; desc->mime_types[i] != NULL; i++) {
                // NOTE: if there are cases where two codecs support the same mimetype, then this
                // needs to use a hash map
                dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &desc->mime_types[i]);
            }
        }
    });
}

void add_metadata_variant(DBusMessageIter *iter, AVDictionary *metadata) {
    ADD_MAP_VARIANT(iter, { add_metadata_entries(&map, metadata); })
}

void add_metadata_dict_entry(DBusMessageIter *iter, AVDictionary *metadata) {
    DBusMessageIter entry;
    dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *val = "Metadata";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &val);
    add_metadata_variant(&entry, metadata);
    dbus_message_iter_close_container(iter, &entry);
}

void notify_properties_changed(DBusConnection *connection, __add_elements_t func, void *userdata) {
    DBusMessageIter iter, sub;
    DBusMessage *signal = dbus_message_new_signal(OBJ_PATH, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged");
    dbus_message_iter_init_append(signal, &iter);
    const char *interface = IFACE_PLAYER;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);

    ADD_CONTAINER(&iter, DBUS_TYPE_ARRAY, "{sv}", { func(&container, userdata); });

    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &sub);
    dbus_message_iter_close_container(&iter, &sub);
    dbus_connection_send(connection, signal, NULL);
}

dbus_bool_t get_relevant_args(DBusMessage *msg, const char **interface, const char **property) {
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
void handle_message(DBusConnection *conn, DBusMessage *msg, ffmpegparams_t *ffmpegparams) {
    DBusMessage *reply = NULL;
    if (dbus_message_is_method_call(msg, IFACE_PLAYER, "OpenUri")) {
        DBusMessageIter args;
        if (!dbus_message_iter_init(msg, &args))
            fprintf(stderr, "Message has no arguments!\n");
        else if (DBUS_TYPE_STRING != dbus_message_iter_get_arg_type(&args))
            fprintf(stderr, "Argument is not string!\n");
        else
            dbus_message_iter_get_basic(&args, &uri);
        openuri(uri, ffmpegparams);
        set_playing();

        reply = dbus_message_new_method_return(msg);
    } else if (dbus_message_is_method_call(msg, IFACE_PLAYER, "Play")) {
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
        reply = dbus_message_new_method_return(msg);
    } else if (dbus_message_is_method_call(msg, IFACE_PLAYER, "Pause")) {
        if (status == PLAYING) {
            av_read_pause(ffmpegparams->fmt);
            set_paused();
        }
        reply = dbus_message_new_method_return(msg);
    } else if (dbus_message_is_method_call(msg, IFACE_PLAYER, "PlayPause")) {
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
        reply = dbus_message_new_method_return(msg);
    } else if (dbus_message_is_method_call(msg, IFACE_PLAYER, "Stop")) {
        if (status != STOPPED) {
            avcodec_free_context(&ffmpegparams->cc);
            avformat_close_input(&ffmpegparams->fmt);
            set_stopped();
        }
        reply = dbus_message_new_method_return(msg);
    } else if (dbus_message_is_method_call(msg, IFACE_ROOT, "Quit")) {
        status = QUITTING;
        reply = dbus_message_new_method_return(msg);
    } else if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "Get")) {
        const char *interface = NULL, *property = NULL;
        if (get_relevant_args(msg, &interface, &property)) {
            DBusMessageIter iter;
            reply = dbus_message_new_method_return(msg);
            dbus_message_iter_init_append(reply, &iter);
            if (strcmp(interface, IFACE_ROOT) == 0) {
                Property *p = BINSEARCH(property, rootprops, (__get_element_t *)getprop);
                if (p) {
                    add_basic_variant(&iter, p->type, &p->value);
                } else if (strcmp(property, "SupportedUriSchemes") == 0) {
                    ADD_ARRAY_VARIANT(&iter, "s", { add_protocols_variant(&array); });
                } else if (strcmp(property, "SupportedMimeTypes") == 0) {
                    ADD_ARRAY_VARIANT(&iter, "s", { add_mimetypes_variant(&array); });
                } else {
                    reply =
                        dbus_message_new_error(msg, "org.freedesktop.DBus.Properties.Get.Error", "No such property");
                }
            } else if (strcmp(interface, IFACE_PLAYER) == 0) {
                struct Property *p = BINSEARCH(property, playerprops, (__get_element_t *)getprop);
                if (p) {
                    add_basic_variant(&iter, p->type, &p->value);
                } else if (strcmp(property, "Metadata") == 0) {
                    add_metadata_variant(&iter, ffmpegparams->fmt->metadata);
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
    } else if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "Set")) {
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
    } else if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "GetAll")) {
        const char *interface = NULL, *property = NULL;
        get_relevant_args(msg, &interface, &property);
        if (strcmp(interface, IFACE_ROOT) == 0) {
            reply = dbus_message_new_method_return(msg);
            DBusMessageIter iter;
            dbus_message_iter_init_append(reply, &iter);
            ADD_CONTAINER(&iter, DBUS_TYPE_ARRAY, "{sv}", {
                for (int i = 0; i < sizeof(rootprops) / sizeof(Property); i++) {
                    Property *p = &rootprops[i];
                    add_dict_entry(&container, p->name, p->type, p->value);
                }
            });
            ADD_DICT_ENTRY(&iter, "SupportedUriSchemes", { add_protocols_variant(&iter); });
            ADD_DICT_ENTRY(&iter, "SupportedMimeTypes", { add_mimetypes_variant(&iter); });
        } else if (strcmp(interface, IFACE_PLAYER) == 0) {
            reply = dbus_message_new_method_return(msg);
            DBusMessageIter iter, sub[2];
            dbus_message_iter_init_append(reply, &iter);
            dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub[0]);
            for (int i = 0; i < sizeof(playerprops) / sizeof(Property); i++) {
                Property *p = &playerprops[i];
                add_dict_entry(&sub[0], p->name, p->type, p->value);
            }
            add_metadata_dict_entry(&sub[0], ffmpegparams->fmt->metadata);
            dbus_message_iter_close_container(&iter, &sub[0]);
        } else {
            reply = dbus_message_new_error(msg, "org.freedesktop.DBus.Properties.GetAll.Error", "No such interface");
        }
    } else if (dbus_message_is_method_call(msg, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
        reply = dbus_message_new_method_return(msg);
        const char *val = XML_DATA;
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &val, DBUS_TYPE_INVALID);
    } else if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
        reply = dbus_message_new_error(msg, "org.mpris.MediaPlayer2.tinyaudio.Error", "Invalid interface or method");
    }
    if (reply) {
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        dbus_connection_flush(conn);
    }
}

dbus_bool_t handle_dbus_error(DBusError *e, const char *msg) {
    if (dbus_error_is_set(e)) {
        fprintf(stderr, "%s: %s\n", msg, e->message);
        dbus_error_free(e);
        return TRUE;
    } else {
        return FALSE;
    }
}
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <uri>\n", argv[0]);
        return 1;
    }
    uri = argv[1];

    DBusError err;
    dbus_error_init(&err);

    DBusConnection *dbus_conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!dbus_conn) {
        const char *msg = "Failed to connect to session bus";
        if (!handle_dbus_error(&err, msg)) {
            fprintf(stderr, "%s\n", msg);
        }
        return 1;
    }

    int has_owner = dbus_bus_name_has_owner(dbus_conn, BUS_NAME, &err);
    if (handle_dbus_error(&err, "NameHasOwner failed")) {
        return 1;
    }

    if (has_owner) {
        /* Call OpenUri on the existing owner and exit */
        DBusMessage *msg = dbus_message_new_method_call(BUS_NAME, OBJ_PATH, IFACE_PLAYER, "OpenUri");
        if (!msg) {
            fprintf(stderr, "Failed to create DBus message\n");
            return 1;
        }
        DBusMessageIter it;
        dbus_message_iter_init_append(msg, &it);
        const char *s = uri;
        if (!dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &s)) {
            fprintf(stderr, "Failed to append argument\n");
            return 1;
        }
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(dbus_conn, msg, -1, &err);
        dbus_message_unref(msg);
        if (!reply) {
            const char *msg = "OpenUri failed";
            if (handle_dbus_error(&err, msg)) {
                fprintf(stderr, "%s\n", msg);
            }
            return 1;
        }
        dbus_message_unref(reply);
    } else {
        int ret = dbus_bus_request_name(dbus_conn, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
        if (handle_dbus_error(&err, "RequestName failed")) {
            return 1;
        }
        if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
            fprintf(stderr, "Could not become primary owner\n");
            return 1;
        }

        __pid_t pid = fork();
        switch (pid) {
            case -1:;
                fprintf(stderr, "Failed to fork\n");
                return 1;
            case 0:;
                audio_t *audio = initaudio();
                if (audio == NULL)
                    return 1;
                openuri(uri, &ffmpegparams);
                set_playing();

                AVFrame *frm = av_frame_alloc();
                AVPacket *pkt = av_packet_alloc();
                // TODO: log when playback started
                while (1) {
                    if (!dbus_connection_read_write(dbus_conn, 0)) {
                        fprintf(stderr, "DBus connection closed");
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
                            notify_properties_changed(dbus_conn, (__add_elements_t)add_metadata_dict_entry, metadata);
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
                    } else if (read_result == AVERROR(EOF)) {
                        avcodec_free_context(&ffmpegparams.cc);
                        avformat_close_input(&ffmpegparams.fmt);
                        set_stopped();
                    } else {
                        fprintf(stderr, "Unexpected stream error!");
                        set_stopped();
                    }
                }
                // TODO: log an error if one occured, log when playback finished

                finishaudio(audio);
                swr_free(&ffmpegparams.swr);
                av_frame_free(&frm);
                av_packet_free(&pkt);

            default:
                return 0;
        }
    }
}
