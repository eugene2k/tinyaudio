#include "dbus/dbus-shared.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <dbus/dbus-protocol.h>
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

#define BUS_NAME "org.mpris.MediaPlayer2.tinyaudio"
#define IFACE_ROOT "org.mpris.MediaPlayer2"
#define IFACE_PLAYER "org.mpris.MediaPlayer2.Player"
#define OBJ_PATH "/org/mpris/MediaPlayer2"

#define SAMPLE_RATE 44100
#define CHANNELS 2

#define XML_DATA                                                                                                       \
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\
         \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\
        <node name=\"/com/example/sample_object0\">\
          <interface name=\"com.example.SampleInterface0\">\
            <method name=\"Frobate\">\
              <arg name=\"foo\" type=\"i\" direction=\"in\"/>\
              <arg name=\"bar\" type=\"s\" direction=\"out\"/>\
              <arg name=\"baz\" type=\"a{us}\" direction=\"out\"/>\
              <annotation name=\"org.freedesktop.DBus.Deprecated\" value=\"true\"/>\
            </method>\
            <method name=\"Bazify\">\
              <arg name=\"bar\" type=\"(iiu)\" direction=\"in\"/>\
              <arg name=\"bar\" type=\"v\" direction=\"out\"/>\
            </method>\
            <method name=\"Mogrify\">\
              <arg name=\"bar\" type=\"(iiav)\" direction=\"in\"/>\
            </method>\
            <signal name=\"Changed\">\
              <arg name=\"new_value\" type=\"b\"/>\
            </signal>\
            <property name=\"Bar\" type=\"y\" access=\"readwrite\"/>\
          </interface>\
          <node name=\"child_of_sample_object\"/>\
          <node name=\"another_child_of_sample_object\"/>\
       </node>"

typedef struct {
    AVFormatContext *fmt;
    int astream;
    AVCodecContext *cc;
    SwrContext *swr;
} ffmpegparams_t;

enum status_t { PLAYING, PAUSED, STOPPED, QUITTING };

enum status_t status = STOPPED;
char *uri = NULL;
int64_t position = 0;
ffmpegparams_t ffmpegparams;

typedef pa_simple audio_t;

const char *tag2xesim(const char *tagname) {
    static const char *tagmap[][2] = {
        {"album", "xesim:album"},       {"album_artist", "xesim:albumArtist"},
        {"artist", "xesim:artist"},     {"comment", "xesim:comment"},
        {"composer", "xesim:composer"}, {"date", "xesim:contentCreated"},
        {"disc", "xesim:discNumber"},   {"genre", "xesim:genre"},
        {"title", "xesim:title"},       {"track", "xesim:trackNumber"},
        {"url", "xesim:url"},
    };
    int first = 0;
    int last = 10;
    int index = -1;
    while (first < last) {
        int cmpresult = strcmp(tagname, tagmap[first][0]);
        if (cmpresult < 0) {
            return NULL;
        } else if (cmpresult == 0) {
            index = first;
            break;
        } else {
            int cmpresult = strcmp(tagname, tagmap[last][0]);
            if (cmpresult > 0) {
                return NULL;
            } else if (cmpresult == 0) {
                index = last;
                break;
            } else {
                if (first + 1 == last)
                    return NULL;
                int mid = (first + last) / 2;
                int cmpresult = strcmp(tagname, tagmap[mid][0]);
                if (cmpresult > 0) {
                    first = mid + 1;
                    last -= 1;
                } else if (cmpresult == 0) {
                    index = mid;
                    break;
                } else {
                    last = mid - 1;
                    first += 1;
                }
            }
        }
    }
    if (index != -1) {
        if (tagmap[index][1])
            return tagmap[index][1];
        else
            return tagname;
    } else
        return NULL;
}

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

// dbus_bool_t add_variant(DBusMessageIter *iter, int type, void *value) {
//     DBusMessageIter sub;
//     char typestr[2];
//     typestr[0] = type;
//     typestr[1] = 0;
//     if (dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, typestr, &sub)) {
//         dbus_message_iter_append_basic(&sub, type, value);
//         dbus_message_iter_close_container(iter, &sub);
//         return TRUE;
//     }
//     return FALSE;
// }

