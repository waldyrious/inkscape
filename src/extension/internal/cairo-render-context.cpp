#define __SP_CAIRO_RENDER_CONTEXT_C__

/** \file
 * Rendering with Cairo.
 */
/*
 * Author:
 *   Miklos Erdelyi <erdelyim@gmail.com>
 *
 * Copyright (C) 2006 Miklos Erdelyi
 *
 * Licensed under GNU GPL
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifndef PANGO_ENABLE_BACKEND
#define PANGO_ENABLE_BACKEND
#endif

#ifndef PANGO_ENABLE_ENGINE
#define PANGO_ENABLE_ENGINE
#endif


#include <signal.h>
#include <errno.h>

#include <libnr/n-art-bpath.h>
#include <libnr/nr-matrix-ops.h>
#include <libnr/nr-matrix-fns.h>
#include <libnr/nr-matrix-translate-ops.h>
#include <libnr/nr-scale-matrix-ops.h>

#include <glib/gmem.h>

#include <glibmm/i18n.h>
#include "display/nr-arena.h"
#include "display/nr-arena-item.h"
#include "display/nr-arena-group.h"
#include "display/curve.h"
#include "display/canvas-bpath.h"
#include "sp-item.h"
#include "sp-item-group.h"
#include "style.h"
#include "sp-linear-gradient.h"
#include "sp-radial-gradient.h"
#include "sp-pattern.h"
#include "sp-mask.h"
#include "sp-clippath.h"

#include <unit-constants.h>

#include "cairo-render-context.h"
#include "cairo-renderer.h"
#include "extension/system.h"

#include "io/sys.h"

#include <cairo.h>

// include support for only the compiled-in surface types
#ifdef CAIRO_HAS_PDF_SURFACE
#include <cairo-pdf.h>
#endif
#ifdef CAIRO_HAS_PS_SURFACE
#include <cairo-ps.h>
#endif


#ifndef PANGO_ENABLE_BACKEND
#include <cairo-ft.h>
#endif

#include <pango/pangofc-fontmap.h>

//#define TRACE(_args) g_printf _args
#define TRACE(_args)
//#define TEST(_args) _args
#define TEST(_args)

// FIXME: expose these from sp-clippath/mask.cpp
struct SPClipPathView {
    SPClipPathView *next;
    unsigned int key;
    NRArenaItem *arenaitem;
    NRRect bbox;
};

struct SPMaskView {
    SPMaskView *next;
    unsigned int key;
    NRArenaItem *arenaitem;
    NRRect bbox;
};

namespace Inkscape {
namespace Extension {
namespace Internal {

static cairo_status_t _write_callback(void *closure, const unsigned char *data, unsigned int length);

CairoRenderContext::CairoRenderContext(CairoRenderer *parent) :
    _dpi(72),
    _stream(NULL),
    _is_valid(FALSE),
    _vector_based_target(FALSE),
    _cr(NULL),
    _surface(NULL),
    _target(CAIRO_SURFACE_TYPE_IMAGE),
    _target_format(CAIRO_FORMAT_ARGB32),
    _layout(NULL),
    _state(NULL),
    _renderer(parent),
    _render_mode(RENDER_MODE_NORMAL),
    _clip_mode(CLIP_MODE_MASK)
{}

CairoRenderContext::~CairoRenderContext(void)
{
    if (_cr) cairo_destroy(_cr);
    if (_surface) cairo_surface_destroy(_surface);
    if (_layout) g_object_unref(_layout);
}

CairoRenderer*
CairoRenderContext::getRenderer(void) const
{
    return _renderer;
}

CairoRenderState*
CairoRenderContext::getCurrentState(void) const
{
    return _state;
}

CairoRenderState*
CairoRenderContext::getParentState(void) const
{
    // if this is the root node just return it
    if (g_slist_length(_state_stack) == 1) {
        return _state;
    } else {
        return (CairoRenderState *)g_slist_nth_data(_state_stack, 1);
    }
}

void
CairoRenderContext::setStateForStyle(SPStyle const *style)
{
    // only opacity & overflow is stored for now
    _state->opacity = SP_SCALE24_TO_FLOAT(style->opacity.value);
    _state->has_overflow = (style->overflow.set && style->overflow.value != SP_CSS_OVERFLOW_VISIBLE);

    if (style->fill.isPaintserver() || style->stroke.isPaintserver())
        _state->merge_opacity = FALSE;

    // disable rendering of opacity if there's a stroke on the fill
    if (_state->merge_opacity
        && !style->fill.isNone()
        && !style->stroke.isNone())
        _state->merge_opacity = FALSE;
}

/**
 * \brief Creates a new render context which will be compatible with the given context's Cairo surface
 *
 * \param width     width of the surface to be created
 * \param height    height of the surface to be created
 */
CairoRenderContext*
CairoRenderContext::cloneMe(double width, double height) const
{
    g_assert( _is_valid );
    g_assert( width > 0.0 && height > 0.0 );

    CairoRenderContext *new_context = _renderer->createContext();
    cairo_surface_t *surface = cairo_surface_create_similar(cairo_get_target(_cr), CAIRO_CONTENT_COLOR_ALPHA,
                                                            (int)ceil(width), (int)ceil(height));
    new_context->_cr = cairo_create(surface);
    new_context->_surface = surface;
    new_context->_is_valid = TRUE;

    return new_context;
}

CairoRenderContext*
CairoRenderContext::cloneMe(void) const
{
    g_assert( _is_valid );

    return cloneMe(_width, _height);
}

bool
CairoRenderContext::setImageTarget(cairo_format_t format)
{
    // format cannot be set on an already initialized surface
    if (_is_valid)
        return false;

    switch (format) {
        case CAIRO_FORMAT_ARGB32:
        case CAIRO_FORMAT_RGB24:
        case CAIRO_FORMAT_A8:
        case CAIRO_FORMAT_A1:
            _target_format = format;
            _target = CAIRO_SURFACE_TYPE_IMAGE;
            return true;
            break;
        default:
            break;
    }

    return false;
}

