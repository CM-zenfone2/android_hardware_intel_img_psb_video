/*
 * INTEL CONFIDENTIAL
 * Copyright 2007 Intel Corporation. All Rights Reserved.
 *
 * The source code contained or described herein and all documents related to
 * the source code ("Material") are owned by Intel Corporation or its suppliers
 * or licensors. Title to the Material remains with Intel Corporation or its
 * suppliers and licensors. The Material may contain trade secrets and
 * proprietary and confidential information of Intel Corporation and its
 * suppliers and licensors, and is protected by worldwide copyright and trade
 * secret laws and treaty provisions. No part of the Material may be used,
 * copied, reproduced, modified, published, uploaded, posted, transmitted,
 * distributed, or disclosed in any way without Intel's prior express written
 * permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery
 * of the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be
 * express and approved by Intel in writing.
 */

/*
 * Authors:
 *    Zhaohan Ren  <zhaohan.ren@intel.com>
 *    Shengquan Yuan  <shengquan.yuan@intel.com>
 *    Jiang Fei <jiang.fei@intel.com>
 *    Binglin Chen <binglin.chen@intel.com>
 *
 */

#include <va/va_backend.h>
#include "psb_output.h"
#include "psb_surface.h"
#include "psb_buffer.h"
#include "psb_overlay.h"
#include "psb_texture.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "psb_android_glue.h"
#include "psb_texstreaming.h"
#include "psb_output_android.h"
#include "psb_HDMIExtMode.h"
#include <wsbm/wsbm_manager.h>
#include <psb_drm.h>

#define INIT_DRIVER_DATA    psb_driver_data_p driver_data = (psb_driver_data_p) ctx->pDriverData;
#define INIT_OUTPUT_PRIV    psb_android_output_p output = (psb_android_output_p)(((psb_driver_data_p)ctx->pDriverData)->ws_priv)

#define SURFACE(id) ((object_surface_p) object_heap_lookup( &driver_data->surface_heap, id ))
#define BUFFER(id)  ((object_buffer_p) object_heap_lookup( &driver_data->buffer_heap, id ))
#define IMAGE(id)  ((object_image_p) object_heap_lookup( &driver_data->image_heap, id ))
#define SUBPIC(id)  ((object_subpic_p) object_heap_lookup( &driver_data->subpic_heap, id ))
#define CONTEXT(id) ((object_context_p) object_heap_lookup( &driver_data->context_heap, id ))

#define GET_SURFACE_INFO_rotate(psb_surface) ((int) psb_surface->extra_info[5])
#define GET_SURFACE_INFO_protect(psb_surface) ((int) psb_surface->extra_info[6])


void *psb_android_output_init(VADriverContextP ctx)
{
    INIT_DRIVER_DATA;
    char put_surface[1024];
    struct drm_psb_register_rw_arg regs;
    psb_android_output_p output = calloc(1, sizeof(psb_android_output_s));
    struct fb_var_screeninfo vinfo;
    int fbfd = -1;
    int ret;

    if (output == NULL) {
        psb__error_message("Can't malloc memory\n");
        return NULL;
    }
    memset(output, 0, sizeof(psb_android_output_s));

    /* Guess the screen size */
    output->screen_width = 800;
    output->screen_height = 480;

    // Open the frame buffer for reading
    memset(&vinfo, 0, sizeof(vinfo));
    fbfd = open("/dev/graphics/fb0", O_RDONLY);
    if (fbfd) {
        if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo))
            psb__information_message("Error reading screen information.\n");
    }
    close(fbfd);
    output->screen_width = vinfo.xres;
    output->screen_height = vinfo.yres;

    /* TS by default */
    driver_data->output_method = PSB_PUTSURFACE_TEXSTREAMING;
    driver_data->color_key = 0x0; /*black*/

    if (psb_parse_config("PSB_VIDEO_CTEXTURES", &put_surface[0]) == 0) {
        psb__information_message("PSB_VIDEO_CTEXTURES is enabled for vaPutSurfaceBuf\n");
        driver_data->ctexture = 1; /* Init CTEXTURE for vaPutSurfaceBuf */
    }

    if (psb_parse_config("PSB_VIDEO_TS", &put_surface[0]) == 0) {
        psb__information_message("Putsurface use texstreaming\n");
        driver_data->output_method = PSB_PUTSURFACE_FORCE_TEXSTREAMING;
    }

    if (psb_parse_config("PSB_VIDEO_COVERLAY", &put_surface[0]) == 0) {
        psb__information_message("Putsurface use client overlay\n");
        driver_data->output_method = PSB_PUTSURFACE_FORCE_COVERLAY;
    }

    if (IS_MFLD(driver_data)) {
        driver_data->coverlay = 1;
        output->psb_HDMIExt_info = psb_HDMIExt_init(ctx, output);
        if (!output->psb_HDMIExt_info) {
            psb__error_message("Failed to init psb_HDMIExt.\n");
            free(output);
            return NULL;
        }
    }

    return output;
}

