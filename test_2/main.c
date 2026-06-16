/*
 * rover-stream : emetteur video H264 -> RTP -> UDP
 * + pupitre de controle dynamique au clavier.
 *
 * Touches (suivies de Entree) :
 *   p  pause / reprise du flux
 *   +  bitrate +250 kbit/s
 *   -  bitrate -250 kbit/s
 *   i  affiche etat + bitrate courants
 *   q  arret propre
 */
#include <gst/gst.h>

#define DEST_HOST "127.0.0.1"   /* IP du recepteur (ton PC) */
#define DEST_PORT 5000

/* Contexte partage entre les callbacks (bus + clavier) */
typedef struct {
    GstElement *pipeline;
    GstElement *encoder;
    GMainLoop  *loop;
} AppData;

/* ---- Callback du bus GStreamer (erreurs / fin de flux) ---- */
static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data)
{
    GMainLoop *loop = (GMainLoop *)data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar  *debug = NULL;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("ERREUR [%s] : %s\n",
                       GST_OBJECT_NAME(msg->src), err->message);
            if (debug)
                g_printerr("  debug : %s\n", debug);
            g_clear_error(&err);
            g_free(debug);
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("Fin du flux (EOS)\n");
            g_main_loop_quit(loop);
            break;
        default:
            break;
    }
    return TRUE;
}

/* ---- Callback clavier : appele par la boucle GLib a chaque ligne sur stdin ---- */
static gboolean clavier_callback(GIOChannel *source, GIOCondition cond, gpointer data)
{
    AppData *app = (AppData *)data;
    gchar  *ligne = NULL;
    gsize   len = 0;

    GIOStatus st = g_io_channel_read_line(source, &ligne, &len, NULL, NULL);

    if (st == G_IO_STATUS_EOF) {        /* Ctrl+D */
        g_main_loop_quit(app->loop);
        return FALSE;
    }
    if (st != G_IO_STATUS_NORMAL) {
        g_free(ligne);
        return TRUE;
    }

    char cmd = ligne[0];
    g_free(ligne);

    switch (cmd) {
        case 'p': {  /* pause / reprise : transition d'etat a chaud */
            GstState etat;
            gst_element_get_state(app->pipeline, &etat, NULL, GST_CLOCK_TIME_NONE);
            if (etat == GST_STATE_PLAYING) {
                gst_element_set_state(app->pipeline, GST_STATE_PAUSED);
                g_print(">> PAUSE\n");
            } else {
                gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
                g_print(">> REPRISE\n");
            }
            break;
        }
        case '+': {  /* augmente le bitrate en live */
            guint bitrate;
            g_object_get(app->encoder, "bitrate", &bitrate, NULL);
            bitrate += 250;
            g_object_set(app->encoder, "bitrate", bitrate, NULL);
            g_print(">> bitrate = %u kbit/s\n", bitrate);
            break;
        }
        case '-': {  /* diminue le bitrate en live */
            guint bitrate;
            g_object_get(app->encoder, "bitrate", &bitrate, NULL);
            bitrate = (bitrate > 250) ? bitrate - 250 : 50;
            g_object_set(app->encoder, "bitrate", bitrate, NULL);
            g_print(">> bitrate = %u kbit/s\n", bitrate);
            break;
        }
        case 'i': {  /* interroge l'etat courant */
            GstState etat;
            guint    bitrate;
            gst_element_get_state(app->pipeline, &etat, NULL, GST_CLOCK_TIME_NONE);
            g_object_get(app->encoder, "bitrate", &bitrate, NULL);
            g_print(">> etat=%s  bitrate=%u kbit/s\n",
                    gst_element_state_get_name(etat), bitrate);
            break;
        }
        case 'q':  /* arret propre */
            g_print(">> arret demande\n");
            g_main_loop_quit(app->loop);
            break;
        default:
            break;
    }
    return TRUE;  /* TRUE = rester abonne a stdin */
}

int main(int argc, char *argv[])
{
    /* 1. Initialisation */
    gst_init(&argc, &argv);

    /* 2. Creation des elements */
    GstElement *source    = gst_element_factory_make("videotestsrc", "source");
    GstElement *capsfilt  = gst_element_factory_make("capsfilter",   "capsfilt");
    GstElement *convert   = gst_element_factory_make("videoconvert",  "convert");
    GstElement *encoder   = gst_element_factory_make("x264enc",       "encoder");
    GstElement *payloader = gst_element_factory_make("rtph264pay",    "payloader");
    GstElement *sink      = gst_element_factory_make("udpsink",       "sink");
    GstElement *pipeline  = gst_pipeline_new("rover-stream");

    if (!source || !capsfilt || !convert || !encoder ||
        !payloader || !sink || !pipeline) {
        g_printerr("Echec creation d'un element. Plugin manquant ?\n");
        return -1;
    }

    /* 3. Configuration */
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "width",     G_TYPE_INT,        640,
        "height",    G_TYPE_INT,        480,
        "framerate", GST_TYPE_FRACTION, 30, 1,
        NULL);
    g_object_set(capsfilt, "caps", caps, NULL);
    gst_caps_unref(caps);

    g_object_set(encoder,
        "tune",         0x00000004,  /* zerolatency */
        "speed-preset", 1,           /* ultrafast   */
        "bitrate",      1000,        /* kbit/s      */
        NULL);

    g_object_set(sink, "host", DEST_HOST, "port", DEST_PORT, NULL);

    /* 4. + 5. Ajout et liaison */
    gst_bin_add_many(GST_BIN(pipeline),
        source, capsfilt, convert, encoder, payloader, sink, NULL);

    if (!gst_element_link_many(source, capsfilt, convert,
                               encoder, payloader, sink, NULL)) {
        g_printerr("Echec de la liaison des elements.\n");
        gst_object_unref(pipeline);
        return -1;
    }

    /* 6. Boucle principale + contexte partage */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    AppData app = { pipeline, encoder, loop };

    /* 6a. Watch du bus GStreamer */
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, loop);
    gst_object_unref(bus);

    /* 6b. Watch du clavier : stdin (fd 0) devient une source d'evenement */
    GIOChannel *clavier = g_io_channel_unix_new(0);
    g_io_add_watch(clavier, G_IO_IN, clavier_callback, &app);

    /* 7. Demarrage */
    g_print("Streaming vers %s:%d\n", DEST_HOST, DEST_PORT);
    g_print("Commandes : [p]ause  [+]/[-] bitrate  [i]nfos  [q]uit\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    /* 8. Boucle : bus ET clavier sont servis ici, sans thread en plus */
    g_main_loop_run(loop);

    /* 9. Nettoyage */
    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_io_channel_unref(clavier);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    return 0;
}