bool
CairoRenderContext::setPdfTarget(gchar const *utf8_fn)
{
#ifndef CAIRO_HAS_PDF_SURFACE
    return false;
#else
    _target = CAIRO_SURFACE_TYPE_PDF;
    _vector_based_target = TRUE;
#endif

    FILE *osf = NULL;
    FILE *osp = NULL;

    gsize bytesRead = 0;
    gsize bytesWritten = 0;
    GError *error = NULL;
    gchar *local_fn = g_filename_from_utf8(utf8_fn,
                                           -1,  &bytesRead,  &bytesWritten, &error);
    gchar const *fn = local_fn;

    /* TODO: Replace the below fprintf's with something that does the right thing whether in
    * gui or batch mode (e.g. --print=blah).  Consider throwing an exception: currently one of
    * the callers (sp_print_document_to_file, "ret = mod->begin(doc)") wrongly ignores the
    * return code.
    */
    if (fn != NULL) {
        if (*fn == '|') {
            fn += 1;
            while (isspace(*fn)) fn += 1;
#ifndef WIN32
            osp = popen(fn, "w");
#else
            osp = _popen(fn, "w");
#endif
            if (!osp) {
                fprintf(stderr, "inkscape: popen(%s): %s\n",
                        fn, strerror(errno));
                return false;
            }
            _stream = osp;
        } else if (*fn == '>') {
            fn += 1;
            while (isspace(*fn)) fn += 1;
            Inkscape::IO::dump_fopen_call(fn, "K");
            osf = Inkscape::IO::fopen_utf8name(fn, "w+");
            if (!osf) {
                fprintf(stderr, "inkscape: fopen(%s): %s\n",
                        fn, strerror(errno));
                return false;
            }
            _stream = osf;
        } else {
            /* put cwd stuff in here */
            gchar *qn = ( *fn
                    ? g_strdup_printf("lpr -P %s", fn)  /* FIXME: quote fn */
                : g_strdup("lpr") );
#ifndef WIN32
            osp = popen(qn, "w");
#else
            osp = _popen(qn, "w");
#endif
            if (!osp) {
                fprintf(stderr, "inkscape: popen(%s): %s\n",
                        qn, strerror(errno));
                return false;
            }
            g_free(qn);
            _stream = osp;
        }
    }

    g_free(local_fn);

    if (_stream) {
        /* fixme: this is kinda icky */
#if !defined(_WIN32) && !defined(__WIN32__)
        (void) signal(SIGPIPE, SIG_IGN);
#endif
    }

    return true;
}

bool
CairoRenderContext::setPsTarget(gchar const *utf8_fn)
{
#ifndef CAIRO_HAS_PS_SURFACE
    return false;
#else
    _target = CAIRO_SURFACE_TYPE_PS;
    _vector_based_target = TRUE;
#endif

    FILE *osf = NULL;
    FILE *osp = NULL;

    gsize bytesRead = 0;
    gsize bytesWritten = 0;
    GError *error = NULL;
    gchar *local_fn = g_filename_from_utf8(utf8_fn,
                                           -1,  &bytesRead,  &bytesWritten, &error);
    gchar const *fn = local_fn;

    /* TODO: Replace the below fprintf's with something that does the right thing whether in
    * gui or batch mode (e.g. --print=blah).  Consider throwing an exception: currently one of
    * the callers (sp_print_document_to_file, "ret = mod->begin(doc)") wrongly ignores the
    * return code.
    */
    if (fn != NULL) {
        if (*fn == '|') {
            fn += 1;
            while (isspace(*fn)) fn += 1;
#ifndef WIN32
            osp = popen(fn, "w");
#else
            osp = _popen(fn, "w");
#endif
            if (!osp) {
                fprintf(stderr, "inkscape: popen(%s): %s\n",
                        fn, strerror(errno));
                return false;
            }
            _stream = osp;
        } else if (*fn == '>') {
            fn += 1;
            while (isspace(*fn)) fn += 1;
            Inkscape::IO::dump_fopen_call(fn, "K");
            osf = Inkscape::IO::fopen_utf8name(fn, "w+");
            if (!osf) {
                fprintf(stderr, "inkscape: fopen(%s): %s\n",
                        fn, strerror(errno));
                return false;
            }
            _stream = osf;
        } else {
            /* put cwd stuff in here */
            gchar *qn = ( *fn
                    ? g_strdup_printf("lpr -P %s", fn)  /* FIXME: quote fn */
                : g_strdup("lpr") );
#ifndef WIN32
            osp = popen(qn, "w");
#else
            osp = _popen(qn, "w");
#endif
            if (!osp) {
                fprintf(stderr, "inkscape: popen(%s): %s\n",
                        qn, strerror(errno));
                return false;
            }
            g_free(qn);
            _stream = osp;
        }
    }

    g_free(local_fn);

    if (_stream) {
        /* fixme: this is kinda icky */
#if !defined(_WIN32) && !defined(__WIN32__)
        (void) signal(SIGPIPE, SIG_IGN);
#endif
    }

    return true;
}

cairo_surface_t*
CairoRenderContext::getSurface(void)
{
    g_assert( _is_valid );

    return _surface;
}

bool
CairoRenderContext::saveAsPng(const char *file_name)
{
    cairo_status_t status = cairo_surface_write_to_png(_surface, file_name);
    if (status)
        return false;
    else
        return true;
}

void
CairoRenderContext::setRenderMode(CairoRenderMode mode)
{
    switch (mode) {
        case RENDER_MODE_NORMAL:
        case RENDER_MODE_CLIP:
            _render_mode = mode;
            break;
        default:
            _render_mode = RENDER_MODE_NORMAL;
            break;
    }
}

CairoRenderContext::CairoRenderMode
CairoRenderContext::getRenderMode(void) const
{
    return _render_mode;
}

void
CairoRenderContext::setClipMode(CairoClipMode mode)
{
    switch (mode) {
        case CLIP_MODE_PATH:
        case CLIP_MODE_MASK:
            _clip_mode = mode;
            break;
        default:
            _clip_mode = CLIP_MODE_PATH;
            break;
    }
}

CairoRenderContext::CairoClipMode
CairoRenderContext::getClipMode(void) const
{
    return _clip_mode;
}

CairoRenderState*
CairoRenderContext::_createState(void)
{
    CairoRenderState *state = (CairoRenderState*)g_malloc(sizeof(CairoRenderState));
    g_assert( state != NULL );

    state->merge_opacity = TRUE;
    state->opacity = 1.0;
    state->need_layer = FALSE;
    state->has_overflow = FALSE;
    state->parent_has_userspace = FALSE;
    state->clip_path = NULL;
    state->mask = NULL;

    return state;
}