VAStatus psb_android_output_deinit(VADriverContextP ctx)
{
    INIT_DRIVER_DATA;
    INIT_OUTPUT_PRIV;
    //psb_android_output_p output = GET_OUTPUT_DATA(ctx);
    if (IS_MFLD(driver_data)) {
        psb_HDMIExt_deinit(output);
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus psb_putsurface_ctexture(
    VADriverContextP ctx,
    VASurfaceID surface,
    unsigned char* data,
    short srcx,
    short srcy,
    unsigned short srcw,
    unsigned short srch,
    short destx,
    short desty,
    unsigned short destw,
    unsigned short desth,
    unsigned int flags /* de-interlacing flags */
)
{
    INIT_DRIVER_DATA;
    INIT_OUTPUT_PRIV;
    object_surface_p obj_surface = SURFACE(surface);
    int offset = 0;
    psb_surface_p psb_surface;

    obj_surface = SURFACE(surface);
    psb_surface = obj_surface->psb_surface;

    //    psb_surface->buf.drm_buf;
    //    psb_surface->buf.pl_flags;
    psb_putsurface_textureblit(ctx, data, surface, srcx, srcy, srcw, srch,
                               destx, desty, destw, desth, 0, /* no subtitle */
                               obj_surface->width, obj_surface->height,
                               psb_surface->stride, psb_surface->buf.drm_buf,
                               psb_surface->buf.pl_flags, 1 /* need wrap dst */);

    psb_android_postBuffer(offset);

    return VA_STATUS_SUCCESS;
}

VAStatus psb_putsurface_coverlay(
    VADriverContextP ctx,
    VASurfaceID surface,
    short srcx,
    short srcy,
    unsigned short srcw,
    unsigned short srch,
    short destx, /* screen cooridination */
    short desty,
    unsigned short destw,
    unsigned short desth,
    unsigned int flags /* de-interlacing flags */
)
{
    INIT_OUTPUT_PRIV;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    /* USE_FIT_SCR_SIZE */
    /* calculate fit screen size of frame */
    unsigned short _scr_x = output->screen_width;
    unsigned short _scr_y = output->screen_height;
    float _slope_xy = (float)srch / srcw;
    unsigned short _destw = (short)(_scr_y / _slope_xy);
    unsigned short _desth = (short)(_scr_x * _slope_xy);
    short _pos_x, _pos_y;

    if (_destw <= _scr_x) {
        _desth = _scr_y;
        _pos_x = (_scr_x - _destw) >> 1;
        _pos_y = 0;
    } else {
        _destw = _scr_x;
        _pos_x = 0;
        _pos_y = (_scr_y - _desth) >> 1;
    }
    destx += _pos_x;
    desty += _pos_y;
    destw = _destw;
    desth = _desth;

    psb__information_message("psb_putsurface_overlay: src (%d, %d, %d, %d), destx (%d, %d, %d, %d).\n",
                             srcx, srcy, srcw, srch, destx, desty, destw, desth);
    /* display by overlay */
    vaStatus = psb_putsurface_overlay(
                   ctx, surface, srcx, srcy, srcw, srch,
                   destx, desty, destw, desth, /* screen coordinate */
                   flags, OVERLAY_A, PIPEA);

    return vaStatus;
}


VAStatus psb_putsurface_ts(
    VADriverContextP ctx,
    VASurfaceID surface,
    void *android_isurface,
    short srcx,
    short srcy,
    unsigned short srcw,
    unsigned short srch,
    short destx,
    short desty,
    unsigned short destw,
    unsigned short desth,
    VARectangle *cliprects, /* client supplied clip list */
    unsigned int number_cliprects, /* number of clip rects in the clip list */
    unsigned int flags /* de-interlacing flags */
)
{
    INIT_DRIVER_DATA;
    INIT_OUTPUT_PRIV;
    object_surface_p obj_surface = SURFACE(surface);
    uint32_t ttm_handle, i;
    psb_surface_p psb_surface;

    /* blend/positioning setting can be called by app directly, or enable VA_ENABLE_BLEND flag to let driver call */
    if (flags & VA_ENABLE_BLEND)
        psb_android_texture_streaming_set_blend(destx, desty, destw, desth,
                                                driver_data->clear_color,
                                                driver_data->blend_color,
                                                driver_data->blend_mode);
    /*cropping can be also used for dynamic resolution change feature, only high to low resolution*/
    /*by default, srcw and srch is set to video width and height*/
    if ((0 == srcw) || (0 == srch)) {
        srcw = obj_surface->width;
        srch = obj_surface->height_origin;
    }
    psb_android_texture_streaming_set_texture_dim(srcw, srch);

#if 0
    /* use cliprect for crop */
    if (cliprects && (number_cliprects == 1))
        psb_android_texture_streaming_set_crop(cliprects->x, cliprects->y, cliprects->width, cliprects->height);
#endif

    psb_surface = obj_surface->psb_surface;
    ttm_handle = (uint32_t)(wsbmKBufHandle(wsbmKBuf(psb_surface->buf.drm_buf)));

    for (i = 0; i < driver_data->bcd_buffer_num; i++) {
        if (driver_data->bcd_ttm_handles[i] == ttm_handle)
            break;
    }
    if (i == driver_data->bcd_buffer_num) {
        psb__error_message("Failed to get buffer index.\n");
        return VA_STATUS_ERROR_UNKNOWN;
    }

    psb_android_texture_streaming_display(i);
    output->colorkey_dirty = 1;

    return VA_STATUS_SUCCESS;
}

static int psb_update_destbox(
    VADriverContextP ctx
)
{
    INIT_DRIVER_DATA;
    INIT_OUTPUT_PRIV;
    short destx;
    short desty;
    unsigned short destw;
    unsigned short desth;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    psb_android_get_destbox(&destx, &desty, &destw, &desth);
    psb__information_message("destbox = (%d,%d,%d,%d)\n", destx, desty, destw, desth);
    if ((destx >= 0) && (desty >= 0) &&
            ((destx + destw) <= output->screen_width) &&
            ((desty + desth) <= output->screen_height) &&
            (output->destx != destx ||
             output->desty != desty ||
             output->destw != destw ||
             output->desth != desth)) {
        output->destx = destx;
        output->desty = desty;
        output->destw = destw;
        output->desth = desth;

        /*destbox is ready. Set the new rotation for overlay*/
        driver_data->local_rotation = driver_data->msvdx_rotate_want;
        if (driver_data->local_rotation == 3)
            driver_data->local_rotation = VA_ROTATION_270;

        LOGD("==========New Destbox=============\n");
        LOGD("output->destbox = (%d,%d,%d,%d)\n", output->destx, output->desty, output->destw, output->desth);
    }

    /*Use default destbox if can't get destbox from surfaceflinger.*/
    if (output->destw == 0 || output->desth == 0) {
        output->destx = destx;
        output->desty = desty;
        output->destw = (destw > output->screen_width) ? output->screen_width : destw;
        output->desth = (desth > output->screen_height) ? output->screen_height : desth;
    }
    return vaStatus;
}

static int psb_check_outputmethod(
    VADriverContextP ctx,
    VASurfaceID surface,
    unsigned short srcw,
    unsigned short srch,
    void *android_isurface,
    psb_hdmi_mode *hdmi_mode
)
{
    INIT_DRIVER_DATA;
    INIT_OUTPUT_PRIV;
    psb_HDMIExt_info_p psb_HDMIExt_info = (psb_HDMIExt_info_p)output->psb_HDMIExt_info;
    object_surface_p obj_surface;
    int rotation;

    if ((srcw >= 2048) || (srch >= 2048)) {
        psb__information_message("Clip size extend overlay hw limit, use texstreaming\n");
        driver_data->output_method = PSB_PUTSURFACE_TEXSTREAMING;
        return 0;
    }

    /* use saved status to avoid per-frame checking */
    if ((driver_data->frame_count % driver_data->outputmethod_checkinterval) != 0) {
        *hdmi_mode = psb_HDMIExt_get_mode(output);
        return 0;
    }

    /* check the status at outputmethod_checkinterval frequency */
    /* at first check HDMI status */
    if (psb_HDMIExt_update(ctx, psb_HDMIExt_info)) {
        psb__error_message("%s: Failed to update HDMIExt info.\n", __FUNCTION__);
        return -1;
    }

    *hdmi_mode = psb_HDMIExt_get_mode(output);
    psb__information_message("hdmi_mode = %d\n", *hdmi_mode);
    if (*hdmi_mode != OFF) {
        psb_HDMIExt_get_prop(output, &driver_data->render_rect.width, &driver_data->render_rect.height,
                             &driver_data->render_rect.x, &driver_data->render_rect.y);
        psb__information_message("Render Rect: (%d,%d,%d,%d)\n",
                                 driver_data->render_rect.x, driver_data->render_rect.y,
                                 driver_data->render_rect.width, driver_data->render_rect.height);
    }

    if ((*hdmi_mode == EXTENDED_VIDEO) || (*hdmi_mode == CLONE)
            || (driver_data->output_method == PSB_PUTSURFACE_FORCE_TEXSTREAMING)
            || (driver_data->output_method == PSB_PUTSURFACE_FORCE_COVERLAY))
        return 0;

    /* HDMI is not enabled */
    psb_android_surfaceflinger_status(android_isurface, &output->sf_composition, &rotation);

    /* only care local rotation */
    if (driver_data->msvdx_rotate_want != rotation) {
        psb__information_message("New rotation degree %d\n", rotation);
        driver_data->msvdx_rotate_want = rotation;
    }

    obj_surface = SURFACE(surface);
    if (GET_SURFACE_INFO_protect(obj_surface->psb_surface)) {
        psb__information_message("Protected surface, use overlay\n");
        driver_data->output_method = PSB_PUTSURFACE_COVERLAY;

        return 0;
    }

    if (output->sf_composition) {
        psb__information_message("Composition is detected, use texstreaming\n");
        driver_data->output_method = PSB_PUTSURFACE_TEXSTREAMING;
        return 0;
    }

    if (rotation != 0) {
        int srf_rotate = GET_SURFACE_INFO_rotate(obj_surface->psb_surface);
        if (srf_rotate == 0) { /* no rotated surface */
            psb__information_message("Rotation degree %d, but no MSVDX rotate\n", rotation);
            driver_data->output_method = PSB_PUTSURFACE_TEXSTREAMING;
            return 0;
        }
    }

    psb__information_message("Rotation degree %d, use overlay\n", rotation);
    driver_data->output_method = PSB_PUTSURFACE_COVERLAY;

    return 0;
}

VAStatus psb_PutSurface(
    VADriverContextP ctx,
    VASurfaceID surface,
    void *android_isurface,
    short srcx,
    short srcy,
    unsigned short srcw,
    unsigned short srch,
    short destx,
    short desty,
    unsigned short destw,
    unsigned short desth,
    VARectangle *cliprects, /* client supplied clip list */
    unsigned int number_cliprects, /* number of clip rects in the clip list */
    unsigned int flags /* de-interlacing flags */
)
{
    INIT_DRIVER_DATA;
    INIT_OUTPUT_PRIV;
    object_surface_p obj_surface;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    PsbPortPrivPtr pPriv = (PsbPortPrivPtr)(&driver_data->coverlay_priv);
    psb_hdmi_mode hdmi_mode = OFF;
    int sf_composition = 0;

    obj_surface = SURFACE(surface);
    if (NULL == obj_surface) {
        vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
        DEBUG_FAILURE;
        return vaStatus;
    }

    if((NULL == cliprects) && (0 != number_cliprects)){
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
        DEBUG_FAILURE;
        return vaStatus;
    }

    if ((srcx < 0) || (srcx > obj_surface->width) || (srcw > (obj_surface->width - srcx)) ||
            (srcy < 0) || (srcy > obj_surface->height_origin) || (srch > (obj_surface->height_origin - srcy))) {
        psb__error_message("vaPutSurface: source rectangle passed from upper layer is not correct.\n");
        return VA_STATUS_ERROR_UNKNOWN;
    }
    if ((destx < 0) || (desty < 0)) {
        psb__error_message("vaPutSurface: dest rectangle passed from upper layer is not correct.\n");
        return VA_STATUS_ERROR_UNKNOWN;
    }

    if (driver_data->dummy_putsurface) {
        psb__information_message("vaPutSurface: dummy mode, return directly\n");
        return VA_STATUS_SUCCESS;
    }

    /* exit MRST path at first */
    if (IS_MRST(driver_data)) {
        if (driver_data->output_method == PSB_PUTSURFACE_FORCE_COVERLAY) { /* overlay is for testing, not POR */
            psb__information_message("Force overlay to display\n");
            vaStatus = psb_putsurface_coverlay(ctx, surface,
                                               srcx, srcy, srcw, srch,
                                               destx, desty, destw, desth,
                                               flags);
        } else {
            psb__information_message("Use texstreaming to display.\n");
            vaStatus = psb_putsurface_ts(ctx, surface, android_isurface,
                                         srcx, srcy, srcw, srch,
                                         destx, desty, destw, desth,
                                         cliprects, number_cliprects, /* number of clip rects in the clip list */
                                         flags);
        }

        return vaStatus;
    }

    if (psb_android_register_isurface(android_isurface, driver_data->bcd_id, srcw, srch)) {
        psb__error_message("In psb_PutSurface, android_isurface is not a valid isurface object.\n");
        return VA_STATUS_ERROR_UNKNOWN;
    }

    /* time for MFLD platform */
    psb_check_outputmethod(ctx, surface, srcw, srch, android_isurface, &hdmi_mode);

    /* Extvideo: Use overlay to render external HDMI display */
    if (hdmi_mode == EXTENDED_VIDEO) {
        psb__information_message("HDMI: ExtVideo mode enabled, use overlay to render external HDMI display.\n");
        /*we also need to clear local display if colorkey dirty.*/
        if (output->colorkey_dirty) {
            psb_android_texture_streaming_display(-1);
            output->colorkey_dirty = 0;
        }
        vaStatus = psb_putsurface_overlay(ctx, surface,
                                          srcx, srcy, srcw, srch,
                                          driver_data->render_rect.x, driver_data->render_rect.y,
                                          driver_data->render_rect.width, driver_data->render_rect.height,
                                          flags, OVERLAY_A, PIPEB);

        return vaStatus;
    }

    /* Clone mode: Use TS to render both MIPI and HDMI display */
    if (hdmi_mode == CLONE) {
        psb__information_message("HDMI: Clone mode enabled, use texsteaming for both devices\n");
        vaStatus = psb_putsurface_ts(ctx, surface, android_isurface,
                                     srcx, srcy, srcw, srch,
                                     destx, desty, destw, desth,
                                     cliprects, number_cliprects, /* number of clip rects in the clip list */
                                     flags);
        return vaStatus;
    }

    /* local video playback */
    if ((driver_data->output_method == PSB_PUTSURFACE_TEXSTREAMING) ||
            (driver_data->output_method == PSB_PUTSURFACE_FORCE_TEXSTREAMING)) {
        psb__information_message("MIPI: Use texstreaming to display.\n");

        vaStatus = psb_putsurface_ts(ctx, surface, android_isurface,
                                     srcx, srcy, srcw, srch,
                                     destx, desty, destw, desth,
                                     cliprects, number_cliprects, /* number of clip rects in the clip list */
                                     flags);
    } else {
        psb__information_message("MIPI: Use overlay to display.\n");
        psb_update_destbox(ctx);

        /* Hack for repaint color key to black(0,0,0). */
        if (output->colorkey_dirty) {
            psb_android_texture_streaming_display(-1);
            output->colorkey_dirty = 0;
            psb__information_message("Hack for repainting color key for overlay.Ignore the error message \"Texture Streaming Fails...\".\n");
        }

        /* Use overlay to render local display*/
        if (destw > output->screen_width)
            destw = output->screen_width;
        if (desth > output->screen_height)
            desth = output->screen_height;

        vaStatus = psb_putsurface_overlay(ctx, surface,
                                          srcx, srcy, srcw, srch,
                                          output->destx, output->desty, output->destw, output->desth,
                                          flags, OVERLAY_A, PIPEA);
    }

    driver_data->frame_count++;

    return vaStatus;
}