dbus_bool_t add_dict_entry(DBusMessageIter *iter, const char *key, int type, const void *value) {
    DBusMessageIter sub[2];
    char typestr[2];
    typestr[0] = type;
    typestr[1] = 0;
    if (dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &sub[0])) {
        dbus_message_iter_append_basic(&sub[0], DBUS_TYPE_STRING, &key);
        if (dbus_message_iter_open_container(&sub[0], DBUS_TYPE_VARIANT, typestr, &sub[1])) {
            dbus_message_iter_append_basic(&sub[1], type, value);
            dbus_message_iter_close_container(&sub[0], &sub[1]);
            dbus_message_iter_close_container(iter, &sub[0]);
            return TRUE;
        } else {
            dbus_message_iter_abandon_container(iter, &sub[0]);
        }
    }
    return FALSE;
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
        status = PLAYING;
        reply = dbus_message_new_method_return(msg);
    } else if (dbus_message_is_method_call(msg, IFACE_PLAYER, "Play")) {
        switch (status) {
            case PAUSED:
                av_read_play(ffmpegparams->fmt);
                status = PLAYING;
                break;
            case STOPPED:
                if (uri != NULL && !openuri(uri, ffmpegparams))
                    status = PLAYING;
                break;
            default:
                break;
        }
        reply = dbus_message_new_method_return(msg);
    } else if (dbus_message_is_method_call(msg, IFACE_PLAYER, "Pause")) {
        if (status == PLAYING) {
            av_read_pause(ffmpegparams->fmt);
            status = PAUSED;
        }
        reply = dbus_message_new_method_return(msg);
    } else if (dbus_message_is_method_call(msg, IFACE_PLAYER, "PlayPause")) {
        switch (status) {
            case PLAYING:
                av_read_pause(ffmpegparams->fmt);
                status = PAUSED;
                break;
            case PAUSED:
                av_read_play(ffmpegparams->fmt);
                status = PLAYING;
                break;
            case STOPPED:
                if (uri != NULL && !openuri(uri, ffmpegparams))
                    status = PLAYING;
                break;
            default:
                break;
        }
        reply = dbus_message_new_method_return(msg);
    } else if (dbus_message_is_method_call(msg, IFACE_PLAYER, "Stop")) {
        if (status != STOPPED) {
            avcodec_free_context(&ffmpegparams->cc);
            avformat_close_input(&ffmpegparams->fmt);
            status = STOPPED;
        }
        reply = dbus_message_new_method_return(msg);
    } else if (dbus_message_is_method_call(msg, IFACE_ROOT, "Quit")) {
        status = QUITTING;
        reply = dbus_message_new_method_return(msg);
    } else if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "Get")) {
        DBusError error;
        char *interface, *property;
        dbus_error_init(&error);
        if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property,
                                   DBUS_TYPE_INVALID)) {
            fprintf(stderr, "Error handling properties: %s\n", error.message);
        }
        dbus_error_free(&error);
        if (!interface || !property) {
            reply = dbus_message_new_error(msg, "org.mpris.MediaPlayer2.tinyaudio.Error",
                                           "Expected interface and property arguments");
        } else {
            reply = dbus_message_new_method_return(msg);
        }
        if (strcmp(interface, IFACE_ROOT) == 0) {
            if (strcmp(property, "CanQuit") == 0) {
                dbus_bool_t val = 0;
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "FullScreen") == 0) {
                dbus_bool_t val = 0;
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "CanSetFullscreen") == 0) {
                dbus_bool_t val = 0;
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "CanRaise") == 0) {
                dbus_bool_t val = 0;
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "HasTrackList") == 0) {
                dbus_bool_t val = 0;
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "Identity") == 0) {
                const char *val = "tinyaudio";
                dbus_message_append_args(reply, DBUS_TYPE_STRING, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "DesktopEntry") == 0) {
                const char *val = "tinyaudio";
                dbus_message_append_args(reply, DBUS_TYPE_STRING, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "SupportedUriSchemes") == 0) {
                DBusMessageIter iter, subiter;
                dbus_message_iter_init_append(reply, &iter);
                if (dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &subiter)) {
                    void *protoiter = NULL;
                    const char *proto = avio_enum_protocols(&protoiter, 0);
                    while (proto != NULL) {
                        if (!dbus_message_iter_append_basic(&subiter, DBUS_TYPE_STRING, &proto)) {
                            dbus_message_iter_abandon_container(&iter, &subiter);
                            goto abort1;
                        }
                        proto = avio_enum_protocols(&protoiter, 0);
                    };
                    dbus_message_iter_close_container(&iter, &subiter);
                abort1:;
                }
            } else if (strcmp(property, "SupportedMimeTypes") == 0) {
                DBusMessageIter iter, subiter;
                dbus_message_iter_init_append(reply, &iter);
                if (dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &subiter)) {
                    for (const AVCodecDescriptor *desc = NULL; desc != NULL; desc = avcodec_descriptor_next(desc)) {
                        for (int i = 0; desc->mime_types[i] != NULL; i++) {
                            // NOTE: if there are cases where two codecs support the same mimetype, then this needs
                            // to use a hash map
                            if (!dbus_message_iter_append_basic(&subiter, DBUS_TYPE_STRING, &desc->mime_types[i])) {
                                dbus_message_iter_abandon_container(&iter, &subiter);
                                goto abort2;
                            }
                        }
                    }
                    dbus_message_iter_close_container(&iter, &subiter);
                abort2:;
                }
            } else {
                reply = dbus_message_new_error(msg, "org.freedesktop.DBus.Properties.Get.Error", "No such property");
            }
        } else if (strcmp(interface, IFACE_PLAYER) == 0) {
            if (strcmp(property, "PlaybackStatus") == 0) {
                char *statusstr = NULL;
                switch (status) {
                    case PLAYING:
                        statusstr = "Playing";
                        break;
                    case PAUSED:
                        statusstr = "Paused";
                        break;
                    case STOPPED:
                        statusstr = "Stopped";
                        break;
                    case QUITTING:
                        break;
                }
                dbus_message_append_args(reply, DBUS_TYPE_STRING, &statusstr, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "LoopStatus") == 0) {
                const char *val = "None";
                dbus_message_append_args(reply, DBUS_TYPE_STRING, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "Rate") == 0) {
                double val = 1.0;
                dbus_message_append_args(reply, DBUS_TYPE_DOUBLE, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "Shuffle") == 0) {
                dbus_bool_t val = 0;
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "Metadata") == 0) {
                DBusMessageIter iter, sub;
                dbus_message_iter_init_append(reply, &iter);
                if (dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub)) {
                    const char *trackid = "mpris:trackId";
                    const char *path = "/nil";

                    if (!add_dict_entry(&sub, trackid, DBUS_TYPE_OBJECT_PATH, &path)) {
                        dbus_message_iter_abandon_container(&iter, &sub);
                    }

                    const AVDictionaryEntry *tag = NULL;
                    while ((tag = av_dict_iterate(ffmpegparams->fmt->metadata, tag))) {
                        const char *key = NULL;
                        if (strcmp(tag->key, "StreamTitle") == 0) {
                            key = "xesim:title";
                        } else if (strcmp(tag->key, "icy-genre") == 0) {
                            key = "xesim:genre";
                        } else {
                            key = tag2xesim(tag->key);
                        };
                        if (key) {
                            add_dict_entry(&sub, key, DBUS_TYPE_STRING, &tag->value);
                        }
                    }
                    while ((tag = av_dict_iterate(ffmpegparams->fmt->streams[ffmpegparams->astream]->metadata, tag))) {
                        const char *key = tag2xesim(tag->key);
                        add_dict_entry(&sub, key, DBUS_TYPE_STRING, &tag->value);
                    }
                    dbus_message_iter_close_container(&iter, &sub);
                }
            } else if (strcmp(property, "Volume") == 0) {
                double val = 1.0;
                dbus_message_append_args(reply, DBUS_TYPE_DOUBLE, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "Position") == 0) {
                dbus_message_append_args(reply, DBUS_TYPE_INT64, &position, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "MinimumRate") == 0) {
                double val = 1.0;
                dbus_message_append_args(reply, DBUS_TYPE_DOUBLE, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "MaximumRate") == 0) {
                double val = 1.0;
                dbus_message_append_args(reply, DBUS_TYPE_DOUBLE, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "CanGoNext") == 0) {
                dbus_bool_t val = 0;
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "CanGoPrevious") == 0) {
                dbus_bool_t val = 0;
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "CanPlay") == 0) {
                dbus_bool_t val = 0;
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "CanPause") == 0) {
                dbus_bool_t val = 0;
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "CanSeek") == 0) {
                dbus_bool_t val = 0;
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
            } else if (strcmp(property, "CanControl") == 0) {
                dbus_bool_t val = 1;
                dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
            } else {
                dbus_message_unref(reply);
                reply = dbus_message_new_error(msg, "org.freedesktop.Properties.Get.Error", "No such property");
            }
        } else {
            dbus_message_unref(reply);
            reply = dbus_message_new_error(msg, "org.freedesktop.Properties.Get.Error", "No such interface");
        }
    } else if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "Set")) {
        char *interface = NULL, *property = NULL;
        DBusMessageIter iter;
        dbus_message_iter_init(msg, &iter);
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&iter, interface);
            if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&iter, property);
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
                        reply = dbus_message_new_error(msg, "org.freedesktop.DBus.Properties.Set.Error",
                                                       "No such property");
                    }
                } else {
                    reply =
                        dbus_message_new_error(msg, "org.freedesktop.DBus.Properties.Set.Error", "No such interface");
                }
            }
        }
    } else if (dbus_message_is_method_call(msg, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
        reply = dbus_message_new_method_return(msg);
        const char *val = XML_DATA;
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &val, DBUS_TYPE_INVALID);
    } else {
        reply = dbus_message_new_error(msg, "org.mpris.MediaPlayer2.tinyaudio.Error", "Invalid interface or method");
    }
    if (reply) {
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        dbus_connection_flush(conn);
    }
}