void
CairoRenderContext::pushLayer(void)
{
    g_assert( _is_valid );

    TRACE(("--pushLayer\n"));
    cairo_push_group(_cr);

    // clear buffer
    if (!_vector_based_target) {
        cairo_save(_cr);
        cairo_set_operator(_cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(_cr);
        cairo_restore(_cr);
    }
}

void
CairoRenderContext::popLayer(void)
{
    g_assert( _is_valid );

    float opacity = _state->opacity;
    TRACE(("--popLayer w/ %f\n", opacity));

    // apply clipPath or mask if present
    SPClipPath *clip_path = _state->clip_path;
    SPMask *mask = _state->mask;
    if (clip_path || mask) {

        CairoRenderContext *clip_ctx = 0;
        cairo_surface_t *clip_mask = 0;

        if (clip_path) {
            if (_render_mode == RENDER_MODE_CLIP)
                mask = NULL;    // disable mask when performing nested clipping

            if (_vector_based_target) {
                setClipMode(CLIP_MODE_PATH);
                if (!mask) {
                    cairo_pop_group_to_source(_cr);
                    _renderer->applyClipPath(this, clip_path);
                    if (opacity == 1.0)
                        cairo_paint(_cr);
                    else
                        cairo_paint_with_alpha(_cr, opacity);

                } else {
                    // the clipPath will be applied before masking
                }
            } else {

                // setup a new rendering context
                clip_ctx = _renderer->createContext();
                clip_ctx->setImageTarget(CAIRO_FORMAT_A8);
                clip_ctx->setClipMode(CLIP_MODE_MASK);
                if (!clip_ctx->setupSurface(_width, _height)) {
                    TRACE(("setupSurface failed\n"));
                    _renderer->destroyContext(clip_ctx);
                    return;
                }

                // clear buffer
                cairo_save(clip_ctx->_cr);
                cairo_set_operator(clip_ctx->_cr, CAIRO_OPERATOR_CLEAR);
                cairo_paint(clip_ctx->_cr);
                cairo_restore(clip_ctx->_cr);

                // if a mask won't be applied set opacity too
                if (!mask)
                    cairo_set_source_rgba(clip_ctx->_cr, 1.0, 1.0, 1.0, opacity);
                else
                    cairo_set_source_rgba(clip_ctx->_cr, 1.0, 1.0, 1.0, 1.0);

                // copy over the correct CTM
                if (_state->parent_has_userspace)
                    clip_ctx->setTransform(&getParentState()->transform);
                else
                    clip_ctx->setTransform(&_state->transform);

                // apply the clip path
                clip_ctx->pushState();
                _renderer->applyClipPath(clip_ctx, clip_path);
                clip_ctx->popState();

                clip_mask = clip_ctx->getSurface();
                TEST(clip_ctx->saveAsPng("clip_mask.png"));

                if (!mask) {
                    cairo_pop_group_to_source(_cr);
                    cairo_mask_surface(_cr, clip_mask, 0, 0);
                    _renderer->destroyContext(clip_ctx);
                }
            }
        }

        if (mask) {
            // create rendering context for mask
            CairoRenderContext *mask_ctx = _renderer->createContext();
            mask_ctx->setupSurface(_width, _height);

            // set rendering mode to normal
            setRenderMode(RENDER_MODE_NORMAL);

            // copy the correct CTM to mask context
            if (_state->parent_has_userspace)
                mask_ctx->setTransform(&getParentState()->transform);
            else
                mask_ctx->setTransform(&_state->transform);

            // render mask contents to mask_ctx
            _renderer->applyMask(mask_ctx, mask);

            TEST(mask_ctx->saveAsPng("mask.png"));

            // composite with clip mask
            if (clip_path && _clip_mode == CLIP_MODE_MASK) {
                cairo_mask_surface(mask_ctx->_cr, clip_mask, 0, 0);
                _renderer->destroyContext(clip_ctx);
            }

            cairo_surface_t *mask_image = mask_ctx->getSurface();
            int width = cairo_image_surface_get_width(mask_image);
            int height = cairo_image_surface_get_height(mask_image);
            int stride = cairo_image_surface_get_stride(mask_image);
            unsigned char *pixels = cairo_image_surface_get_data(mask_image);

            // premultiply with opacity
            if (_state->opacity != 1.0) {
                TRACE(("premul w/ %f\n", opacity));
                guint8 int_opacity = (guint8)(255 * opacity);
                for (int row = 0 ; row < height; row++) {
                    unsigned char *row_data = pixels + (row * stride);
                    for (int i = 0 ; i < width; i++) {
                        guint32 *pixel = (guint32 *)row_data + i;
                        *pixel = ((((*pixel & 0x00ff0000) >> 16) * 13817 +
                                ((*pixel & 0x0000ff00) >>  8) * 46518 +
                                ((*pixel & 0x000000ff)      ) * 4688) *
                                int_opacity);
                    }
                }
            }

            cairo_pop_group_to_source(_cr);
            if (_clip_mode == CLIP_MODE_PATH) {
                // we have to do the clipping after cairo_pop_group_to_source
                _renderer->applyClipPath(this, clip_path);
            }
            // apply the mask onto the layer
            cairo_mask_surface(_cr, mask_image, 0, 0);
            _renderer->destroyContext(mask_ctx);
        }
    } else {
        cairo_pop_group_to_source(_cr);
        if (opacity == 1.0)
            cairo_paint(_cr);
        else
            cairo_paint_with_alpha(_cr, opacity);
    }
}

void
CairoRenderContext::addClipPath(NArtBpath const *bp, SPIEnum const *fill_rule)
{
    g_assert( _is_valid );

    // here it should be checked whether the current clip winding changed
    // so we could switch back to masked clipping
    if (fill_rule->value == SP_WIND_RULE_EVENODD) {
        cairo_set_fill_rule(_cr, CAIRO_FILL_RULE_EVEN_ODD);
    } else {
        cairo_set_fill_rule(_cr, CAIRO_FILL_RULE_WINDING);
    }
    addBpath(bp);
}

void
CairoRenderContext::addClippingRect(double x, double y, double width, double height)
{
    g_assert( _is_valid );

    cairo_rectangle(_cr, x, y, width, height);
    cairo_clip(_cr);
}

bool
CairoRenderContext::setupSurface(double width, double height)
{
    if (_vector_based_target && _stream == NULL)
        return false;

    cairo_surface_t *surface = NULL;
    switch (_target) {
        case CAIRO_SURFACE_TYPE_IMAGE:
            surface = cairo_image_surface_create(_target_format, (int)ceil(width), (int)ceil(height));
            break;
#ifdef CAIRO_HAS_PDF_SURFACE
        case CAIRO_SURFACE_TYPE_PDF:
            surface = cairo_pdf_surface_create_for_stream(Inkscape::Extension::Internal::_write_callback, _stream, width, height);
            break;
#endif
#ifdef CAIRO_HAS_PS_SURFACE
        case CAIRO_SURFACE_TYPE_PS:
            surface = cairo_ps_surface_create_for_stream(Inkscape::Extension::Internal::_write_callback, _stream, width, height);
            break;
#endif
        default:
            return false;
            break;
    }

    return _finishSurfaceSetup (surface);
}

bool
CairoRenderContext::setSurface(cairo_surface_t *surface)
{
    if (_is_valid || !surface)
        return false;

    bool ret = _finishSurfaceSetup (surface);
    if (ret)
        cairo_surface_reference (surface);
    return ret;
}

bool
CairoRenderContext::_finishSurfaceSetup(cairo_surface_t *surface)
{
    _cr = cairo_create(surface);
    _surface = surface;

    if (_vector_based_target) {
        cairo_scale(_cr, PT_PER_PX, PT_PER_PX);
    } else if (cairo_surface_get_content(_surface) != CAIRO_CONTENT_ALPHA) {
        // set background color on non-alpha surfaces
        // TODO: bgcolor should be derived from SPDocument
        cairo_set_source_rgb(_cr, 1.0, 1.0, 1.0);
        cairo_rectangle(_cr, 0, 0, _width, _height);
        cairo_fill(_cr);
    }

    _is_valid = TRUE;

    return true;
}

bool
CairoRenderContext::finish(void)
{
    g_assert( _is_valid );

    if (_vector_based_target)
        cairo_show_page(_cr);

    cairo_destroy(_cr);
    cairo_surface_destroy(_surface);
    _cr = NULL;
    _surface = NULL;

    if (_layout)
        g_object_unref(_layout);

    _is_valid = FALSE;

    if (_vector_based_target) {
        /* Flush stream to be sure. */
        (void) fflush(_stream);

        fclose(_stream);
        _stream = NULL;
    }

    return true;
}

void
CairoRenderContext::transform(NRMatrix const *transform)
{
    g_assert( _is_valid );

    cairo_matrix_t matrix;
    _initCairoMatrix(&matrix, transform);
    cairo_transform(_cr, &matrix);

    // store new CTM
    getTransform(&_state->transform);
}

void
CairoRenderContext::setTransform(NRMatrix const *transform)
{
    g_assert( _is_valid );

    cairo_matrix_t matrix;
    _initCairoMatrix(&matrix, transform);
    cairo_set_matrix(_cr, &matrix);
    _state->transform = *transform;
}

void
CairoRenderContext::getTransform(NRMatrix *copy) const
{
    g_assert( _is_valid );

    cairo_matrix_t ctm;
    cairo_get_matrix(_cr, &ctm);
    (*copy)[0] = ctm.xx;
    (*copy)[1] = ctm.yx;
    (*copy)[2] = ctm.xy;
    (*copy)[3] = ctm.yy;
    (*copy)[4] = ctm.x0;
    (*copy)[5] = ctm.y0;
}

void
CairoRenderContext::getParentTransform(NRMatrix *copy) const
{
    g_assert( _is_valid );

    CairoRenderState *parent_state = getParentState();
    memcpy(copy, &parent_state->transform, sizeof(NRMatrix));
}

void
CairoRenderContext::pushState(void)
{
    g_assert( _is_valid );

    cairo_save(_cr);

    CairoRenderState *new_state = _createState();
    // copy current state's transform
    new_state->transform = _state->transform;
    _state_stack = g_slist_prepend(_state_stack, new_state);
    _state = new_state;
}

void
CairoRenderContext::popState(void)
{
    g_assert( _is_valid );

    cairo_restore(_cr);

    g_free(_state_stack->data);
    _state_stack = g_slist_remove_link(_state_stack, _state_stack);
    _state = (CairoRenderState*)_state_stack->data;

    g_assert( g_slist_length(_state_stack) > 0 );
}

static bool pattern_hasItemChildren (SPPattern *pat)
{
    for (SPObject *child = sp_object_first_child(SP_OBJECT(pat)) ; child != NULL; child = SP_OBJECT_NEXT(child) ) {
        if (SP_IS_ITEM (child)) {
            return true;
        }
    }
    return false;
}

cairo_pattern_t*
CairoRenderContext::_createPatternPainter(SPPaintServer const *const paintserver, NRRect const *pbox)
{
    g_assert( SP_IS_PATTERN(paintserver) );

    SPPattern *pat = SP_PATTERN (paintserver);

    NRMatrix ps2user, pcs2dev;
    nr_matrix_set_identity(&ps2user);
    nr_matrix_set_identity(&pcs2dev);

    double x = pattern_x(pat);
    double y = pattern_y(pat);
    double width = pattern_width(pat);
    double height = pattern_height(pat);
    double bbox_width_scaler;
    double bbox_height_scaler;

    TRACE(("%f x %f pattern\n", width, height));

    if (pbox && pattern_patternUnits(pat) == SP_PATTERN_UNITS_OBJECTBOUNDINGBOX) {
        //NR::Matrix bbox2user (pbox->x1 - pbox->x0, 0.0, 0.0, pbox->y1 - pbox->y0, pbox->x0, pbox->y0);
        bbox_width_scaler = pbox->x1 - pbox->x0;
        bbox_height_scaler = pbox->y1 - pbox->y0;
        ps2user[4] = x * bbox_width_scaler + pbox->x0;
        ps2user[5] = y * bbox_height_scaler + pbox->y0;
    } else {
        bbox_width_scaler = 1.0;
        bbox_height_scaler = 1.0;
        ps2user[4] = x;
        ps2user[5] = y;
    }

    // apply pattern transformation
    NRMatrix pattern_transform(pattern_patternTransform(pat));
    nr_matrix_multiply(&ps2user, &ps2user, &pattern_transform);

    // create pattern contents coordinate system
    if (pat->viewBox_set) {
        NRRect *view_box = pattern_viewBox(pat);

        double x, y, w, h;
        double view_width, view_height;
        x = 0;
        y = 0;
        w = width * bbox_width_scaler;
        h = height * bbox_height_scaler;

        view_width = view_box->x1 - view_box->x0;
        view_height = view_box->y1 - view_box->y0;

        //calculatePreserveAspectRatio(pat->aspect_align, pat->aspect_clip, view_width, view_height, &x, &y, &w, &h);
        pcs2dev[0] = w / view_width;
        pcs2dev[3] = h / view_height;
        pcs2dev[4] = x - view_box->x0 * pcs2dev[0];
        pcs2dev[5] = y - view_box->y0 * pcs2dev[3];
    } else if (pbox && pattern_patternContentUnits(pat) == SP_PATTERN_UNITS_OBJECTBOUNDINGBOX) {
        pcs2dev[0] = pbox->x1 - pbox->x0;
        pcs2dev[3] = pbox->y1 - pbox->y0;
    }

    // calculate the size of the surface which has to be created
    // the scaling needs to be taken into account in the ctm after the pattern transformation
    NRMatrix temp;
    nr_matrix_multiply(&temp, &pattern_transform, &_state->transform);
    double width_scaler = sqrt(temp[0] * temp[0] + temp[2] * temp[2]);
    double height_scaler = sqrt(temp[1] * temp[1] + temp[3] * temp[3]);

    if (_vector_based_target) {
        // eliminate PT_PER_PX mul from these
        width_scaler *= 1.25;
        height_scaler *= 1.25;
    }
    double surface_width = ceil(bbox_width_scaler * width_scaler * width);
    double surface_height = ceil(bbox_height_scaler * height_scaler * height);
    TRACE(("surface size: %f x %f\n", surface_width, surface_height));
    // create new rendering context
    CairoRenderContext *pattern_ctx = cloneMe(surface_width, surface_height);

    // adjust the size of the painted pattern to fit exactly the created surface
    // this has to be done because of the rounding to obtain an integer pattern surface width/height
    double scale_width = surface_width / (bbox_width_scaler * width);
    double scale_height = surface_height / (bbox_height_scaler * height);
    if (scale_width != 1.0 || scale_height != 1.0 || _vector_based_target) {
        TRACE(("needed to scale with %f %f\n", scale_width, scale_height));
        NRMatrix scale;
        nr_matrix_set_scale(&scale, 1.0 / scale_width, 1.0 / scale_height);
        nr_matrix_multiply(&pcs2dev, &pcs2dev, &scale);

        nr_matrix_set_scale(&scale, scale_width, scale_height);
        nr_matrix_multiply(&ps2user, &ps2user, &scale);
    }

    pattern_ctx->setTransform(&pcs2dev);
    pattern_ctx->pushState();

    // create arena and group
    NRArena *arena = NRArena::create();
    unsigned dkey = sp_item_display_key_new(1);

    // show items and render them
    for (SPPattern *pat_i = pat; pat_i != NULL; pat_i = pat_i->ref ? pat_i->ref->getObject() : NULL) {
        if (pat_i && SP_IS_OBJECT (pat_i) && pattern_hasItemChildren(pat_i)) { // find the first one with item children
            for (SPObject *child = sp_object_first_child(SP_OBJECT(pat_i)) ; child != NULL; child = SP_OBJECT_NEXT(child) ) {
                if (SP_IS_ITEM (child)) {
                    sp_item_invoke_show (SP_ITEM (child), arena, dkey, SP_ITEM_REFERENCE_FLAGS);
                    _renderer->renderItem(pattern_ctx, SP_ITEM (child));
                }
            }
            break; // do not go further up the chain if children are found
        }
    }

    pattern_ctx->popState();

    // setup a cairo_pattern_t
    cairo_surface_t *pattern_surface = pattern_ctx->getSurface();
    TEST(pattern_ctx->saveAsPng("pattern.png"));
    cairo_pattern_t *result = cairo_pattern_create_for_surface(pattern_surface);
    cairo_pattern_set_extend(result, CAIRO_EXTEND_REPEAT);

    // set pattern transformation
    cairo_matrix_t pattern_matrix;
    _initCairoMatrix(&pattern_matrix, &ps2user);
    cairo_matrix_invert(&pattern_matrix);
    cairo_pattern_set_matrix(result, &pattern_matrix);

    delete pattern_ctx;

    // hide all items
    for (SPPattern *pat_i = pat; pat_i != NULL; pat_i = pat_i->ref ? pat_i->ref->getObject() : NULL) {
        if (pat_i && SP_IS_OBJECT (pat_i) && pattern_hasItemChildren(pat_i)) { // find the first one with item children
            for (SPObject *child = sp_object_first_child(SP_OBJECT(pat_i)) ; child != NULL; child = SP_OBJECT_NEXT(child) ) {
                if (SP_IS_ITEM (child)) {
                    sp_item_invoke_hide (SP_ITEM (child), dkey);
                }
            }
            break; // do not go further up the chain if children are found
        }
    }

    return result;
}

cairo_pattern_t*
CairoRenderContext::_createPatternForPaintServer(SPPaintServer const *const paintserver,
                                                 NRRect const *pbox, float alpha)
{
    cairo_pattern_t *pattern = NULL;
    bool apply_bbox2user = FALSE;

    if (SP_IS_LINEARGRADIENT (paintserver)) {

            SPLinearGradient *lg=SP_LINEARGRADIENT(paintserver);

            sp_gradient_ensure_vector(SP_GRADIENT(lg)); // when exporting from commandline, vector is not built

            NR::Point p1 (lg->x1.computed, lg->y1.computed);
            NR::Point p2 (lg->x2.computed, lg->y2.computed);
            if (pbox && SP_GRADIENT(lg)->units == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX) {
                // convert to userspace
                NR::Matrix bbox2user(pbox->x1 - pbox->x0, 0, 0, pbox->y1 - pbox->y0, pbox->x0, pbox->y0);
                p1 *= bbox2user;
                p2 *= bbox2user;
            }

            // create linear gradient pattern
            pattern = cairo_pattern_create_linear(p1[NR::X], p1[NR::Y], p2[NR::X], p2[NR::Y]);

            // add stops
            for (gint i = 0; unsigned(i) < lg->vector.stops.size(); i++) {
                float rgb[3];
                sp_color_get_rgb_floatv(&lg->vector.stops[i].color, rgb);
                cairo_pattern_add_color_stop_rgba(pattern, lg->vector.stops[i].offset, rgb[0], rgb[1], rgb[2], lg->vector.stops[i].opacity * alpha);
            }
    } else if (SP_IS_RADIALGRADIENT (paintserver)) {

        SPRadialGradient *rg=SP_RADIALGRADIENT(paintserver);

        sp_gradient_ensure_vector(SP_GRADIENT(rg)); // when exporting from commandline, vector is not built

        NR::Point c (rg->cx.computed, rg->cy.computed);
        NR::Point f (rg->fx.computed, rg->fy.computed);
        double r = rg->r.computed;
        if (pbox && SP_GRADIENT(rg)->units == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX)
            apply_bbox2user = true;

        // create radial gradient pattern
        pattern = cairo_pattern_create_radial(f[NR::X], f[NR::Y], 0, c[NR::X], c[NR::Y], r);

        // add stops
        for (gint i = 0; unsigned(i) < rg->vector.stops.size(); i++) {
            float rgb[3];
            sp_color_get_rgb_floatv(&rg->vector.stops[i].color, rgb);
            cairo_pattern_add_color_stop_rgba(pattern, rg->vector.stops[i].offset, rgb[0], rgb[1], rgb[2], rg->vector.stops[i].opacity * alpha);
        }
    } else if (SP_IS_PATTERN (paintserver)) {

        pattern = _createPatternPainter(paintserver, pbox);
    } else {
        return NULL;
    }

    if (pattern && SP_IS_GRADIENT (paintserver)) {
        SPGradient *g = SP_GRADIENT(paintserver);

        // set extend type
        SPGradientSpread spread = sp_gradient_get_spread(g);
        switch (spread) {
            case SP_GRADIENT_SPREAD_REPEAT: {
                cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
                break;
            }
            case SP_GRADIENT_SPREAD_REFLECT: {      // not supported by cairo-pdf yet
                cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REFLECT);
                break;
            }
            case SP_GRADIENT_SPREAD_PAD: {    // not supported by cairo-pdf yet
                cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
                break;
            }
            default: {
                cairo_pattern_set_extend(pattern, CAIRO_EXTEND_NONE);
                break;
            }
        }

        cairo_matrix_t pattern_matrix;
        if (g->gradientTransform_set) {
            // apply gradient transformation
            cairo_matrix_init(&pattern_matrix,
                g->gradientTransform[0], g->gradientTransform[1],
                g->gradientTransform[2], g->gradientTransform[3],
                g->gradientTransform[4], g->gradientTransform[5]);
        } else {
            cairo_matrix_init_identity (&pattern_matrix);
        }

        if (apply_bbox2user) {
            // convert to userspace
            cairo_matrix_t bbox2user;
            cairo_matrix_init (&bbox2user, pbox->x1 - pbox->x0, 0, 0, pbox->y1 - pbox->y0, pbox->x0, pbox->y0);
            cairo_matrix_multiply (&pattern_matrix, &bbox2user, &pattern_matrix);
        }
        cairo_matrix_invert(&pattern_matrix);   // because Cairo expects a userspace->patternspace matrix
        cairo_pattern_set_matrix(pattern, &pattern_matrix);
    }

    return pattern;
}

