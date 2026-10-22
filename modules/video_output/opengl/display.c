/**
 * @file display.c
 * @brief OpenGL video output module
 */
/*****************************************************************************
 * Copyright © 2010-2011 Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <assert.h>
#include <math.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_opengl.h>
#ifdef HAVE_STEREO_DECLARE
# include <vlc_xlib.h>
# include <stereo-declare.h>
# ifdef VLC_GL_WAYLAND
#  include <wayland-client.h>
#  include "../wayland/viewporter-client-protocol.h"
# endif
#endif
#include "vout_helper.h"

/* Plugin callbacks */
static int Open (vlc_object_t *);
static void Close (vlc_object_t *);

#define GL_TEXT N_("OpenGL extension")
#define GLES2_TEXT N_("OpenGL ES 2 extension")
#define PROVIDER_LONGTEXT N_( \
    "Extension through which to use the Open Graphics Library (OpenGL).")

vlc_module_begin ()
#if defined (USE_OPENGL_ES2)
# define API VLC_OPENGL_ES2
# define MODULE_VARNAME "gles2"
    set_shortname (N_("OpenGL ES2"))
    set_description (N_("OpenGL for Embedded Systems 2 video output"))
    set_capability ("vout display", 265)
    set_callbacks (Open, Close)
    add_shortcut ("opengles2", "gles2")
    add_module ("gles2", "opengl es2", NULL,
                GLES2_TEXT, PROVIDER_LONGTEXT, true)

#else

# define API VLC_OPENGL
# define MODULE_VARNAME "gl"
    set_shortname (N_("OpenGL"))
    set_description (N_("OpenGL video output"))
    set_category (CAT_VIDEO)
    set_subcategory (SUBCAT_VIDEO_VOUT)
    set_capability ("vout display", 270)
    set_callbacks (Open, Close)
    add_shortcut ("opengl", "gl")
    add_module ("gl", "opengl", NULL,
                GL_TEXT, PROVIDER_LONGTEXT, true)
#endif
    add_glopts ()
vlc_module_end ()

struct vout_display_sys_t
{
    vout_display_opengl_t *vgl;
    vlc_gl_t *gl;
    picture_pool_t *pool;
#ifdef HAVE_STEREO_DECLARE
    Display *x11; /* connection the video window is declared with, if any */
# ifdef VLC_GL_WAYLAND
    struct wl_event_queue *stereo_queue;
    struct wp_viewport *viewport;
# endif
    bool stereo;
#endif
};

#ifdef HAVE_STEREO_DECLARE
#ifdef VLC_GL_WAYLAND
static void ViewporterGlobal(void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface, uint32_t version)
{
    (void)version;
    if (strcmp(interface, "wp_viewporter") == 0)
        *(struct wp_viewporter **)data = wl_registry_bind(registry, name,
                                             &wp_viewporter_interface, 1);
}

