/*
 * Copyright © 2013-2017  Rinat Ibragimov
 *
 * This file is part of FreshPlayerPlugin.
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

#include "config.h"
#include "ppb_instance.h"
#include "screensaver_control.h"
#include "tables.h"
#include "trace_core.h"
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <fcntl.h>
#include <glib.h>
#include <pango/pangoft2.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <va/va_x11.h>

NPNetscapeFuncs     npn;
struct display_s    display;

static GHashTable  *pp_to_np_ht;
static GHashTable  *npobj_to_npp_ht = NULL;     // NPObject-to-NPP mapping

static PangoContext *pango_ctx = NULL;
static PangoFontMap *pango_fm = NULL;

static pthread_mutex_t  lock;
static int urandom_fd = -1;

static
void
__attribute__((constructor))
constructor_tables(void)
{
    // hash tables
    pp_to_np_ht =       g_hash_table_new(g_direct_hash, g_direct_equal);
    npobj_to_npp_ht =   g_hash_table_new(g_direct_hash, g_direct_equal);

    // pango
    pango_fm = pango_ft2_font_map_new();
    pango_ctx = pango_font_map_create_context(pango_fm);

    // mutex
    pthread_mutex_init(&lock, NULL);

    // urandom
    urandom_fd = open("/dev/urandom", O_RDONLY);
    srand(time(NULL) + 42);
}

static
void
__attribute__((destructor))
destructor_tables(void)
{
    // hash tables
    g_hash_table_unref(pp_to_np_ht);
    g_hash_table_unref(npobj_to_npp_ht);

    // pango
    g_object_unref(pango_ctx);
    g_object_unref(pango_fm);
    pango_ctx = NULL;
    pango_fm = NULL;

    // mutex
    pthread_mutex_destroy(&lock);

    // urandom
    close(urandom_fd);
}

int
tables_get_urandom_fd(void)
{
    return urandom_fd;
}

struct pp_instance_s *
tables_get_pp_instance(PP_Instance instance)
{
    pthread_mutex_lock(&lock);
    struct pp_instance_s *pp_i = g_hash_table_lookup(pp_to_np_ht, GINT_TO_POINTER(instance));
    pthread_mutex_unlock(&lock);
    return pp_i;
}

void
tables_add_pp_instance(PP_Instance instance, struct pp_instance_s *pp_i)
{
    pthread_mutex_lock(&lock);
    g_hash_table_replace(pp_to_np_ht, GINT_TO_POINTER(instance), pp_i);
    pthread_mutex_unlock(&lock);
}

void
tables_remove_pp_instance(PP_Instance instance)
{
    pthread_mutex_lock(&lock);
    g_hash_table_remove(pp_to_np_ht, GINT_TO_POINTER(instance));
    pthread_mutex_unlock(&lock);
}

struct pp_instance_s *
tables_get_some_pp_instance(void)
{
    GHashTableIter iter;
    gpointer key, value;
    gpointer result = NULL;

    pthread_mutex_lock(&lock);
    g_hash_table_iter_init (&iter, pp_to_np_ht);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        struct pp_instance_s *pp_i = value;
        if (pp_i && pp_i->npp)
            result = value;
    }
    pthread_mutex_unlock(&lock);

    return result;
}

PangoContext *
tables_get_pango_ctx(void)
{
    return pango_ctx;
}

PangoFontMap *
tables_get_pango_font_map(void)
{
    return pango_fm;
}

void
tables_add_npobj_npp_mapping(NPObject *npobj, NPP npp)
{
    pthread_mutex_lock(&lock);
    g_hash_table_insert(npobj_to_npp_ht, npobj, npp);
    pthread_mutex_unlock(&lock);
}

NPP
tables_get_npobj_npp_mapping(NPObject *npobj)
{
    pthread_mutex_lock(&lock);
    NPP npp = g_hash_table_lookup(npobj_to_npp_ht, npobj);
    pthread_mutex_unlock(&lock);
    return npp;
}

void
tables_remove_npobj_npp_mapping(NPObject *npobj)
{
    pthread_mutex_lock(&lock);
    g_hash_table_remove(npobj_to_npp_ht, npobj);
    pthread_mutex_unlock(&lock);
}

static
void
check_glx_extensions(void)
{
    const char *glx_ext_str = glXQueryExtensionsString(display.x, 0);
    if (!glx_ext_str)
        return;

    display.glx_arb_create_context = !!strstr(glx_ext_str, "GLX_ARB_create_context");
    display.glx_arb_create_context_profile = !!strstr(glx_ext_str,
                                                      "GLX_ARB_create_context_profile");
    display.glx_ext_create_context_es2_profile = !!strstr(glx_ext_str,
                                                          "GLX_EXT_create_context_es2_profile");
    display.glXCreateContextAttribsARB = (glx_create_context_attribs_arb_f)
        glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
    display.glXBindTexImageEXT = (glx_bind_tex_image_ext_f)
        glXGetProcAddress((GLubyte *)"glXBindTexImageEXT");
    display.glXReleaseTexImageEXT = (glx_release_tex_image_ext_f)
        glXGetProcAddress((GLubyte *)"glXReleaseTexImageEXT");
    display.glXGetVideoSyncSGI = (glx_get_video_sync_sgi_f)
        glXGetProcAddress((GLubyte *)"glXGetVideoSyncSGI");
    display.glXWaitVideoSyncSGI = (glx_wait_video_sync_sgi_f)
        glXGetProcAddress((GLubyte *)"glXWaitVideoSyncSGI");
}

#if HAVE_HWDEC
static
void *
get_proc_helper(VdpFuncId func_id)
{
    void *func = NULL;
    if (display.vdp_get_proc_address(display.vdp_device, func_id, &func) != VDP_STATUS_OK) {
        trace_error("%s, can't get VDPAU function %d address\n", __func__, (int)func_id);
        func = NULL;
    }

    return func;
}

static
void
initialize_vaapi(void)
{
    VAStatus st;
    int major, minor;

    display.va = vaGetDisplay(display.x);
    st = vaInitialize(display.va, &major, &minor);
    if (st == VA_STATUS_SUCCESS) {
        trace_info_f("libva version %d.%d\n", major, minor);
        trace_info_f("libva driver vendor: %s\n", vaQueryVendorString(display.va));
        display.va_available = 1;
    } else {
        trace_info_f("%s, failed to initialize VA device, %d, %s\n", __func__, (int)st,
                     vaErrorStr(st));
        trace_info_f("%s, no VA-API available\n", __func__);
    }
}

static
void
deinitialize_vaapi(void)
{
    if (display.va)
        vaTerminate(display.va);
    display.va = NULL;
}

static
void
initialize_vdpau(void)
{
    VdpStatus st;

    display.vdp_device = VDP_INVALID_HANDLE;
    st = vdp_device_create_x11(display.x, DefaultScreen(display.x), &display.vdp_device,
                               &display.vdp_get_proc_address);

    if (st == VDP_STATUS_OK && display.vdp_get_proc_address) {
        display.vdp_get_error_string = get_proc_helper(VDP_FUNC_ID_GET_ERROR_STRING);
        display.vdp_get_information_string = get_proc_helper(VDP_FUNC_ID_GET_INFORMATION_STRING);
        display.vdp_device_destroy = get_proc_helper(VDP_FUNC_ID_DEVICE_DESTROY);
        display.vdp_decoder_create = get_proc_helper(VDP_FUNC_ID_DECODER_CREATE);
        display.vdp_decoder_destroy = get_proc_helper(VDP_FUNC_ID_DECODER_DESTROY);
        display.vdp_decoder_render = get_proc_helper(VDP_FUNC_ID_DECODER_RENDER);
        display.vdp_video_surface_create = get_proc_helper(VDP_FUNC_ID_VIDEO_SURFACE_CREATE);
        display.vdp_video_surface_destroy = get_proc_helper(VDP_FUNC_ID_VIDEO_SURFACE_DESTROY);
        display.vdp_presentation_queue_target_create_x11 =
                                get_proc_helper(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_CREATE_X11);
        display.vdp_presentation_queue_target_destroy =
                                    get_proc_helper(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_DESTROY);
        display.vdp_presentation_queue_create =
                                            get_proc_helper(VDP_FUNC_ID_PRESENTATION_QUEUE_CREATE);
        display.vdp_presentation_queue_destroy =
                                            get_proc_helper(VDP_FUNC_ID_PRESENTATION_QUEUE_DESTROY);
        display.vdp_presentation_queue_display =
                                            get_proc_helper(VDP_FUNC_ID_PRESENTATION_QUEUE_DISPLAY);
        display.vdp_output_surface_create = get_proc_helper(VDP_FUNC_ID_OUTPUT_SURFACE_CREATE);
        display.vdp_output_surface_destroy = get_proc_helper(VDP_FUNC_ID_OUTPUT_SURFACE_DESTROY);
        display.vdp_video_mixer_create = get_proc_helper(VDP_FUNC_ID_VIDEO_MIXER_CREATE);
        display.vdp_video_mixer_destroy = get_proc_helper(VDP_FUNC_ID_VIDEO_MIXER_DESTROY);
        display.vdp_video_mixer_render = get_proc_helper(VDP_FUNC_ID_VIDEO_MIXER_RENDER);

        if (display.vdp_get_error_string && display.vdp_get_information_string &&
            display.vdp_device_destroy && display.vdp_decoder_create &&
            display.vdp_decoder_destroy && display.vdp_decoder_render &&
            display.vdp_video_surface_create && display.vdp_video_surface_destroy &&
            display.vdp_presentation_queue_target_create_x11 &&
            display.vdp_presentation_queue_target_destroy &&
            display.vdp_presentation_queue_create && display.vdp_presentation_queue_destroy &&
            display.vdp_presentation_queue_display && display.vdp_output_surface_create &&
            display.vdp_output_surface_destroy && display.vdp_video_mixer_create &&
            display.vdp_video_mixer_destroy && display.vdp_video_mixer_render)
        {
            display.vdpau_available = 1;

            const char *info_str;
            if (display.vdp_get_information_string(&info_str) == VDP_STATUS_OK)
                trace_info_f("VDPAU driver: %s\n", info_str);
            else
                trace_error("%s, failed to get VDPAU driver version\n", __func__);

        } else {
            trace_error("%s, some essential VDPAU functions missing\n", __func__);
        }

     } else {
         trace_info_f("%s, failed to initialize VDPAU device, no VDPAU available\n", __func__);
     }
}

static
void
deinitialize_vdpau(void)
{
    if (display.vdp_device_destroy && display.vdp_device != VDP_INVALID_HANDLE) {
        display.vdp_device_destroy(display.vdp_device);
        display.vdp_device = VDP_INVALID_HANDLE;
    }
}
#endif // HAVE_HWDEC

int
tables_open_display(void)
{
    int retval = 0;
    int major, minor;

    pthread_mutexattr_init(&display.mutex_attr_recursive);
    pthread_mutexattr_settype(&display.mutex_attr_recursive, PTHREAD_MUTEX_RECURSIVE);

    pthread_mutex_init(&display.lock, &display.mutex_attr_recursive);
    pthread_mutex_lock(&display.lock);
    display.x = XOpenDisplay(NULL);
    if (!display.x) {
        trace_error("%s, can't open X Display\n", __func__);
        retval = 1;
        goto quit;
    }

    if (config.quirks.x_synchronize)
        XSynchronize(display.x, True);

    display.dri_fd = open("/dev/dri/card0", O_RDWR);

#if HAVE_HWDEC

    display.va_available = 0;
    display.vdpau_available = 0;

    if (config.enable_hwdec) {
        if (config.enable_vaapi)
            initialize_vaapi();

        if (config.enable_vdpau)
            initialize_vdpau();
    }

#endif // HAVE_HWDEC

    if (!glXQueryVersion(display.x, &major, &minor)) {
        trace_error("%s, glXQueryVersion returned False\n", __func__);
    } else {
        trace_info_f("GLX version %d.%d\n", major, minor);
    }

    check_glx_extensions();

    // initialize screensaver inhibition library
    screensaver_connect();
    display.screensaver_types = screensaver_type_detect(display.x);

    gchar *s = g_strdup_printf("screensavers found:%s%s%s%s%s",
        (display.screensaver_types & SST_XSCREENSAVER) ? " XScreenSaver" : "",
        (display.screensaver_types & SST_FDO_SCREENSAVER) ? " fd.o-screensaver" : "",
        (display.screensaver_types & SST_CINNAMON_SCREENSAVER) ? " cinnamon-screensaver" : "",
        (display.screensaver_types & SST_GNOME_SCREENSAVER) ? " gnome-screensaver" : "",
        (display.screensaver_types & SST_KDE_SCREENSAVER) ? " kscreensaver" : "");
    trace_info_f("%s\n", s);
    g_free(s);

    // create transparent cursor
    const char t_pixmap_data = 0;
    XColor t_color = {};
    Pixmap t_pixmap = XCreateBitmapFromData(display.x, DefaultRootWindow(display.x),
                                            &t_pixmap_data, 1, 1);
    display.transparent_cursor = XCreatePixmapCursor(display.x, t_pixmap, t_pixmap, &t_color,
                                                     &t_color, 0, 0);
    XFreePixmap(display.x, t_pixmap);

    // determine minimal size across all screens
    display.min_width = (uint32_t)-1;
    display.min_height = (uint32_t)-1;
    XRRScreenResources *sr = XRRGetScreenResources(display.x, DefaultRootWindow(display.x));
    if (sr) {
        for (int k = 0; k < sr->ncrtc; k ++) {
            XRRCrtcInfo *ci = XRRGetCrtcInfo(display.x, sr, sr->crtcs[k]);

            if (ci && ci->width > 0 && ci->height > 0) {
                display.min_width = MIN(display.min_width, ci->width);
                display.min_height = MIN(display.min_height, ci->height);
            }

            if (ci)
                XRRFreeCrtcInfo(ci);
        }
        XRRFreeScreenResources(sr);
    }

    if (display.min_width == (uint32_t)-1 || display.min_height == (uint32_t)-1) {
        display.min_width = 300;
        display.min_height = 300;
    }

    // apply size override from the configuration file
    if (config.fullscreen_width > 0)
        display.min_width = config.fullscreen_width;
    if (config.fullscreen_height > 0)
        display.min_height = config.fullscreen_height;

    int xrender_event_base, xrender_error_base;
    if (XRenderQueryExtension(display.x, &xrender_event_base, &xrender_error_base)) {
        trace_info_f("found XRender\n");
        display.have_xrender = 1;
    } else {
        trace_info_f("no XRender available\n");
        display.have_xrender = 0;
    }

    if (!config.enable_xrender) {
        trace_info_f("XRender is disabled\n");
        display.have_xrender = 0;
    }

    if (display.have_xrender) {
        display.pictfmt_rgb24 = XRenderFindStandardFormat(display.x, PictStandardRGB24);
        display.pictfmt_argb32 = XRenderFindStandardFormat(display.x, PictStandardARGB32);
    }

quit:
    pthread_mutex_unlock(&display.lock);
    return retval;
}

void
tables_close_display(void)
{
    pthread_mutex_lock(&display.lock);
    screensaver_disconnect();

#if HAVE_HWDEC

    if (config.enable_hwdec) {
        if (config.enable_vaapi)
            deinitialize_vaapi();

        if (config.enable_vdpau)
            deinitialize_vdpau();
    }

#endif // HAVE_HWDEC

    close(display.dri_fd);
    display.dri_fd = -1;

    XFreeCursor(display.x, display.transparent_cursor);
    XCloseDisplay(display.x);
    pthread_mutex_unlock(&display.lock);
    pthread_mutex_destroy(&display.lock);
    pthread_mutexattr_destroy(&display.mutex_attr_recursive);
}

PP_Instance
tables_generate_new_pp_instance_id(void)
{
    static int32_t instance_id = 10;

    pthread_mutex_lock(&lock);
    int32_t result = instance_id++;
    pthread_mutex_unlock(&lock);

    return result;
}