void
CairoRenderContext::_setFillStyle(SPStyle const *const style, NRRect const *pbox)
{
    g_return_if_fail( style->fill.isColor()
                      || style->fill.isPaintserver() );

    float alpha = SP_SCALE24_TO_FLOAT(style->fill_opacity.value);
    if (_state->merge_opacity) {
        alpha *= _state->opacity;
        TRACE(("merged op=%f\n", alpha));
    }

    if (style->fill.isColor()) {
        float rgb[3];
        sp_color_get_rgb_floatv(&style->fill.value.color, rgb);

        cairo_set_source_rgba(_cr, rgb[0], rgb[1], rgb[2], alpha);
    } else {
        g_assert( style->fill.isPaintserver()
                  || SP_IS_GRADIENT(SP_STYLE_FILL_SERVER(style))
                  || SP_IS_PATTERN(SP_STYLE_FILL_SERVER(style)) );

        cairo_pattern_t *pattern = _createPatternForPaintServer(SP_STYLE_FILL_SERVER(style), pbox, alpha);

        if (pattern) {
            cairo_set_source(_cr, pattern);
            cairo_pattern_destroy(pattern);
        }
    }
}

void
CairoRenderContext::_setStrokeStyle(SPStyle const *style, NRRect const *pbox)
{
    float alpha = SP_SCALE24_TO_FLOAT(style->stroke_opacity.value);
    if (_state->merge_opacity)
        alpha *= _state->opacity;

    if (style->stroke.isColor()) {
        float rgb[3];
        sp_color_get_rgb_floatv(&style->stroke.value.color, rgb);

        cairo_set_source_rgba(_cr, rgb[0], rgb[1], rgb[2], alpha);
    } else {
        g_assert( style->fill.isPaintserver()
                  || SP_IS_GRADIENT(SP_STYLE_STROKE_SERVER(style))
                  || SP_IS_PATTERN(SP_STYLE_STROKE_SERVER(style)) );

        cairo_pattern_t *pattern = _createPatternForPaintServer(SP_STYLE_STROKE_SERVER(style), pbox, alpha);

        if (pattern) {
            cairo_set_source(_cr, pattern);
            cairo_pattern_destroy(pattern);
        }
    }

    if (style->stroke_dash.n_dash   &&
        style->stroke_dash.dash       )
    {
        cairo_set_dash(_cr, style->stroke_dash.dash, style->stroke_dash.n_dash, style->stroke_dash.offset);
    } else {
    	cairo_set_dash(_cr, NULL, 0, 0.0);	// disable dashing
    }

    cairo_set_line_width(_cr, style->stroke_width.computed);

    // set line join type
    cairo_line_join_t join = CAIRO_LINE_JOIN_MITER;
    switch (style->stroke_linejoin.computed) {
    	case SP_STROKE_LINEJOIN_MITER:
    	    join = CAIRO_LINE_JOIN_MITER;
    	    break;
    	case SP_STROKE_LINEJOIN_ROUND:
    	    join = CAIRO_LINE_JOIN_ROUND;
    	    break;
    	case SP_STROKE_LINEJOIN_BEVEL:
    	    join = CAIRO_LINE_JOIN_BEVEL;
    	    break;
    }
    cairo_set_line_join(_cr, join);

    // set line cap type
    cairo_line_cap_t cap = CAIRO_LINE_CAP_BUTT;
    switch (style->stroke_linecap.computed) {
    	case SP_STROKE_LINECAP_BUTT:
    	    cap = CAIRO_LINE_CAP_BUTT;
    	    break;
    	case SP_STROKE_LINECAP_ROUND:
    	    cap = CAIRO_LINE_CAP_ROUND;
    	    break;
    	case SP_STROKE_LINECAP_SQUARE:
    	    cap = CAIRO_LINE_CAP_SQUARE;
    	    break;
    }
    cairo_set_line_cap(_cr, cap);
    cairo_set_miter_limit(_cr, MAX(1, style->stroke_miterlimit.value));
}