static void ViewporterRemoved(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static void SetViewSize(vout_display_sys_t *sys, unsigned width, unsigned height)
{
    if (sys->viewport == NULL)
        return;
    float scale = var_GetFloat(sys->gl->surface, "qt-wayland-scale");
    if (scale <= 0.f)
        scale = 1.f;
    /* wl_buffer has two full views; the subsurface occupies one logical view.
     * Without this destination KWin clips a double-width child to its parent. */
    int logical_width = width / scale + .5f;
    int logical_height = height / scale + .5f;
    wp_viewport_set_destination(sys->viewport,
        logical_width > 0 ? logical_width : 1,
        logical_height > 0 ? logical_height : 1);
}
#endif
/**
 * Declares the video window to the compositor as full side by side stereo
 * content, left eye first, while a stereoscopic video plays. The compositor
 * then holds both views in a window twice as wide as the one the video asked
 * for, and the video output draws one view in each half.
 */
static void DeclareStereo (vout_display_t *vd, vout_window_t *surface)
{
    vout_display_sys_t *sys = vd->sys;

    switch (vd->fmt.multiview_mode)
    {
        case MULTIVIEW_STEREO_SBS:
        case MULTIVIEW_STEREO_SBS_RIGHT_FIRST:
        case MULTIVIEW_STEREO_TB:
        case MULTIVIEW_STEREO_TB_RIGHT_FIRST:
        case MULTIVIEW_STEREO_ROW:
        case MULTIVIEW_STEREO_ROW_RIGHT_FIRST:
        case MULTIVIEW_STEREO_COL:
        case MULTIVIEW_STEREO_COL_RIGHT_FIRST:
        case MULTIVIEW_STEREO_CHECKERBOARD:
        case MULTIVIEW_STEREO_CHECKERBOARD_RIGHT_FIRST:
            break;
        default:
            return;
    }

#ifdef VLC_GL_WAYLAND
    if (surface->type == VOUT_WINDOW_TYPE_WAYLAND)
    {
        /* Only dispatch our own registry events on the video thread. Qt keeps
         * ownership of its display and default event queue. */
        struct wl_event_queue *queue = wl_display_create_queue(surface->display.wl);
        if (queue == NULL)
            return;
        if (stereo_declare_wayland_init(surface->display.wl, queue) != 0)
        {
            wl_event_queue_destroy(queue);
            return;
        }
        struct wp_viewporter *viewporter = NULL;
        struct wl_display *wrapper = wl_proxy_create_wrapper(surface->display.wl);
        if (wrapper == NULL)
        {
            stereo_declare_wayland_finish();
            wl_event_queue_destroy(queue);
            return;
        }
        wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);
        struct wl_registry *registry = wl_display_get_registry(wrapper);
        wl_proxy_wrapper_destroy(wrapper);
        if (registry == NULL)
        {
            stereo_declare_wayland_finish();
            wl_event_queue_destroy(queue);
            return;
        }
        static const struct wl_registry_listener listener = {
            ViewporterGlobal, ViewporterRemoved
        };
        wl_registry_add_listener(registry, &listener, &viewporter);
        if (wl_display_roundtrip_queue(surface->display.wl, queue) >= 0
         && viewporter != NULL
         && (sys->viewport = wp_viewporter_get_viewport(viewporter, surface->handle.wl)) != NULL
         && stereo_declare_wayland(surface->handle.wl, STEREO_SBS_FULL,
                                  STEREO_CLASS_VIDEO, STEREO_VIDEO_CURRENT) == 0)
        {
            sys->stereo_queue = queue;
            sys->stereo = true;
        }
        else
        {
            if (sys->viewport != NULL)
                wp_viewport_destroy(sys->viewport);
            sys->viewport = NULL;
            stereo_declare_wayland_finish();
        }
        if (viewporter != NULL)
            wp_viewporter_destroy(viewporter);
        wl_registry_destroy(registry);
        if (!sys->stereo)
            wl_event_queue_destroy(queue);
        return;
    }
#endif
    if (surface->type != VOUT_WINDOW_TYPE_XID || !vlc_xlib_init (VLC_OBJECT(vd)))
        return;

    Display *x11 = XOpenDisplay (surface->display.x11);
    if (x11 == NULL)
        return;

    if (stereo_supported_x11 (x11) >= 3
     && stereo_declare_x11 (x11, surface->handle.xid, STEREO_SBS_FULL,
                            STEREO_CLASS_VIDEO, STEREO_VIDEO_CURRENT) == 0)
    {
        sys->x11 = x11;
        sys->stereo = true;
    }
    else
        XCloseDisplay (x11);
}

/**
 * Takes back the declaration of the video window.
 */
static void RemoveStereo (vout_display_sys_t *sys, vout_window_t *surface)
{
    if (sys->x11 != NULL)
    {
        stereo_remove_x11 (sys->x11, surface->handle.xid);
        XCloseDisplay (sys->x11);
        sys->x11 = NULL;
    }
# ifdef VLC_GL_WAYLAND
    if (sys->stereo_queue != NULL)
    {
        wp_viewport_destroy(sys->viewport);
        stereo_remove_wayland(surface->handle.wl);
        stereo_declare_wayland_finish();
        wl_event_queue_destroy(sys->stereo_queue);
        sys->stereo_queue = NULL;
        sys->viewport = NULL;
    }
# endif
    sys->stereo = false;
}
#endif