static void dbus_fatal(DBusError *err, const char *msg) {
    if (dbus_error_is_set(err)) {
        fprintf(stderr, "%s: %s\n", msg, err->message);
        dbus_error_free(err);
    } else {
        fprintf(stderr, "%s\n", msg);
    }
    exit(1);
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
    if (!dbus_conn)
        dbus_fatal(&err, "Failed to connect to session bus");

    int has_owner = dbus_bus_name_has_owner(dbus_conn, BUS_NAME, &err);
    if (dbus_error_is_set(&err))
        dbus_fatal(&err, "NameHasOwner failed");

    if (has_owner) {
        /* Call OpenUri on the existing owner and exit */
        DBusMessage *msg = dbus_message_new_method_call(BUS_NAME, OBJ_PATH, IFACE_PLAYER, "OpenUri");
        if (!msg) {
            fprintf(stderr, "Failed to create DBus message\n");
            exit(1);
        }
        DBusMessageIter it;
        dbus_message_iter_init_append(msg, &it);
        const char *s = uri;
        if (!dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &s)) {
            fprintf(stderr, "Failed to append argument\n");
            exit(1);
        }
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(dbus_conn, msg, -1, &err);
        dbus_message_unref(msg);
        if (!reply)
            dbus_fatal(&err, "OpenUri call failed");
        dbus_message_unref(reply);
    } else {
        int ret = dbus_bus_request_name(dbus_conn, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
        if (dbus_error_is_set(&err))
            dbus_fatal(&err, "RequestName failed");
        if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
            fprintf(stderr, "Could not become primary owner\n");
            exit(1);
        }

        __pid_t pid = fork();
        switch (pid) {
            case -1:
                fprintf(stderr, "Failed to fork\n");
                return 1;
            case 0:
                audio_t *audio = initaudio();
                if (audio == NULL)
                    return 1;
                openuri(uri, &ffmpegparams);
                status = PLAYING;

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
                    } else if (read_result == AVERROR(EOF)) {
                        avcodec_free_context(&ffmpegparams.cc);
                        avformat_close_input(&ffmpegparams.fmt);
                        status = STOPPED;
                    } else {
                        fprintf(stderr, "Unexpected stream error!");
                    }
                    av_packet_unref(pkt);
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