bool
CairoRenderContext::renderPath(NRBPath const *bpath, SPStyle const *style, NRRect const *pbox)
{
    g_assert( _is_valid );

    if (_render_mode == RENDER_MODE_CLIP) {
        if (_clip_mode == CLIP_MODE_PATH) {
            addClipPath(bpath->path, &style->fill_rule);
        } else {
            setBpath(bpath->path);
            if (style->fill_rule.value == SP_WIND_RULE_EVENODD) {
                cairo_set_fill_rule(_cr, CAIRO_FILL_RULE_EVEN_ODD);
            } else {
                cairo_set_fill_rule(_cr, CAIRO_FILL_RULE_WINDING);
            }
            cairo_fill(_cr);
            cairo_surface_write_to_png (_surface, "gtar2.png");
        }
        return true;
    }

    if (style->fill.isNone() && style->stroke.isNone())
        return true;

    bool need_layer = ( !_state->merge_opacity && !_state->need_layer &&
                        ( _state->opacity != 1.0 || _state->clip_path != NULL || _state->mask != NULL ) );

    if (!need_layer)
        cairo_save(_cr);
    else
        pushLayer();

    if (!style->fill.isNone()) {
        _setFillStyle(style, pbox);
        setBpath(bpath->path);

        if (style->fill_rule.value == SP_WIND_RULE_EVENODD) {
            cairo_set_fill_rule(_cr, CAIRO_FILL_RULE_EVEN_ODD);
        } else {
            cairo_set_fill_rule(_cr, CAIRO_FILL_RULE_WINDING);
        }

        if (style->stroke.isNone())
            cairo_fill(_cr);
        else
            cairo_fill_preserve(_cr);
    }

    if (!style->stroke.isNone()) {
        _setStrokeStyle(style, pbox);
        if (style->fill.isNone())
            setBpath(bpath->path);

        cairo_stroke(_cr);
    }

    if (need_layer)
        popLayer();
    else
        cairo_restore(_cr);

    return true;
}

