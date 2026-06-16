/*
 * rover-stream : emetteur video H264 -> RTP -> UDP
 * Construit le pipeline element par element via l'API GStreamer.
 *
 * Equivalent CLI :
 *   gst-launch-1.0 videotestsrc ! video/x-raw,width=640,height=480,framerate=30/1 \
 *     ! videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=1000 \
 *     ! rtph264pay ! udpsink host=127.0.0.1 port=5000
 */
#include <gst/gst.h>

#define DEST_HOST "127.0.0.1"   /* IP du recepteur (ton PC) */
#define DEST_PORT 5000

/* Appelee a chaque message poste sur le bus du pipeline */
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
    return TRUE;  /* TRUE = on reste abonne au bus */
}

int main(int argc, char *argv[])
{
    /* 1. Initialisation de la bibliotheque */
    gst_init(&argc, &argv);

    /* 2. Creation des elements (un .so plugin par element) */
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

    /* 3. Configuration des proprietes */

    /* 3a. Format impose en sortie de la source (les "caps") */
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "width",     G_TYPE_INT,        640,
        "height",    G_TYPE_INT,        480,
        "framerate", GST_TYPE_FRACTION, 30, 1,
        NULL);
    g_object_set(capsfilt, "caps", caps, NULL);
    gst_caps_unref(caps);

    /* 3b. Encodeur regle pour la faible latence */
    g_object_set(encoder,
        "tune",         0x00000004,  /* zerolatency (flag) */
        "speed-preset", 1,           /* ultrafast (enum)   */
        "bitrate",      1000,        /* kbit/s             */
        NULL);

    /* 3c. Destination UDP */
    g_object_set(sink,
        "host", DEST_HOST,
        "port", DEST_PORT,
        NULL);

    /* 4. Ajout de tous les elements dans le pipeline (le conteneur) */
    gst_bin_add_many(GST_BIN(pipeline),
        source, capsfilt, convert, encoder, payloader, sink, NULL);

    /* 5. Liaison dans l'ordre du flux : src ! caps ! convert ! enc ! pay ! sink */
    if (!gst_element_link_many(source, capsfilt, convert,
                               encoder, payloader, sink, NULL)) {
        g_printerr("Echec de la liaison des elements (caps incompatibles ?).\n");
        gst_object_unref(pipeline);
        return -1;
    }

    /* 6. Surveillance du bus : on branche notre callback */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, loop);
    gst_object_unref(bus);

    /* 7. Demarrage : on passe le pipeline en lecture */
    g_print("Streaming vers %s:%d ... (Ctrl+C pour arreter)\n",
            DEST_HOST, DEST_PORT);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    /* 8. Boucle principale : bloque ici jusqu'a une erreur ou un EOS */
    g_main_loop_run(loop);

    /* 9. Nettoyage */
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    return 0;
}