/* Display callbacks */
static picture_pool_t *Pool (vout_display_t *, unsigned);
static void PictureRender (vout_display_t *, picture_t *, subpicture_t *);
static void PictureDisplay (vout_display_t *, picture_t *, subpicture_t *);
static int Control (vout_display_t *, int, va_list);
static int ChangeDisplay (vout_display_t *, const vout_display_cfg_t *);
static void PlaceOnDisplay (vout_display_t *, const vout_display_cfg_t *,
                            vout_display_place_t *, float *,
                            unsigned *, unsigned *, int *);

/**
 * Allocates a surface and an OpenGL context for video output.
 */
static int Open (vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = malloc (sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    sys->gl = NULL;
    sys->pool = NULL;
#ifdef HAVE_STEREO_DECLARE
    sys->x11 = NULL;
    sys->stereo = false;
# ifdef VLC_GL_WAYLAND
    sys->stereo_queue = NULL;
    sys->viewport = NULL;
# endif
#endif

    vout_window_t *surface = vout_display_NewWindow (vd, VOUT_WINDOW_TYPE_INVALID);
    if (surface == NULL)
    {
        msg_Err (vd, "parent window not available");
        goto error;
    }

    const char *gl_name = "$" MODULE_VARNAME;

    /* VDPAU GL interop works only with GLX. Override the "gl" option to force
     * it. */
#ifndef USE_OPENGL_ES2
    if (surface->type == VOUT_WINDOW_TYPE_XID)
    {
        switch (vd->fmt.i_chroma)
        {
            case VLC_CODEC_VDPAU_VIDEO_444:
            case VLC_CODEC_VDPAU_VIDEO_422:
            case VLC_CODEC_VDPAU_VIDEO_420:
            {
                /* Force the option only if it was not previously set */
                char *str = var_InheritString(surface, MODULE_VARNAME);
                if (str == NULL || str[0] == 0 || strcmp(str, "any") == 0)
                    gl_name = "glx";
                free(str);
                break;
            }
            default:
                break;
        }
    }
#endif

    sys->gl = vlc_gl_Create (surface, API, gl_name);
    if (sys->gl == NULL)
        goto error;

    vd->sys = sys;
#ifdef HAVE_STEREO_DECLARE
    DeclareStereo (vd, surface);
#endif
    /* The surface has its size before its first buffer is drawn, and the
     * views of a stereoscopic video their own resolution from the first
     * picture */
    vout_display_place_t place;
    float aspect;
    unsigned width, height;
    int stride;
    PlaceOnDisplay (vd, vd->cfg, &place, &aspect, &width, &height, &stride);
    vlc_gl_Resize (sys->gl, width, height);

    /* Initialize video display */
    const vlc_fourcc_t *spu_chromas;

    if (vlc_gl_MakeCurrent (sys->gl))
        goto error;

    sys->vgl = vout_display_opengl_New (&vd->fmt, &spu_chromas, sys->gl,
                                        &vd->cfg->viewpoint);
    vlc_gl_ReleaseCurrent (sys->gl);

    if (sys->vgl == NULL)
        goto error;

    if (ChangeDisplay (vd, vd->cfg) != VLC_SUCCESS)
        msg_Warn (vd, "cannot set the place of the picture yet");
    vd->info.has_pictures_invalid = false;
    vd->info.subpicture_chromas = spu_chromas;
    vd->pool = Pool;
    vd->prepare = PictureRender;
    vd->display = PictureDisplay;
    vd->control = Control;
    return VLC_SUCCESS;

error:
#ifdef HAVE_STEREO_DECLARE
    if (surface != NULL)
        RemoveStereo (sys, surface);
#endif
    if (sys->gl != NULL)
        vlc_gl_Release (sys->gl);
    if (surface != NULL)
        vout_display_DeleteWindow (vd, surface);
    free (sys);
    return VLC_EGENERIC;
}

/**
 * Destroys the OpenGL context.
 */
static void Close (vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = vd->sys;
    vlc_gl_t *gl = sys->gl;
    vout_window_t *surface = gl->surface;

#ifdef HAVE_STEREO_DECLARE
    RemoveStereo (sys, surface);
#endif

    vlc_gl_MakeCurrent (gl);
    vout_display_opengl_Delete (sys->vgl);
    vlc_gl_ReleaseCurrent (gl);

    vlc_gl_Release (gl);
    vout_display_DeleteWindow (vd, surface);
    free (sys);
}

/**
 * Returns picture buffers
 */
static picture_pool_t *Pool (vout_display_t *vd, unsigned count)
{
    vout_display_sys_t *sys = vd->sys;

    if (!sys->pool && vlc_gl_MakeCurrent (sys->gl) == VLC_SUCCESS)
    {
        sys->pool = vout_display_opengl_GetPool (sys->vgl, count);
        vlc_gl_ReleaseCurrent (sys->gl);
    }
    return sys->pool;
}

static void PictureRender (vout_display_t *vd, picture_t *pic, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;

    if (vlc_gl_MakeCurrent (sys->gl) == VLC_SUCCESS)
    {
        vout_display_opengl_Prepare (sys->vgl, pic, subpicture);
        vlc_gl_ReleaseCurrent (sys->gl);
    }
}

static void PictureDisplay (vout_display_t *vd, picture_t *pic, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;

    if (vlc_gl_MakeCurrent (sys->gl) == VLC_SUCCESS)
    {
        vout_display_opengl_Display (sys->vgl, &vd->source);
        vlc_gl_ReleaseCurrent (sys->gl);
    }

    picture_Release (pic);
    if (subpicture != NULL)
        subpicture_Delete(subpicture);
}

#ifdef HAVE_STEREO_DECLARE
/**
 * The size of the surface and the place of the picture in it, for the place
 * the core gives the picture in the display. The surface of a stereoscopic
 * video that the compositor takes holds both views at the resolution the
 * video carries them: the display measured in a view's own samples, so that
 * each view is drawn as it is, never scaled, and the compositor resizes the
 * surface once to the display (SetViewSize). Otherwise, or where that would
 * be larger than a texture can be (16384), the display's pixels, twice as wide
 * for the two views of a stereoscopic surface.
 */
static void PlaceOnSurface(vout_display_t *vd, const vout_display_cfg_t *cfg,
                           vout_display_place_t *place, unsigned *width,
                           unsigned *height, int *stride)
{
    vout_display_sys_t *sys = vd->sys;
    const video_format_t *src = &vd->source;
    unsigned view_w = src->i_visible_width, view_h = src->i_visible_height;

    *width = cfg->display.width * (sys->stereo ? 2 : 1);
    *height = cfg->display.height;
    *stride = sys->stereo ? cfg->display.width : 0;
    if (!sys->stereo)
        return;
    switch (src->multiview_mode)
    {
    case MULTIVIEW_STEREO_SBS:
    case MULTIVIEW_STEREO_SBS_RIGHT_FIRST:
    case MULTIVIEW_STEREO_COL:
    case MULTIVIEW_STEREO_COL_RIGHT_FIRST:
    case MULTIVIEW_STEREO_CHECKERBOARD:
    case MULTIVIEW_STEREO_CHECKERBOARD_RIGHT_FIRST:
        view_w /= 2;
        break;
    case MULTIVIEW_STEREO_TB:
    case MULTIVIEW_STEREO_TB_RIGHT_FIRST:
    case MULTIVIEW_STEREO_ROW:
    case MULTIVIEW_STEREO_ROW_RIGHT_FIRST:
        view_h /= 2;
        break;
    default:
        return;
    }
    if (view_w == 0 || view_h == 0 || place->width == 0 || place->height == 0)
        return;
    /* the view's samples per pixel of the display, along each axis */
    double kx = (double)view_w / place->width;
    double ky = (double)view_h / place->height;
    int x = lround(place->x * kx), y = lround(place->y * ky);
    unsigned w = __MAX((unsigned)lround(cfg->display.width * kx), x + view_w);
    unsigned h = __MAX((unsigned)lround(cfg->display.height * ky), y + view_h);
    if (2 * w > 16384 || h > 16384)
        return;
    *width = 2 * w;
    *height = h;
    *stride = w;
    *place = (vout_display_place_t) { .x = x, .y = y, .width = view_w, .height = view_h };
}
#endif

/**
 * Sizes the surface, its viewport and the place of the picture for a display
 * configuration.
 */
/**
 * The place of the picture for a display configuration, the aspect ratio of
 * that place, and the size of the surface (see PlaceOnSurface).
 */
static void PlaceOnDisplay (vout_display_t *vd, const vout_display_cfg_t *cfg,
                            vout_display_place_t *place, float *aspect,
                            unsigned *width, unsigned *height, int *stride)
{
    vout_display_cfg_t c = *cfg;

    /* Reverse vertical alignment as the GL tex are Y inverted */
    if (c.align.vertical == VOUT_DISPLAY_ALIGN_TOP)
        c.align.vertical = VOUT_DISPLAY_ALIGN_BOTTOM;
    else if (c.align.vertical == VOUT_DISPLAY_ALIGN_BOTTOM)
        c.align.vertical = VOUT_DISPLAY_ALIGN_TOP;

    vout_display_PlacePicture (place, &vd->source, &c, false);
    *aspect = (float)place->width / place->height;
    *width = c.display.width;
    *height = c.display.height;
    *stride = 0;
#ifdef HAVE_STEREO_DECLARE
    PlaceOnSurface(vd, &c, place, width, height, stride);
#endif
}

static int ChangeDisplay (vout_display_t *vd, const vout_display_cfg_t *cfg)
{
    vout_display_sys_t *sys = vd->sys;
    vout_display_place_t place;
    float aspect;
    unsigned width, height;
    int stride;

    PlaceOnDisplay (vd, cfg, &place, &aspect, &width, &height, &stride);
#if defined(HAVE_STEREO_DECLARE) && defined(VLC_GL_WAYLAND)
    SetViewSize(sys, cfg->display.width, cfg->display.height);
#endif
    vlc_gl_Resize (sys->gl, width, height);
    if (vlc_gl_MakeCurrent (sys->gl) != VLC_SUCCESS)
        return VLC_EGENERIC;
    vout_display_opengl_SetWindowAspectRatio(sys->vgl, aspect);
    vout_display_opengl_Viewport(sys->vgl, place.x, place.y, place.width, place.height);
#ifdef HAVE_STEREO_DECLARE
    /* The second view is in the second half of the declared window */
    vout_display_opengl_SetViewStride(sys->vgl, stride);
#else
    VLC_UNUSED(stride);
#endif
    vlc_gl_ReleaseCurrent (sys->gl);
    return VLC_SUCCESS;
}

static int Control (vout_display_t *vd, int query, va_list ap)
{
    vout_display_sys_t *sys = vd->sys;

    switch (query)
    {
#ifndef NDEBUG
      case VOUT_DISPLAY_RESET_PICTURES: // not needed
        vlc_assert_unreachable();
#endif

      case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
      case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
      case VOUT_DISPLAY_CHANGE_ZOOM:
        return ChangeDisplay (vd, va_arg (ap, const vout_display_cfg_t *));

      case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
      case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
      {
        const vout_display_cfg_t *cfg = vd->cfg;
        vout_display_place_t place;

        vout_display_PlacePicture (&place, &vd->source, cfg, false);
        const float aspect = (float)place.width / place.height;
#ifdef HAVE_STEREO_DECLARE
        unsigned width, height;
        int stride;
        PlaceOnSurface(vd, cfg, &place, &width, &height, &stride);
        vlc_gl_Resize (sys->gl, width, height);
#endif
        if (vlc_gl_MakeCurrent (sys->gl) != VLC_SUCCESS)
            return VLC_EGENERIC;
        vout_display_opengl_SetWindowAspectRatio(sys->vgl, aspect);
        vout_display_opengl_Viewport(sys->vgl, place.x, place.y, place.width, place.height);
#ifdef HAVE_STEREO_DECLARE
        vout_display_opengl_SetViewStride(sys->vgl, stride);
#endif
        vlc_gl_ReleaseCurrent (sys->gl);
        return VLC_SUCCESS;
      }
      case VOUT_DISPLAY_CHANGE_VIEWPOINT:
        return vout_display_opengl_SetViewpoint (sys->vgl,
            &va_arg (ap, const vout_display_cfg_t* )->viewpoint);
      default:
        msg_Err (vd, "Unknown request %d", query);
    }
    return VLC_EGENERIC;
}