bool
CairoRenderContext::renderImage(guchar *px, unsigned int w, unsigned int h, unsigned int rs,
                                NRMatrix const *image_transform, SPStyle const *style)
{
    g_assert( _is_valid );

    if (_render_mode == RENDER_MODE_CLIP)
        return true;

    guchar* px_rgba = (guchar*)g_malloc(4 * w * h);
    if (!px_rgba)
        return false;

    float opacity;
    if (_state->merge_opacity)
        opacity = _state->opacity;
    else
        opacity = 1.0;

    // make a copy of the original pixbuf with premultiplied alpha
    // if we pass the original pixbuf it will get messed up
    for (unsigned i = 0; i < h; i++) {
    	for (unsigned j = 0; j < w; j++) {
            guchar const *src = px + i * rs + j * 4;
            guint32 *dst = (guint32 *)(px_rgba + i * rs + j * 4);
            guchar r, g, b, alpha_dst;

            // calculate opacity-modified alpha
            alpha_dst = src[3];
            if (opacity != 1.0 && _vector_based_target)
                alpha_dst = (guchar)ceil((float)alpha_dst * opacity);

            // premul alpha (needed because this will be undone by cairo-pdf)
            r = src[0]*alpha_dst/255;
            g = src[1]*alpha_dst/255;
            b = src[2]*alpha_dst/255;

            *dst = (((alpha_dst) << 24) | (((r)) << 16) | (((g)) << 8) | (b));
    	}
    }

    cairo_surface_t *image_surface = cairo_image_surface_create_for_data(px_rgba, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    if (cairo_surface_status(image_surface)) {
        TRACE(("Image surface creation failed:\n%s\n", cairo_status_to_string(cairo_surface_status(image_surface))));
    	return false;
    }

    // setup automatic freeing of the image data when destroying the surface
    static cairo_user_data_key_t key;
    cairo_surface_set_user_data(image_surface, &key, px_rgba, (cairo_destroy_func_t)g_free);

    cairo_save(_cr);

    // scaling by width & height is not needed because it will be done by Cairo
    if (image_transform)
        transform(image_transform);

    cairo_set_source_surface(_cr, image_surface, 0.0, 0.0);

    // set clip region so that the pattern will not be repeated (bug in Cairo-PDF)
    if (_vector_based_target) {
        cairo_new_path(_cr);
        cairo_rectangle(_cr, 0, 0, w, h);
        cairo_clip(_cr);
    }

    if (_vector_based_target)
        cairo_paint(_cr);
    else
        cairo_paint_with_alpha(_cr, opacity);

    cairo_restore(_cr);

    cairo_surface_destroy(image_surface);

    return true;
}

#define GLYPH_ARRAY_SIZE 64

unsigned int
CairoRenderContext::_showGlyphs(cairo_t *cr, PangoFont *font, std::vector<CairoGlyphInfo> const &glyphtext, bool is_stroke)
{
    cairo_glyph_t glyph_array[GLYPH_ARRAY_SIZE];
    cairo_glyph_t *glyphs = glyph_array;
    unsigned int num_glyphs = glyphtext.size();
    if (num_glyphs > GLYPH_ARRAY_SIZE)
        glyphs = (cairo_glyph_t*)g_malloc(sizeof(cairo_glyph_t) * num_glyphs);

    unsigned int num_invalid_glyphs = 0;
    unsigned int i = 0;
    for (std::vector<CairoGlyphInfo>::const_iterator it_info = glyphtext.begin() ; it_info != glyphtext.end() ; it_info++) {
        // skip glyphs which are PANGO_GLYPH_EMPTY (0x0FFFFFFF)
        // or have the PANGO_GLYPH_UNKNOWN_FLAG (0x10000000) set
        if (it_info->index == 0x0FFFFFFF || it_info->index & 0x10000000) {
            TRACE(("INVALID GLYPH found\n"));
            num_invalid_glyphs++;
            continue;
        }
        glyphs[i - num_invalid_glyphs].index = it_info->index;
        glyphs[i - num_invalid_glyphs].x = it_info->x;
        glyphs[i - num_invalid_glyphs].y = it_info->y;
        i++;
    }

    if (is_stroke)
        cairo_glyph_path(cr, glyphs, num_glyphs - num_invalid_glyphs);
    else
        cairo_show_glyphs(cr, glyphs, num_glyphs - num_invalid_glyphs);

    if (num_glyphs > GLYPH_ARRAY_SIZE)
        g_free(glyphs);

    return num_glyphs - num_invalid_glyphs;
}

bool
CairoRenderContext::renderGlyphtext(PangoFont *font, NRMatrix const *font_matrix,
                                    std::vector<CairoGlyphInfo> const &glyphtext, SPStyle const *style)
{
    // create a cairo_font_face from PangoFont
    double size = style->font_size.computed;
    PangoFcFont *fc_font = PANGO_FC_FONT(font);
    FcPattern *fc_pattern = fc_font->font_pattern;

    cairo_save(_cr);

#ifndef PANGO_ENABLE_BACKEND
    cairo_font_face_t *font_face = cairo_ft_font_face_create_for_pattern(fc_pattern);
    cairo_set_font_face(_cr, font_face);

    if (FcPatternGetDouble(fc_pattern, FC_PIXEL_SIZE, 0, &size) != FcResultMatch)
        size = 12.0;

    // set the given font matrix
    cairo_matrix_t matrix;
    _initCairoMatrix(&matrix, font_matrix);
    cairo_set_font_matrix(_cr, &matrix);

    if (_render_mode == RENDER_MODE_CLIP) {
        if (_clip_mode == CLIP_MODE_MASK) {
            if (style->fill_rule.value == SP_WIND_RULE_EVENODD) {
                cairo_set_fill_rule(_cr, CAIRO_FILL_RULE_EVEN_ODD);
            } else {
                cairo_set_fill_rule(_cr, CAIRO_FILL_RULE_WINDING);
            }
            cairo_set_source_rgba (_cr, 1.0, 1.0, 1.0, 1.0);
            cairo_rectangle (_cr, 0, 0, 30, 40);
            cairo_fill (_cr);
            _showGlyphs(_cr, font, glyphtext, FALSE);
            //cairo_fill(_cr);
        } else {
            // just add the glyph paths to the current context
            _showGlyphs(_cr, font, glyphtext, TRUE);
        }
    } else {

        if (style->fill.type == SP_PAINT_TYPE_COLOR || style->fill.type == SP_PAINT_TYPE_PAINTSERVER) {
            // set fill style
            _setFillStyle(style, NULL);

            _showGlyphs(_cr, font, glyphtext, FALSE);
        }

        if (style->stroke.type == SP_PAINT_TYPE_COLOR || style->stroke.type == SP_PAINT_TYPE_PAINTSERVER) {
            // set stroke style
            _setStrokeStyle(style, NULL);

            // paint stroke
            _showGlyphs(_cr, font, glyphtext, TRUE);
            cairo_stroke(_cr);
        }
    }

    cairo_restore(_cr);

    cairo_font_face_destroy(font_face);
#else
    (void)size;
    (void)fc_pattern;
#endif

    return true;
}

/* Helper functions */

void
CairoRenderContext::addBpath(NArtBpath const *bp)
{
    bool closed = false;
    while (bp->code != NR_END) {
        switch (bp->code) {
            case NR_MOVETO:
                if (closed) {
                    cairo_close_path(_cr);
                }
                closed = true;
                cairo_move_to(_cr, bp->x3, bp->y3);
                break;
            case NR_MOVETO_OPEN:
                if (closed) {
                    cairo_close_path(_cr);
                }
                closed = false;
                cairo_move_to(_cr, bp->x3, bp->y3);
                break;
            case NR_LINETO:
                cairo_line_to(_cr, bp->x3, bp->y3);
                break;
            case NR_CURVETO:
                cairo_curve_to(_cr, bp->x1, bp->y1, bp->x2, bp->y2, bp->x3, bp->y3);
                break;
            default:
                break;
        }
        bp += 1;
    }
    if (closed) {
        cairo_close_path(_cr);
    }
}

void
CairoRenderContext::setBpath(NArtBpath const *bp)
{
    cairo_new_path(_cr);
    if (bp)
        addBpath(bp);
}

void
CairoRenderContext::_concatTransform(cairo_t *cr, double xx, double yx, double xy, double yy, double x0, double y0)
{
    cairo_matrix_t matrix;

    cairo_matrix_init(&matrix, xx, yx, xy, yy, x0, y0);
    cairo_transform(cr, &matrix);
}

void
CairoRenderContext::_initCairoMatrix(cairo_matrix_t *matrix, NRMatrix const *transform)
{
    matrix->xx = (*transform)[0];
    matrix->yx = (*transform)[1];
    matrix->xy = (*transform)[2];
    matrix->yy = (*transform)[3];
    matrix->x0 = (*transform)[4];
    matrix->y0 = (*transform)[5];
}

void
CairoRenderContext::_concatTransform(cairo_t *cr, NRMatrix const *transform)
{
    _concatTransform(cr, (*transform)[0], (*transform)[1],
                     (*transform)[2], (*transform)[3],
                     (*transform)[4], (*transform)[5]);
}

static cairo_status_t
_write_callback(void *closure, const unsigned char *data, unsigned int length)
{
    size_t written;
    FILE *file = (FILE*)closure;

    written = fwrite (data, 1, length, file);

    if (written == length)
	return CAIRO_STATUS_SUCCESS;
    else
	return CAIRO_STATUS_WRITE_ERROR;
}

#include "clear-n_.h"

}  /* namespace Internal */
}  /* namespace Extension */
}  /* namespace Inkscape */

#undef TRACE
#undef TEST

/* End of GNU GPL code */


/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=99 :
