///
///	@file video.c	@brief Video module
///
///	Copyright (c) 2009 - 2015 by Johns.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
///	$Id$
//////////////////////////////////////////////////////////////////////////////

///
///	@defgroup Video The video module.
///
///	This module contains all video rendering functions.
///
///	@todo disable screen saver support
///
///	Uses Xlib where it is needed for VA-API or vdpau or cuvid.  XCB is used for
///	everything else.
///
///	- X11
///	- OpenGL rendering
///	- OpenGL rendering with GLX texture-from-pixmap
///	- Xrender rendering
///
///	@todo FIXME: use vaErrorStr for all VA-API errors.
///

#define USE_XLIB_XCB			///< use xlib/xcb backend
#define noUSE_SCREENSAVER		///< support disable screensaver
#define USE_AUTOCROP			///< compile auto-crop support
#define USE_GRAB			///< experimental grab code
#define USE_DOUBLEBUFFER		///< use GLX double buffers
//#define USE_VAAPI				///< enable vaapi support
//#define USE_VDPAU				///< enable vdpau support
//#define USE_BITMAP			///< use vdpau bitmap surface
//#define AV_INFO				///< log a/v sync informations
#ifndef AV_INFO_TIME
#define AV_INFO_TIME (50 * 60)		///< a/v info every minute
#endif

#define USE_VIDEO_THREAD		///< run decoder in an own thread
//#define USE_VIDEO_THREAD2		///< run decoder+display in own threads

#include <sys/time.h>
#include <sys/shm.h>
#include <sys/ipc.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#ifdef USE_VIDEO_THREAD
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>
#include <time.h>
#include <signal.h>
#ifndef HAVE_PTHREAD_NAME
    /// only available with newer glibc
#define pthread_setname_np(thread, name)
#endif
#endif

#ifdef USE_XLIB_XCB
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>
//#include <xcb/bigreq.h>
#ifdef xcb_USE_GLX
#include <xcb/glx.h>
#endif
//#include <xcb/randr.h>
#ifdef USE_SCREENSAVER
#include <xcb/screensaver.h>
#include <xcb/dpms.h>
#endif
//#include <xcb/shm.h>
//#include <xcb/xv.h>

//#include <xcb/xcb_image.h>
//#include <xcb/xcb_event.h>
//#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#ifdef XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS
#include <xcb/xcb_ewmh.h>
#else // compatibility hack for old xcb-util

/**
 * @brief Action on the _NET_WM_STATE property
 */
typedef enum
{
    /* Remove/unset property */
    XCB_EWMH_WM_STATE_REMOVE = 0,
    /* Add/set property */
    XCB_EWMH_WM_STATE_ADD = 1,
    /* Toggle property	*/
    XCB_EWMH_WM_STATE_TOGGLE = 2
} xcb_ewmh_wm_state_action_t;
#endif
#endif

#ifdef USE_GLX
#define GL_GLEXT_PROTOTYPES
#define GLX_GLXEXT_PROTOTYPES
#include <GL/gl.h>			// For GL_COLOR_BUFFER_BIT
#include <GL/glx.h>
// only for gluErrorString
#include <GL/glu.h>
#include <GL/glext.h>
#include <GL/glxext.h>
#endif

#ifdef USE_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
//#include <EGL/eglplatform.h>
//#include <GLES2/gl2.h>
//#include <GLES2/gl2ext.h>
#endif

#ifdef USE_VAAPI
#include <va/va_x11.h>
#if VA_CHECK_VERSION(0,33,99)
#include <va/va_vpp.h>
#endif
#ifndef VAAPI_NO_GLX
#ifdef USE_GLX
#include <va/va_glx.h>
#endif
#endif
#ifndef VA_SURFACE_ATTRIB_SETTABLE
/// make source compatible with stable libva
#define vaCreateSurfaces(d, f, w, h, s, ns, a, na) \
    vaCreateSurfaces(d, w, h, f, ns, s)
#endif
#ifdef USE_EGL
#include <va/va_drmcommon.h>
#endif
#endif

#ifdef USE_VDPAU
#include <vdpau/vdpau_x11.h>
#include <libavcodec/vdpau.h>
#include <libavutil/hwcontext_vdpau.h>
#endif

#ifdef USE_CUVID
#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_loader.h>
#include <libavutil/hwcontext_cuda.h>
#endif

#include <libavcodec/avcodec.h>
#ifdef USE_AVFILTER
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#endif
#include <libavutil/opt.h>
#ifdef USE_SWSCALE
#include <libswscale/swscale.h>
#endif

// support old ffmpeg versions <1.0
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,18,102)
#define AVCodecID CodecID
#define AV_CODEC_ID_H263 CODEC_ID_H263
#define AV_CODEC_ID_H264 CODEC_ID_H264
#define AV_CODEC_ID_MPEG1VIDEO CODEC_ID_MPEG1VIDEO
#define AV_CODEC_ID_MPEG2VIDEO CODEC_ID_MPEG2VIDEO
#define AV_CODEC_ID_MPEG4 CODEC_ID_MPEG4
#define AV_CODEC_ID_VC1 CODEC_ID_VC1
#define AV_CODEC_ID_WMV3 CODEC_ID_WMV3
#endif
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(55,50,100)
#include <libavutil/imgutils.h>
#endif
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#ifdef USE_VAAPI
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,74,100)
#include <libavcodec/vaapi.h>
#else
#include <libavutil/hwcontext_vaapi.h>
#endif
#endif

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(54,86,100) && \
    LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56,60,100)
    ///
    /// ffmpeg version 1.1.1 calls get_format with zero width and height
    /// for H264 codecs.
    /// since version 1.1.3 get_format is called twice.
    /// ffmpeg 1.2 still buggy
    ///
#define FFMPEG_BUG1_WORKAROUND		///< get_format bug workaround
#endif

#include "iatomic.h"			// portable atomic_t
#include "misc.h"
#include "video.h"
#include "audio.h"
#include "codec.h"

#define ARRAY_ELEMS(array) (sizeof(array)/sizeof(array[0]))

#ifdef USE_XLIB_XCB

//----------------------------------------------------------------------------
//	Declarations
//----------------------------------------------------------------------------

///
///	Video deinterlace modes.
///
typedef enum _video_deinterlace_modes_
{
    VideoDeinterlaceBob,		///< bob deinterlace
    VideoDeinterlaceWeave,		///< weave deinterlace
    VideoDeinterlaceTemporal,		///< temporal deinterlace
    VideoDeinterlaceTemporalSpatial,	///< temporal spatial deinterlace
    VideoDeinterlaceSoftBob,		///< software bob deinterlace
    VideoDeinterlaceSoftSpatial,	///< software spatial deinterlace
} VideoDeinterlaceModes;

///
///	Video scaleing modes.
///
typedef enum _video_scaling_modes_
{
    VideoScalingNormal,			///< normal scaling
    VideoScalingFast,			///< fastest scaling
    VideoScalingHQ,			///< high quality scaling
    VideoScalingAnamorphic,		///< anamorphic scaling
} VideoScalingModes;

///
///	Video zoom modes.
///
typedef enum _video_zoom_modes_
{
    VideoNormal,			///< normal
    VideoStretch,			///< stretch to all edges
    VideoCenterCutOut,			///< center and cut out
    VideoAnamorphic,			///< anamorphic scaled (unsupported)
} VideoZoomModes;

///
///	Video color space conversions.
///
typedef enum _video_color_space_
{
    VideoColorSpaceNone,		///< no conversion
    VideoColorSpaceBt601,		///< ITU.BT-601 Y'CbCr
    VideoColorSpaceBt709,		///< ITU.BT-709 HDTV Y'CbCr
    VideoColorSpaceSmpte240		///< SMPTE-240M Y'PbPr
} VideoColorSpace;

///
///	Video output module structure and typedef.
///
typedef struct _video_module_
{
    const char *Name;			///< video output module name
    char Enabled;			///< flag output module enabled

    /// allocate new video hw decoder
    VideoHwDecoder *(*const NewHwDecoder)(VideoStream *);
    void (*const DelHwDecoder) (VideoHwDecoder *);
    unsigned (*const GetSurface) (VideoHwDecoder *, const AVCodecContext *);
    void (*const ReleaseSurface) (VideoHwDecoder *, unsigned);
    void (*const UnregisterSurface) (VideoHwDecoder *);
    enum AVPixelFormat (*const get_format) (VideoHwDecoder *, AVCodecContext *,
	const enum AVPixelFormat *);
    void (*const RenderFrame) (VideoHwDecoder *, const AVCodecContext *,
	const AVFrame *);
    void *(*const GetHwAccelContext)(VideoHwDecoder *);
    void (*const SetClock) (VideoHwDecoder *, int64_t);
     int64_t(*const GetClock) (const VideoHwDecoder *);
    void (*const SetClosing) (const VideoHwDecoder *);
    void (*const ResetStart) (const VideoHwDecoder *);
    void (*const SetTrickSpeed) (const VideoHwDecoder *, int);
    uint8_t *(*const GrabOutput)(int *, int *, int *);
    void (*const GetStats) (VideoHwDecoder *, int *, int *, int *, int *, int *);
    void (*const SetBackground) (uint32_t);
    void (*const SetVideoMode) (void);
    void (*const ResetAutoCrop) (void);

    /// module display handler thread
    void (*const DisplayHandlerThread) (void);

    void (*const OsdClear) (void);	///< clear OSD
    /// draw OSD ARGB area
    void (*const OsdDrawARGB) (int, int, int, int, int, const uint8_t *, int,
	int);
    void (*const OsdInit) (int, int);	///< initialize OSD
    void (*const OsdExit) (void);	///< cleanup OSD
    int (*const MaxPixmapSize) (void);

    int (*const Init) (const char *);	///< initialize video output module
    void (*const Exit) (void);		///< cleanup video output module
} VideoModule;

///
///     Video configuration values typedef.
///
typedef struct _video_config_values_
{
    int   active;
    float min_value;
    float max_value;
    float def_value;
    float step;
    float scale;     // scale is normalized to match UI requirements
    float drv_scale; // re-normalizing requires the original scale required for latching data to the driver
} VideoConfigValues;

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

#define CODEC_SURFACES_MAX	31	///< maximal of surfaces

#define CODEC_SURFACES_DEFAULT	21	///< default of surfaces
// FIXME: video-xvba only supports 14
#define xCODEC_SURFACES_DEFAULT	14	///< default of surfaces

#define CODEC_SURFACES_MPEG2	3	///< 1 decode, up to  2 references
#define CODEC_SURFACES_MPEG4	3	///< 1 decode, up to  2 references
#define CODEC_SURFACES_H264	21	///< 1 decode, up to 20 references
#define CODEC_SURFACES_VC1	3	///< 1 decode, up to  2 references

#define VIDEO_SURFACES_MAX	4	///< video output surfaces for queue
#define POSTPROC_SURFACES_MAX	8	///< video postprocessing surfaces for queue
#define FIELD_SURFACES_MAX	POSTPROC_SURFACES_MAX / 2	///< video postprocessing surfaces for queue
#define OUTPUT_SURFACES_MAX	4	///< output surfaces for flip page

#define MAX_PIXMAP_SIZE_VAAPI	8192
#define MAX_PIXMAP_SIZE_VDPAU	8192

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

#ifdef USE_VDPAU
static VideoConfigValues VdpauConfigBrightness =
{ .active = 1, .min_value = -1000.0, .max_value = 1000.0, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues VdpauConfigContrast =
{ .active = 1, .min_value = 0.0, .max_value = 10000.0, .def_value = 1000.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues VdpauConfigSaturation =
{ .active = 1, .min_value = 0.0, .max_value = 10000.0, .def_value = 1000.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues VdpauConfigHue =
{ .active = 1, .min_value = -1000.0 * M_PI, .max_value = 1000.0 * M_PI, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues VdpauConfigDenoise =
{ .active = 1, .min_value = 0.0, .max_value = 1000.0, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues VdpauConfigSharpen =
{ .active = 1, .min_value = -1000.0, .max_value = 1000.0, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues VdpauConfigStde =
{ .active = 0, .min_value = 0.0, .max_value = 1.0, .def_value = 1.0, .step = 1.0, .scale = 1.0, .drv_scale = 1.0 };
#endif

#ifdef USE_VAAPI
// Brightness (-100.00 - 100.00 ++ 1.00 = 0.00)
static VideoConfigValues VaapiConfigBrightness =
{ .active = 0, .min_value = -100.0, .max_value = 100.0, .def_value = 0.0, .step = 1.0, .scale = 1.0, .drv_scale = 1.0 };

// Contrast (0.00 - 10.00 ++ 0.10 = 1.00)
static VideoConfigValues VaapiConfigContrast =
{ .active = 0, .min_value = 0.0, .max_value = 10.0, .def_value = 1.0, .step = 0.1, .scale = 1.0, .drv_scale = 1.0 };

// Saturation (0.00 - 10.00 ++ 0.10 = 1.00)
static VideoConfigValues VaapiConfigSaturation =
{ .active = 0, .min_value = 0.0, .max_value = 10.0, .def_value = 1.0, .step = 0.1, .scale = 1.0, .drv_scale = 1.0 };

// Hue (-180.00 - 180.00 ++ 1.00 = 0.00)
static VideoConfigValues VaapiConfigHue =
{ .active = 0, .min_value = -180.0, .max_value = 180.0, .def_value = 0.0, .step = 1.0, .scale = 1.0, .drv_scale = 1.0 };

// Denoise (0.00 - 1.00 ++ 0.03 = 0.50)
static VideoConfigValues VaapiConfigDenoise =
{ .active = 0, .min_value = 0.0, .max_value = 1.0, .def_value = 0.5, .step = 0.03, .scale = 1.0, .drv_scale = 1.0 };

// Sharpen (0.00 - 1.00 ++ 0.03 = 0.50)
static VideoConfigValues VaapiConfigSharpen =
{ .active = 0, .min_value = 0.0, .max_value = 1.0, .def_value = 0.5, .step = 0.03, .scale = 1.0, .drv_scale = 1.0 };

static VideoConfigValues VaapiConfigStde =
{ .active = 1, .min_value = 0.0, .max_value = 4.0, .def_value = 0.0, .step = 1.0, .scale = 1.0, .drv_scale = 1.0 };
#endif

#ifdef USE_CUVID
static VideoConfigValues CuvidConfigBrightness =
{ .active = 1, .min_value = -1000.0, .max_value = 1000.0, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues CuvidConfigContrast =
{ .active = 1, .min_value = 0.0, .max_value = 10000.0, .def_value = 1000.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues CuvidConfigSaturation =
{ .active = 1, .min_value = 0.0, .max_value = 10000.0, .def_value = 1000.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues CuvidConfigHue =
{ .active = 1, .min_value = -1000.0 * M_PI, .max_value = 1000.0 * M_PI, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues CuvidConfigDenoise =
{ .active = 1, .min_value = 0.0, .max_value = 1000.0, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues CuvidConfigSharpen =
{ .active = 1, .min_value = -1000.0, .max_value = 1000.0, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues CuvidConfigStde =
{ .active = 0, .min_value = 0.0, .max_value = 1.0, .def_value = 1.0, .step = 1.0, .scale = 1.0, .drv_scale = 1.0 };


static VideoConfigValues NVdecConfigBrightness =
{ .active = 1, .min_value = -1000.0, .max_value = 1000.0, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues NVdecConfigContrast =
{ .active = 1, .min_value = 0.0, .max_value = 10000.0, .def_value = 1000.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues NVdecConfigSaturation =
{ .active = 1, .min_value = 0.0, .max_value = 10000.0, .def_value = 1000.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues NVdecConfigHue =
{ .active = 1, .min_value = -1000.0 * M_PI, .max_value = 1000.0 * M_PI, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues NVdecConfigDenoise =
{ .active = 1, .min_value = 0.0, .max_value = 1000.0, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues NVdecConfigSharpen =
{ .active = 1, .min_value = -1000.0, .max_value = 1000.0, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues NVdecConfigStde =
{ .active = 0, .min_value = 0.0, .max_value = 1.0, .def_value = 1.0, .step = 1.0, .scale = 1.0, .drv_scale = 1.0 };

#endif

#if defined USE_GLX || defined USE_EGL
static VideoConfigValues CpuConfigBrightness =
{ .active = 1, .min_value = -1000.0, .max_value = 1000.0, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues CpuConfigContrast =
{ .active = 1, .min_value = 0.0, .max_value = 10000.0, .def_value = 1000.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues CpuConfigSaturation =
{ .active = 1, .min_value = 0.0, .max_value = 10000.0, .def_value = 1000.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues CpuConfigHue =
{ .active = 1, .min_value = -1000.0 * M_PI, .max_value = 1000.0 * M_PI, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues CpuConfigDenoise =
{ .active = 1, .min_value = 0.0, .max_value = 1000.0, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues CpuConfigSharpen =
{ .active = 1, .min_value = -1000.0, .max_value = 1000.0, .def_value = 0.0, .step = 1.0, .scale = 0.001, .drv_scale = 1.0 };

static VideoConfigValues CpuConfigStde =
{ .active = 0, .min_value = 0.0, .max_value = 1.0, .def_value = 1.0, .step = 1.0, .scale = 1.0, .drv_scale = 1.0 };
#endif


char VideoIgnoreRepeatPict;		///< disable repeat pict warning

static const char *VideoDriverName;	///< video output device
static Display *XlibDisplay;		///< Xlib X11 display
static xcb_connection_t *Connection;	///< xcb connection
static xcb_colormap_t VideoColormap;	///< video colormap
static xcb_window_t VideoWindow;	///< video window
static xcb_screen_t const *VideoScreen;	///< video screen
static uint32_t VideoBlankTick;		///< blank cursor timer
static xcb_pixmap_t VideoCursorPixmap;	///< blank curosr pixmap
static xcb_cursor_t VideoBlankCursor;	///< empty invisible cursor

static int VideoWindowX;		///< video output window x coordinate
static int VideoWindowY;		///< video outout window y coordinate
static unsigned VideoWindowWidth;	///< video output window width
static unsigned VideoWindowHeight;	///< video output window height
static unsigned VideoScreenWidth;	///< video screen width
static unsigned VideoScreenHeight;	///< video screen height
static char VideoGeometry[25];

static const VideoModule NoopModule;	///< forward definition of noop module

    /// selected video module
static const VideoModule *VideoUsedModule = &NoopModule;

enum VideoHardwareDecoderMode VideoHardwareDecoder = HWOn;	///< flag use hardware decoder

static char VideoSurfaceModesChanged;	///< flag surface modes changed

static uint32_t VideoBackground;	///< video background color
static char VideoStudioLevels;		///< flag use studio levels

    /// Default skin tone enhancement mode.
static int VideoSkinToneEnhancement = 0;

    /// Default deinterlace mode.
static VideoDeinterlaceModes VideoDeinterlace[VideoResolutionMax];
#ifdef USE_VDPAU
    /// flag use transparent OSD.
static const char VideoTransparentOsd = 1;

    /// Default number of deinterlace surfaces
static const int VideoDeinterlaceSurfaces = 4;
#endif
    /// Default skip chroma deinterlace flag (VDPAU only).
static char VideoSkipChromaDeinterlace[VideoResolutionMax];

    /// Default inverse telecine flag (VDPAU only).
static char VideoInverseTelecine[VideoResolutionMax];

    /// Default amount of noise reduction algorithm to apply (0 .. 1000).
static int VideoDenoise[VideoResolutionMax];

    /// Default amount of sharpening, or blurring, to apply (-1000 .. 1000).
static int VideoSharpen[VideoResolutionMax];

    /// Default cut top and bottom in pixels
static int VideoCutTopBottom[VideoResolutionMax];

    /// Default cut left and right in pixels
static int VideoCutLeftRight[VideoResolutionMax];

    /// Default field ordering for first field
static int VideoFirstField[VideoResolutionMax];

    /// Default field ordering for second field
static int VideoSecondField[VideoResolutionMax];

    /// Color space ITU-R BT.601, ITU-R BT.709, ...
static const VideoColorSpace VideoColorSpaces[VideoResolutionMax] = {
    VideoColorSpaceBt601, VideoColorSpaceBt709, VideoColorSpaceBt709,
    VideoColorSpaceBt709, VideoColorSpaceBt709
};

    /// Default scaling mode
static VideoScalingModes VideoScaling[VideoResolutionMax];

    /// Default audio/video delay
int VideoAudioDelay;

    /// Default zoom mode for 4:3
static VideoZoomModes Video4to3ZoomMode;

    /// Default zoom mode for 16:9 and others
static VideoZoomModes VideoOtherZoomMode;

static char Video60HzMode;		///< handle 60hz displays
volatile char VideoSoftStartSync;		///< soft start sync audio/video
static const int VideoSoftStartFrames = 100;	///< soft start frames
static char VideoShowBlackPicture;	///< flag show black picture

static xcb_atom_t WmDeleteWindowAtom;	///< WM delete message atom
static xcb_atom_t NetWmState;		///< wm-state message atom
static xcb_atom_t NetWmStateFullscreen;	///< fullscreen wm-state message atom

#ifdef DEBUG
extern uint32_t VideoSwitch;		///< ticks for channel switch
#endif
extern void AudioVideoReady(int64_t);	///< tell audio video is ready
extern int IsReplay(void);

#ifdef USE_VIDEO_THREAD

static pthread_t VideoThread;		///< video decode thread
static pthread_cond_t VideoWakeupCond;	///< wakeup condition variable
static pthread_mutex_t VideoMutex;	///< video condition mutex
static pthread_mutex_t VideoLockMutex;	///< video lock mutex

#endif

#ifdef USE_VIDEO_THREAD2

static pthread_t VideoDisplayThread;	///< video decode thread
static pthread_cond_t VideoWakeupCond;	///< wakeup condition variable
static pthread_mutex_t VideoDisplayMutex;	///< video condition mutex
static pthread_mutex_t VideoDisplayLockMutex;	///< video lock mutex

#endif

static int OsdConfigWidth;		///< osd configured width
static int OsdConfigHeight;		///< osd configured height
static char OsdShown;			///< flag show osd
static char Osd3DMode;			///< 3D OSD mode
static int OsdWidth;			///< osd width
static int OsdHeight;			///< osd height
static int OsdDirtyX;			///< osd dirty area x
static int OsdDirtyY;			///< osd dirty area y
static int OsdDirtyWidth;		///< osd dirty area width
static int OsdDirtyHeight;		///< osd dirty area height

#ifdef USE_OPENGLOSD
static int OsdNeedRestart = 0;		/// osd restart flag for openglosd, use for VDPAU
#endif

static int64_t VideoDeltaPTS;		///< FIXME: fix pts

#ifdef USE_SCREENSAVER
static char DPMSDisabled;		///< flag we have disabled dpms
static char EnableDPMSatBlackScreen;	///< flag we should enable dpms at black screen
static char DisableScreensaver;		///< flag we disable screensaver
#endif

extern volatile char AudioRunning;
extern volatile char AudioStarted;
extern pthread_cond_t AudioStartCond;	///< condition variable
volatile int EnoughVideo;
extern volatile int EnoughAudio;
volatile VideoResolutions VideoResolution;
static int VideoStartThreshold_SD = 16;
void AudioDelayms(int);
//----------------------------------------------------------------------------
//	Common Functions
//----------------------------------------------------------------------------

static void VideoThreadLock(void);	///< lock video thread
static void VideoThreadUnlock(void);	///< unlock video thread
static void VideoThreadExit(void);	///< exit/kill video thread

#ifdef USE_SCREENSAVER
static void X11SuspendScreenSaver(xcb_connection_t *, int);
static int X11HaveDPMS(xcb_connection_t *);
static void X11DPMSReenable(xcb_connection_t *);
static void X11DPMSDisable(xcb_connection_t *);
#endif

///
///	Update video pts.
///
///	@param pts_p		pointer to pts
///	@param interlaced	interlaced flag (frame isn't right)
///	@param frame		frame to display
///
///	@note frame->interlaced_frame can't be used for interlace detection
///
static void VideoSetPts(int64_t * pts_p, int interlaced,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    int64_t pts;
    int duration;
    static int64_t lastpts;

    //
    //	Get duration for this frame.
    //	FIXME: using framerate as workaround for av_frame_get_pkt_duration
    //
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56,13,100)
    // version for older ffmpeg without framerate
    if (video_ctx->time_base.num && video_ctx->time_base.den) {
	duration =
	    (video_ctx->ticks_per_frame * 1000 * video_ctx->time_base.num) /
	    video_ctx->time_base.den;
    } else {
	duration = interlaced ? 40 : 20;	// 50Hz -> 20ms default
    }
    Debug(4, "video: %d/%d %" PRIx64 " -> %d\n", video_ctx->time_base.den,
	video_ctx->time_base.num, av_frame_get_pkt_duration(frame), duration);
#else
    if (video_ctx->framerate.num && video_ctx->framerate.den) {
	duration = 1000 * video_ctx->framerate.den / video_ctx->framerate.num;
    } else {
	duration = interlaced ? 40 : 20;	// 50Hz -> 20ms default
    }
    Debug(4, "video: %d/%d %" PRIx64 " -> %d\n", video_ctx->framerate.den,
	video_ctx->framerate.num,
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(57,30,100)
	frame->pkt_duration,
#else
	frame->duration,
#endif
	duration);
#endif
    // update video clock
    if (*pts_p != (int64_t) AV_NOPTS_VALUE) {
	*pts_p += duration * 90;
	//Info("video: %s +pts\n", Timestamp2String(*pts_p));
    }
    //av_opt_ptr(avcodec_get_frame_class(), frame, "best_effort_timestamp");
    //pts = frame->best_effort_timestamp;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,61,100)
    pts = frame->pkt_pts;
#else
    pts = frame->pts;
#endif
    if (pts == (int64_t) AV_NOPTS_VALUE || !pts) {
	// libav: 0.8pre didn't set pts
	pts = frame->pkt_dts;
    }
    // libav: sets only pkt_dts which can be 0
    if (pts && pts != (int64_t) AV_NOPTS_VALUE) {
	// build a monotonic pts
	if (*pts_p != (int64_t) AV_NOPTS_VALUE) {
	    int64_t delta;

	    delta = pts - *pts_p;
	    // ignore negative jumps
	    if (delta > -600 * 90 && delta <= -40 * 90) {
		if (-delta > VideoDeltaPTS) {
		    VideoDeltaPTS = -delta;
		    Debug(4,
			"video: %#012" PRIx64 "->%#012" PRIx64 " delta%+4"
			PRId64 " pts\n", *pts_p, pts, pts - *pts_p);
		}
		return;
	    }
	} else {			// first new clock value
	    EnoughVideo = 0;
	    AudioVideoReady(pts);
	}
	if (*pts_p != pts && lastpts != pts) {
	    Debug(4,
		"video: %#012" PRIx64 "->%#012" PRIx64 " delta=%4" PRId64
		" pts\n", *pts_p, pts, pts - *pts_p);
	    *pts_p = pts;
	}
    }
    lastpts = pts;
}

///
///	Update output for new size or aspect ratio.
///
///	@param input_aspect_ratio	video stream aspect
///
static void VideoUpdateOutput(AVRational input_aspect_ratio, int input_width,
    int input_height, VideoResolutions resolution, int video_x, int video_y,
    int video_width, int video_height, int *output_x, int *output_y,
    int *output_width, int *output_height, int *crop_x, int *crop_y,
    int *crop_width, int *crop_height)
{
    AVRational display_aspect_ratio;
    AVRational tmp_ratio;

    if (!input_aspect_ratio.num || !input_aspect_ratio.den) {
	input_aspect_ratio.num = 1;
	input_aspect_ratio.den = 1;
	Debug(3, "video: aspect defaults to %d:%d\n", input_aspect_ratio.num,
	    input_aspect_ratio.den);
    }

    av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den,
	input_width * input_aspect_ratio.num,
	input_height * input_aspect_ratio.den, 1024 * 1024);

    // InputWidth/Height can be zero = uninitialized
    if (!input_aspect_ratio.num || !input_aspect_ratio.den) {
	input_aspect_ratio.num = 1;
	input_aspect_ratio.den = 1;
    }

    display_aspect_ratio.num =
	VideoScreen->width_in_pixels * VideoScreen->height_in_millimeters;
    display_aspect_ratio.den =
	VideoScreen->height_in_pixels * VideoScreen->width_in_millimeters;

    display_aspect_ratio = av_mul_q(input_aspect_ratio, display_aspect_ratio);
    Debug(3, "video: aspect %d:%d\n", display_aspect_ratio.num,
	display_aspect_ratio.den);

    *crop_x = VideoCutLeftRight[resolution];
    *crop_y = VideoCutTopBottom[resolution];
    *crop_width = input_width - VideoCutLeftRight[resolution] * 2;
    *crop_height = input_height - VideoCutTopBottom[resolution] * 2;

    // FIXME: store different positions for the ratios
    tmp_ratio.num = 4;
    tmp_ratio.den = 3;
#ifdef DEBUG
    fprintf(stderr, "ratio: %d:%d %d:%d\n", input_aspect_ratio.num,
	input_aspect_ratio.den, display_aspect_ratio.num,
	display_aspect_ratio.den);
#endif
    if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	switch (Video4to3ZoomMode) {
	    case VideoNormal:
		goto normal;
	    case VideoStretch:
		goto stretch;
	    case VideoCenterCutOut:
		goto center_cut_out;
	    case VideoAnamorphic:
		// FIXME: rest should be done by hardware
		goto stretch;
	}
    }
    switch (VideoOtherZoomMode) {
	case VideoNormal:
	    goto normal;
	case VideoStretch:
	    goto stretch;
	case VideoCenterCutOut:
	    goto center_cut_out;
	case VideoAnamorphic:
	    // FIXME: rest should be done by hardware
	    goto stretch;
    }

  normal:
    *output_x = video_x;
    *output_y = video_y;
    *output_width =
	(video_height * display_aspect_ratio.num + display_aspect_ratio.den -
	1) / display_aspect_ratio.den;
    *output_height =
	(video_width * display_aspect_ratio.den + display_aspect_ratio.num -
	1) / display_aspect_ratio.num;
    if (*output_width > video_width) {
	*output_width = video_width;
	*output_y += (video_height - *output_height) / 2;
    } else if (*output_height > video_height) {
	*output_height = video_height;
	*output_x += (video_width - *output_width) / 2;
    }
    Debug(3, "video: aspect output %dx%d%+d%+d\n", *output_width,
	*output_height, *output_x, *output_y);
    return;

  stretch:
    *output_x = video_x;
    *output_y = video_y;
    *output_width = video_width;
    *output_height = video_height;
    Debug(3, "video: stretch output %dx%d%+d%+d\n", *output_width,
	*output_height, *output_x, *output_y);
    return;

  center_cut_out:
    *output_x = video_x;
    *output_y = video_y;
    *output_height = video_height;
    *output_width = video_width;

    *crop_width =
	(video_height * display_aspect_ratio.num + display_aspect_ratio.den -
	1) / display_aspect_ratio.den;
    *crop_height =
	(video_width * display_aspect_ratio.den + display_aspect_ratio.num -
	1) / display_aspect_ratio.num;

    // look which side must be cut
    if (*crop_width > video_width) {
	int tmp;

	*crop_height = input_height - VideoCutTopBottom[resolution] * 2;

	// adjust scaling
	tmp = ((*crop_width - video_width) * input_width) / (2 * video_width);
	// FIXME: round failure?
	if (tmp > *crop_x) {
	    *crop_x = tmp;
	}
	*crop_width = input_width - *crop_x * 2;
    } else if (*crop_height > video_height) {
	int tmp;

	*crop_width = input_width - VideoCutLeftRight[resolution] * 2;

	// adjust scaling
	tmp = ((*crop_height - video_height) * input_height)
	    / (2 * video_height);
	// FIXME: round failure?
	if (tmp > *crop_y) {
	    *crop_y = tmp;
	}
	*crop_height = input_height - *crop_y * 2;
    } else {
	*crop_width = input_width - VideoCutLeftRight[resolution] * 2;
	*crop_height = input_height - VideoCutTopBottom[resolution] * 2;
    }
    Debug(3, "video: aspect crop %dx%d%+d%+d\n", *crop_width, *crop_height,
	*crop_x, *crop_y);
    return;
}

//----------------------------------------------------------------------------
//	OPENGL
//----------------------------------------------------------------------------
#if defined USE_GLX || defined USE_EGL

static GLuint OsdGlTextures[2];		///< gl texture for OSD
static GLuint OsdGlTexture = 0;		///< texture for openglosd
static int OsdIndex = 0;			///< index into OsdGlTextures
static GLint maxTextureSize;
static void GlCheck(void);

GLuint vao_buffer;
GLuint gl_prog = 0, egl_prog_osd = 0;      // shader programm
GLint gl_colormatrix, gl_colormatrix_c;

#include "shaders.h"

#endif

//----------------------------------------------------------------------------
//	GLX
//----------------------------------------------------------------------------

#ifdef USE_GLX

static int GlxEnabled;			///< use GLX
static int GlxVSyncEnabled;		///< enable/disable v-sync
static GLXContext GlxSharedContext;	///< shared gl context
static GLXContext GlxContext;		///< our gl context

#ifdef USE_VIDEO_THREAD
static GLXContext GlxThreadContext;	///< our gl context for the thread
#endif

//static GLXFBConfig *GlxFBConfigs;	///< our gl fb configs
static XVisualInfo *GlxVisualInfo;	///< our gl visual

///
///	GLX extension functions
///@{
#ifdef GLX_MESA_swap_control
static PFNGLXSWAPINTERVALMESAPROC GlxSwapIntervalMESA;
#endif
#ifdef GLX_SGI_video_sync
static PFNGLXGETVIDEOSYNCSGIPROC GlxGetVideoSyncSGI;
#endif
#ifdef GLX_SGI_swap_control
static PFNGLXSWAPINTERVALSGIPROC GlxSwapIntervalSGI;
#endif
#ifdef GLX_MESA_copy_sub_buffer
static PFNGLXCOPYSUBBUFFERMESAPROC GlXCopySubBufferMESA;
#endif

///@}

///
///	GLX check error.
///
static void GlxCheck(void)
{
    GLenum err;

    if ((err = glGetError()) != GL_NO_ERROR) {
	Debug(3, "video/glx: error %d '%s'\n", err, gluErrorString(err));
    }
}

///
///	GLX check if a GLX extension is supported.
///
///	@param ext	extension to query
///	@returns true if supported, false otherwise
///
static int GlxIsExtensionSupported(const char *ext)
{
    const char *extensions;

    if ((extensions =
	    glXQueryExtensionsString(XlibDisplay,
		DefaultScreen(XlibDisplay)))) {
	const char *s;
	int l;

	s = strstr(extensions, ext);
	l = strlen(ext);
	return s && (s[l] == ' ' || s[l] == '\0');
    }
    return 0;
}

#if defined USE_VAAPI || defined USE_VDPAU
///
///	Setup GLX decoder
///
///	@param width		input video textures width
///	@param height		input video textures height
///	@param[OUT] textures	created and prepared textures
///
static void GlxSetupDecoder(int width, int height, GLuint * textures)
{
    int i;

    glEnable(GL_TEXTURE_2D);		// create 2d texture
    glGenTextures(2, textures);
    GlxCheck();
    for (i = 0; i < 2; ++i) {
	glBindTexture(GL_TEXTURE_2D, textures[i]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA,
	    GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
    }
    glDisable(GL_TEXTURE_2D);
    GlxCheck();
}
#endif

///
///	Render texture.
///
///	@param texture	2d texture
///	@param x	window x
///	@param y	window y
///	@param width	window width
///	@param height	window height
///
static inline void GlxRenderTexture(GLuint texture, int x, int y, int width,
    int height)
{
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);	// no color
    glBegin(GL_QUADS); {
	glTexCoord2f(1.0f, 1.0f);
	glVertex2i(x + width, y + height);
	glTexCoord2f(0.0f, 1.0f);
	glVertex2i(x, y + height);
	glTexCoord2f(0.0f, 0.0f);
	glVertex2i(x, y);
	glTexCoord2f(1.0f, 0.0f);
	glVertex2i(x + width, y);
    }
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

///
///	Upload OSD texture.
///
///	@param x	x coordinate texture
///	@param y	y coordinate texture
///	@param width	argb image width
///	@param height	argb image height
///	@param argb	argb image
///
static void GlxUploadOsdTexture(int x, int y, int width, int height,
    const uint8_t * argb)
{
    // FIXME: use other / faster uploads
    // ARB_pixelbuffer_object GL_PIXEL_UNPACK_BUFFER glBindBufferARB()
    // glMapBuffer() glUnmapBuffer()

    glEnable(GL_TEXTURE_2D);		// upload 2d texture

    glBindTexture(GL_TEXTURE_2D, OsdGlTextures[OsdIndex]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_BGRA,
	GL_UNSIGNED_BYTE, argb);
    glBindTexture(GL_TEXTURE_2D, 0);

    glDisable(GL_TEXTURE_2D);
}

///
///	GLX initialize OSD.
///
///	@param width	osd width
///	@param height	osd height
///
static void GlxOsdInit(int width, int height)
{
    int i;
    // not init with openglosd
    if (OsdGlTexture) return;

    if (!GlxEnabled || !GlxContext) {
	Debug(3, "video/glx: %s called without glx enabled\n", __FUNCTION__);
	return;
    }

    Debug(3, "video/glx: osd init context %p <-> %p\n", glXGetCurrentContext(),
	GlxContext);

    if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext)) {
	Error(_("video/glx: can't make glx context current\n"));
	return;
    }

    //
    //	create a RGBA texture.
    //
    glEnable(GL_TEXTURE_2D);		// create 2d texture(s)

    glGenTextures(2, OsdGlTextures);
    for (i = 0; i < 2; ++i) {
	glBindTexture(GL_TEXTURE_2D, OsdGlTextures[i]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA,
	    GL_UNSIGNED_BYTE, NULL);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

///
///	GLX cleanup osd.
///
static void GlxOsdExit(void)
{
    if (OsdGlTexture) return;
    if (OsdGlTextures[0]) {
	glDeleteTextures(2, OsdGlTextures);
	OsdGlTextures[0] = 0;
	OsdGlTextures[1] = 0;
    }
}

///
///	Upload ARGB image to texture.
///
///	@param xi	x-coordinate in argb image
///	@param yi	y-coordinate in argb image
///	@paran height	height in pixel in argb image
///	@paran width	width in pixel in argb image
///	@param pitch	pitch of argb image
///	@param argb	32bit ARGB image data
///	@param x	x-coordinate on screen of argb image
///	@param y	y-coordinate on screen of argb image
///
///	@note looked by caller
///
static void GlxOsdDrawARGB(int xi, int yi, int width, int height, int pitch,
    const uint8_t * argb, int x, int y)
{
    uint8_t *tmp;

#ifdef DEBUG
    uint32_t start;
    uint32_t end;
#endif

    if (!GlxEnabled) {
	Debug(3, "video/glx: %s called without glx enabled\n", __FUNCTION__);
	return;
    }
#ifdef DEBUG
    start = GetMsTicks();
    Debug(3, "video/glx: osd context %p <-> %p\n", glXGetCurrentContext(),
	GlxContext);
#endif
    if (!GlxContext) return;

    // FIXME: faster way
    tmp = malloc(width * height * 4);
    if (tmp) {
	int i;

	for (i = 0; i < height; ++i) {
	    memcpy(tmp + i * width * 4, argb + xi * 4 + (i + yi) * pitch,
		width * 4);
	}
	// set glx context
	if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext)) {
	    Error(_("video/glx: can't make glx context current\n"));
	    free(tmp);
	    return;
	}
	GlxUploadOsdTexture(x, y, width, height, tmp);
	glXMakeCurrent(XlibDisplay, None, NULL);

	free(tmp);
    }
#ifdef DEBUG
    end = GetMsTicks();

    Debug(3, "video/glx: osd upload %dx%d%+d%+d %dms %d\n", width, height, x,
	y, end - start, width * height * 4);
#endif
}

///
///	Clear OSD texture.
///
///	@note looked by caller
///
static void GlxOsdClear(void)
{
    void *texbuf;

    if (!GlxContext || OsdGlTexture) return;

    if (!GlxEnabled) {
	Debug(3, "video/glx: %s called without glx enabled\n", __FUNCTION__);
	return;
    }

    Debug(3, "video/glx: osd context %p <-> %p\n", glXGetCurrentContext(),
	GlxContext);

    // FIXME: any opengl function to clear an area?
    // FIXME: if not; use zero buffer
    // FIXME: if not; use dirty area
    // set glx context
    if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext)) {
	Error(_("video/glx: can't make glx context current\n"));
	return;
    }

    texbuf = calloc(OsdWidth * OsdHeight, 4);
    GlxUploadOsdTexture(0, 0, OsdWidth, OsdHeight, texbuf);
    glXMakeCurrent(XlibDisplay, None, NULL);

    free(texbuf);
}

static int GlxMaxPixmapSize (void)
{
    return maxTextureSize;
}

///
///	Setup GLX window.
///
///	@param window	xcb window id
///	@param width	window width
///	@param height	window height
///	@param context	GLX context
///
static void GlxSetupWindow(xcb_window_t window, int width, int height,
    GLXContext context)
{
#ifdef DEBUG
    uint32_t start;
    uint32_t end;
    int i;
    unsigned count;
#endif

    Debug(3, "video/glx: %s %x %dx%d context:%p", __FUNCTION__, window, width,
	height, context);

    // set glx context
    if (!glXMakeCurrent(XlibDisplay, window, context)) {
	Error(_("video/glx: can't make glx context current\n"));
	GlxEnabled = 0;
	return;
    }

    Debug(3, "video/glx: ok\n");

#ifdef DEBUG
    // check if v-sync is working correct
    end = GetMsTicks();
    for (i = 0; i < 10; ++i) {
	start = end;

	glClear(GL_COLOR_BUFFER_BIT);
	glXSwapBuffers(XlibDisplay, window);
	end = GetMsTicks();

	if (GlxGetVideoSyncSGI)
	    GlxGetVideoSyncSGI(&count);
	Debug(3, "video/glx: %5d frame rate %dms\n", count, end - start);
	// nvidia can queue 5 swaps
	if (i > 5 && (end - start) < 15) {
	    Warning(_("video/glx: no v-sync\n"));
	}
    }
#endif

    // viewpoint
    GlxCheck();
    glViewport(0, 0, width, height);
    glDepthRange(-1.0, 1.0);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glColor3f(1.0f, 1.0f, 1.0f);
    glClearDepth(1.0);
    GlxCheck();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
    GlxCheck();

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);		// setup 2d drawing
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
#ifdef USE_DOUBLEBUFFER
    glDrawBuffer(GL_BACK);
#else
    glDrawBuffer(GL_FRONT);
#endif
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

#ifdef USE_DOUBLEBUFFER
    glDrawBuffer(GL_FRONT);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawBuffer(GL_BACK);
#endif

    // clear
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);	// intial background color
    glClear(GL_COLOR_BUFFER_BIT);
    GlxCheck();

    //get GL_MAX_TEXTURE_SIZE
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
}

///
///	Initialize GLX.
///
static void GlxInit(void)
{
    static GLint visual_attr[] = {
	GLX_RGBA,
	GLX_RED_SIZE, 8,
	GLX_GREEN_SIZE, 8,
	GLX_BLUE_SIZE, 8,
#ifdef USE_DOUBLEBUFFER
	GLX_DOUBLEBUFFER,
#endif
	None
    };
    XVisualInfo *vi;
    GLXContext context;

    int major;
    int minor;
    int glx_GLX_EXT_swap_control;
    int glx_GLX_MESA_swap_control;
    int glx_GLX_SGI_swap_control;
    int glx_GLX_SGI_video_sync;
    int glX_GLX_MESA_copy_sub_buffer;

    if (!glXQueryVersion(XlibDisplay, &major, &minor)) {
	Error(_("video/glx: no GLX support\n"));
	GlxEnabled = 0;
	return;
    }
    Info(_("video/glx: glx version %d.%d\n"), major, minor);

    //
    //	check which extension are supported
    //
    glx_GLX_EXT_swap_control = GlxIsExtensionSupported("GLX_EXT_swap_control");
    glx_GLX_MESA_swap_control =
	GlxIsExtensionSupported("GLX_MESA_swap_control");
    glx_GLX_SGI_swap_control = GlxIsExtensionSupported("GLX_SGI_swap_control");
    glx_GLX_SGI_video_sync = GlxIsExtensionSupported("GLX_SGI_video_sync");
    glX_GLX_MESA_copy_sub_buffer = GlxIsExtensionSupported("GLX_MESA_copy_sub_buffer");

#ifdef GLX_MESA_swap_control
    if (glx_GLX_MESA_swap_control) {
	GlxSwapIntervalMESA = (PFNGLXSWAPINTERVALMESAPROC)
	    glXGetProcAddress((const GLubyte *)"glXSwapIntervalMESA");
    }
    Debug(3, "video/glx: GlxSwapIntervalMESA=%p\n", GlxSwapIntervalMESA);
#endif
#ifdef GLX_SGI_swap_control
    if (glx_GLX_SGI_swap_control) {
	GlxSwapIntervalSGI = (PFNGLXSWAPINTERVALSGIPROC)
	    glXGetProcAddress((const GLubyte *)"glXSwapIntervalSGI");
    }
    Debug(3, "video/glx: GlxSwapIntervalSGI=%p\n", GlxSwapIntervalSGI);
#endif
#ifdef GLX_SGI_video_sync
    if (glx_GLX_SGI_video_sync) {
	GlxGetVideoSyncSGI = (PFNGLXGETVIDEOSYNCSGIPROC)
	    glXGetProcAddress((const GLubyte *)"glXGetVideoSyncSGI");
    }
    Debug(3, "video/glx: GlxGetVideoSyncSGI=%p\n", GlxGetVideoSyncSGI);
#endif
    // glXGetVideoSyncSGI glXWaitVideoSyncSGI
#ifdef GLX_MESA_copy_sub_buffer
    if (glX_GLX_MESA_copy_sub_buffer) {
	GlXCopySubBufferMESA = (PFNGLXCOPYSUBBUFFERMESAPROC)
	    glXGetProcAddress((const GLubyte *)"glXCopySubBufferMESA");
    }
    Debug(3, "video/glx: GlXCopySubBufferMESA=%p\n", GlXCopySubBufferMESA);
#endif

#if 0
    // FIXME: use xcb: xcb_glx_create_context
#endif

    // create glx context
    glXMakeCurrent(XlibDisplay, None, NULL);

    vi = glXChooseVisual(XlibDisplay, DefaultScreen(XlibDisplay), visual_attr);
    if (!vi) {
	Error(_("video/glx: can't get a RGB visual\n"));
	GlxEnabled = 0;
	return;
    }
    if (!vi->visual) {
	Error(_("video/glx: no valid visual found\n"));
	GlxEnabled = 0;
	return;
    }
    if (vi->bits_per_rgb < 8) {
	Error(_("video/glx: need atleast 8-bits per RGB\n"));
	GlxEnabled = 0;
	return;
    }

    context = glXCreateContext(XlibDisplay, vi, NULL, GL_TRUE);
    if (!context) {
	Error(_("video/glx: can't create glx context\n"));
	GlxEnabled = 0;
	return;
    }
    GlxSharedContext = context;

    context = glXCreateContext(XlibDisplay, vi, GlxSharedContext, GL_TRUE);
    if (!context) {
	Error(_("video/glx: can't create glx context\n"));
	GlxEnabled = 0;
	glXDestroyContext(XlibDisplay, GlxSharedContext);
	GlxSharedContext = 0;
	return;
    }
    GlxContext = context;

    GlxVisualInfo = vi;
    Debug(3, "video/glx: visual %#02x depth %u\n", (unsigned)vi->visualid,
	vi->depth);

    //
    //	query default v-sync state
    //
    if (glx_GLX_EXT_swap_control) {
	unsigned tmp;
	tmp = -1;
	glXQueryDrawable(XlibDisplay, DefaultRootWindow(XlibDisplay),
	    GLX_SWAP_INTERVAL_EXT, &tmp);
	GlxCheck();

	Debug(3, "video/glx: default v-sync is %d\n", tmp);
    } else {
	Debug(3, "video/glx: default v-sync is unknown\n");
    }

    //
    //	disable wait on v-sync
    //
    // FIXME: sleep before swap / busy waiting hardware
    // FIXME: 60hz lcd panel
    // FIXME: config: default, on, off
#ifdef GLX_SGI_swap_control
    if (GlxVSyncEnabled < 0 && GlxSwapIntervalSGI) {
	if (GlxSwapIntervalSGI(0)) {
	    GlxCheck();
	    Warning(_("video/glx: can't disable v-sync\n"));
	} else {
	    Info(_("video/glx: v-sync disabled\n"));
	}
    } else
#endif
#ifdef GLX_MESA_swap_control
    if (GlxVSyncEnabled < 0 && GlxSwapIntervalMESA) {
	if (GlxSwapIntervalMESA(0)) {
	    GlxCheck();
	    Warning(_("video/glx: can't disable v-sync\n"));
	} else {
	    Info(_("video/glx: v-sync disabled\n"));
	}
    }
#endif

    //
    //	enable wait on v-sync
    //
#ifdef GLX_SGI_swap_control
    if (GlxVSyncEnabled > 0 && GlxSwapIntervalMESA) {
	if (GlxSwapIntervalMESA(1)) {
	    GlxCheck();
	    Warning(_("video/glx: can't enable v-sync\n"));
	} else {
	    Info(_("video/glx: v-sync enabled\n"));
	}
    } else
#endif
#ifdef GLX_MESA_swap_control
    if (GlxVSyncEnabled > 0 && GlxSwapIntervalSGI) {
	if (GlxSwapIntervalSGI(1)) {
	    GlxCheck();
	    Warning(_("video/glx: can't enable v-sync\n"));
	} else {
	    Info(_("video/glx: v-sync enabled\n"));
	}
    }
#endif
}

///
///	Cleanup GLX.
///
static void GlxExit(void)
{
    Debug(3, "video/glx: %s\n", __FUNCTION__);

    glFinish();

    // must destroy glx
    if (glXGetCurrentContext()) {
	// if currently used, set to none
	glXMakeCurrent(XlibDisplay, None, NULL);
    }
    if (GlxSharedContext) {
	glXDestroyContext(XlibDisplay, GlxSharedContext);
    }
    if (GlxContext) {
	glXDestroyContext(XlibDisplay, GlxContext);
    }
    GlxContext = NULL;
    if (GlxThreadContext) {
	glXDestroyContext(XlibDisplay, GlxThreadContext);
    }
    if (OsdGlTexture)
        OsdGlTexture = 0;
    GlxThreadContext = NULL;
/*    if (GlxVisualInfo) {
	XFree(GlxVisualInfo);
	GlxVisualInfo = NULL;
    }
*/
}

#endif

//----------------------------------------------------------------------------
//	EGL
//----------------------------------------------------------------------------

#ifdef USE_EGL
static int EglEnabled;			///< use EGL

static EGLContext EglSharedContext;     ///< shared gl context
static EGLContext EglContext;           ///< our gl context
static EGLConfig EglConfig;
static EGLDisplay EglDisplay;
static EGLSurface EglSurface;
#ifdef USE_VIDEO_THREAD
static EGLContext EglThreadContext;     ///< our gl context for the thread
#endif

PFNEGLCREATEIMAGEKHRPROC EglCreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC EglDestroyImageKHR;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC GlEGLImageTargetTexture2DOES;

static EGLint EglContextAttr[] =
{
    EGL_CONTEXT_OPENGL_PROFILE_MASK,
    EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
    EGL_CONTEXT_MAJOR_VERSION, 3,
    EGL_CONTEXT_MINOR_VERSION, 0,
    EGL_NONE
};

#define CASE_STR(value) case value: return #value;
const char* EglErrorString(EGLint error)
{
    switch(error)
    {
        CASE_STR(EGL_SUCCESS)
        CASE_STR(EGL_NOT_INITIALIZED)
        CASE_STR(EGL_BAD_ACCESS)
        CASE_STR(EGL_BAD_ALLOC)
        CASE_STR(EGL_BAD_ATTRIBUTE)
        CASE_STR(EGL_BAD_CONTEXT)
        CASE_STR(EGL_BAD_CONFIG)
        CASE_STR(EGL_BAD_CURRENT_SURFACE)
        CASE_STR(EGL_BAD_DISPLAY)
        CASE_STR(EGL_BAD_SURFACE)
        CASE_STR(EGL_BAD_MATCH)
        CASE_STR(EGL_BAD_PARAMETER)
        CASE_STR(EGL_BAD_NATIVE_PIXMAP)
        CASE_STR(EGL_BAD_NATIVE_WINDOW)
        CASE_STR(EGL_CONTEXT_LOST)
        default: return "Unknown";
    }
}
#undef CASE_STR

void EglCheck(void)
{
    EGLint err;

    if ((err = eglGetError()) != EGL_SUCCESS) {
	Debug(3, "video/egl: error %d %s\n", err, EglErrorString(err));
    }
}

#ifdef USE_VAAPI
///
///	Setup EGL decoder
///
///	@param width		input video textures width
///	@param height		input video textures height
///	@param[OUT] textures	created and prepared textures
///
static void EglSetupDecoder(int width, int height, GLuint * textures)
{
    int i;

    glEnable(GL_TEXTURE_2D);		// create 2d texture
    glGenTextures(2, textures);
    EglCheck();
    for (i = 0; i < 2; ++i) {
	glBindTexture(GL_TEXTURE_2D, textures[i]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA,
	    GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
    }
    glDisable(GL_TEXTURE_2D);
    EglCheck();
}
#endif

static inline void EglRenderTexture(GLuint texture, int x, int y, int width,
    int height)
{
    GLint texLoc;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (egl_prog_osd == 0)
        egl_prog_osd = sc_generate_osd(egl_prog_osd);
    if (!egl_prog_osd) return;

    glUseProgram(egl_prog_osd);
    texLoc = glGetUniformLocation(egl_prog_osd, "texture0");
    glUniform1i(texLoc, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    render_pass_quad(0, 0, 0);

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    (void)x; //may be need later
    (void)y;
    (void)width;
    (void)height;
}

///
///	Upload OSD texture.
///
///	@param x	x coordinate texture
///	@param y	y coordinate texture
///	@param width	argb image width
///	@param height	argb image height
///	@param argb	argb image
///
static void EglUploadOsdTexture(int x, int y, int width, int height,
    const uint8_t * argb)
{
    glEnable(GL_TEXTURE_2D);		// upload 2d texture

    glBindTexture(GL_TEXTURE_2D, OsdGlTextures[OsdIndex]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_BGRA,
	GL_UNSIGNED_BYTE, argb);
    glBindTexture(GL_TEXTURE_2D, 0);

    glDisable(GL_TEXTURE_2D);
}

///
///	EGL initialize OSD.
///
///	@param width	osd width
///	@param height	osd height
///
static void EglOsdInit(int width, int height)
{
    int i;
    // not init with openglosd
    if (OsdGlTexture) return;

    if (!EglEnabled || !EglContext) {
	Debug(3, "video/egl: %s called without egl enabled\n", __FUNCTION__);
	return;
    }

    Debug(3, "video/egl: osd init context %p <-> %p\n", eglGetCurrentContext(),
	EglContext);

    if (!eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglContext)) {
	Error(_("video/egl: can't make egl context current\n"));
	return;
    }

    //
    //	create a RGBA texture.
    //
    glEnable(GL_TEXTURE_2D);		// create 2d texture(s)

    glGenTextures(2, OsdGlTextures);
    for (i = 0; i < 2; ++i) {
	glBindTexture(GL_TEXTURE_2D, OsdGlTextures[i]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA,
	    GL_UNSIGNED_BYTE, NULL);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

///
///	EGL cleanup osd.
///
static void EglOsdExit(void)
{
    if (egl_prog_osd)
        glDeleteProgram(egl_prog_osd);
    egl_prog_osd = 0;

    if (OsdGlTexture) return;
    if (OsdGlTextures[0]) {
	glDeleteTextures(2, OsdGlTextures);
	OsdGlTextures[0] = 0;
	OsdGlTextures[1] = 0;
    }
}

///
///	Upload ARGB image to texture.
///
///	@param xi	x-coordinate in argb image
///	@param yi	y-coordinate in argb image
///	@paran height	height in pixel in argb image
///	@paran width	width in pixel in argb image
///	@param pitch	pitch of argb image
///	@param argb	32bit ARGB image data
///	@param x	x-coordinate on screen of argb image
///	@param y	y-coordinate on screen of argb image
///
///	@note looked by caller
///
static void EglOsdDrawARGB(int xi, int yi, int width, int height, int pitch,
    const uint8_t * argb, int x, int y)
{
    uint8_t *tmp;

#ifdef DEBUG
    uint32_t start;
    uint32_t end;
#endif

    if (!EglEnabled) {
	Debug(3, "video/egl: %s called without egl enabled\n", __FUNCTION__);
	return;
    }
#ifdef DEBUG
    start = GetMsTicks();
    Debug(3, "video/egl: osd context %p <-> %p\n", eglGetCurrentContext(),
	EglContext);
#endif
    if (!EglContext) return;

    // FIXME: faster way
    tmp = malloc(width * height * 4);
    if (tmp) {
	int i;

	for (i = 0; i < height; ++i) {
	    memcpy(tmp + i * width * 4, argb + xi * 4 + (i + yi) * pitch,
		width * 4);
	}
	// set egl context
	if (!eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglContext)) {
	    Error(_("video/glx: can't make glx context current\n"));
	    return;
	}
	EglUploadOsdTexture(x, y, width, height, tmp);
	eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	free(tmp);
    }
#ifdef DEBUG
    end = GetMsTicks();

    Debug(3, "video/egl: osd upload %dx%d%+d%+d %dms %d\n", width, height, x,
	y, end - start, width * height * 4);
#endif
}

///
///	Clear OSD texture.
///
///	@note looked by caller
///
static void EglOsdClear(void)
{
    void *texbuf;

    if (!EglContext || OsdGlTexture) return;

    if (!EglEnabled) {
	Debug(3, "video/egl: %s called without egl enabled\n", __FUNCTION__);
	return;
    }

    Debug(3, "video/egl: osd context %p <-> %p\n", eglGetCurrentContext(),
	EglContext);

    // set egl context
    if (!eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglContext)) {
	Error(_("video/egl: can't make egl context current\n"));
	return;
    }

    texbuf = calloc(OsdWidth * OsdHeight, 4);
    EglUploadOsdTexture(0, 0, OsdWidth, OsdHeight, texbuf);
    eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    free(texbuf);
}

static int EglMaxPixmapSize (void)
{
    return maxTextureSize;
}

static void EglSetupWindow(xcb_window_t window, int width, int height, EGLContext context)
{
    Debug(3, "video/egl: %s %x %dx%d context: %p", __FUNCTION__, window, width, height, context);
    eglMakeCurrent(EglDisplay, EglSurface, EglSurface, context);
    EglCheck();
    // viewpoint
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0f, (GLfloat)width, 0.0f, (GLfloat)height, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);	// intial background color
    EglCheck();
    // get GL_MAX_TEXTURE_SIZE
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

    glGenBuffers(1,&vao_buffer);
    EglCheck();

    eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EglCheck();

    (void)window;
}


static void EglInit(void)
{
    EGLConfig config;
    EGLint numConfig;
    EGLContext context;

    EGLint visual_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };

    EglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
    EglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
    GlEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");

    EglDisplay = eglGetDisplay(XlibDisplay);
    if (EglDisplay == EGL_NO_DISPLAY) {
        EglEnabled = 0;
        return;
    }

    eglInitialize(EglDisplay, NULL, NULL);
    EglCheck();

    eglBindAPI(EGL_OPENGL_API);
    EglCheck();

    Debug(3,"video/egl: egl version: %s", eglQueryString(EglDisplay, EGL_VERSION));
    Debug(3,"video/egl: egl vendor: %s", eglQueryString(EglDisplay, EGL_VENDOR));
    Debug(3,"video/egl: egl extensions: %s", eglQueryString(EglDisplay, EGL_EXTENSIONS));
    Debug(3,"video/egl: egl apis: %s", eglQueryString(EglDisplay, EGL_CLIENT_APIS));

    eglChooseConfig(EglDisplay, visual_attr, &config, 1, &numConfig);
    EglCheck();
    if (numConfig < 1) {
	Error(_("video/egl: can't choose egl config\n"));
        EglEnabled = 0;
        return;
    }

    EglSurface = eglCreateWindowSurface(EglDisplay, config, (EGLNativeWindowType) VideoWindow, NULL);
    EglCheck();
    if (EglSurface == EGL_NO_SURFACE) {
	Error(_("video/egl: can't create egl surface\n"));
        EglEnabled = 0;
        return;
    }

    context = eglCreateContext(EglDisplay, config, EGL_NO_CONTEXT, EglContextAttr);
    EglCheck();
    if (context == EGL_NO_CONTEXT) {
	Error(_("video/egl: can't create egl context\n"));
        EglEnabled = 0;
        return;
    }
    EglSharedContext = context;

    context = eglCreateContext(EglDisplay, config, EglSharedContext, EglContextAttr);
    EglCheck();
    if (!context) {
	Error(_("video/egl: can't create egl context\n"));
        EglEnabled = 0;
        return;
    }
    EglContext = context;
}

static void EglExit(void)
{
    Debug(3, "video/egl: %s\n", __FUNCTION__);

    glFinish();

    // must destroy contet
    if (eglGetCurrentContext()) {
        // if currently used, set to none
        eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    if (EglSharedContext) {
        eglDestroyContext(EglDisplay, EglSharedContext);
        EglCheck();
    }

    if (EglContext) {
        eglDestroyContext(EglDisplay, EglContext);
        EglCheck();
        EglContext = NULL;
    }

    eglTerminate(EglDisplay);

    if (OsdGlTexture)
        OsdGlTexture = 0;
    EglThreadContext = NULL;
}

#endif

void GlCheck(void)
{
#ifdef USE_GLX
    if (GlxEnabled)
        GlxCheck();
#endif
#ifdef USE_EGL
    if (EglEnabled)
        EglCheck();
#endif
}

//----------------------------------------------------------------------------
//	common functions
//----------------------------------------------------------------------------

///
///	Calculate resolution group.
///
///	@param width		video picture raw width
///	@param height		video picture raw height
///	@param interlace	flag interlaced video picture
///
///	@note interlace isn't used yet and probably wrong set by caller.
///
static VideoResolutions VideoResolutionGroup(int width, int height,
    __attribute__ ((unused))
    int interlace)
{
    if (height == 2160) {
	return VideoResolutionUHD;
    }
    if (height <= 576) {
	return VideoResolution576i;
    }
    if (height <= 720) {
	return VideoResolution720p;
    }
    if (height < 1080) {
    	return VideoResolutionFake1080i;
    }
    if (width < 1920) {
	return VideoResolutionFake1080i;
    }
    return VideoResolution1080i;
}

///
///     Clamp given value against config limits
///
///     @param config   config struct
///     @param valueIn  sample value
///     @return clamped value
///
static inline int VideoConfigClamp(VideoConfigValues * config, float valueIn)
{
    if (valueIn < config->min_value)
        return config->min_value;
    else if (valueIn > config->max_value)
        return config->def_value;
    return valueIn;
}

//----------------------------------------------------------------------------
//	auto-crop
//----------------------------------------------------------------------------

///
///	auto-crop context structure and typedef.
///
typedef struct _auto_crop_ctx_
{
    int X1;				///< detected left border
    int X2;				///< detected right border
    int Y1;				///< detected top border
    int Y2;				///< detected bottom border

    int Count;				///< counter to delay switch
    int State;				///< auto-crop state (0, 14, 16)

} AutoCropCtx;

#ifdef USE_AUTOCROP

#define YBLACK 0x20			///< below is black
#define UVBLACK 0x80			///< around is black
#define M64 UINT64_C(0x0101010101010101)	///< 64bit multiplicator

    /// auto-crop percent of video width to ignore logos
static const int AutoCropLogoIgnore = 24;
static int AutoCropInterval;		///< auto-crop check interval
static int AutoCropDelay;		///< auto-crop switch delay
static int AutoCropTolerance;		///< auto-crop tolerance

///
///	Detect black line Y.
///
///	@param data	Y plane pixel data
///	@param length	number of pixel to check
///	@param pitch	offset of pixels
///
///	@note 8 pixel are checked at once, all values must be 8 aligned
///
static int AutoCropIsBlackLineY(const uint8_t * data, int length, int pitch)
{
    int n;
    int o;
    uint64_t r;
    const uint64_t *p;

#ifdef DEBUG
    if ((size_t) data & 0x7 || pitch & 0x7) {
	abort();
    }
#endif
    p = (const uint64_t *)data;
    n = length;				// FIXME: can remove n
    o = pitch / 8;

    r = 0UL;
    while (--n >= 0) {
	r |= *p;
	p += o;
    }

    // below YBLACK(0x20) is black
    return !(r & ~((YBLACK - 1) * M64));
}

///
///	Auto detect black borders and crop them.
///
///	@param autocrop auto-crop variables
///	@param width	frame width in pixel
///	@param height	frame height in pixel
///	@param data	frame planes data (Y, U, V)
///	@param pitches	frame planes pitches (Y, U, V)
///
///	@note FIXME: can reduce the checked range, left, right crop isn't
///		used yet.
///
///	@note FIXME: only Y is checked, for black.
///
static void AutoCropDetect(AutoCropCtx * autocrop, int width, int height,
    void *data[3], uint32_t pitches[3])
{
    const void *data_y;
    unsigned length_y;
    int x;
    int y;
    int x1;
    int x2;
    int y1;
    int y2;
    int logo_skip;

    //
    //	ignore top+bottom 6 lines and left+right 8 pixels
    //
#define SKIP_X	8
#define SKIP_Y	6
    x1 = width - 1;
    x2 = 0;
    y1 = height - 1;
    y2 = 0;
    logo_skip = SKIP_X + (((width * AutoCropLogoIgnore) / 100 + 8) / 8) * 8;

    data_y = data[0];
    length_y = pitches[0];

    //
    //	search top
    //
    for (y = SKIP_Y; y < y1; ++y) {
	if (!AutoCropIsBlackLineY(data_y + logo_skip + y * length_y,
		(width - 2 * logo_skip) / 8, 8)) {
	    if (y == SKIP_Y) {
		y = 0;
	    }
	    y1 = y;
	    break;
	}
    }
    //
    //	search bottom
    //
    for (y = height - SKIP_Y - 1; y > y2; --y) {
	if (!AutoCropIsBlackLineY(data_y + logo_skip + y * length_y,
		(width - 2 * logo_skip) / 8, 8)) {
	    if (y == height - SKIP_Y - 1) {
		y = height - 1;
	    }
	    y2 = y;
	    break;
	}
    }
    //
    //	search left
    //
    for (x = SKIP_X; x < x1; x += 8) {
	if (!AutoCropIsBlackLineY(data_y + x + SKIP_Y * length_y,
		height - 2 * SKIP_Y, length_y)) {
	    if (x == SKIP_X) {
		x = 0;
	    }
	    x1 = x;
	    break;
	}
    }
    //
    //	search right
    //
    for (x = width - SKIP_X - 8; x > x2; x -= 8) {
	if (!AutoCropIsBlackLineY(data_y + x + SKIP_Y * length_y,
		height - 2 * SKIP_Y * 8, length_y)) {
	    if (x == width - SKIP_X - 8) {
		x = width - 1;
	    }
	    x2 = x;
	    break;
	}
    }

    if (0 && (y1 > SKIP_Y || x1 > SKIP_X)) {
	Debug(3, "video/autocrop: top=%d bottom=%d left=%d right=%d\n", y1, y2,
	    x1, x2);
    }

    autocrop->X1 = x1;
    autocrop->X2 = x2;
    autocrop->Y1 = y1;
    autocrop->Y2 = y2;
}

#endif

//----------------------------------------------------------------------------
//	software - deinterlace
//----------------------------------------------------------------------------

// FIXME: move general software deinterlace functions to here.

//----------------------------------------------------------------------------
//	VA-API
//----------------------------------------------------------------------------

#ifdef USE_VAAPI

static char VaapiBuggyXvBA;		///< fix xvba-video bugs
static char VaapiBuggyVdpau;		///< fix libva-driver-vdpau bugs
static char VaapiBuggyIntel;		///< fix libva-driver-intel i965 bugs
static char VaapiBuggyIntelHD;		///< fix libva-driver-intel iHD bugs

static VADisplay *VaDisplay;		///< VA-API display

static VAImage VaOsdImage = {
    .image_id = VA_INVALID_ID
};					///< osd VA-API image

static VASubpictureID VaOsdSubpicture = VA_INVALID_ID;	///< osd VA-API subpicture
static char VaapiUnscaledOsd;		///< unscaled osd supported

#if VA_CHECK_VERSION(0,33,99)
static char VaapiVideoProcessing;	///< supports video processing
#endif

    /// VA-API decoder typedef
typedef struct _vaapi_decoder_ VaapiDecoder;

///
///	VA-API decoder
///
struct _vaapi_decoder_
{
    VADisplay *VaDisplay;		///< VA-API display

    xcb_window_t Window;		///< output window

    int VideoX;				///< video base x coordinate
    int VideoY;				///< video base y coordinate
    int VideoWidth;			///< video base width
    int VideoHeight;			///< video base height

    int ScaleX;				///< video scale x coordinate
    int ScaleY;				///< video scale y coordinate
    int ScaleWidth;			///< video scale width
    int ScaleHeight;			///< video scale height

    int OutputX;			///< real video output x coordinate
    int OutputY;			///< real video output y coordinate
    int OutputWidth;			///< real video output width
    int OutputHeight;			///< real video output height

    /// flags for put surface for different resolutions groups
    unsigned SurfaceFlagsTable[VideoResolutionMax];
    unsigned SurfaceDeintTable[VideoResolutionMax];

    enum AVPixelFormat PixFmt;		///< ffmpeg frame pixfmt
    int Interlaced;			///< ffmpeg interlaced flag
    int Deinterlaced;			///< vpp deinterlace was run / not run
    int TopFieldFirst;			///< ffmpeg top field displayed first

    VAImage DeintImages[5];		///< deinterlace image buffers

    int GetPutImage;			///< flag get/put image can be used
    VAImage Image[1];			///< image buffer to update surface

    VAProfile Profile;			///< VA-API profile
    VAEntrypoint Entrypoint;		///< VA-API entrypoint
    VAEntrypoint VppEntrypoint;		///< VA-API postprocessing entrypoint
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,74,100)
    struct vaapi_context VaapiContext[1];	///< ffmpeg VA-API context
#endif
    VAConfigID VppConfig;		///< VPP Config
    VAContextID	vpp_ctx;		///< VPP Context

    int SurfacesNeeded;			///< number of surface to request
    int SurfaceUsedN;			///< number of used surfaces
    /// used surface ids
    VASurfaceID SurfacesUsed[CODEC_SURFACES_MAX];
    int SurfaceFreeN;			///< number of free surfaces
    /// free surface ids
    VASurfaceID SurfacesFree[CODEC_SURFACES_MAX];

    int InputWidth;			///< video input width
    int InputHeight;			///< video input height
    AVRational InputAspect;		///< video input aspect ratio
    VideoResolutions Resolution;	///< resolution group

    int CropX;				///< video crop x
    int CropY;				///< video crop y
    int CropWidth;			///< video crop width
    int CropHeight;			///< video crop height
#ifdef USE_AUTOCROP
    AutoCropCtx AutoCrop[1];		///< auto-crop variables
#endif
#ifdef USE_GLX
    void *GlxSurfaces[2];		///< VA-API/GLX surface
#endif
#if defined USE_GLX || defined USE_EGL
    GLuint GlTextures[2];		///< gl texture for VA-API
#endif
#ifdef USE_EGL
    EGLImage EglImages[2];
    enum AVColorSpace  ColorSpace;	/// ffmpeg ColorSpace
#endif
    VASurfaceID BlackSurface;		///< empty black surface

    /// video surface ring buffer
    VASurfaceID SurfacesRb[VIDEO_SURFACES_MAX];
    VASurfaceID PostProcSurfacesRb[POSTPROC_SURFACES_MAX];	///< Posprocessing result surfaces
    VASurfaceID FirstFieldHistory[FIELD_SURFACES_MAX];	///< Postproc history result surfaces
    VASurfaceID SecondFieldHistory[FIELD_SURFACES_MAX];	///< Postproc history result surfaces

    VASurfaceID * ForwardRefSurfaces;	///< Forward referencing surfaces for post processing
    VASurfaceID * BackwardRefSurfaces;	///< Backward referencing surfaces for post processing

    unsigned int ForwardRefCount;	///< Current number of forward references
    unsigned int BackwardRefCount;	///< Current number of backward references

    VASurfaceID PlaybackSurface;	///< Currently playing surface

#ifdef VA_EXP
    VASurfaceID LastSurface;		///< last surface
#endif
    int SurfaceWrite;			///< write pointer
    int SurfaceRead;			///< read pointer
    atomic_t SurfacesFilled;		///< how many of the buffer is used

    int PostProcSurfaceWrite;		///< postprocessing write pointer

    int SurfaceField;			///< current displayed field
    int TrickSpeed;			///< current trick speed
    int TrickCounter;			///< current trick speed counter
    struct timespec FrameTime;		///< time of last display
    VideoStream *Stream;		///< video stream
    int Closing;			///< flag about closing current stream
    int SyncOnAudio;			///< flag sync to audio
    int64_t PTS;			///< video PTS clock

    int LastAVDiff;			///< last audio - video difference
    int SyncCounter;			///< counter to sync frames
    int StartCounter;			///< counter for video start
    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
    VABufferID filters[VAProcFilterCount]; ///< video postprocessing filters via vpp
    VABufferID gpe_filters[VAProcFilterCount]; ///< video postprocessing filters via gpe
    unsigned filter_n;                  ///< number of postprocessing filters
    unsigned gpe_filter_n;              ///< number of gpe postprocessing filters
    unsigned MaxSupportedDeinterlacer;	///< greatest supported deinterlacing method
    VABufferID* vpp_deinterlace_buf;	///< video postprocessing deinterlace buffer
    VABufferID* vpp_denoise_buf;	///< video postprocessing denoise buffer
    VABufferID* vpp_cbal_buf;           ///< video color balance filters via vpp
    VABufferID* vpp_sharpen_buf;	///< video postprocessing sharpen buffer
    VABufferID* vpp_stde_buf;		///< video postprocessing skin tone enhancement buffer
    int vpp_brightness_idx;		///< video postprocessing brightness buffer index
    int vpp_contrast_idx;		///< video postprocessing contrast buffer index
    int vpp_hue_idx;			///< video postprocessing hue buffer index
    int vpp_saturation_idx;		///< video postprocessing saturation buffer index
};

static VaapiDecoder *VaapiDecoders[2];	///< open decoder streams
static int VaapiDecoderN;		///< number of decoder streams

    /// forward display back surface
static void VaapiBlackSurface(VaapiDecoder *);

    /// forward destroy deinterlace images
static void VaapiDestroyDeinterlaceImages(VaapiDecoder *);

    /// forward definition release surface
static void VaapiReleaseSurface(VaapiDecoder *, VASurfaceID);
static void VaapiOsdInit(int width, int height);
static void VaapiOsdExit();
static unsigned int lastWindowWidth, lastWindowHeight;	/// for scaled osd

//----------------------------------------------------------------------------
//	VA-API Functions
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

///
///	Output video messages.
///
///	Reduce output.
///
///	@param level	message level (Error, Warning, Info, Debug, ...)
///	@param format	printf format string (NULL to flush messages)
///	@param ...	printf arguments
///
///	@returns true, if message shown
///
///	@todo FIXME: combine VdpauMessage and VaapiMessage
///
static int VaapiMessage(int level, const char *format, ...)
{
    if (LogLevel > level || DebugLevel > level) {
	static const char *last_format;
	static char buf[256];
	va_list ap;

	va_start(ap, format);
	if (format != last_format) {	// don't repeat same message
	    if (buf[0]) {		// print last repeated message
		syslog(LOG_ERR, "%s", buf);
		buf[0] = '\0';
	    }

	    if (format) {
		last_format = format;
		vsyslog(LOG_ERR, format, ap);
	    }
	    va_end(ap);
	    return 1;
	}
	if (format)
	    vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
    }
    return 0;
}

//	Surfaces -------------------------------------------------------------

///
///	Associate OSD with surface.
///
///	@param decoder	VA-API decoder
///
static void VaapiAssociate(VaapiDecoder * decoder)
{
    int x;
    int y;
    int w;
    int h;
    VAStatus va_status;

    if (VaOsdSubpicture == VA_INVALID_ID) {
	Warning(_("video/vaapi: no osd subpicture yet\n"));
	return;
    }

    x = 0;
    y = 0;
    w = VaOsdImage.width;
    h = VaOsdImage.height;

    // FIXME: associate only if osd is displayed
    if (VaapiUnscaledOsd) {
	if (decoder->SurfaceFreeN
	    && vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN, x, y, w, h, 0, 0,
		VideoWindowWidth, VideoWindowHeight,
		VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
	if (decoder->SurfaceUsedN
	    && vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesUsed, decoder->SurfaceUsedN, x, y, w, h, 0, 0,
		VideoWindowWidth, VideoWindowHeight,
		VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
        va_status = vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
                                      decoder->PostProcSurfacesRb, POSTPROC_SURFACES_MAX, x, y, w, h, 0, 0,
                                      VideoWindowWidth, VideoWindowHeight,
                                      VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD);
    } else {
	if (decoder->SurfaceFreeN
	    && vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN, x, y, w, h,
		0, 0, decoder->InputWidth,
		decoder->InputHeight, 0)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
	if (decoder->SurfaceUsedN
	    && vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesUsed, decoder->SurfaceUsedN, x, y, w, h,
		0, 0, decoder->InputWidth,
		decoder->InputHeight, 0)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
        va_status = vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
                                      decoder->PostProcSurfacesRb, POSTPROC_SURFACES_MAX, x, y, w, h, 0, 0,
                                      decoder->InputWidth, decoder->InputHeight,
                                      0);
    }

    if (va_status != VA_STATUS_SUCCESS)
        Error(_("video/vaapi: can't associate subpicture\n"));
}

///
///	Deassociate OSD with surface.
///
///	@param decoder	VA-API decoder
///
static void VaapiDeassociate(VaapiDecoder * decoder)
{
    if (VaOsdSubpicture != VA_INVALID_ID) {
	if (decoder->SurfaceFreeN
	    && vaDeassociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't deassociate %d surfaces\n"),
		decoder->SurfaceFreeN);
	}

	if (decoder->SurfaceUsedN
	    && vaDeassociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesUsed, decoder->SurfaceUsedN)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't deassociate %d surfaces\n"),
		decoder->SurfaceUsedN);
	}

        vaDeassociateSubpicture(VaDisplay, VaOsdSubpicture,
                                decoder->PostProcSurfacesRb, POSTPROC_SURFACES_MAX);
    }
}

///
///	Create surfaces for VA-API decoder.
///
///	@param decoder	VA-API decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void VaapiCreateSurfaces(VaapiDecoder * decoder, int width, int height)
{
    unsigned int format = VA_RT_FORMAT_YUV420;
#ifdef DEBUG
    if (!decoder->SurfacesNeeded) {
	Error(_("video/vaapi: surface needed not set\n"));
	decoder->SurfacesNeeded = 3 + VIDEO_SURFACES_MAX;
    }
#endif
    Debug(3, "video/vaapi: %s: %dx%d * %d\n", __FUNCTION__, width, height,
	decoder->SurfacesNeeded);

    if (VaapiBuggyIntelHD && decoder->Profile == VAProfileHEVCMain10) format = VA_RT_FORMAT_YUV420_10BPP;

    decoder->SurfaceFreeN = decoder->SurfacesNeeded;
    // VA_RT_FORMAT_YUV420 VA_RT_FORMAT_YUV422 VA_RT_FORMAT_YUV444
    if (vaCreateSurfaces(decoder->VaDisplay, format, width,
	    height, decoder->SurfacesFree, decoder->SurfaceFreeN, NULL,
	    0) != VA_STATUS_SUCCESS) {
	Fatal(_("video/vaapi: can't create %d surfaces\n"),
	    decoder->SurfaceFreeN);
	// FIXME: write error handler / fallback
    }

    if (vaCreateSurfaces(decoder->VaDisplay, format, width,
	    height, decoder->PostProcSurfacesRb, POSTPROC_SURFACES_MAX, NULL,
	    0) != VA_STATUS_SUCCESS) {
	Fatal(_("video/vaapi: can't create %d posproc surfaces\n"),
	    VIDEO_SURFACES_MAX);
    }

}

///
///	Destroy surfaces of VA-API decoder.
///
///	@param decoder	VA-API decoder
///
static void VaapiDestroySurfaces(VaapiDecoder * decoder)
{
    Debug(3, "video/vaapi: %s:\n", __FUNCTION__);

    //
    //	update OSD associate
    //
    VaapiDeassociate(decoder);

#ifdef USE_EGL
    if (gl_prog)
        glDeleteProgram(gl_prog);
    gl_prog = 0;
#endif

    if (vaDestroySurfaces(decoder->VaDisplay, decoder->SurfacesFree,
	    decoder->SurfaceFreeN)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy %d surfaces\n"),
	    decoder->SurfaceFreeN);
    }
    decoder->SurfaceFreeN = 0;
    if (vaDestroySurfaces(decoder->VaDisplay, decoder->SurfacesUsed,
	    decoder->SurfaceUsedN)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy %d surfaces\n"),
	    decoder->SurfaceUsedN);
    }
    decoder->SurfaceUsedN = 0;

    // FIXME surfaces used for output
}

///
///	Get a free surface.
///
///	@param decoder	VA-API decoder
///
///	@returns the oldest free surface
///
static VASurfaceID VaapiGetSurface0(VaapiDecoder * decoder)
{
    VASurfaceID surface;
    VASurfaceStatus status;
    int i;

    // try to use oldest surface
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	surface = decoder->SurfacesFree[i];
	if (vaQuerySurfaceStatus(decoder->VaDisplay, surface, &status)
	    != VA_STATUS_SUCCESS) {
	    // this fails with XvBA und mpeg softdecoder
	    if (!VaapiBuggyXvBA) {
		Error(_("video/vaapi: vaQuerySurface failed\n"));
	    }
	    status = VASurfaceReady;
	}
	// surface still in use, try next
	if (status != VASurfaceReady && !VaapiBuggyXvBA) {
	    Debug(4, "video/vaapi: surface %#010x not ready: %d\n", surface,
		status);
	    if (!VaapiBuggyVdpau || i < 1) {
		continue;
	    }
	    usleep(1 * 1000);
	}
	// copy remaining surfaces down
	decoder->SurfaceFreeN--;
	for (; i < decoder->SurfaceFreeN; ++i) {
	    decoder->SurfacesFree[i] = decoder->SurfacesFree[i + 1];
	}
	decoder->SurfacesFree[i] = VA_INVALID_ID;

	// save as used
	decoder->SurfacesUsed[decoder->SurfaceUsedN++] = surface;

	return surface;
    }

    Error(_("video/vaapi: out of surfaces\n"));
    return VA_INVALID_ID;
}

///
///	Release a surface.
///
///	@param decoder	VA-API decoder
///	@param surface	surface no longer used
///
static void VaapiReleaseSurface(VaapiDecoder * decoder, VASurfaceID surface)
{
    int i;

    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	if (decoder->SurfacesUsed[i] == surface) {
	    // no problem, with last used
	    decoder->SurfacesUsed[i] =
		decoder->SurfacesUsed[--decoder->SurfaceUsedN];
	    decoder->SurfacesFree[decoder->SurfaceFreeN++] = surface;
	    return;
	}
    }
    Error(_("video/vaapi: release surface %#010x, which is not in use\n"),
	surface);
}

//	Init/Exit ------------------------------------------------------------

///
///	Debug VA-API decoder frames drop...
///
///	@param decoder	video hardware decoder
///
static void VaapiPrintFrames(const VaapiDecoder * decoder)
{
    Debug(3, "video/vaapi: %d missed, %d duped, %d dropped frames of %d,%d\n",
	decoder->FramesMissed, decoder->FramesDuped, decoder->FramesDropped,
	decoder->FrameCounter, decoder->FramesDisplayed);
#ifndef DEBUG
    (void)decoder;
#endif
}

///
///	Scale value from one range to another
///
///	@param valueIn	value to scale
///	@param baseMin	original range min value
///	@param baseMax	original range max value
///	@param limitMin	new range min value
///	@param limitMax	new range max value
///	@return	scaled value
///
static inline float VaapiScale(float valueIn, float baseMin, float baseMax, float limitMin, float limitMax)
{
    return ((limitMax - limitMin) * (valueIn - baseMin) / (baseMax - baseMin)) + limitMin;
}

///
///	Normalize config values for UI
///
///	@param config	config struct to normalize
///	@param valueMin	range min value
///	@param valueMax	range max value
///	@param valueDef	range default value
///	@param step	range step
///
static inline void VaapiNormalizeConfig(VideoConfigValues * config, float valueMin, float valueMax, float valueDef, float step)
{
    config->min_value = valueMin;
    config->max_value = valueMax;
    config->def_value = valueDef;
    config->step = step;
    config->scale = config->drv_scale;
    // normalize values for UI
    while (config && config->step < 1) {
	config->min_value *= 10;
	config->max_value *= 10;
	config->def_value *= 10;
	config->step *= 10;
	config->scale /= 10;
    }
}

///
///	Initialize surface flags.
///
///	@param decoder	video hardware decoder
///
static void VaapiInitSurfaceFlags(VaapiDecoder * decoder)
{
    int i;

    for (i = 0; i < VideoResolutionMax; ++i) {
	decoder->SurfaceFlagsTable[i] = VA_CLEAR_DRAWABLE;
	// color space conversion none, ITU-R BT.601, ITU-R BT.709, ...
	switch (VideoColorSpaces[i]) {
	    case VideoColorSpaceNone:
		break;
	    case VideoColorSpaceBt601:
		decoder->SurfaceFlagsTable[i] |= VA_SRC_BT601;
		break;
	    case VideoColorSpaceBt709:
		decoder->SurfaceFlagsTable[i] |= VA_SRC_BT709;
		break;
	    case VideoColorSpaceSmpte240:
		decoder->SurfaceFlagsTable[i] |= VA_SRC_SMPTE_240;
		break;
	}

	// scaling flags FAST, HQ, NL_ANAMORPHIC
	switch (VideoScaling[i]) {
	    case VideoScalingNormal:
		decoder->SurfaceFlagsTable[i] |= VA_FILTER_SCALING_DEFAULT;
		break;
	    case VideoScalingFast:
		decoder->SurfaceFlagsTable[i] |= VA_FILTER_SCALING_FAST;
		break;
	    case VideoScalingHQ:
		// vdpau backend supports only VA_FILTER_SCALING_HQ
		// vdpau backend with advanced deinterlacer and my GT-210
		// is too slow
		decoder->SurfaceFlagsTable[i] |= VA_FILTER_SCALING_HQ;
		break;
	    case VideoScalingAnamorphic:
		// intel backend supports only VA_FILTER_SCALING_NL_ANAMORPHIC;
		// FIXME: Highlevel should display 4:3 as 16:9 to support this
		decoder->SurfaceFlagsTable[i] |=
		    VA_FILTER_SCALING_NL_ANAMORPHIC;
		break;
	}

	// deinterlace flags (not yet supported by libva)
	switch (VideoDeinterlace[i]) {
	    case VideoDeinterlaceBob:
                decoder->SurfaceDeintTable[i] = VAProcDeinterlacingBob;
		break;
	    case VideoDeinterlaceWeave:
                decoder->SurfaceDeintTable[i] = VAProcDeinterlacingWeave;
		break;
	    case VideoDeinterlaceTemporal:
                decoder->SurfaceDeintTable[i] = VAProcDeinterlacingMotionAdaptive;
		break;
	    case VideoDeinterlaceTemporalSpatial:
                decoder->SurfaceDeintTable[i] = VAProcDeinterlacingMotionCompensated;
		break;
	    default:
		break;
	}
        if (decoder->SurfaceDeintTable[i] > decoder->MaxSupportedDeinterlacer)
            Error("Selected deinterlacer for resolution %d is not supported by HW\n", i);

    }
    if (decoder->vpp_denoise_buf) {
        VAProcFilterParameterBuffer *denoise_param;
        VAStatus va_status = vaMapBuffer(VaDisplay, *decoder->vpp_denoise_buf, (void**)&denoise_param);
        if (va_status == VA_STATUS_SUCCESS) {

            /* Assuming here that the type is set before and does not need to be modified */
            denoise_param->value = VideoDenoise[decoder->Resolution] * VaapiConfigDenoise.scale;
            vaUnmapBuffer(VaDisplay, *decoder->vpp_denoise_buf);
        }
    }
    if (decoder->vpp_sharpen_buf) {
        VAProcFilterParameterBuffer *sharpen_param;
        VAStatus va_status = vaMapBuffer(VaDisplay, *decoder->vpp_sharpen_buf, (void**)&sharpen_param);
        if (va_status == VA_STATUS_SUCCESS) {
            /* Assuming here that the type is set before and does not need to be modified */
            sharpen_param->value = VideoSharpen[decoder->Resolution] * VaapiConfigSharpen.scale;
            vaUnmapBuffer(VaDisplay, *decoder->vpp_sharpen_buf);
        }
    }
    if (decoder->vpp_stde_buf) {
        VAProcFilterParameterBuffer *stde_param;
        VAStatus va_status = vaMapBuffer(VaDisplay, *decoder->vpp_stde_buf, (void**)&stde_param);
        if (va_status == VA_STATUS_SUCCESS) {
            /* Assuming here that the type is set before and does not need to be modified */
            stde_param->value = VideoSkinToneEnhancement * VaapiConfigStde.scale;
            vaUnmapBuffer(VaDisplay, *decoder->vpp_stde_buf);
        }
    }
}

///
///	Allocate new VA-API decoder.
///
///	@returns a new prepared VA-API hardware decoder.
///
static VaapiDecoder *VaapiNewHwDecoder(VideoStream * stream)
{
    VaapiDecoder *decoder;
    int i;

    if ((unsigned)VaapiDecoderN >=
	sizeof(VaapiDecoders) / sizeof(*VaapiDecoders)) {
	    Error(_("video/vaapi: out of decoders\n"));
	return NULL;
    }

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
	Fatal(_("video/vaapi: out of memory\n"));
    }
    decoder->VaDisplay = VaDisplay;
    decoder->Window = VideoWindow;
    decoder->VideoX = 0;
    decoder->VideoY = 0;
    decoder->VideoWidth = VideoWindowWidth;
    decoder->VideoHeight = VideoWindowHeight;

    VaapiInitSurfaceFlags(decoder);

    decoder->DeintImages[0].image_id = VA_INVALID_ID;
    decoder->DeintImages[1].image_id = VA_INVALID_ID;
    decoder->DeintImages[2].image_id = VA_INVALID_ID;
    decoder->DeintImages[3].image_id = VA_INVALID_ID;
    decoder->DeintImages[4].image_id = VA_INVALID_ID;

    decoder->Image->image_id = VA_INVALID_ID;

    for (i = 0; i < CODEC_SURFACES_MAX; ++i) {
	decoder->SurfacesUsed[i] = VA_INVALID_ID;
	decoder->SurfacesFree[i] = VA_INVALID_ID;
    }

    // setup video surface ring buffer
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VA_INVALID_ID;
    }
    for (i = 0; i < POSTPROC_SURFACES_MAX; ++i) {
	decoder->PostProcSurfacesRb[i] = VA_INVALID_ID;
    }
    for (i = 0; i < FIELD_SURFACES_MAX; ++i) {
	decoder->FirstFieldHistory[i] = VA_INVALID_ID;
	decoder->SecondFieldHistory[i] = VA_INVALID_ID;
    }

    // Initialize postprocessing surfaces to 0
    // They are allocated on-demand
    if (decoder->ForwardRefSurfaces)
        free(decoder->ForwardRefSurfaces);
    decoder->ForwardRefSurfaces = NULL;
    decoder->ForwardRefCount = 0;

    decoder->PlaybackSurface = VA_INVALID_ID;

    if (decoder->BackwardRefSurfaces)
        free(decoder->BackwardRefSurfaces);
    decoder->BackwardRefSurfaces = NULL;
    decoder->BackwardRefCount = 0;

    // Initialize vpp filter chain
    for (i = 0; i < VAProcFilterCount; ++i) {
        decoder->filters[i] = VA_INVALID_ID;
        decoder->gpe_filters[i] = VA_INVALID_ID;
    }
    decoder->filter_n = 0;
    decoder->gpe_filter_n = 0;

    decoder->MaxSupportedDeinterlacer = 0;

    decoder->vpp_deinterlace_buf = NULL;
    decoder->vpp_denoise_buf = NULL;
    decoder->vpp_sharpen_buf = NULL;
    decoder->vpp_stde_buf = NULL;
    decoder->vpp_brightness_idx = -1;
    decoder->vpp_contrast_idx = -1;
    decoder->vpp_saturation_idx = -1;
    decoder->vpp_hue_idx = -1;

#ifdef VA_EXP
    decoder->LastSurface = VA_INVALID_ID;
#endif

    decoder->BlackSurface = VA_INVALID_ID;

    //
    //	Setup ffmpeg vaapi context
    //
    decoder->Profile = VA_INVALID_ID;
    decoder->Entrypoint = VA_INVALID_ID;
    decoder->VppEntrypoint = VA_INVALID_ID;
    decoder->VppConfig = VA_INVALID_ID;
    decoder->vpp_ctx = VA_INVALID_ID;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,74,100)
    decoder->VaapiContext->display = VaDisplay;
    decoder->VaapiContext->config_id = VA_INVALID_ID;
    decoder->VaapiContext->context_id = VA_INVALID_ID;
#endif

#ifdef USE_GLX
    decoder->GlxSurfaces[0] = NULL;
    decoder->GlxSurfaces[1] = NULL;
    if (GlxEnabled) {
	// FIXME: create GLX context here
    }
#endif

    decoder->OutputWidth = VideoWindowWidth;
    decoder->OutputHeight = VideoWindowHeight;

    decoder->PixFmt = AV_PIX_FMT_NONE;

    decoder->Stream = stream;
    if (!VaapiDecoderN) {		// FIXME: hack sync on audio
	decoder->SyncOnAudio = 1;
    }
    decoder->Closing = -300 - 1;

    decoder->PTS = AV_NOPTS_VALUE;

    // old va-api intel driver didn't supported get/put-image.
#if VA_CHECK_VERSION(0,33,99)
    // FIXME: not the exact version with support
    decoder->GetPutImage = 1;
#else
    decoder->GetPutImage = !VaapiBuggyIntel;
#endif

    VaapiDecoders[VaapiDecoderN++] = decoder;

    return decoder;
}

///
///	Cleanup VA-API.
///
///	@param decoder	va-api hw decoder
///
static void VaapiCleanup(VaapiDecoder * decoder)
{
    int filled;
    VASurfaceID surface;
    unsigned int i;

    pthread_mutex_lock(&VideoMutex);

    // flush output queue, only 1-2 frames buffered, no big loss
    while ((filled = atomic_read(&decoder->SurfacesFilled))) {
	decoder->SurfaceRead = (decoder->SurfaceRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&decoder->SurfacesFilled);

	surface = decoder->SurfacesRb[decoder->SurfaceRead];
	if (surface == VA_INVALID_ID) {
	    Error(_("video/vaapi: invalid surface in ringbuffer\n"));
	    continue;
	}
	// can crash and hang
	if (0 && vaSyncSurface(decoder->VaDisplay, surface)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
    }

#ifdef DEBUG
    if (decoder->SurfaceRead != decoder->SurfaceWrite) {
        Error("Surface queue mismatch. SurfaceRead = %d, SurfaceWrite = %d, SurfacesFilled = %d\n",
              decoder->SurfaceRead, decoder->SurfaceWrite, atomic_read(&decoder->SurfacesFilled));
    }
#endif

    // clear ring buffer
    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VA_INVALID_ID;
    }
    if(!VaapiBuggyVdpau)
        vaDestroySurfaces(VaDisplay, decoder->PostProcSurfacesRb, POSTPROC_SURFACES_MAX);
    for (i = 0; i < POSTPROC_SURFACES_MAX; ++i) {
	decoder->PostProcSurfacesRb[i] = VA_INVALID_ID;
    }
    for (i = 0; i < FIELD_SURFACES_MAX; ++i) {
	decoder->FirstFieldHistory[i] = VA_INVALID_ID;
	decoder->SecondFieldHistory[i] = VA_INVALID_ID;
    }

    // clear forward/backward references for vpp
    if (decoder->ForwardRefSurfaces)
        free(decoder->ForwardRefSurfaces);
    decoder->ForwardRefSurfaces = NULL;
    decoder->ForwardRefCount = 0;

    decoder->PlaybackSurface = VA_INVALID_ID;

    if (decoder->BackwardRefSurfaces)
        free(decoder->BackwardRefSurfaces);
    decoder->BackwardRefSurfaces = NULL;
    decoder->BackwardRefCount = 0;

    // Free & clear vpp filter chain
    for (i = 0; i < decoder->filter_n; ++i) {
        vaDestroyBuffer(VaDisplay, decoder->filters[i]);
        vaDestroyBuffer(VaDisplay, decoder->gpe_filters[i]);
        decoder->filters[i] = VA_INVALID_ID;
        decoder->gpe_filters[i] = VA_INVALID_ID;
    }
    decoder->filter_n = 0;
    decoder->gpe_filter_n = 0;

#ifdef VA_EXP
    decoder->LastSurface = VA_INVALID_ID;
#endif

    //	cleanup image
    if (decoder->Image->image_id != VA_INVALID_ID) {
	if (vaDestroyImage(VaDisplay,
		decoder->Image->image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	decoder->Image->image_id = VA_INVALID_ID;
    }
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,74,100)
    //	cleanup context and config
    if (decoder->VaapiContext) {

	if (decoder->VaapiContext->context_id != VA_INVALID_ID) {
	    if (vaDestroyContext(VaDisplay,
		    decoder->VaapiContext->context_id) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't destroy context!\n"));
	    }
	    decoder->VaapiContext->context_id = VA_INVALID_ID;
	}

	if (decoder->VaapiContext->config_id != VA_INVALID_ID) {
	    if (vaDestroyConfig(VaDisplay,
		    decoder->VaapiContext->config_id) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't destroy config!\n"));
	    }
	    decoder->VaapiContext->config_id = VA_INVALID_ID;
	}
    }
#endif
    if (vaDestroyContext(VaDisplay, decoder->vpp_ctx) != VA_STATUS_SUCCESS) {
        Error(_("video/vaapi: can't destroy postproc context!\n"));
    }
    decoder->vpp_ctx = VA_INVALID_ID;

    if (vaDestroyConfig(VaDisplay, decoder->VppConfig) != VA_STATUS_SUCCESS) {
        Error(_("video/vaapi: can't destroy config!\n"));
    }
    decoder->VppConfig = VA_INVALID_ID;

    //	cleanup surfaces
    if (decoder->SurfaceFreeN || decoder->SurfaceUsedN) {
	VaapiDestroySurfaces(decoder);
    }
    // cleanup images
    if (decoder->DeintImages[0].image_id != VA_INVALID_ID) {
	VaapiDestroyDeinterlaceImages(decoder);
    }

    decoder->SurfaceRead = 0;
    decoder->SurfaceWrite = 0;
    decoder->SurfaceField = 0;
    decoder->Interlaced = 0;
    decoder->PostProcSurfaceWrite = 0;
    decoder->SyncCounter = 0;
    decoder->FrameCounter = 0;
    decoder->FramesDisplayed = 0;
    decoder->StartCounter = 0;
    decoder->Closing = 0;
    decoder->PTS = AV_NOPTS_VALUE;
    VideoDeltaPTS = 0;
    pthread_mutex_unlock(&VideoMutex);
}

///
///	Destroy a VA-API decoder.
///
///	@param decoder	VA-API decoder
///
static void VaapiDelHwDecoder(VaapiDecoder * decoder)
{
    int i;

    for (i = 0; i < VaapiDecoderN; ++i) {
	if (VaapiDecoders[i] == decoder) {
	    VaapiDecoders[i] = NULL;
	    VaapiDecoderN--;
	    // FIXME: must copy last slot into empty slot and --
	    break;
	}
    }

    VaapiCleanup(decoder);

    if (decoder->BlackSurface != VA_INVALID_ID) {
	//
	//	update OSD associate
	//
	if (VaOsdSubpicture != VA_INVALID_ID) {
	    if (vaDeassociateSubpicture(VaDisplay, VaOsdSubpicture,
		    &decoder->BlackSurface, 1) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't deassociate black surfaces\n"));
	    }
	}
	if (vaDestroySurfaces(decoder->VaDisplay, &decoder->BlackSurface, 1)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy a surface\n"));
	}
    }
    if (!VaapiDecoders[0]) {
#if defined USE_GLX && !defined VAAPI_NO_GLX
    if (decoder->GlxSurfaces[0]) {
	if (vaDestroySurfaceGLX(VaDisplay, decoder->GlxSurfaces[0])
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy glx surface!\n"));
	}
	decoder->GlxSurfaces[0] = NULL;
    }
    if (decoder->GlxSurfaces[1]) {
	if (vaDestroySurfaceGLX(VaDisplay, decoder->GlxSurfaces[1])
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy glx surface!\n"));
	}
	decoder->GlxSurfaces[1] = NULL;
    }
#endif
#if defined USE_GLX || defined USE_EGL
    if (decoder->GlTextures[0]) {
	glDeleteTextures(2, decoder->GlTextures);
    }
#endif
    }
    VaapiPrintFrames(decoder);

    free(decoder);
}

#ifdef DEBUG				// currently unused, keep it for later

static VAProfile VaapiFindProfile(const VAProfile * profiles, unsigned n,
    VAProfile profile);
static VAEntrypoint VaapiFindEntrypoint(const VAEntrypoint * entrypoints,
    unsigned n, VAEntrypoint entrypoint);

///
///	1080i
///
static void Vaapi1080i(void)
{
    VAProfile profiles[vaMaxNumProfiles(VaDisplay)];
    int profile_n;
    VAEntrypoint entrypoints[vaMaxNumEntrypoints(VaDisplay)];
    int entrypoint_n;
    int p;
    int e;
    VAConfigAttrib attrib;
    VAConfigID config_id;
    VAContextID context_id;
    VASurfaceID surfaces[32];
    VAImage image[1];
    int n;
    uint32_t start_tick;
    uint32_t tick;

    p = -1;
    e = -1;

    //	prepare va-api profiles
    if (vaQueryConfigProfiles(VaDisplay, profiles, &profile_n)) {
	Error(_("codec: vaQueryConfigProfiles failed"));
	return;
    }
    // check profile
    p = VaapiFindProfile(profiles, profile_n, VAProfileH264High);
    if (p == -1) {
	p = VaapiFindProfile(profiles, profile_n, VAProfileHEVCMain10);
    }
    if (p == -1) {
	p = VaapiFindProfile(profiles, profile_n, VAProfileHEVCMain);
    }
    if (p == -1) {
	Debug(3, "\tno profile found\n");
	return;
    }
    // prepare va-api entry points
    if (vaQueryConfigEntrypoints(VaDisplay, p, entrypoints, &entrypoint_n)) {
	Error(_("codec: vaQueryConfigEntrypoints failed"));
	return;
    }
    e = VaapiFindEntrypoint(entrypoints, entrypoint_n, VAEntrypointVLD);
    if (e == -1) {
	Warning(_("codec: unsupported: slow path\n"));
	return;
    }
    memset(&attrib, 0, sizeof(attrib));
    attrib.type = VAConfigAttribRTFormat;
    attrib.value = VA_RT_FORMAT_YUV420;
    // create a configuration for the decode pipeline
    if (vaCreateConfig(VaDisplay, p, e, &attrib, 1, &config_id)) {
	Error(_("codec: can't create config"));
	return;
    }
    if (vaCreateSurfaces(VaDisplay, VA_RT_FORMAT_YUV420, 1920, 1080, surfaces,
	    32, NULL, 0) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create surfaces\n"));
	return;
    }
    // bind surfaces to context
    if (vaCreateContext(VaDisplay, config_id, 1920, 1080, VA_PROGRESSIVE,
	    surfaces, 32, &context_id)) {
	Error(_("codec: can't create context"));
	return;
    }
#if 1
    // without this 1080i will crash
    image->image_id = VA_INVALID_ID;
    if (vaDeriveImage(VaDisplay, surfaces[0], image)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed\n"));
    }
    if (image->image_id != VA_INVALID_ID) {
	if (vaDestroyImage(VaDisplay, image->image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
    }
#else
    vaBeginPicture(VaDisplay, context_id, surfaces[0]);
    vaRenderPicture(VaDisplay, context_id, NULL, 0);
    // aborts without valid buffers upload
    vaEndPicture(VaDisplay, context_id);
#endif

    start_tick = GetMsTicks();
    for (n = 1; n < 2; ++n) {
	if (vaPutSurface(VaDisplay, surfaces[0], VideoWindow,
		// decoder src
		0, 0, 1920, 1080,
		// video dst
		0, 0, 1920, 1080, NULL, 0, VA_TOP_FIELD | VA_CLEAR_DRAWABLE)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaPutSurface failed\n"));
	}
	if (vaPutSurface(VaDisplay, surfaces[0], VideoWindow,
		// decoder src
		0, 0, 1920, 1080,
		// video dst
		0, 0, 1920, 1080, NULL, 0, VA_BOTTOM_FIELD | VA_CLEAR_DRAWABLE)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaPutSurface failed\n"));
	}
	tick = GetMsTicks();
#ifdef DEBUG
	if (!(n % 10)) {
	    fprintf(stderr, "%dms / frame\n", (tick - start_tick) / n);
	}
#endif
    }

    // destory the stuff.
    if (vaDestroyContext(VaDisplay, context_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy context!\n"));
    }
    if (vaDestroySurfaces(VaDisplay, surfaces, 32) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy surfaces\n"));
    }
    if (vaDestroyConfig(VaDisplay, config_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy config!\n"));
    }
#ifdef DEBUG
    fprintf(stderr, "done\n");
#endif
}

#endif

///
///	VA-API setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns true if VA-API could be initialized, false otherwise.
///
static int VaapiInit(const char *display_name)
{
    int major;
    int minor;
    VADisplayAttribute attr;
    const char *s;

    VaOsdImage.image_id = VA_INVALID_ID;
    VaOsdSubpicture = VA_INVALID_ID;

#if defined USE_GLX && !defined VAAPI_NO_GLX
    if (GlxEnabled) {			// support glx
	VaDisplay = vaGetDisplayGLX(XlibDisplay);
    } else
#endif
    {
	VaDisplay = vaGetDisplay(XlibDisplay);
    }
    if (!VaDisplay) {
	Error(_("video/vaapi: Can't connect VA-API to X11 server on '%s'\n"),
	    display_name);
	return 0;
    }
    // XvBA needs this:
    setenv("DISPLAY", display_name, 1);

#ifndef DEBUG
#if VA_CHECK_VERSION(1,0,0)
    vaSetErrorCallback(VaDisplay, NULL, NULL);
    vaSetInfoCallback(VaDisplay, NULL, NULL);
#elif VA_CHECK_VERSION(0,40,0)
    vaSetErrorCallback(NULL);
    vaSetInfoCallback(NULL);
#endif
#endif
    if (vaInitialize(VaDisplay, &major, &minor) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: Can't inititialize VA-API on '%s'\n"),
	    display_name);
	vaTerminate(VaDisplay);
	VaDisplay = NULL;
	return 0;
    }
    s = vaQueryVendorString(VaDisplay);
    Info(_("video/vaapi: libva %d.%d (%s) initialized\n"), major, minor, s);

    //
    //	Setup fixes for driver bugs.
    //
    if (strstr(s, "VDPAU")) {
	Info(_("video/vaapi: use vdpau bug workaround\n"));
	setenv("VDPAU_VIDEO_PUTSURFACE_FAST", "0", 0);
	VaapiBuggyVdpau = 1;
    }
    if (strstr(s, "XvBA")) {
	VaapiBuggyXvBA = 1;
    }
    if (strstr(s, "AMD")) {
	VaapiBuggyXvBA = 1;
    }
    if (strstr(s, "Intel i965")) {
	VaapiBuggyIntel = 1;
    }
    if (strstr(s, "Intel iHD")) {
	VaapiBuggyIntelHD = 1;
	VaapiBuggyIntel = 1;
    }
    //
    //	check which attributes are supported
    //
    attr.type = VADisplayAttribBackgroundColor;
    attr.flags = VA_DISPLAY_ATTRIB_SETTABLE;
    if (vaGetDisplayAttributes(VaDisplay, &attr, 1) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: Can't get background-color attribute\n"));
	attr.value = 1;
    }
    Info(_("video/vaapi: background-color is %s\n"),
	attr.value ? _("supported") : _("unsupported"));

    // FIXME: VaapiSetBackground(VideoBackground);

#if 0
    //
    //	check the chroma format
    //
    attr.type = VAConfigAttribRTFormat attr.flags = VA_DISPLAY_ATTRIB_GETTABLE;
    Vaapi1080i();
#endif

#if VA_CHECK_VERSION(0,33,99)
    //
    //	check vpp support
    //
    if (1) {
	VAEntrypoint entrypoints[vaMaxNumEntrypoints(VaDisplay)];
	int entrypoint_n;
	int i;

	VaapiVideoProcessing = 0;
	if (!vaQueryConfigEntrypoints(VaDisplay, VAProfileNone, entrypoints,
		&entrypoint_n)) {

	    for (i = 0; i < entrypoint_n; i++) {
		if (entrypoints[i] == VAEntrypointVideoProc) {
		    Info("video/vaapi: supports video processing\n");
		    VaapiVideoProcessing = 1;
		    break;
		}
	    }
	}
    }
#endif
    return 1;
}

#ifdef USE_GLX

///
///	VA-API GLX setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns true if VA-API could be initialized, false otherwise.
///
static int VaapiGlxInit(const char *display_name)
{
    GlxEnabled = 1;

    GlxInit();
    if (GlxEnabled) {
	GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight,
	    GlxSharedContext);
    }
    if (!GlxEnabled) {
	Error(_("video/glx: glx error\n"));
    }

    return VaapiInit(display_name);
}

#endif

#ifdef USE_EGL

///
///	VA-API EGL setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns true if VA-API could be initialized, false otherwise.
///
static int VaapiEglInit(const char *display_name)
{
    EglEnabled = 1;

    EglInit();
    if (EglEnabled) {
	EglSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight,
	    EglSharedContext);
    }
    if (!EglEnabled) {
	Error(_("video/egl: eglx error\n"));
    }

    return VaapiInit(display_name);
}

#endif

///
///	VA-API cleanup
///
static void VaapiExit(void)
{
    int i;

// FIXME: more VA-API cleanups...

    for (i = 0; i < VaapiDecoderN; ++i) {
	if (VaapiDecoders[i]) {
	    VaapiDelHwDecoder(VaapiDecoders[i]);
	    VaapiDecoders[i] = NULL;
	}
    }
    VaapiDecoderN = 0;
#ifdef USE_EGL
    if (vao_buffer)
        glDeleteBuffers(1, &vao_buffer);
    vao_buffer = 0;
#endif
    //for scaled osd
    lastWindowWidth = 0;
    lastWindowHeight = 0;

    if (!VaDisplay) {
	vaTerminate(VaDisplay);
	VaDisplay = NULL;
    }
}

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57,74,100)
void vaapi_uninit(AVCodecContext *s)
{
    VideoDecoder *ist = s->opaque;

    ist->hwaccel_uninit = NULL;
    av_buffer_unref(&s->hw_frames_ctx);
}

static int vaapi_alloc(AVCodecContext *s)
{
    VideoDecoder *ist = s->opaque;
    int ret;

    AVHWDeviceContext *device_ctx;
    AVVAAPIDeviceContext *device_hwctx;
    AVHWFramesContext *frames_ctx;
    AVBufferRef *hw_device_ctx;

    Debug(3, "vaapi_alloc\n");

    ist->hwaccel_uninit = vaapi_uninit;

    hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
    if (!hw_device_ctx) {
	Debug(3, "VAAPI init failed for av_hwdevice_ctx_alloc\n");
	goto fail;
    }

    s->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    device_ctx = (AVHWDeviceContext*)s->hw_device_ctx->data;
    device_hwctx = device_ctx->hwctx;
    device_hwctx->display = VaDisplay;

    ret = av_hwdevice_ctx_init(s->hw_device_ctx);
    if (ret < 0) {
	Debug(3, "VAAPI init failed for av_hwdevice_ctx_init\n");
	goto fail;
    }

    s->hw_frames_ctx = av_hwframe_ctx_alloc(s->hw_device_ctx);
    if (!s->hw_frames_ctx) {
	Debug(3, "VAAPI init failed for av_hwframe_ctx_alloc\n");
	goto fail;
    }

    frames_ctx = (AVHWFramesContext*)s->hw_frames_ctx->data;
    frames_ctx->format = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_YUV420P;
    frames_ctx->width = s->width;
    frames_ctx->height = s->height;

    ret = av_hwframe_ctx_init(s->hw_frames_ctx);
    if (ret < 0) {
	Debug(3, "VAAPI init failed for av_hwframe_ctx_init\n");
	goto fail;
    }

    return 0;
fail:
    Debug(3, "VAAPI init failed for stream #\n");
    vaapi_uninit(s);
    return AVERROR(EINVAL);
}

int vaapi_init(AVCodecContext *s)
{
    if (s->hw_frames_ctx)
	vaapi_uninit(s);
    if (!s->hw_frames_ctx) {
	int ret = vaapi_alloc(s);
	if (ret < 0)
	    return ret;
    }
    return 0;
}
#endif

//----------------------------------------------------------------------------

///
///	Update output for new size or aspect ratio.
///
///	@param decoder	VA-API decoder
///
static void VaapiUpdateOutput(VaapiDecoder * decoder)
{
    VideoUpdateOutput(decoder->InputAspect, decoder->InputWidth,
	decoder->InputHeight, decoder->Resolution, decoder->VideoX,
	decoder->VideoY, decoder->VideoWidth, decoder->VideoHeight,
	&decoder->OutputX, &decoder->OutputY, &decoder->OutputWidth,
	&decoder->OutputHeight, &decoder->CropX, &decoder->CropY,
	&decoder->CropWidth, &decoder->CropHeight);
#ifdef USE_AUTOCROP
    decoder->AutoCrop->State = 0;
    decoder->AutoCrop->Count = AutoCropDelay;
#endif
}

///
///	Find VA-API image format.
///
///	@param decoder	VA-API decoder
///	@param pix_fmt		ffmpeg pixel format
///	@param[out] format	image format
///
///	FIXME: can fallback from I420 to YV12, if not supported
///	FIXME: must check if put/get with this format is supported (see intel)
///
static int VaapiFindImageFormat(VaapiDecoder * decoder,
    enum AVPixelFormat pix_fmt, VAImageFormat * format)
{
    VAImageFormat *imgfrmts;
    int imgfrmt_n;
    int i;
    unsigned fourcc;

    switch (pix_fmt) {			// convert ffmpeg to VA-API
	    // NV12, YV12, I420, BGRA
	    // intel: I420 is native format for MPEG-2 decoded surfaces
	    // intel: NV12 is native format for H.264 decoded surfaces
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
	    // fourcc = VA_FOURCC_YV12; // YVU
	    fourcc = VA_FOURCC('I', '4', '2', '0');	// YUV
	    break;
	case AV_PIX_FMT_NV12:
	    fourcc = VA_FOURCC_NV12;
	    break;
	case AV_PIX_FMT_BGRA:
	    fourcc = VA_FOURCC_BGRX;
	    break;
	case AV_PIX_FMT_RGBA:
	    fourcc = VA_FOURCC_RGBX;
	    break;
	default:
	    Fatal(_("video/vaapi: unsupported pixel format %d\n"), pix_fmt);
    }

    imgfrmt_n = vaMaxNumImageFormats(decoder->VaDisplay);
    imgfrmts = alloca(imgfrmt_n * sizeof(*imgfrmts));

    if (vaQueryImageFormats(decoder->VaDisplay, imgfrmts, &imgfrmt_n)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaQueryImageFormats failed\n"));
	return 0;
    }
    Debug(3, "video/vaapi: search format %c%c%c%c in %d image formats\n",
	fourcc, fourcc >> 8, fourcc >> 16, fourcc >> 24, imgfrmt_n);
    Debug(3, "video/vaapi: supported image formats:\n");
    for (i = 0; i < imgfrmt_n; ++i) {
	Debug(3, "video/vaapi:\t%c%c%c%c\t%d\n", imgfrmts[i].fourcc,
	    imgfrmts[i].fourcc >> 8, imgfrmts[i].fourcc >> 16,
	    imgfrmts[i].fourcc >> 24, imgfrmts[i].depth);
    }
    //
    //	search image format
    //
    for (i = 0; i < imgfrmt_n; ++i) {
	if (imgfrmts[i].fourcc == fourcc) {
	    *format = imgfrmts[i];
	    Debug(3, "video/vaapi: use\t%c%c%c%c\t%d\n", imgfrmts[i].fourcc,
		imgfrmts[i].fourcc >> 8, imgfrmts[i].fourcc >> 16,
		imgfrmts[i].fourcc >> 24, imgfrmts[i].depth);
	    return 1;
	}
    }

    Fatal("video/vaapi: pixel format %d unsupported by VA-API\n", pix_fmt);
    // FIXME: no fatal error!

    return 0;
}

///
///	Verify & Run arbitrary VPP processing on src/dst surface(s)
///
///	@param ctx[in]			VA-API postprocessing context
///	@param src[in]			source surface to scale
///	@param dst[in]			destination surface to put result in
///	@param filters[in]		array of VABufferID filters to run
///	@param num_filters[in]		number of VABufferID filters supplied
///	@param filter_flags[in]		filter flags to provide to postprocessing
///	@param pipeline_flags[in]	pipeline flags to provide to postprocessing
///	@param frefs[in]		array of forward reference surface ids
///	@param num_frefs[in,out]	number of forward reference surface ids supplied/needed
///	@param brefs[in]		array of backward reference surface ids
///	@param num_brefs[in,out]	number of backward reference surface ids supplied/needed
///
static VAStatus VaapiPostprocessSurface(VAContextID ctx,
		    VASurfaceID src, VASurfaceID dst,
		    VABufferID* filters, unsigned int num_filters,
		    int filter_flags, int pipeline_flags,
		    VASurfaceID* frefs, unsigned int* num_frefs,
		    VASurfaceID* brefs, unsigned int* num_brefs)
{
    unsigned int i;
    unsigned int tmp_num_frefs = 0;
    unsigned int tmp_num_brefs = 0;
    VAStatus va_status;
    VASurfaceStatus va_surf_status;
    VABufferID pipeline_buf;
    VAProcPipelineCaps pipeline_caps;
    VAProcPipelineParameterBuffer pipeline_param;

    if (!num_frefs)
	num_frefs = &tmp_num_frefs;
    if (!num_brefs)
	num_brefs = &tmp_num_brefs;


    /* Make sure rendering is finished in earliest forward reference surface */
    if (*num_frefs)
        vaSyncSurface(VaDisplay, frefs[*num_frefs - 1]);

    /* Skip postprocessing if queue is not deinterlaceable */
    for (i = 0; i < *num_brefs; ++i) {
        va_status = vaQuerySurfaceStatus(VaDisplay, brefs[i], &va_surf_status);
        if (va_status != VA_STATUS_SUCCESS) {
            Error("vaapi/vpp: Surface %d query status failed (0x%X): %s\n", i, va_status, vaErrorStr(va_status));
            return va_status;
        }
        if (va_surf_status != VASurfaceReady) {
            if (!VaapiBuggyXvBA) {
                Info("Backward reference surface %d is not ready, surf_status = %d\n", i, va_surf_status);
                return VA_STATUS_ERROR_SURFACE_BUSY;
            }
        }
    }

    for (i = 0; i < *num_frefs; ++i) {
        va_status = vaQuerySurfaceStatus(VaDisplay, frefs[i], &va_surf_status);
        if (va_status != VA_STATUS_SUCCESS) {
            Error("Surface %d query status = 0x%X: %s\n", i, va_status, vaErrorStr(va_status));
            return va_status;
        }
        if (va_surf_status != VASurfaceReady) {
            if (!VaapiBuggyXvBA) {
                Info("Forward reference surface %d is not ready, surf_status = %d\n", i, va_surf_status);
                return VA_STATUS_ERROR_SURFACE_BUSY;
            }
        }
    }

    va_status = vaQueryVideoProcPipelineCaps(VaDisplay, ctx,
					     filters, num_filters,
					     &pipeline_caps);
    if (va_status != VA_STATUS_SUCCESS) {
        Error("vaapi/vpp: query pipeline caps failed (0x%x): %s\n", va_status, vaErrorStr(va_status));
        return va_status;
    }

    if (pipeline_caps.num_forward_references != *num_frefs) {
	Debug(3, "vaapi/vpp: Wrong number of forward references. Needed %d, got %d",
		pipeline_caps.num_forward_references, *num_frefs);
	/* Fail operation when needing more references than currently have */
	if (pipeline_caps.num_forward_references > *num_frefs) {
	    *num_frefs = pipeline_caps.num_forward_references;
	    *num_brefs = pipeline_caps.num_backward_references;
	    return VA_STATUS_ERROR_INVALID_PARAMETER;
	}
    }

    if (pipeline_caps.num_backward_references != *num_brefs) {
	Debug(3, "vaapi/vpp: Wrong number of backward references. Needed %d, got %d",
		pipeline_caps.num_forward_references, *num_brefs);
	/* Fail operation when needing more references than currently have */
	if (pipeline_caps.num_backward_references > *num_brefs) {
	    *num_frefs = pipeline_caps.num_forward_references;
	    *num_brefs = pipeline_caps.num_backward_references;
	    return VA_STATUS_ERROR_INVALID_PARAMETER;
	}
    }

    *num_frefs = pipeline_caps.num_forward_references;
    *num_brefs = pipeline_caps.num_backward_references;

    if (src == VA_INVALID_ID || dst == VA_INVALID_ID || src == dst)
	return VA_STATUS_ERROR_INVALID_PARAMETER;

    memset(&pipeline_param, '\0', sizeof(VAProcPipelineParameterBuffer));

    pipeline_param.surface                 = src;
    pipeline_param.surface_region          = NULL;
    pipeline_param.surface_color_standard  = VAProcColorStandardNone;
    pipeline_param.output_region           = NULL;
    pipeline_param.output_background_color = 0xff000000;
    pipeline_param.output_color_standard   = VAProcColorStandardNone;
    pipeline_param.pipeline_flags          = pipeline_flags;
    pipeline_param.filter_flags            = filter_flags;
    pipeline_param.filters                 = filters;
    pipeline_param.num_filters             = num_filters;

    pipeline_param.forward_references      = frefs;
    pipeline_param.num_forward_references  = *num_frefs;
    pipeline_param.backward_references     = brefs;
    pipeline_param.num_backward_references = *num_brefs;


    va_status = vaCreateBuffer(VaDisplay, ctx,
			       VAProcPipelineParameterBufferType, sizeof(VAProcPipelineParameterBuffer), 1,
			       &pipeline_param, &pipeline_buf);
    if (va_status != VA_STATUS_SUCCESS) {
        Error("vaapi/vpp: createbuffer failed (0x%x): %s\n", va_status, vaErrorStr(va_status));
        return va_status;
    }

    va_status = vaBeginPicture(VaDisplay, ctx, dst);
    if (va_status != VA_STATUS_SUCCESS) {
        Error("vaapi/vpp: begin picture failed (0x%x): %s\n", va_status, vaErrorStr(va_status));
        return va_status;
    }

    va_status = vaRenderPicture(VaDisplay, ctx, &pipeline_buf, 1);
    if (va_status != VA_STATUS_SUCCESS) {
        Error("vaapi/vpp: Postprocessing failed (0x%X): %s\n", va_status, vaErrorStr(va_status));
        return va_status;
    }
    vaEndPicture(VaDisplay, ctx);
    vaDestroyBuffer(VaDisplay, pipeline_buf);
    return VA_STATUS_SUCCESS;
}

///
///	Convert & Scale between source / destination surfaces
///
///	@param ctx[in]			VA-API postprocessing context
///	@param src[in]			source surface to scale
///	@param dst[in]			destination surface to put result in
static inline VAStatus VaapiRunScaling(VAContextID ctx,
					VASurfaceID src, VASurfaceID dst)
{
    return VaapiPostprocessSurface(ctx, src, dst,
		NULL, 0, VA_FILTER_SCALING_HQ, VA_PROC_PIPELINE_SUBPICTURES,
		NULL, 0, NULL, 0);
}

///
///	Construct and apply filters to a surface (should be called after queuing new surface)
///
///	@param decoder	VA-API decoder
///	@param top_field top field is first
///	@return Pointer to postprocessed surface or NULL if postprocessing failed
///
///	@note we can't mix software and hardware decoder surfaces
///
static VASurfaceID* VaapiApplyFilters(VaapiDecoder * decoder, int top_field)
{
    unsigned int i;
    unsigned int filter_count = 0;
    unsigned int filter_flags = decoder->SurfaceFlagsTable[decoder->Resolution];
    unsigned int tmp_forwardRefCount = decoder->ForwardRefCount;
    unsigned int tmp_backwardRefCount = decoder->BackwardRefCount;
    VAStatus va_status;
    VABufferID filters_to_run[VAProcFilterCount];
    VAProcFilterParameterBufferDeinterlacing *deinterlace = NULL;
    VASurfaceID *surface = NULL;
    VASurfaceID *gpe_surface = NULL;

    /* No postprocessing filters enabled */
    if (!decoder->filter_n)
	return NULL;

    /* Get next postproc surface to write from ring buffer */
    decoder->PostProcSurfaceWrite = (decoder->PostProcSurfaceWrite + 1) % POSTPROC_SURFACES_MAX;
    surface = &decoder->PostProcSurfacesRb[decoder->PostProcSurfaceWrite];

    if (decoder->Deinterlaced || !decoder->Interlaced)
	filter_flags |= VA_FRAME_PICTURE;
    else if (decoder->Interlaced)
	filter_flags |= top_field ? VA_TOP_FIELD : VA_BOTTOM_FIELD;


    memcpy(filters_to_run, decoder->filters, VAProcFilterCount * sizeof(VABufferID));
    filter_count = decoder->filter_n;

    /* Map deinterlace buffer and handle field ordering */
    if (!(VaapiBuggyIntelHD && !decoder->Interlaced) && decoder->vpp_deinterlace_buf) {

        va_status = vaMapBuffer(VaDisplay, *decoder->vpp_deinterlace_buf, (void**)&deinterlace);
        if (va_status != VA_STATUS_SUCCESS) {
            Error("deint map buffer va_status = 0x%X\n", va_status);
            return NULL;
        }
        /* Change deint algorithm as set in plugin menu */
        deinterlace->algorithm = decoder->SurfaceDeintTable[decoder->Resolution];

        if (top_field)
            deinterlace->flags = 0;
        else
            deinterlace->flags = VA_DEINTERLACING_BOTTOM_FIELD;

        if (!decoder->TopFieldFirst)
            deinterlace->flags |= VA_DEINTERLACING_BOTTOM_FIELD_FIRST;
        /* If non-interlaced then override flags with one field setup */
        if (!decoder->Interlaced)
            deinterlace->flags = VA_DEINTERLACING_ONE_FIELD;

        /* This block of code skips various filters in-flight if source/settings
           disallow running the filter in question */
        filter_count = 0;
        for (i = 0; i < decoder->filter_n; ++i) {

            /* Skip deinterlacer if disabled or source is not interlaced */
            if (decoder->filters[i] == *decoder->vpp_deinterlace_buf) {
                if (!decoder->Interlaced)
                    continue;
                if (deinterlace->algorithm == VAProcDeinterlacingNone ||
                    deinterlace->algorithm == VAProcDeinterlacingWeave)
                    continue;
                if (deinterlace->algorithm > decoder->MaxSupportedDeinterlacer)
                    continue;
            }

            /* Skip denoise if value is set to 0 ("off") */
            if (decoder->vpp_denoise_buf &&
                decoder->filters[i] == *decoder->vpp_denoise_buf) {
                if (!VideoDenoise[decoder->Resolution])
                    continue;
            }

            /* Skip skin tone enhancement if value is set to 0 ("off") */
            if (decoder->vpp_stde_buf &&
                decoder->filters[i] == *decoder->vpp_stde_buf) {
                if (!VideoSkinToneEnhancement)
                    continue;
            }

            filters_to_run[filter_count++] = decoder->filters[i];
        }

        vaUnmapBuffer(VaDisplay, *decoder->vpp_deinterlace_buf);
    }

    if (!filter_count)
	return NULL; /* no postprocessing if no filters applied */

    va_status = VaapiPostprocessSurface(decoder->vpp_ctx, decoder->PlaybackSurface, *surface,
		    filters_to_run, filter_count,
		    filter_flags, 0,
		    decoder->ForwardRefSurfaces, &tmp_forwardRefCount,
		    decoder->BackwardRefSurfaces, &tmp_backwardRefCount);

    if (tmp_forwardRefCount != decoder->ForwardRefCount) {
        Info("Changing to %d forward reference surfaces for postprocessing\n", tmp_forwardRefCount);
        decoder->ForwardRefSurfaces = realloc(decoder->ForwardRefSurfaces, tmp_forwardRefCount * sizeof(VASurfaceID));
        decoder->ForwardRefCount = tmp_forwardRefCount;
    }

    if (tmp_backwardRefCount != decoder->BackwardRefCount) {
        Info("Changing to %d backward reference surfaces for postprocessing\n", tmp_backwardRefCount);
        decoder->BackwardRefSurfaces = realloc(decoder->BackwardRefSurfaces, tmp_backwardRefCount * sizeof(VASurfaceID));
        decoder->BackwardRefCount = tmp_backwardRefCount;
    }

    if (va_status != VA_STATUS_SUCCESS)
	return NULL;

    /* Skip sharpening if off */
    if (!decoder->vpp_sharpen_buf || !VideoSharpen[decoder->Resolution])
        return surface;

    vaSyncSurface(VaDisplay, *surface);

    /* Get postproc surface for gpe pipeline */
    decoder->PostProcSurfaceWrite = (decoder->PostProcSurfaceWrite + 1) % POSTPROC_SURFACES_MAX;
    gpe_surface = &decoder->PostProcSurfacesRb[decoder->PostProcSurfaceWrite];

    va_status = VaapiPostprocessSurface(decoder->vpp_ctx, *surface, *gpe_surface,
		    decoder->gpe_filters, decoder->gpe_filter_n,
		    VA_FRAME_PICTURE, 0,
                    NULL, NULL,
                    NULL, NULL);

    /* Failed to sharpen? Return previous surface */
    if (va_status != VA_STATUS_SUCCESS)
	return surface;

    return gpe_surface;
}

///
///	Clamp given value to range that fits in uint8_t
///
///	@param value[in]	input value to clamp
///
static inline uint8_t VaapiClampToUint8(const int value)
{
    if (value > 0xFF)
	return 0xFF;
    else if (value < 0)
	return 0;
    return value;
}

///
///	Grab output surface in YUV format and convert to bgra.
///
///	@param decoder[in]		VA-API decoder
///	@param src[in]			Source VASurfaceID to grab
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *VaapiGrabOutputSurfaceYUV(VaapiDecoder * decoder,
    VASurfaceID src, int *ret_size, int *ret_width, int *ret_height)
{
    int i, j;
    VAStatus status;
    VAImage image;
    VAImageFormat format[1];
    uint8_t *image_buffer = NULL;
    uint8_t *bgra = NULL;

    status = vaDeriveImage(VaDisplay, src, &image);
    if (status != VA_STATUS_SUCCESS) {
	Warning(_("video/vaapi: Failed to derive image: %s\n Falling back to GetImage\n"),
	    vaErrorStr(status));

	if (!decoder->GetPutImage) {
	    Error(_("video/vaapi: Image grabbing not supported by HW\n"));
	    return NULL;
	}

	if (!VaapiFindImageFormat(decoder, AV_PIX_FMT_NV12, format)) {
	    Error(_("video/vaapi: Image format suitable for grab not supported\n"));
	    return NULL;
	}

	status = vaCreateImage(VaDisplay, format, *ret_width, *ret_height, &image);
	if (status != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: Failed to create image for grab: %s\n"),
		vaErrorStr(status));
	    return NULL;
	}

	status = vaGetImage(VaDisplay, src,
	    0, 0,
	    *ret_width, *ret_height, image.image_id);
	if (status != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: Failed to capture image: %s\n"),
		vaErrorStr(status));
	    goto out_destroy;
	}
    }
    VaapiFindImageFormat(decoder, AV_PIX_FMT_NV12, format);

    // Sanity check for image format
    if (image.format.fourcc != VA_FOURCC_NV12 &&
	image.format.fourcc != VA_FOURCC('I','4','2','0')) {
	Error(_("video/vaapi: Image format mismatch! (fourcc: 0x%x, planes: %d)\n"),
	    image.format.fourcc, image.num_planes);
	goto out_destroy;
    }

    status = vaMapBuffer(VaDisplay, image.buf, (void**)&image_buffer);
    if (status != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: Could not map grabbed image for access: %s\n"),
	    vaErrorStr(status));
	goto out_destroy;
    }

    bgra = malloc(*ret_size);
    if (!bgra) {
	Error(_("video/vaapi: Grab failed: Out of memory\n"));
	goto out_unmap;
    }

    for (j = 0; j < *ret_height; ++j) {
	for (i = 0; i < *ret_width; ++i) {
	    unsigned int uv_index, u_index, v_index;
	    uint8_t y = image_buffer[j * image.pitches[0] + i];
	    uint8_t u, v;
	    int b, g, r;

	    if (image.format.fourcc == VA_FOURCC_NV12) {
		uv_index = image.offsets[1] + (image.pitches[1] * (j / 2)) + (i / 2) * 2;
		u = image_buffer[uv_index];
		v = image_buffer[uv_index + 1];
	    } else if (image.format.fourcc == VA_FOURCC('I','4','2','0')) {
		u_index = image.offsets[1] + (image.pitches[1] * (j / 2) + (i / 2));
		v_index = image.offsets[2] + (image.pitches[2] * (j / 2) + (i / 2));
		u = image_buffer[u_index];
		v = image_buffer[v_index];
	    } else {
		/* Use only y-plane if plane format is unknown */
		u = v = y;
	    }

	    b = 1.164 * (y-16) + 2.018 * (u - 128);
	    g = 1.164 * (y-16) - 0.813 * (v - 128) - 0.391 * (u - 128);
	    r = 1.164 * (y-16) + 1.596 * (v - 128);

	    bgra[(i + j * *ret_width) * 4 + 0] = VaapiClampToUint8(b);
	    bgra[(i + j * *ret_width) * 4 + 1] = VaapiClampToUint8(g);
	    bgra[(i + j * *ret_width) * 4 + 2] = VaapiClampToUint8(r);
	    bgra[(i + j * *ret_width) * 4 + 3] = 0x00;
	}
    }

out_unmap:
    vaUnmapBuffer(VaDisplay, image.buf);
out_destroy:
    vaDestroyImage(VaDisplay, image.image_id);
    return bgra;
}

///
///	Grab output surface by utilizing VA-API surface color conversion HW.
///
///	@param decoder[in]		VA-API decoder
///	@param src[in]			Source VASurfaceID to grab
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *VaapiGrabOutputSurfaceHW(VaapiDecoder * decoder,
    VASurfaceID src, int *ret_size, int *ret_width, int *ret_height)
{
    int j;
    VAStatus status;
    VAImage image;
    VAImageFormat format[1];
    uint8_t *image_buffer = NULL;
    uint8_t *bgra = NULL;

    if (!decoder->GetPutImage) {
	Error(_("video/vaapi: Image grabbing not supported by HW\n"));
	return NULL;
    }

    if (!VaapiFindImageFormat(decoder, AV_PIX_FMT_BGRA, format)) {
        Error(_("video/vaapi: Image format suitable for grab not supported\n"));
        return NULL;
    }

    status = vaCreateImage(VaDisplay, format, *ret_width, *ret_height, &image);
    if (status != VA_STATUS_SUCCESS) {
        Error(_("video/vaapi: Failed to create image for grab: %s\n"),
	    vaErrorStr(status));
	return NULL;
    }

    status = vaGetImage(VaDisplay, src,
	0, 0, *ret_width, *ret_height, image.image_id);
    if (status != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: Failed to capture image: %s\n"),
	    vaErrorStr(status));
	return NULL;
    }

    status = vaMapBuffer(VaDisplay, image.buf, (void**)&image_buffer);
    if (status != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: Could not map grabbed image for access: %s\n"),
	    vaErrorStr(status));
	goto out_destroy;
    }

    bgra = malloc(*ret_size);
    if (!bgra) {
	Error(_("video/vaapi: Grab failed: Out of memory\n"));
	goto out_unmap;
    }

    for (j = 0; j < *ret_height; ++j) {
	memcpy(bgra + j * *ret_width * 4, image_buffer + j * image.pitches[0],
	    *ret_width * 4);
    }

out_unmap:
    vaUnmapBuffer(VaDisplay, image.buf);
out_destroy:
    vaDestroyImage(VaDisplay, image.image_id);
    return bgra;
}

///
///	Grab output surface.
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *VaapiGrabOutputSurface(int *ret_size, int *ret_width,
    int *ret_height)
{
    uint8_t *bgra = NULL;
    VAStatus status;
    VaapiDecoder *decoder = NULL;
    VASurfaceID scaled[1] = { VA_INVALID_ID };
    VASurfaceID grabbing = VA_INVALID_ID;
    VAContextID scaling_ctx;

    if (!(decoder = VaapiDecoders[0])) {
	Error(_("video/vaapi: Decoder not available for GRAB\n"));
	return NULL;
    }

    grabbing = decoder->SurfacesRb[decoder->SurfaceRead];

    if (*ret_width <= 0)
	*ret_width = decoder->InputWidth;
    if (*ret_height <= 0)
	*ret_height = decoder->InputHeight;

    *ret_size = *ret_width * *ret_height * 4;

    status = vaCreateSurfaces(VaDisplay, VA_RT_FORMAT_YUV420,
		*ret_width, *ret_height, scaled, ARRAY_ELEMS(scaled),
		NULL, 0);
    if (status != VA_STATUS_SUCCESS) {
        Error(_("video/vaapi: can't create scaling surface for grab: %s\n"), vaErrorStr(status));
    }

    status = vaCreateContext(VaDisplay, decoder->VppConfig,
			     *ret_width, *ret_height,
			     VA_PROGRESSIVE, scaled, ARRAY_ELEMS(scaled),
			     &scaling_ctx);
    if (status != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create scaling context for grab: %s\n"), vaErrorStr(status));
	vaDestroySurfaces(VaDisplay, scaled, ARRAY_ELEMS(scaled));
	scaled[0] = VA_INVALID_ID;
    }

    status = VaapiRunScaling(scaling_ctx, grabbing, scaled[0]);
    if (status != VA_STATUS_SUCCESS) {
	vaDestroyContext(VaDisplay, scaling_ctx);
	vaDestroySurfaces(VaDisplay, scaled, ARRAY_ELEMS(scaled));
	scaled[0] = VA_INVALID_ID;
    } else {
	grabbing = scaled[0];
    }


    bgra = VaapiGrabOutputSurfaceHW(decoder, grabbing, ret_size, ret_width, ret_height);
    if (!bgra)
	bgra = VaapiGrabOutputSurfaceYUV(decoder, grabbing, ret_size, ret_width, ret_height);

    if (scaled[0] != VA_INVALID_ID) {
	vaDestroyContext(VaDisplay, scaling_ctx);
	vaDestroySurfaces(VaDisplay, scaled, ARRAY_ELEMS(scaled));
    }

    return bgra;
}

///
///	Configure VA-API for new video format.
///
///	@param decoder	VA-API decoder
///
static void VaapiSetup(VaapiDecoder * decoder,
    const AVCodecContext * video_ctx)
{
    int width;
    int height;

    VAImageFormat format[1];

    // create initial black surface and display
    VaapiBlackSurface(decoder);
    // cleanup last context
    VaapiCleanup(decoder);

    width = video_ctx->width;
    height = video_ctx->height;
#ifdef DEBUG
    // FIXME: remove this if
    if (decoder->Image->image_id != VA_INVALID_ID) {
	abort();			// should be done by VaapiCleanup()
    }
#endif
    // FIXME: PixFmt not set!
    //VaapiFindImageFormat(decoder, decoder->PixFmt, format);
    VaapiFindImageFormat(decoder, AV_PIX_FMT_NV12, format);

    // FIXME: this image is only needed for software decoder and auto-crop
    if (decoder->GetPutImage
	&& vaCreateImage(VaDisplay, format, width, height,
	    decoder->Image) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create image!\n"));
    }
    Debug(3,
	"video/vaapi: created image %dx%d with id 0x%08x and buffer id 0x%08x\n",
	width, height, decoder->Image->image_id, decoder->Image->buf);

    // FIXME: interlaced not valid here?
    decoder->Resolution =
	VideoResolutionGroup(width, height, decoder->Interlaced);
    VideoResolution = decoder->Resolution;
    VaapiCreateSurfaces(decoder, width, height);

#if defined USE_GLX && !defined VAAPI_NO_GLX
    if (GlxEnabled) {
        if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext)) {
	    Error(_("video/glx: can't make glx context current\n"));
	    return;
        }
	GlxSetupDecoder(decoder->InputWidth, decoder->InputHeight,
	    decoder->GlTextures);
	if (vaCreateSurfaceGLX(decoder->VaDisplay, GL_TEXTURE_2D,
		decoder->GlTextures[0], &decoder->GlxSurfaces[0])
	    != VA_STATUS_SUCCESS) {
	    Fatal(_("video/glx: can't create glx surfaces\n"));
	    // FIXME: no fatal here
	}
    }
#endif
#ifdef USE_EGL
    if (EglEnabled) {
        if (!eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglThreadContext)) {
	    Error(_("video/egl: can't make egl context current\n"));
	    return;
        }
	EglSetupDecoder(decoder->InputWidth, decoder->InputHeight,
	    decoder->GlTextures);
    }
#endif

    VaapiUpdateOutput(decoder);

    //
    //	update OSD associate
    //
#ifdef USE_GLX
    if (GlxEnabled) {
	return;
    }
#endif
#ifdef USE_EGL
    if (EglEnabled) {
	return;
    }
#endif
    VaapiAssociate(decoder);
}

///
///	Generic helper to set-up ParameterBuffer filters
///	(like NoiseReduction, SkinToneEnhancement, Sharpening...).
///
///	@param decoder	VA-API decoder
///	@param type	Type of filter to set-up
///	@param value	Value of the filter to set-up to
///	@return 	Buffer ID for the filter or VA_INVALID_ID if unsuccessful
///
static VABufferID VaapiSetupParameterBufferProcessing(VaapiDecoder * decoder, VAProcFilterType type, float value)
{
    VAProcFilterParameterBuffer param_buf;
    VABufferID filter_buf_id;
    unsigned int cap_n = 1;
    VAProcFilterCap caps[cap_n];

    VAStatus va_status = vaQueryVideoProcFilterCaps(VaDisplay, decoder->vpp_ctx,
						    type, caps, &cap_n);
    if (va_status != VA_STATUS_SUCCESS) {
        Error("Failed to query filter #%02x capabilities: %s\n", type, vaErrorStr(va_status));
        return VA_INVALID_ID;
    }
    if (type == VAProcFilterSkinToneEnhancement && cap_n == 0) { // Intel driver doesn't return caps
       cap_n = 1;
       caps->range.min_value = 0.0;
       caps->range.max_value = 4.0;
       caps->range.default_value = 0.0;
       caps->range.step = 1.0;
       VaapiConfigStde.drv_scale = 3.0;
    }
    if (cap_n != 1) {
        Error("Wrong number of capabilities (%d) for filter %#010x\n", cap_n, type);
        return VA_INVALID_ID;
    }

    Info("video/vaapi: %.2f - %.2f ++ %.2f = %.2f\n",
	 caps->range.min_value,
	 caps->range.max_value,
	 caps->range.step,
	 caps->range.default_value);

    switch (type) {
	case VAProcFilterNoiseReduction:
	    VaapiNormalizeConfig(&VaapiConfigDenoise, caps->range.min_value, caps->range.max_value, caps->range.default_value, caps->range.step);
	    break;
	case VAProcFilterSharpening:
	    VaapiNormalizeConfig(&VaapiConfigSharpen, caps->range.min_value, caps->range.max_value, caps->range.default_value, caps->range.step);
	    break;
	case VAProcFilterSkinToneEnhancement:
	    VaapiNormalizeConfig(&VaapiConfigStde, caps->range.min_value, caps->range.max_value, caps->range.default_value, caps->range.step);
	    break;
	default:
	    break;
    }

    param_buf.type = type;
    param_buf.value = value;
    va_status = vaCreateBuffer(VaDisplay, decoder->vpp_ctx,
			       VAProcFilterParameterBufferType, sizeof(param_buf), 1,
			       &param_buf, &filter_buf_id);

    if (va_status != VA_STATUS_SUCCESS) {
        Error("Could not create buffer for filter #%02x: %s\n", type, vaErrorStr(va_status));
        return VA_INVALID_ID;
    }
    return filter_buf_id;
}

///
///	Configure VA-API for new video format.
///
///	@param decoder	VA-API decoder
///
static void VaapiSetupVideoProcessing(VaapiDecoder * decoder)
{
#if VA_CHECK_VERSION(0,33,99)
    VAStatus va_status;
    VAProcFilterType filtertypes[VAProcFilterCount];
    unsigned filtertype_n;
    unsigned u;
    unsigned v;
    VAProcFilterCapDeinterlacing deinterlacing_caps[VAProcDeinterlacingCount];
    VAProcFilterParameterBufferDeinterlacing deinterlace;
    unsigned deinterlacing_cap_n;
    VAProcFilterParameterBufferColorBalance cbal_param[VAProcColorBalanceCount];
    VAProcFilterCapColorBalance colorbalance_caps[VAProcColorBalanceCount];
    unsigned colorbalance_cap_n;

    VABufferID filter_buf_id;

    VAProcPipelineCaps pipeline_caps;
    VAProcColorStandardType in_color_standards[VAProcColorStandardCount];
    VAProcColorStandardType out_color_standards[VAProcColorStandardCount];

    if (!VaapiVideoProcessing) {
	return;
    }

    //
    //	display and filter infos.
    //
    filtertype_n = VAProcFilterCount;	// API break this must be done
    vaQueryVideoProcFilters(VaDisplay, decoder->vpp_ctx,
	filtertypes, &filtertype_n);

    for (u = 0; u < filtertype_n; ++u) {
	switch (filtertypes[u]) {
	    case VAProcFilterNoiseReduction:
		Info("video/vaapi: noise reduction supported\n");
		VaapiConfigDenoise.active = 1;
		filter_buf_id = VaapiSetupParameterBufferProcessing(decoder, filtertypes[u], VaapiConfigDenoise.def_value *
								    VaapiConfigDenoise.scale);
		if (filter_buf_id != VA_INVALID_ID) {
		    Info("Enabling denoise filter (pos = %d)\n", decoder->filter_n);
		    decoder->vpp_denoise_buf = &decoder->filters[decoder->filter_n];
		    decoder->filters[decoder->filter_n++] = filter_buf_id;
		}
		break;
	    case VAProcFilterDeinterlacing:
		Info("video/vaapi: deinterlacing supported\n");

		deinterlacing_cap_n = VAProcDeinterlacingCount;
		vaQueryVideoProcFilterCaps(VaDisplay,
		    decoder->vpp_ctx,
		    VAProcFilterDeinterlacing, deinterlacing_caps,
		    &deinterlacing_cap_n);
		for (v = 0; v < deinterlacing_cap_n; ++v) {

		    /* Deinterlacing parameters */
		    deinterlace.type = VAProcFilterDeinterlacing;
		    deinterlace.flags = 0;

		    switch (deinterlacing_caps[v].type) {
			case VAProcDeinterlacingNone:
			    Info("video/vaapi: none deinterlace supported\n");
			    deinterlace.algorithm = VAProcDeinterlacingNone;
			    break;
			case VAProcDeinterlacingBob:
			    Info("video/vaapi: bob deinterlace supported\n");
			    deinterlace.algorithm = VAProcDeinterlacingBob;
			    break;
			case VAProcDeinterlacingWeave:
			    Info("video/vaapi: weave deinterlace supported\n");
			    deinterlace.algorithm = VAProcDeinterlacingWeave;
			    break;
			case VAProcDeinterlacingMotionAdaptive:
			    Info("video/vaapi: motion adaptive deinterlace supported\n");
			    deinterlace.algorithm = VAProcDeinterlacingMotionAdaptive;
			    break;
			case VAProcDeinterlacingMotionCompensated:
			    Info("video/vaapi: motion compensated deinterlace supported\n");
			    deinterlace.algorithm = VAProcDeinterlacingMotionCompensated;
			    break;
			default:
			    Info("video/vaapi: unsupported deinterlace #%02x\n", deinterlacing_caps[v].type);
			    break;
		    }
		}
		/* Enabling the deint algorithm that was seen last */
		Info("Enabling Deint (pos = %d)\n", decoder->filter_n);
		decoder->MaxSupportedDeinterlacer = deinterlace.algorithm;
		va_status = vaCreateBuffer(VaDisplay, decoder->vpp_ctx,
					   VAProcFilterParameterBufferType, sizeof(deinterlace), 1,
					   &deinterlace, &filter_buf_id);
		decoder->vpp_deinterlace_buf = &decoder->filters[decoder->filter_n];
		decoder->filters[decoder->filter_n++] = filter_buf_id;
		break;
	    case VAProcFilterSharpening:
		Info("video/vaapi: sharpening supported\n");
		VaapiConfigSharpen.active = 1;
		// Sharpening needs to on a separated pipeline apart from vebox
		filter_buf_id = VaapiSetupParameterBufferProcessing(decoder, filtertypes[u], VaapiConfigSharpen.def_value *
								    VaapiConfigSharpen.scale);
		if (filter_buf_id != VA_INVALID_ID) {
		    Info("Enabling sharpening filter (pos = %d)\n", decoder->gpe_filter_n);
		    decoder->vpp_sharpen_buf = &decoder->gpe_filters[decoder->gpe_filter_n];
		    decoder->gpe_filters[decoder->gpe_filter_n++] = filter_buf_id;
		}
		break;
	    case VAProcFilterColorBalance:
		Info("video/vaapi: enabling color balance filters\n");
		colorbalance_cap_n = VAProcColorBalanceCount;
		vaQueryVideoProcFilterCaps(VaDisplay, decoder->vpp_ctx,
		     VAProcFilterColorBalance, colorbalance_caps,
		     &colorbalance_cap_n);

		Info("video/vaapi: Supported color balance filter count: %d\n", colorbalance_cap_n);

		if (!colorbalance_cap_n)
		    break;

		/* Set each color balance filter individually */
		for (v = 0; v < colorbalance_cap_n; ++v) {

		    switch(colorbalance_caps[v].type) {
			case VAProcColorBalanceNone:
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)\n", "None",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value, decoder->filter_n);
			    break;
			case VAProcColorBalanceHue:
			    VaapiConfigHue.active = 1;
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)\n", "Hue",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value, decoder->filter_n);
			    VaapiNormalizeConfig(&VaapiConfigHue, colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
						 colorbalance_caps[v].range.default_value, colorbalance_caps[v].range.step);
			    decoder->vpp_hue_idx = v;
			    break;
			case VAProcColorBalanceSaturation:
			    VaapiConfigSaturation.active = 1;
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)\n", "Saturation",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value, decoder->filter_n);
			    VaapiNormalizeConfig(&VaapiConfigSaturation, colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
						 colorbalance_caps[v].range.default_value, colorbalance_caps[v].range.step);
			    decoder->vpp_saturation_idx = v;
			    break;
			case VAProcColorBalanceBrightness:
			    VaapiConfigBrightness.active = 1;
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)\n", "Brightness",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value, decoder->filter_n);
			    VaapiNormalizeConfig(&VaapiConfigBrightness, colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
						 colorbalance_caps[v].range.default_value, colorbalance_caps[v].range.step);
			    decoder->vpp_brightness_idx = v;
			    break;
			case VAProcColorBalanceContrast:
			    VaapiConfigContrast.active = 1;
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)\n", "Contrast",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value, decoder->filter_n);
			    VaapiNormalizeConfig(&VaapiConfigContrast, colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
						 colorbalance_caps[v].range.default_value, colorbalance_caps[v].range.step);
			    decoder->vpp_contrast_idx = v;
			    break;
			case VAProcColorBalanceAutoSaturation:
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)\n", "AutoSaturation",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value, decoder->filter_n);
			    break;
			case VAProcColorBalanceAutoBrightness:
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)\n", "AutoBrightness",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value, decoder->filter_n);
			    break;
			case VAProcColorBalanceAutoContrast:
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)\n", "AutoContrast",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value, decoder->filter_n);
			    break;

			default:
			    Info("video/vaapi: unsupported color balance filter #%02x\n", colorbalance_caps[v].type);
			    break;
		    }

		    cbal_param[v].type = VAProcFilterColorBalance;
		    cbal_param[v].attrib = colorbalance_caps[v].type;
		    cbal_param[v].value = colorbalance_caps[v].range.default_value;
		}
		va_status = vaCreateBuffer(VaDisplay, decoder->vpp_ctx,
					   VAProcFilterParameterBufferType,
					   sizeof(VAProcFilterParameterBufferColorBalance), colorbalance_cap_n,
					   &cbal_param, &filter_buf_id);
		if (va_status != VA_STATUS_SUCCESS) {
		    Error("video/vaapi: Could not create buffer for color balance settings: %s\n", vaErrorStr(va_status));
		    break;
		}

		decoder->vpp_cbal_buf = &decoder->filters[decoder->filter_n];
		decoder->filters[decoder->filter_n++] = filter_buf_id;
		break;
	    case VAProcFilterSkinToneEnhancement:
		VaapiConfigStde.active = 1;
		Info("video/vaapi: skin tone enhancement supported\n");
		filter_buf_id = VaapiSetupParameterBufferProcessing(decoder, filtertypes[u], VaapiConfigStde.def_value *
                                                                    VaapiConfigStde.scale);
		if (filter_buf_id != VA_INVALID_ID) {
		    Info("Enabling skin tone filter (pos = %d)\n", decoder->filter_n);
		    decoder->vpp_stde_buf = &decoder->filters[decoder->filter_n];
		    decoder->filters[decoder->filter_n++] = filter_buf_id;
		}
		break;
	    default:
		Info("video/vaapi: unsupported filter #%02x\n", filtertypes[u]);
		break;
	}
	VaapiInitSurfaceFlags(decoder);
    }
    //
    //	query pipeline caps
    //
    pipeline_caps.input_color_standards      = in_color_standards;
    pipeline_caps.num_input_color_standards  = ARRAY_ELEMS(in_color_standards);
    pipeline_caps.output_color_standards     = out_color_standards;
    pipeline_caps.num_output_color_standards = ARRAY_ELEMS(out_color_standards);

    va_status = vaQueryVideoProcPipelineCaps(VaDisplay, decoder->vpp_ctx,
                                 decoder->filters, decoder->filter_n, &pipeline_caps);
    if (va_status != VA_STATUS_SUCCESS) {
	Fatal("Failed to query proc pipeline caps, error = %s\n", vaErrorStr(va_status));
    }

    Info("Allocating %d forward reference surfaces for postprocessing\n", pipeline_caps.num_forward_references);
    decoder->ForwardRefSurfaces = realloc(decoder->ForwardRefSurfaces, pipeline_caps.num_forward_references * sizeof(VASurfaceID));
    decoder->ForwardRefCount = pipeline_caps.num_forward_references;

    Info("Allocating %d backward reference surfaces for postprocessing\n", pipeline_caps.num_backward_references);
    decoder->BackwardRefSurfaces = realloc(decoder->BackwardRefSurfaces, pipeline_caps.num_backward_references * sizeof(VASurfaceID));
    decoder->BackwardRefCount = pipeline_caps.num_backward_references;

    //TODO: Verify that rest of the capabilities are set properly

#endif
}

///
///	Get a free surface.  Called from ffmpeg.
///
///	@param decoder		VA-API decoder
///	@param video_ctx	ffmpeg video codec context
///
///	@returns the oldest free surface
///
static VASurfaceID VaapiGetSurface(VaapiDecoder * decoder,
    const AVCodecContext * video_ctx)
{
#ifdef FFMPEG_BUG1_WORKAROUND
    // get_format not called with valid informations.
    if (video_ctx->width != decoder->InputWidth
	|| video_ctx->height != decoder->InputHeight) {
	VAStatus status;

	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;

	VaapiSetup(decoder, video_ctx);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,74,100)
	// create a configuration for the decode pipeline
	if ((status =
		vaCreateConfig(decoder->VaDisplay, decoder->Profile,
		    decoder->Entrypoint, NULL, 0,
		    &decoder->VaapiContext->config_id))) {
	    Error(_("video/vaapi: can't create config '%s'\n"),
		vaErrorStr(status));
	    // bind surfaces to context
	} else if ((status =
		vaCreateContext(decoder->VaDisplay,
		    decoder->VaapiContext->config_id, video_ctx->width,
		    video_ctx->height, VA_PROGRESSIVE, decoder->SurfacesFree,
		    decoder->SurfaceFreeN,
		    &decoder->VaapiContext->context_id))) {
	    Error(_("video/vaapi: can't create context '%s'\n"),
		vaErrorStr(status));
	}
#endif
        status = vaCreateConfig(decoder->VaDisplay, VAProfileNone,
                                decoder->VppEntrypoint, NULL, 0,
                                &decoder->VppConfig);
        if (status != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create config '%s'\n"),
		vaErrorStr(status));
        }
        status = vaCreateContext(decoder->VaDisplay, decoder->VppConfig,
                                 video_ctx->width, video_ctx->height,
                                 VA_PROGRESSIVE, decoder->PostProcSurfacesRb, POSTPROC_SURFACES_MAX,
                                 &decoder->vpp_ctx);
        if (status != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create context '%s'\n"),
		vaErrorStr(status));
        }

	// FIXME: too late to switch to software rending on failures
	VaapiSetupVideoProcessing(decoder);
    }
#else
    (void)video_ctx;
#endif
    return VaapiGetSurface0(decoder);
}

///
///	Find VA-API profile.
///
///	Check if the requested profile is supported by VA-API.
///
///	@param profiles a table of all supported profiles
///	@param n	number of supported profiles
///	@param profile	requested profile
///
///	@returns the profile if supported, -1 if unsupported.
///
static VAProfile VaapiFindProfile(const VAProfile * profiles, unsigned n,
    VAProfile profile)
{
    unsigned u;

    for (u = 0; u < n; ++u) {
	if (profiles[u] == profile) {
	    return profile;
	}
    }
    return -1;
}

///
///	Find VA-API entry point.
///
///	Check if the requested entry point is supported by VA-API.
///
///	@param entrypoints	a table of all supported entrypoints
///	@param n		number of supported entrypoints
///	@param entrypoint	requested entrypoint
///
///	@returns the entry point if supported, -1 if unsupported.
///
static VAEntrypoint VaapiFindEntrypoint(const VAEntrypoint * entrypoints,
    unsigned n, VAEntrypoint entrypoint)
{
    unsigned u;

    for (u = 0; u < n; ++u) {
	if (entrypoints[u] == entrypoint) {
	    return entrypoint;
	}
    }
    return -1;
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param fmt	is the list of formats which are supported by the codec,
///			it is terminated by -1 as 0 is a valid format, the
///			formats are ordered by quality.
///
///	@note + 2 surface for software deinterlace
///
static enum AVPixelFormat Vaapi_get_format(VaapiDecoder * decoder,
    AVCodecContext * video_ctx, const enum AVPixelFormat *fmt)
{
    const enum AVPixelFormat *fmt_idx;
    VAProfile profiles[vaMaxNumProfiles(VaDisplay)];
    int profile_n;
    VAEntrypoint entrypoints[vaMaxNumEntrypoints(VaDisplay)];
    int entrypoint_n;
    int p;
    int e;
    int i;
    int ret;
    VAConfigAttrib attrib;
    VideoDecoder *ist = video_ctx->opaque;

    if (!VideoHardwareDecoder || (video_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO
	    && VideoHardwareDecoder == HWmpeg2Off)
	) {				// hardware disabled by config
	Debug(3, "codec: hardware acceleration disabled\n");
	goto slow_path;
    }

    p = -1;
    e = -1;

    //	prepare va-api profiles
    if (vaQueryConfigProfiles(VaDisplay, profiles, &profile_n)) {
	Error(_("codec: vaQueryConfigProfiles failed"));
	goto slow_path;
    }
    Debug(3, "codec: %d profiles\n", profile_n);

    // check profile
    switch (video_ctx->codec_id) {
	case AV_CODEC_ID_MPEG2VIDEO:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_MPEG2 + VIDEO_SURFACES_MAX + 2;
	    p = VaapiFindProfile(profiles, profile_n, VAProfileMPEG2Main);
	    break;
	case AV_CODEC_ID_MPEG4:
	case AV_CODEC_ID_H263:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_MPEG4 + VIDEO_SURFACES_MAX + 2;
	    p = VaapiFindProfile(profiles, profile_n,
		VAProfileMPEG4AdvancedSimple);
	    break;
	case AV_CODEC_ID_H264:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_H264 + VIDEO_SURFACES_MAX + 2;
	    // try more simple formats, fallback to better
	    if (video_ctx->profile == FF_PROFILE_H264_BASELINE) {
#if VA_CHECK_VERSION(1,0,8)
		p = VaapiFindProfile(profiles, profile_n, VAProfileH264ConstrainedBaseline);
#else
		p = VaapiFindProfile(profiles, profile_n, VAProfileH264Baseline);
#endif
		if (p == -1) {
		    p = VaapiFindProfile(profiles, profile_n,
			VAProfileH264Main);
		}
	    } else if (video_ctx->profile == FF_PROFILE_H264_MAIN) {
		p = VaapiFindProfile(profiles, profile_n, VAProfileH264Main);
	    }
	    if (p == -1) {
		p = VaapiFindProfile(profiles, profile_n, VAProfileH264High);
	    }
	    break;
       case AV_CODEC_ID_HEVC:
            decoder->SurfacesNeeded =
               CODEC_SURFACES_H264 + VIDEO_SURFACES_MAX + 2;
            // try more simple formats, fallback to better
            if (video_ctx->profile == FF_PROFILE_HEVC_MAIN_10) {
               p = VaapiFindProfile(profiles, profile_n,
                   VAProfileHEVCMain10);
               if (p == -1) {
                   p = VaapiFindProfile(profiles, profile_n,
                       VAProfileHEVCMain);
               }
            } else if (video_ctx->profile == FF_PROFILE_HEVC_MAIN) {
               p = VaapiFindProfile(profiles, profile_n, VAProfileHEVCMain);
            }
            if (p == -1) {
                p = VaapiFindProfile(profiles, profile_n, VAProfileHEVCMain10);
            }
           break;
	case AV_CODEC_ID_WMV3:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_VC1 + VIDEO_SURFACES_MAX + 2;
	    p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Main);
	    break;
	case AV_CODEC_ID_VC1:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_VC1 + VIDEO_SURFACES_MAX + 2;
	    p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Advanced);
	    break;
	default:
	    goto slow_path;
    }
    if (p == -1) {
	Debug(3, "\tno profile found\n");
	goto slow_path;
    }
    Debug(3, "\tprofile %d\n", p);

    // prepare va-api entry points
    if (vaQueryConfigEntrypoints(VaDisplay, p, entrypoints, &entrypoint_n)) {
	Error(_("codec: vaQueryConfigEntrypoints failed"));
	goto slow_path;
    }
    Debug(3, "codec: %d entrypoints\n", entrypoint_n);
    //	look through formats
    for (fmt_idx = fmt; *fmt_idx != AV_PIX_FMT_NONE; fmt_idx++) {
	Debug(3,"\t%#010x %s\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx));
	// check supported pixel format with entry point
	switch (*fmt_idx) {
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(54,31,100)
	    case AV_PIX_FMT_VAAPI_VLD:
#else
	    case AV_PIX_FMT_VAAPI:
#endif
		e = VaapiFindEntrypoint(entrypoints, entrypoint_n,
		    VAEntrypointVLD);
		break;
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(54,31,100)
	    case AV_PIX_FMT_VAAPI_MOCO:
	    case AV_PIX_FMT_VAAPI_IDCT:
#endif
	    default:
		Debug(3,"codec: this VA-API pixel format is not supported\n");
		continue;
	}
	if (e != -1) {
	    Debug(3, "\tentry point %d\n", e);
	    break;
	}
    }
    if (e == -1) {
	Warning(_("codec: unsupported: slow path\n"));
	goto slow_path;
    }

    if (ist->GetFormatDone) return *fmt_idx;
    //
    //	prepare decoder config
    //
    memset(&attrib, 0, sizeof(attrib));
    attrib.type = VAConfigAttribRTFormat;
    if (vaGetConfigAttributes(decoder->VaDisplay, p, e, &attrib, 1)) {
	Error(_("codec: can't get attributes"));
	goto slow_path;
    }
    if (attrib.value & VA_RT_FORMAT_YUV420) {
	Info(_("codec: YUV 420 supported\n"));
    }
    if (attrib.value & VA_RT_FORMAT_YUV420_10BPP) {
	Info(_("codec: YUV 420 10 bit supported\n"));
    }
    if (attrib.value & VA_RT_FORMAT_YUV422) {
	Info(_("codec: YUV 422 supported\n"));
    }
    if (attrib.value & VA_RT_FORMAT_YUV444) {
	Info(_("codec: YUV 444 supported\n"));
    }

    if (!(attrib.value & VA_RT_FORMAT_YUV420)) {
	Warning(_("codec: YUV 420 not supported\n"));
	goto slow_path;
    }
    if (!(attrib.value & VA_RT_FORMAT_YUV420_10BPP)
        && video_ctx->profile == FF_PROFILE_HEVC_MAIN_10) {
	Warning(_("codec: YUV 420 10 bit not supported\n"));
	goto slow_path;
    }

    vaQueryConfigEntrypoints(VaDisplay, VAProfileNone, entrypoints,
                             &entrypoint_n);

    for (i = 0; i < entrypoint_n; i++) {
        if (entrypoints[i] == VAEntrypointVideoProc) {
            decoder->VppEntrypoint = entrypoints[i];
            break;
        }
    }

    if (decoder->VppEntrypoint == VA_INVALID_ID)
        Error("Could not locate Vpp EntryPoint!!\n");
    else
        Info("Using entrypoint for vpp: %d\n", decoder->VppEntrypoint);

    decoder->Profile = p;
    decoder->Entrypoint = e;
    decoder->PixFmt = AV_PIX_FMT_NV12;
    decoder->InputWidth = 0;
    decoder->InputHeight = 0;

    if (video_ctx->width && video_ctx->height) {
	VAStatus status;

	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;
	ist->hwaccel_pix_fmt = AV_PIX_FMT_VAAPI;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57,74,100)
	ret = vaapi_init(video_ctx);  // init HWACCEL
	if (ret < 0) {
	    Debug(3, "vaapi_init failed\n");
	    goto slow_path;
	}
#endif
	VaapiSetup(decoder, video_ctx);
	// FIXME: move the following into VaapiSetup
	// create a configuration for the decode pipeline
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,74,100)
	if ((status =
		vaCreateConfig(decoder->VaDisplay, p, e, &attrib, 1,
		    &decoder->VaapiContext->config_id))) {
	    Error(_("codec: can't create config '%s'\n"), vaErrorStr(status));
	    goto slow_path;
	}
	// bind surfaces to context
	if ((status =
		vaCreateContext(decoder->VaDisplay,
		    decoder->VaapiContext->config_id, video_ctx->width,
		    video_ctx->height, VA_PROGRESSIVE, decoder->SurfacesFree,
		    decoder->SurfaceFreeN,
		    &decoder->VaapiContext->context_id))) {
	    Error(_("codec: can't create context '%s'\n"), vaErrorStr(status));
	    goto slow_path;
	}
#endif
	status = vaCreateConfig(decoder->VaDisplay, VAProfileNone,
				decoder->VppEntrypoint, NULL, 0,
				&decoder->VppConfig);
	if (status != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create config '%s'\n"),
		  vaErrorStr(status));
	}
	status = vaCreateContext(decoder->VaDisplay, decoder->VppConfig,
				 video_ctx->width, video_ctx->height,
				 VA_PROGRESSIVE, decoder->PostProcSurfacesRb, POSTPROC_SURFACES_MAX,
				 &decoder->vpp_ctx);
	if (status != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create context '%s'\n"),
		  vaErrorStr(status));
	}

	VaapiSetupVideoProcessing(decoder);
    }

    ist->GetFormatDone = 1;
    Debug(3, "\t%#010x %s\n", fmt_idx[0], av_get_pix_fmt_name(fmt_idx[0]));
    return *fmt_idx;

  slow_path:
    // no accelerated format found
    decoder->Profile = VA_INVALID_ID;
    decoder->Entrypoint = VA_INVALID_ID;
    decoder->VppEntrypoint = VA_INVALID_ID;
    decoder->VppConfig = VA_INVALID_ID;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,74,100)
    decoder->VaapiContext->config_id = VA_INVALID_ID;
#endif
    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX + 2;
    decoder->PixFmt = AV_PIX_FMT_NONE;
    video_ctx->draw_horiz_band = NULL;

    decoder->InputWidth = 0;
    decoder->InputHeight = 0;
    video_ctx->hwaccel_context = NULL;
    ist->hwaccel_pix_fmt = AV_PIX_FMT_NONE;
    ist->GetFormatDone = 1;
    return avcodec_default_get_format(video_ctx, fmt);
}

///
///	Draw surface of the VA-API decoder with x11.
///
///	vaPutSurface with intel backend does sync on v-sync.
///
///	@param decoder	VA-API decoder
///	@param surface		VA-API surface id
///	@param interlaced	flag interlaced source
///	@param deinterlaced	flag source was deinterlaced
///	@param top_field_first	flag top_field_first for interlaced source
///	@param field		interlaced draw: 0 first field, 1 second field
///
static void VaapiPutSurfaceX11(VaapiDecoder * decoder, VASurfaceID surface,
    int interlaced, int deinterlaced, int top_field_first, int field)
{
    unsigned type;
    VAStatus status;
    uint32_t s;
    uint32_t e;

    // deinterlace
    if (interlaced && !deinterlaced
	&& VideoDeinterlace[decoder->Resolution] < VideoDeinterlaceSoftBob
	&& VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceWeave) {
	if (top_field_first) {
	    if (field) {
		type = VA_BOTTOM_FIELD;
	    } else {
		type = VA_TOP_FIELD;
	    }
	} else {
	    if (field) {
		type = VA_TOP_FIELD;
	    } else {
		type = VA_BOTTOM_FIELD;
	    }
	}
    } else {
	type = VA_FRAME_PICTURE;
    }

    //unscaled osd with any size can associate with any video surface
    if (VaapiUnscaledOsd && (lastWindowWidth != VideoWindowWidth || lastWindowHeight != VideoWindowHeight)) {
        VaapiDeassociate(decoder);
        VaapiAssociate(decoder);
        lastWindowWidth = VideoWindowWidth;
        lastWindowHeight = VideoWindowHeight;
    } //scaled osd can have size as video surface only
    else if (!VaapiUnscaledOsd && ((int)lastWindowWidth != decoder->InputWidth || (int)lastWindowHeight != decoder->InputHeight)) {
        VaapiDeassociate(decoder);
        VaapiOsdExit();
        OsdWidth = decoder->InputWidth;
        OsdHeight = decoder->InputHeight;
        VaapiOsdInit(OsdWidth, OsdHeight);
        VaapiAssociate(decoder);
        lastWindowWidth = decoder->InputWidth;
        lastWindowHeight = decoder->InputHeight;
    }

    s = GetMsTicks();
    xcb_flush(Connection);
    status = vaSyncSurface(decoder->VaDisplay, surface);
    if (status != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed: %s\n"), vaErrorStr(status));
	return;
    }
    if ((status = vaPutSurface(decoder->VaDisplay, surface, decoder->Window,
		// decoder src
		decoder->CropX, decoder->CropY, decoder->CropWidth,
		decoder->CropHeight,
		// video dst
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth,
		decoder->OutputHeight, NULL, 0,
		type | decoder->SurfaceFlagsTable[decoder->Resolution]))
	!= VA_STATUS_SUCCESS) {
	// switching video kills VdpPresentationQueueBlockUntilSurfaceIdle
	Error(_("video/vaapi: vaPutSurface failed: %s\n"), vaErrorStr(status));
    }
    status = vaSyncSurface(decoder->VaDisplay, surface);
    if (status != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed: %s\n"), vaErrorStr(status));
    }
    e = GetMsTicks();
    if (e - s > 2000) {
	Error(_("video/vaapi: gpu hung %dms %d\n"), e - s,
	    decoder->FrameCounter);
#ifdef DEBUG
	fprintf(stderr, _("video/vaapi: gpu hung %dms %d\n"), e - s,
	    decoder->FrameCounter);
#endif
    }

    if (0) {
	// check if surface is really ready
	// VDPAU backend, says always ready
	VASurfaceStatus status;

	if (vaQuerySurfaceStatus(decoder->VaDisplay, surface, &status)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaQuerySurface failed\n"));
	    status = VASurfaceReady;
	}
	if (status != VASurfaceReady) {
	    Warning(_
		("video/vaapi: surface %#010x not ready: still displayed %d\n"),
		surface, status);
	    return;
	}
    }

    if (0) {
	int i;

	// look how the status changes the next 40ms
	for (i = 0; i < 40; ++i) {
	    VASurfaceStatus status;

	    if (vaQuerySurfaceStatus(VaDisplay, surface,
		    &status) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaQuerySurface failed\n"));
	    }
	    Debug(3, "video/vaapi: %2d %d\n", i, status);
	    usleep(1 * 1000);
	}
    }
}

#ifdef USE_GLX

///
///	Draw surface of the VA-API decoder with glx.
///
///	@param decoder	VA-API decoder
///	@param surface		VA-API surface id
///	@param interlaced	flag interlaced source
///	@param deinterlaced	flag source was deinterlaced
///	@param top_field_first	flag top_field_first for interlaced source
///	@param field		interlaced draw: 0 first field, 1 second field
///
static void VaapiPutSurfaceGLX(VaapiDecoder * decoder, VASurfaceID surface,
    int interlaced, int deinterlaced, int top_field_first, int field)
{
    unsigned type;
    int y;
    float xcropf, ycropf,wcropf,hcropf;
    //uint32_t start;
    //uint32_t copy;
    //uint32_t end;

    // deinterlace
    if (interlaced && !deinterlaced
	&& VideoDeinterlace[decoder->Resolution] < VideoDeinterlaceSoftBob
	&& VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceWeave) {
	if (top_field_first) {
	    if (field) {
		type = VA_BOTTOM_FIELD;
	    } else {
		type = VA_TOP_FIELD;
	    }
	} else {
	    if (field) {
		type = VA_TOP_FIELD;
	    } else {
		type = VA_BOTTOM_FIELD;
	    }
	}
    } else {
	type = VA_FRAME_PICTURE;
    }
#ifndef VAAPI_NO_GLX
    //start = GetMsTicks();
    if (vaCopySurfaceGLX(decoder->VaDisplay, decoder->GlxSurfaces[0], surface,
	    type | decoder->SurfaceFlagsTable[decoder->Resolution]) !=
	VA_STATUS_SUCCESS) {
	Error(_("video/glx: vaCopySurfaceGLX failed\n"));
	return;
    }
    //copy = GetMsTicks();
#endif

    xcropf = (float) decoder->CropX / (float) decoder->InputWidth * (float) VideoWindowWidth;
    ycropf = (float) decoder->CropY / (float) decoder->InputHeight * (float) VideoWindowHeight;
    wcropf = (float) decoder->CropWidth / (float) decoder->InputWidth * (float) VideoWindowWidth;
    hcropf = (float) decoder->CropHeight / (float) decoder->InputHeight * (float) VideoWindowHeight;

    y = VideoWindowHeight - decoder->OutputY - decoder->OutputHeight;
    if (y < 0)
        y = 0;

    glViewport(decoder->OutputX, y, decoder->OutputWidth, decoder->OutputHeight);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(xcropf, wcropf, hcropf, ycropf, -1.0, 1.0);

    GlxRenderTexture(decoder->GlTextures[0], 0, 0, VideoWindowWidth, VideoWindowHeight);
    //end = GetMsTicks();
    //Debug(3, "video/vaapi/glx: %d copy %d render\n", copy - start, end - copy);
}

#endif

#ifdef USE_EGL

///
///	Draw surface of the VA-API decoder with egl.
///
///	@param decoder	VA-API decoder
///	@param surface		VA-API surface id
///	@param interlaced	flag interlaced source
///	@param deinterlaced	flag source was deinterlaced
///	@param top_field_first	flag top_field_first for interlaced source
///	@param field		interlaced draw: 0 first field, 1 second field
///
static void VaapiPutSurfaceEGL(VaapiDecoder * decoder, VASurfaceID surface,
    int interlaced, int deinterlaced, int top_field_first, int field)
{
    //unsigned type;
    int y;
    VADRMPRIMESurfaceDescriptor prime;
    float xcropf, ycropf;
    GLint texLoc;

#if VA_CHECK_VERSION(1,1,0)
    // convert the frame into a pair of DRM-PRIME FDs

    if (vaExportSurfaceHandle(decoder->VaDisplay, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
            &prime) != VA_STATUS_SUCCESS) {
	Error(_("video/egl: vaExportSurfaceHandle failed\n"));
        EglCheck();
	return;
    }
    vaSyncSurface(decoder->VaDisplay, surface);

    for (int i = 0;  i < 2;  ++i) {
        EGLint img_attr[] = {
            EGL_LINUX_DRM_FOURCC_EXT,      prime.layers[i].drm_format,
            EGL_WIDTH,                     decoder->InputWidth  / (i + 1),  // half size
            EGL_HEIGHT,                    decoder->InputHeight / (i + 1),  // for chroma
            EGL_DMA_BUF_PLANE0_FD_EXT,     prime.objects[prime.layers[i].object_index[0]].fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, prime.layers[i].offset[0],
            EGL_DMA_BUF_PLANE0_PITCH_EXT,  prime.layers[i].pitch[0],
            EGL_NONE
        };
        decoder->EglImages[i] = EglCreateImageKHR(EglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr);
        EglCheck();
        if (decoder->EglImages[i]) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, decoder->GlTextures[i]);
            GlEGLImageTargetTexture2DOES(GL_TEXTURE_2D, decoder->EglImages[i]);
            EglCheck();
        } else {
            for (int i = 0;  i < (int)prime.num_objects;  ++i) {
                close(prime.objects[i].fd);
            }
            Error(_("video/egl: eglCreateImageKHR failed\n"));
            return;
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    xcropf = (float) decoder->CropX / (float) decoder->InputWidth;
    ycropf = (float) decoder->CropY / (float) decoder->InputHeight;

    // Render Progressive frame and simple interlaced
    y = VideoWindowHeight - decoder->OutputY - decoder->OutputHeight;
    if (y < 0)
        y = 0;
    glViewport(decoder->OutputX, y, decoder->OutputWidth, decoder->OutputHeight);

    if (gl_prog == 0)
        gl_prog = sc_generate(gl_prog, decoder->ColorSpace);    // generate shader programm
    if (!gl_prog) return;

    glUseProgram(gl_prog);
    texLoc = glGetUniformLocation(gl_prog, "texture0");
    glUniform1i(texLoc, 0);
    texLoc = glGetUniformLocation(gl_prog, "texture1");
    glUniform1i(texLoc, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,decoder->GlTextures[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D,decoder->GlTextures[1]);

    render_pass_quad(0, xcropf, ycropf);

    glUseProgram(0);

    for (int i = 0;  i < 2;  ++i) {
        EglDestroyImageKHR(EglDisplay, decoder->EglImages[i]);
        EglCheck();
        decoder->EglImages[i] = NULL;
        if(prime.objects[i].fd)
            close(prime.objects[i].fd);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
#endif
    (void)interlaced; (void)deinterlaced; (void)top_field_first; (void)field;
}

#endif

#ifdef USE_AUTOCROP

///
///	VA-API auto-crop support.
///
///	@param decoder	VA-API hw decoder
///
static void VaapiAutoCrop(VaapiDecoder * decoder)
{
    VASurfaceID surface;
    uint32_t width;
    uint32_t height;
    void *va_image_data;
    void *data[3];
    uint32_t pitches[3];
    int crop14;
    int crop16;
    int next_state;
    int i;

    width = decoder->InputWidth;
    height = decoder->InputHeight;

  again:
    if (decoder->GetPutImage && decoder->Image->image_id == VA_INVALID_ID) {
	VAImageFormat format[1];

	Debug(3, "video/vaapi: download image not available\n");

	// FIXME: PixFmt not set!
	//VaapiFindImageFormat(decoder, decoder->PixFmt, format);
	VaapiFindImageFormat(decoder, AV_PIX_FMT_NV12, format);
	//VaapiFindImageFormat(decoder, AV_PIX_FMT_YUV420P, format);
	if (vaCreateImage(VaDisplay, format, width, height,
		decoder->Image) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create image!\n"));
	    return;
	}
    }
    // no problem to go back, we just wrote it
    // FIXME: we can pass the surface through.
    surface =
	decoder->SurfacesRb[(decoder->SurfaceWrite + VIDEO_SURFACES_MAX -
	    1) % VIDEO_SURFACES_MAX];

    //	Copy data from frame to image
    if (!decoder->GetPutImage
	&& vaDeriveImage(decoder->VaDisplay, surface,
	    decoder->Image) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed\n"));
	decoder->GetPutImage = 1;
	goto again;
    }
    if (decoder->GetPutImage
	&& (i =
	    vaGetImage(decoder->VaDisplay, surface, 0, 0, decoder->InputWidth,
		decoder->InputHeight,
		decoder->Image->image_id)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't get auto-crop image %d\n"), i);
	printf(_("video/vaapi: can't get auto-crop image %d\n"), i);
	return;
    }
    if (vaMapBuffer(VaDisplay, decoder->Image->buf, &va_image_data)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't map auto-crop image!\n"));
	return;
    }
    // convert vaapi to our frame format
    for (i = 0; (unsigned)i < decoder->Image->num_planes; ++i) {
	data[i] = va_image_data + decoder->Image->offsets[i];
	pitches[i] = decoder->Image->pitches[i];
    }

    AutoCropDetect(decoder->AutoCrop, width, height, data, pitches);

    if (vaUnmapBuffer(VaDisplay, decoder->Image->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap auto-crop image!\n"));
    }
    if (!decoder->GetPutImage) {
	if (vaDestroyImage(VaDisplay, decoder->Image->image_id)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	decoder->Image->image_id = VA_INVALID_ID;
    }
    // FIXME: this a copy of vdpau, combine the two same things

    // ignore black frames
    if (decoder->AutoCrop->Y1 >= decoder->AutoCrop->Y2) {
	return;
    }

    crop14 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 14);
    crop14 = (decoder->InputHeight - crop14) / 2;
    crop16 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 16);
    crop16 = (decoder->InputHeight - crop16) / 2;

    if (decoder->AutoCrop->Y1 >= crop16 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop16 - AutoCropTolerance) {
	next_state = 16;
    } else if (decoder->AutoCrop->Y1 >= crop14 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop14 - AutoCropTolerance) {
	next_state = 14;
    } else {
	next_state = 0;
    }

    if (decoder->AutoCrop->State == next_state) {
	return;
    }

    Debug(3, "video: crop aspect %d:%d %d/%d %+d%+d\n",
	decoder->InputAspect.num, decoder->InputAspect.den, crop14, crop16,
	decoder->AutoCrop->Y1, decoder->InputHeight - decoder->AutoCrop->Y2);

    Debug(3, "video: crop aspect %d -> %d\n", decoder->AutoCrop->State,
	next_state);

    switch (decoder->AutoCrop->State) {
	case 16:
	case 14:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay / 2) {
		return;
	    }
	    break;
	case 0:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay) {
		return;
	    }
	    break;
    }

    decoder->AutoCrop->State = next_state;
    if (next_state) {
	decoder->CropX = VideoCutLeftRight[decoder->Resolution];
	decoder->CropY =
	    (next_state ==
	    16 ? crop16 : crop14) + VideoCutTopBottom[decoder->Resolution];
	decoder->CropWidth = decoder->InputWidth - decoder->CropX * 2;
	decoder->CropHeight = decoder->InputHeight - decoder->CropY * 2;

	// FIXME: this overwrites user choosen output position
	// FIXME: resize kills the auto crop values
	// FIXME: support other 4:3 zoom modes
	decoder->OutputX = decoder->VideoX;
	decoder->OutputY = decoder->VideoY;
	decoder->OutputWidth = (decoder->VideoHeight * next_state) / 9;
	decoder->OutputHeight = (decoder->VideoWidth * 9) / next_state;
	if (decoder->OutputWidth > decoder->VideoWidth) {
	    decoder->OutputWidth = decoder->VideoWidth;
	    decoder->OutputY +=
		(decoder->VideoHeight - decoder->OutputHeight) / 2;
	} else if (decoder->OutputHeight > decoder->VideoHeight) {
	    decoder->OutputHeight = decoder->VideoHeight;
	    decoder->OutputX +=
		(decoder->VideoWidth - decoder->OutputWidth) / 2;
	}
	Debug(3, "video: aspect output %dx%d %dx%d%+d%+d\n",
	    decoder->InputWidth, decoder->InputHeight, decoder->OutputWidth,
	    decoder->OutputHeight, decoder->OutputX, decoder->OutputY);
    } else {
	// sets AutoCrop->Count
	VaapiUpdateOutput(decoder);
    }
    decoder->AutoCrop->Count = 0;

    //
    //	update OSD associate
    //
    VaapiDeassociate(decoder);
    VaapiAssociate(decoder);
}

///
///	VA-API check if auto-crop todo.
///
///	@param decoder	VA-API hw decoder
///
///	@note a copy of VdpauCheckAutoCrop
///	@note auto-crop only supported with normal 4:3 display mode
///
static void VaapiCheckAutoCrop(VaapiDecoder * decoder)
{
    // reduce load, check only n frames
    if (Video4to3ZoomMode == VideoNormal && AutoCropInterval
	&& !(decoder->FrameCounter % AutoCropInterval)) {
	AVRational input_aspect_ratio;
	AVRational tmp_ratio;

	av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den,
	    decoder->InputWidth * decoder->InputAspect.num,
	    decoder->InputHeight * decoder->InputAspect.den, 1024 * 1024);

	tmp_ratio.num = 4;
	tmp_ratio.den = 3;
	// 4:3 with 16:9/14:9 inside
	if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	    VaapiAutoCrop(decoder);
	} else {
	// 15:11 with 16:9/14:9 inside
	    tmp_ratio.num = 15;
	    tmp_ratio.den = 11;
	    if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	        VaapiAutoCrop(decoder);
	    } else {
	        decoder->AutoCrop->Count = 0;
	        decoder->AutoCrop->State = 0;
	    }
	}
    }
}

///
///	VA-API reset auto-crop.
///
static void VaapiResetAutoCrop(void)
{
    int i;

    for (i = 0; i < VaapiDecoderN; ++i) {
	VaapiDecoders[i]->AutoCrop->State = 0;
	VaapiDecoders[i]->AutoCrop->Count = 0;
    }
}

#endif


///
///	Queue output surface.
///
///	@param decoder	VA-API decoder
///	@param surface	output surface
///
///	@note we can't mix software and hardware decoder surfaces
///
static void VaapiQueueSurfaceNew(VaapiDecoder * decoder, VASurfaceID surface)
{
    unsigned int i;

    /* Advance surfaces in queue:
     * Playback position -> last forward temporal reference (past-surface)
     * Nearest surface to playback -> playback position
     * New surface -> first surface in backward (future-surface) reference queue
     */
    if (decoder->ForwardRefCount) {
	for (i = 0; i < decoder->ForwardRefCount - 1; ++i) {
	    decoder->ForwardRefSurfaces[i] = decoder->ForwardRefSurfaces[i + 1];
	}
	decoder->ForwardRefSurfaces[decoder->ForwardRefCount - 1] = decoder->PlaybackSurface;
    }

    if (decoder->BackwardRefCount) {
	decoder->PlaybackSurface = decoder->BackwardRefSurfaces[0];
	for (i = decoder->BackwardRefCount - 1; i > 0; --i) {
	    decoder->BackwardRefSurfaces[i - 1] = decoder->BackwardRefSurfaces[i];
	}
	decoder->BackwardRefSurfaces[decoder->BackwardRefCount - 1] = surface;
    } else {
	/* No backward (future-surface) references needed so put new surface
	   to playback position */
	decoder->PlaybackSurface = surface;
    }
}

static void VaapiAddToHistoryQueue(VASurfaceID* queue, VASurfaceID surface)
{
    unsigned int i;

    for (i = FIELD_SURFACES_MAX - 1; i > 0; --i) {
        queue[i] = queue[i - 1];
    }
    queue[0] = surface;
}

///
///	Queue output surface.
///
///	@param decoder	VA-API decoder
///	@param surface	output surface
///	@param softdec  software decoder
///
///	@note we can't mix software and hardware decoder surfaces
///
static void VaapiQueueSurface(VaapiDecoder * decoder, VASurfaceID surface,
    int softdec)
{
    VASurfaceID old;
    VASurfaceID * firstfield = NULL;
    VASurfaceID * secondfield = NULL;
    ++decoder->FrameCounter;

    if (1) {				// can't wait for output queue empty
	if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX - 1) {
	    ++decoder->FramesDropped;
	    Warning(_("video: output buffer full, dropping frame (%d/%d)\n"),
		decoder->FramesDropped, decoder->FrameCounter);
	    if (!(decoder->FramesDisplayed % 300)) {
		VaapiPrintFrames(decoder);
	    }
	    if (softdec) {		// software surfaces only
		VaapiReleaseSurface(decoder, surface);
	    }
	    return;
	}
#if 0
    } else {				// wait for output queue empty
	while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX - 1) {
	    VideoDisplayHandler();
	}
#endif
    }

    //
    //	    Check and release, old surface
    //
    if ((old = decoder->SurfacesRb[decoder->SurfaceWrite])
	!= VA_INVALID_ID) {

#if 0
	if (vaSyncSurface(decoder->VaDisplay, old) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
	VASurfaceStatus status;

	if (vaQuerySurfaceStatus(decoder->VaDisplay, old, &status)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaQuerySurface failed\n"));
	    status = VASurfaceReady;
	}
	if (status != VASurfaceReady) {
	    Warning(_
		("video/vaapi: surface %#010x not ready: still displayed %d\n"),
		old, status);
	    if (0
		&& vaSyncSurface(decoder->VaDisplay,
		    old) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaSyncSurface failed\n"));
	    }
	}
#endif

	// now we can release the surface
	if (softdec) {			// software surfaces only
	    VaapiReleaseSurface(decoder, old);
	}
    }
#if 0
    // FIXME: intel seems to forget this, nvidia GT 210 has speed problems here
    if (VaapiBuggyIntel && VaOsdSubpicture != VA_INVALID_ID) {
	// FIXME: associate only if osd is displayed

	//
	//	associate the OSD with surface
	//
	if (VaapiUnscaledOsd) {
	    if (vaAssociateSubpicture(VaDisplay, VaOsdSubpicture, &surface, 1,
		    0, 0, VaOsdImage.width, VaOsdImage.height, 0, 0,
		    VideoWindowWidth, VideoWindowHeight,
		    VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't associate subpicture\n"));
	    }
	} else {
	    // FIXME: auto-crop wrong position
	    if (vaAssociateSubpicture(VaDisplay, VaOsdSubpicture, &surface, 1,
		    0, 0, VaOsdImage.width, VaOsdImage.height, 0, 0,
		    decoder->InputWidth, decoder->InputHeight, 0)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't associate subpicture\n"));
	    }
	}
    }
#endif

    /* No point in adding new surface if cleanup is in progress */
    if (pthread_mutex_trylock(&VideoMutex))
        return;

    /* Queue new surface and run postprocessing filters */
    VaapiQueueSurfaceNew(decoder, surface);
    firstfield = VaapiApplyFilters(decoder, decoder->TopFieldFirst ? 1 : 0);
    if (!firstfield) {
        /* Use unprocessed surface if postprocessing fails */
        decoder->Deinterlaced = 0;

        VaapiAddToHistoryQueue(decoder->FirstFieldHistory, surface);
    } else {
        decoder->Deinterlaced = 1;

        VaapiAddToHistoryQueue(decoder->FirstFieldHistory, *firstfield);
    }

    /* Queue the first field */
    decoder->SurfacesRb[decoder->SurfaceWrite] = decoder->FirstFieldHistory[VideoFirstField[decoder->Resolution]];
    decoder->SurfaceWrite = (decoder->SurfaceWrite + 1) % VIDEO_SURFACES_MAX;
    decoder->SurfaceField = decoder->TopFieldFirst ? 0 : 1;
    atomic_inc(&decoder->SurfacesFilled);

    /* Run postprocessing twice for top & bottom fields */
    if (decoder->Interlaced) {
        secondfield = VaapiApplyFilters(decoder, decoder->TopFieldFirst ? 0 : 1);
        if (!secondfield) {
            /* Use unprocessed surface if postprocessing fails */
            decoder->Deinterlaced = 0;

            VaapiAddToHistoryQueue(decoder->SecondFieldHistory, surface);
        } else {
            decoder->Deinterlaced = 1;

            VaapiAddToHistoryQueue(decoder->SecondFieldHistory, *secondfield);
        }
        decoder->SurfacesRb[decoder->SurfaceWrite] = decoder->SecondFieldHistory[VideoSecondField[decoder->Resolution]];
        decoder->SurfaceWrite = (decoder->SurfaceWrite + 1) % VIDEO_SURFACES_MAX;
        decoder->SurfaceField = decoder->TopFieldFirst ? 1 : 0;
        atomic_inc(&decoder->SurfacesFilled);
    }

    pthread_mutex_unlock(&VideoMutex);

    Debug(4, "video/vaapi: yy video surface %#010x ready\n", surface);
}

///
///	Create and display a black empty surface.
///
///	@param decoder	VA-API decoder
///
static void VaapiBlackSurface(VaapiDecoder * decoder)
{
    VAStatus status;

#ifdef DEBUG
    uint32_t start;
#endif
    uint32_t sync;
    uint32_t put1;
    int width, height;

#ifdef USE_GLX
    if (GlxEnabled) {			// already done
	return;
    }
#endif
#ifdef USE_EGL
    if (EglEnabled) {			// already done
	return;
    }
#endif

    // wait until we have osd subpicture
    if (VaOsdSubpicture == VA_INVALID_ID) {
	Warning(_("video/vaapi: no osd subpicture yet\n"));
	return;
    }

    if (VaapiUnscaledOsd) {
	width = VideoWindowWidth;
	height = VideoWindowHeight;
    } else {
	width = OsdWidth;
	height = OsdHeight;
    }

    if (decoder->BlackSurface == VA_INVALID_ID) {
	uint8_t *va_image_data;
	unsigned u;

	status =
	    vaCreateSurfaces(decoder->VaDisplay, VA_RT_FORMAT_YUV420,
	    width, height, &decoder->BlackSurface, 1,
	    NULL, 0);
	if (status != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create a surface: %s\n"),
		vaErrorStr(status));
	    return;
	}
	// full sized surface, no difference unscaled/scaled osd
	status =
	    vaAssociateSubpicture(decoder->VaDisplay, VaOsdSubpicture,
	    &decoder->BlackSurface, 1, 0, 0, VaOsdImage.width,
	    VaOsdImage.height, 0, 0, width, height, 0);
	if (status != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture: %s\n"),
		vaErrorStr(status));
	}
	Debug(3, "video/vaapi: associate %08x\n", decoder->BlackSurface);

	if (decoder->Image->image_id == VA_INVALID_ID) {
	    VAImageFormat format[1];

	    VaapiFindImageFormat(decoder, AV_PIX_FMT_NV12, format);
	    status =
		vaCreateImage(VaDisplay, format, width,
		height, decoder->Image);
	    if (status != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't create image: %s\n"),
		    vaErrorStr(status));
		return;
	    }
	}

	status =
	    vaMapBuffer(VaDisplay, decoder->Image->buf,
	    (void **)&va_image_data);
	if (status != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't map the image: %s\n"),
		vaErrorStr(status));
	    return;
	}

	for (u = 0; u < decoder->Image->data_size; ++u) {
	    if (u < decoder->Image->offsets[1]) {
		va_image_data[u] = 0x00;	// Y
	    } else if (u % 2 == 0) {
		va_image_data[u] = 0x80;	// U
	    } else {
#if 0
		// make black surface visible
		va_image_data[u] = 0xFF;	// V
#else
		va_image_data[u] = 0x80;	// V
#endif
	    }
	}

	if (vaUnmapBuffer(VaDisplay, decoder->Image->buf) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't unmap the image!\n"));
	}

	if (decoder->GetPutImage) {
	    status =
		vaPutImage(VaDisplay, decoder->BlackSurface,
		decoder->Image->image_id, 0, 0, width,
		height, 0, 0, width, height);
	    if (status != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't put image!\n"));
	    }
	} else {
	    // FIXME: PutImage isn't always supported
	    Debug(3,
		"video/vaapi: put image not supported, alternative path not written\n");
	}

#ifdef DEBUG
	start = GetMsTicks();
#endif
	if (vaSyncSurface(decoder->VaDisplay,
		decoder->BlackSurface) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
    } else {
#ifdef DEBUG
	start = GetMsTicks();
#endif
    }

    Debug(4, "video/vaapi: yy black video surface %#010x displayed\n",
	decoder->BlackSurface);
    sync = GetMsTicks();
    xcb_flush(Connection);
    if ((status =
	    vaPutSurface(decoder->VaDisplay, decoder->BlackSurface,
		decoder->Window,
		// decoder src
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth,
		decoder->OutputHeight,
		// video dst
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth,
		decoder->OutputHeight, NULL, 0,
		VA_FRAME_PICTURE)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaPutSurface failed %d\n"), status);
    }
    clock_gettime(CLOCK_MONOTONIC, &decoder->FrameTime);

    put1 = GetMsTicks();
    if (put1 - sync > 2000) {
	Error(_("video/vaapi: gpu hung %dms %d\n"), put1 - sync,
	    decoder->FrameCounter);
#ifdef DEBUG
	fprintf(stderr, _("video/vaapi: gpu hung %dms %d\n"), put1 - sync,
	    decoder->FrameCounter);
#endif
    }
    Debug(4, "video/vaapi: sync %2u put1 %2u\n", sync - start, put1 - sync);

    if (0 && vaSyncSurface(decoder->VaDisplay, decoder->BlackSurface)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }
    usleep(1 * 1000);
}

#define noUSE_VECTOR			///< use gcc vector extension
#ifdef USE_VECTOR

typedef char v16qi __attribute__ ((vector_size(16)));
typedef char v8qi __attribute__ ((vector_size(8)));
typedef int16_t v4hi __attribute__ ((vector_size(4)));
typedef int16_t v8hi __attribute__ ((vector_size(8)));

///
///	ELA Edge-based Line Averaging
///	Low-Complexity Interpolation Method
///
///	abcdefg	   abcdefg	abcdefg	 abcdefg    abcdefg
///	   x	     x		  x	    x		 x
///	hijklmn	 hijklmn    hijklmn	   hijklmn	 hijklmn
///
static void FilterLineSpatial(uint8_t * dst, const uint8_t * cur, int width,
    int above, int below, int next)
{
    int x;

    // 8/16 128bit xmm register

    for (x = 0; x < width; x += 8) {
	v8qi c;
	v8qi d;
	v8qi e;
	v8qi j;
	v8qi k;
	v8qi l;
	v8qi t1;
	v8qi t2;
	v8qi pred;
	v8qi score_l;
	v8qi score_h;
	v8qi t_l;
	v8qi t_h;
	v8qi zero;

	// ignore bound violation
	d = *(v8qi *) & cur[above + x];
	k = *(v8qi *) & cur[below + x];
	pred = __builtin_ia32_pavgb(d, k);

	// score = ABS(c - j) + ABS(d - k) + ABS(e - l);
	c = *(v8qi *) & cur[above + x - 1 * next];
	e = *(v8qi *) & cur[above + x + 1 * next];
	j = *(v8qi *) & cur[below + x - 1 * next];
	l = *(v8qi *) & cur[below + x + 1 * next];

	t1 = __builtin_ia32_psubusb(c, j);
	t2 = __builtin_ia32_psubusb(j, c);
	t1 = __builtin_ia32_pmaxub(t1, t2);
	zero ^= zero;
	score_l = __builtin_ia32_punpcklbw(t1, zero);
	score_h = __builtin_ia32_punpckhbw(t1, zero);

	t1 = __builtin_ia32_psubusb(d, k);
	t2 = __builtin_ia32_psubusb(k, d);
	t1 = __builtin_ia32_pmaxub(t1, t2);
	t_l = __builtin_ia32_punpcklbw(t1, zero);
	t_h = __builtin_ia32_punpckhbw(t1, zero);
	score_l = __builtin_ia32_paddw(score_l, t_l);
	score_h = __builtin_ia32_paddw(score_h, t_h);

	t1 = __builtin_ia32_psubusb(e, l);
	t2 = __builtin_ia32_psubusb(l, e);
	t1 = __builtin_ia32_pmaxub(t1, t2);
	t_l = __builtin_ia32_punpcklbw(t1, zero);
	t_h = __builtin_ia32_punpckhbw(t1, zero);
	score_l = __builtin_ia32_paddw(score_l, t_l);
	score_h = __builtin_ia32_paddw(score_h, t_h);

	*(v8qi *) & dst[x] = pred;
    }
}

#else

    /// Return the absolute value of an integer.
#define ABS(i)	((i) >= 0 ? (i) : (-(i)))

///
///	ELA Edge-based Line Averaging
///	Low-Complexity Interpolation Method
///
///	abcdefg	   abcdefg	abcdefg	 abcdefg    abcdefg
///	   x	     x		  x	    x		 x
///	hijklmn	 hijklmn    hijklmn	   hijklmn	 hijklmn
///
static void FilterLineSpatial(uint8_t * dst, const uint8_t * cur, int width,
    int above, int below, int next)
{
    int a, b, c, d, e, f, g, h, i, j, k, l, m, n;
    int spatial_pred;
    int spatial_score;
    int score;
    int x;

    for (x = 0; x < width; ++x) {
	a = cur[above + x - 3 * next];	// ignore bound violation
	b = cur[above + x - 2 * next];
	c = cur[above + x - 1 * next];
	d = cur[above + x + 0 * next];
	e = cur[above + x + 1 * next];
	f = cur[above + x + 2 * next];
	g = cur[above + x + 3 * next];

	h = cur[below + x - 3 * next];
	i = cur[below + x - 2 * next];
	j = cur[below + x - 1 * next];
	k = cur[below + x + 0 * next];
	l = cur[below + x + 1 * next];
	m = cur[below + x + 2 * next];
	n = cur[below + x + 3 * next];

	spatial_pred = (d + k) / 2;	// 0 pixel
	spatial_score = ABS(c - j) + ABS(d - k) + ABS(e - l);

	score = ABS(b - k) + ABS(c - l) + ABS(d - m);
	if (score < spatial_score) {
	    spatial_pred = (c + l) / 2;	// 1 pixel
	    spatial_score = score;
	    score = ABS(a - l) + ABS(b - m) + ABS(c - n);
	    if (score < spatial_score) {
		spatial_pred = (b + m) / 2;	// 2 pixel
		spatial_score = score;
	    }
	}
	score = ABS(d - i) + ABS(e - j) + ABS(f - k);
	if (score < spatial_score) {
	    spatial_pred = (e + j) / 2;	// -1 pixel
	    spatial_score = score;
	    score = ABS(e - h) + ABS(f - i) + ABS(g - j);
	    if (score < spatial_score) {
		spatial_pred = (f + i) / 2;	// -2 pixel
		spatial_score = score;
	    }
	}

	dst[x + 0] = spatial_pred;
    }
}

#endif

///
///	Vaapi spatial deinterlace.
///
///	@note FIXME: use common software deinterlace functions.
///
static void VaapiSpatial(VaapiDecoder * decoder, VAImage * src, VAImage * dst1,
    VAImage * dst2)
{
#ifdef DEBUG
    uint32_t tick1;
    uint32_t tick2;
    uint32_t tick3;
    uint32_t tick4;
    uint32_t tick5;
    uint32_t tick6;
    uint32_t tick7;
    uint32_t tick8;
#endif
    void *src_base;
    void *dst1_base;
    void *dst2_base;
    unsigned y;
    unsigned p;
    uint8_t *tmp;
    int pitch;
    int width;

#ifdef DEBUG
    tick1 = GetMsTicks();
#endif
    if (vaMapBuffer(decoder->VaDisplay, src->buf,
	    &src_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
#ifdef DEBUG
    tick2 = GetMsTicks();
#endif
    if (vaMapBuffer(decoder->VaDisplay, dst1->buf,
	    &dst1_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
#ifdef DEBUG
    tick3 = GetMsTicks();
#endif
    if (vaMapBuffer(decoder->VaDisplay, dst2->buf,
	    &dst2_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
#ifdef DEBUG
    tick4 = GetMsTicks();
#endif

    if (0) {				// test all updated
	memset(dst1_base, 0x00, dst1->data_size);
	memset(dst2_base, 0xFF, dst2->data_size);
    }
    // use tmp copy FIXME: only for intel needed
    tmp = malloc(src->data_size);
    memcpy(tmp, src_base, src->data_size);

    if (src->num_planes == 2) {		// NV12
	pitch = src->pitches[0];
	width = src->width;
	for (y = 0; y < (unsigned)src->height; y++) {	// Y
	    const uint8_t *cur;

	    cur = tmp + src->offsets[0] + y * pitch;
	    if (y & 1) {
		// copy to 2nd
		memcpy(dst2_base + src->offsets[0] + y * pitch, cur, width);
		// create 1st
		FilterLineSpatial(dst1_base + src->offsets[0] + y * pitch, cur,
		    width, y ? -pitch : pitch,
		    y + 1 < (unsigned)src->height ? pitch : -pitch, 1);
	    } else {
		// copy to 1st
		memcpy(dst1_base + src->offsets[0] + y * pitch, cur, width);
		// create 2nd
		FilterLineSpatial(dst2_base + src->offsets[0] + y * pitch, cur,
		    width, y ? -pitch : pitch,
		    y + 1 < (unsigned)src->height ? pitch : -pitch, 1);
	    }
	}
	if (VideoSkipChromaDeinterlace[decoder->Resolution]) {
	    for (y = 0; y < (unsigned)src->height / 2; y++) {	// UV
		const uint8_t *cur;

		cur = tmp + src->offsets[1] + y * pitch;
		// copy to 1st
		memcpy(dst1_base + src->offsets[1] + y * pitch, cur, width);
		// copy to 2nd
		memcpy(dst2_base + src->offsets[1] + y * pitch, cur, width);
	    }
	} else {
	    for (y = 0; y < (unsigned)src->height / 2; y++) {	// UV
		const uint8_t *cur;

		cur = tmp + src->offsets[1] + y * pitch;
		if (y & 1) {
		    // copy to 2nd
		    memcpy(dst2_base + src->offsets[1] + y * pitch, cur,
			width);
		    // create 1st
		    FilterLineSpatial(dst1_base + src->offsets[1] + y * pitch,
			cur, width, y ? -pitch : pitch,
			y + 1 < (unsigned)src->height / 2 ? pitch : -pitch, 2);
		} else {
		    // copy to 1st
		    memcpy(dst1_base + src->offsets[1] + y * pitch, cur,
			width);
		    // create 2nd
		    FilterLineSpatial(dst2_base + src->offsets[1] + y * pitch,
			cur, width, y ? -pitch : pitch,
			y + 1 < (unsigned)src->height / 2 ? pitch : -pitch, 2);
		}
	    }
	}
    } else {				// YV12 or I420
	for (p = 0; p < src->num_planes; ++p) {
	    pitch = src->pitches[p];
	    width = src->width >> (p != 0);
	    if (VideoSkipChromaDeinterlace[decoder->Resolution] && p) {
		for (y = 0; y < (unsigned)(src->height >> 1); y++) {
		    const uint8_t *cur;

		    cur = tmp + src->offsets[p] + y * pitch;
		    // copy to 1st
		    memcpy(dst1_base + src->offsets[p] + y * pitch, cur,
			width);
		    // copy to 2nd
		    memcpy(dst2_base + src->offsets[p] + y * pitch, cur,
			width);
		}
	    } else {
		for (y = 0; y < (unsigned)(src->height >> (p != 0)); y++) {
		    const uint8_t *cur;

		    cur = tmp + src->offsets[p] + y * pitch;
		    if (y & 1) {
			// copy to 2nd
			memcpy(dst2_base + src->offsets[p] + y * pitch, cur,
			    width);
			// create 1st
			FilterLineSpatial(dst1_base + src->offsets[p] +
			    y * pitch, cur, width, y ? -pitch : pitch,
			    y + 1 < (unsigned)(src->height >> (p != 0))
			    ? pitch : -pitch, 1);
		    } else {
			// copy to 1st
			memcpy(dst1_base + src->offsets[p] + y * pitch, cur,
			    width);
			// create 2nd
			FilterLineSpatial(dst2_base + src->offsets[p] +
			    y * pitch, cur, width, y ? -pitch : pitch,
			    y + 1 < (unsigned)(src->height >> (p != 0))
			    ? pitch : -pitch, 1);
		    }
		}
	    }
	}
    }
    free(tmp);

#ifdef DEBUG
    tick5 = GetMsTicks();
#endif
    if (vaUnmapBuffer(decoder->VaDisplay, dst2->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
#ifdef DEBUG
    tick6 = GetMsTicks();
#endif
    if (vaUnmapBuffer(decoder->VaDisplay, dst1->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
#ifdef DEBUG
    tick7 = GetMsTicks();
#endif
    if (vaUnmapBuffer(decoder->VaDisplay, src->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
#ifdef DEBUG
    tick8 = GetMsTicks();

    Debug(3, "video/vaapi: map=%2d/%2d/%2d deint=%2d umap=%2d/%2d/%2d\n",
	tick2 - tick1, tick3 - tick2, tick4 - tick3, tick5 - tick4,
	tick6 - tick5, tick7 - tick6, tick8 - tick7);
#endif
}

///
///	Vaapi bob deinterlace.
///
///	@note FIXME: use common software deinterlace functions.
///
static void VaapiBob(VaapiDecoder * decoder, VAImage * src, VAImage * dst1,
    VAImage * dst2)
{
#ifdef DEBUG
    uint32_t tick1;
    uint32_t tick2;
    uint32_t tick3;
    uint32_t tick4;
    uint32_t tick5;
    uint32_t tick6;
    uint32_t tick7;
    uint32_t tick8;
#endif
    void *src_base;
    void *dst1_base;
    void *dst2_base;
    unsigned y;
    unsigned p;

#ifdef DEBUG
    tick1 = GetMsTicks();
#endif
    if (vaMapBuffer(decoder->VaDisplay, src->buf,
	    &src_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
#ifdef DEBUG
    tick2 = GetMsTicks();
#endif
    if (vaMapBuffer(decoder->VaDisplay, dst1->buf,
	    &dst1_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
#ifdef DEBUG
    tick3 = GetMsTicks();
#endif
    if (vaMapBuffer(decoder->VaDisplay, dst2->buf,
	    &dst2_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
#ifdef DEBUG
    tick4 = GetMsTicks();
#endif

    if (0) {				// test all updated
	memset(dst1_base, 0x00, dst1->data_size);
	memset(dst2_base, 0xFF, dst2->data_size);
	return;
    }
#if 0
    // interleave
    for (p = 0; p < src->num_planes; ++p) {
	for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
	    memcpy(dst1_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst1_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src->pitches[p]);

	    memcpy(dst2_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst2_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	}
    }
#endif
#if 1
    // use tmp copy
    if (1) {
	uint8_t *tmp;

	tmp = malloc(src->data_size);
	memcpy(tmp, src_base, src->data_size);

	for (p = 0; p < src->num_planes; ++p) {
	    for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
		memcpy(dst1_base + src->offsets[p] + (y + 0) * src->pitches[p],
		    tmp + src->offsets[p] + (y + 0) * src->pitches[p],
		    src->pitches[p]);
		memcpy(dst1_base + src->offsets[p] + (y + 1) * src->pitches[p],
		    tmp + src->offsets[p] + (y + 0) * src->pitches[p],
		    src->pitches[p]);

		memcpy(dst2_base + src->offsets[p] + (y + 0) * src->pitches[p],
		    tmp + src->offsets[p] + (y + 1) * src->pitches[p],
		    src->pitches[p]);
		memcpy(dst2_base + src->offsets[p] + (y + 1) * src->pitches[p],
		    tmp + src->offsets[p] + (y + 1) * src->pitches[p],
		    src->pitches[p]);
	    }

	}
	free(tmp);
    }
#endif
#if 0
    // use multiple tmp copy
    if (1) {
	uint8_t *tmp_src;
	uint8_t *tmp_dst1;
	uint8_t *tmp_dst2;

	tmp_src = malloc(src->data_size);
	memcpy(tmp_src, src_base, src->data_size);
	tmp_dst1 = malloc(src->data_size);
	tmp_dst2 = malloc(src->data_size);

	for (p = 0; p < src->num_planes; ++p) {
	    for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
		memcpy(tmp_dst1 + src->offsets[p] + (y + 0) * src->pitches[p],
		    tmp_src + src->offsets[p] + (y + 0) * src->pitches[p],
		    src->pitches[p]);
		memcpy(tmp_dst1 + src->offsets[p] + (y + 1) * src->pitches[p],
		    tmp_src + src->offsets[p] + (y + 0) * src->pitches[p],
		    src->pitches[p]);

		memcpy(tmp_dst2 + src->offsets[p] + (y + 0) * src->pitches[p],
		    tmp_src + src->offsets[p] + (y + 1) * src->pitches[p],
		    src->pitches[p]);
		memcpy(tmp_dst2 + src->offsets[p] + (y + 1) * src->pitches[p],
		    tmp_src + src->offsets[p] + (y + 1) * src->pitches[p],
		    src->pitches[p]);
	    }
	}
	memcpy(dst1_base, tmp_dst1, src->data_size);
	memcpy(dst2_base, tmp_dst2, src->data_size);

	free(tmp_src);
	free(tmp_dst1);
	free(tmp_dst2);
    }
#endif
#if 0
    // dst1 first
    for (p = 0; p < src->num_planes; ++p) {
	for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
	    memcpy(dst1_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst1_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src->pitches[p]);
	}
    }
    // dst2 next
    for (p = 0; p < src->num_planes; ++p) {
	for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
	    memcpy(dst2_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst2_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	}
    }
#endif

#ifdef DEBUG
    tick5 = GetMsTicks();
#endif

    if (vaUnmapBuffer(decoder->VaDisplay, dst2->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
#ifdef DEBUG
    tick6 = GetMsTicks();
#endif
    if (vaUnmapBuffer(decoder->VaDisplay, dst1->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
#ifdef DEBUG
    tick7 = GetMsTicks();
#endif
    if (vaUnmapBuffer(decoder->VaDisplay, src->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
#ifdef DEBUG
    tick8 = GetMsTicks();

    Debug(4, "video/vaapi: map=%2d/%2d/%2d deint=%2d umap=%2d/%2d/%2d\n",
	tick2 - tick1, tick3 - tick2, tick4 - tick3, tick5 - tick4,
	tick6 - tick5, tick7 - tick6, tick8 - tick7);
#endif
}

///
///	Create software deinterlace images.
///
///	@param decoder		VA-API decoder
///
static void VaapiCreateDeinterlaceImages(VaapiDecoder * decoder)
{
    VAImageFormat format[1];
    int i;

    // NV12, YV12, I420, BGRA
    // NV12 Y U/V 2x2
    // YV12 Y V U 2x2
    // I420 Y U V 2x2

    // Intel needs NV12
    VaapiFindImageFormat(decoder, AV_PIX_FMT_NV12, format);
    //VaapiFindImageFormat(decoder, AV_PIX_FMT_YUV420P, format);
    for (i = 0; i < 5; ++i) {
	if (vaCreateImage(decoder->VaDisplay, format, decoder->InputWidth,
		decoder->InputHeight,
		decoder->DeintImages + i) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create image!\n"));
	}
    }
#ifdef DEBUG
    if (1) {
	VAImage *img;

	img = decoder->DeintImages;
	Debug(3, "video/vaapi: %c%c%c%c %dx%d*%d\n", img->format.fourcc,
	    img->format.fourcc >> 8, img->format.fourcc >> 16,
	    img->format.fourcc >> 24, img->width, img->height,
	    img->num_planes);
    }
#endif
}

///
///	Destroy software deinterlace images.
///
///	@param decoder	VA-API decoder
///
static void VaapiDestroyDeinterlaceImages(VaapiDecoder * decoder)
{
    int i;

    for (i = 0; i < 5; ++i) {
	if (vaDestroyImage(decoder->VaDisplay,
		decoder->DeintImages[i].image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	decoder->DeintImages[i].image_id = VA_INVALID_ID;
    }
}

///
///	Vaapi software deinterlace.
///
///	@param decoder	VA-API decoder
///	@param surface	interlaced hardware surface
///
static void VaapiCpuDerive(VaapiDecoder * decoder, VASurfaceID surface)
{
    //
    //	vaPutImage not working, vaDeriveImage
    //
#ifdef DEBUG
    uint32_t tick1;
    uint32_t tick2;
    uint32_t tick3;
    uint32_t tick4;
    uint32_t tick5;
#endif
    VAImage image[1];
    VAImage dest1[1];
    VAImage dest2[1];
    VAStatus status;
    VASurfaceID out1;
    VASurfaceID out2;

#ifdef DEBUG
    tick1 = GetMsTicks();
#endif
#if 0
    // get image test
    if (decoder->Image->image_id == VA_INVALID_ID) {
	VAImageFormat format[1];

	VaapiFindImageFormat(decoder, AV_PIX_FMT_NV12, format);
	if (vaCreateImage(VaDisplay, format, decoder->InputWidth,
		decoder->InputHeight, decoder->Image) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create image!\n"));
	}
    }
    if (vaGetImage(decoder->VaDisplay, surface, 0, 0, decoder->InputWidth,
	    decoder->InputHeight, decoder->Image->image_id)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't get source image\n"));
	VaapiQueueSurface(decoder, surface, 0);
	VaapiQueueSurface(decoder, surface, 0);
	return;
    }
    *image = *decoder->Image;
#else
    if ((status =
	    vaDeriveImage(decoder->VaDisplay, surface,
		image)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed %d\n"), status);
	VaapiQueueSurface(decoder, surface, 0);
	VaapiQueueSurface(decoder, surface, 0);
	return;
    }
#endif
#ifdef DEBUG
    tick2 = GetMsTicks();
#endif

    Debug(4, "video/vaapi: %c%c%c%c %dx%d*%d\n", image->format.fourcc,
	image->format.fourcc >> 8, image->format.fourcc >> 16,
	image->format.fourcc >> 24, image->width, image->height,
	image->num_planes);

    // get a free surfaces
    out1 = VaapiGetSurface0(decoder);
    if (out1 == VA_INVALID_ID) {
	abort();
    }
    if ((status =
	    vaDeriveImage(decoder->VaDisplay, out1,
		dest1)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed %d\n"), status);
    }
#ifdef DEBUG
    tick3 = GetMsTicks();
#endif
    out2 = VaapiGetSurface0(decoder);
    if (out2 == VA_INVALID_ID) {
	abort();
    }
    if ((status =
	    vaDeriveImage(decoder->VaDisplay, out2,
		dest2)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed %d\n"), status);
    }
#ifdef DEBUG
    tick4 = GetMsTicks();
#endif

    switch (VideoDeinterlace[decoder->Resolution]) {
	case VideoDeinterlaceSoftBob:
	default:
	    VaapiBob(decoder, image, dest1, dest2);
	    break;
	case VideoDeinterlaceSoftSpatial:
	    VaapiSpatial(decoder, image, dest1, dest2);
	    break;
    }
#ifdef DEBUG
    tick5 = GetMsTicks();
#endif

#if 1
    if (vaDestroyImage(VaDisplay, image->image_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy image!\n"));
    }
#endif
    if (vaDestroyImage(VaDisplay, dest1->image_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy image!\n"));
    }
    if (vaDestroyImage(VaDisplay, dest2->image_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy image!\n"));
    }

    VaapiQueueSurface(decoder, out1, 1);
    VaapiQueueSurface(decoder, out2, 1);

#ifdef DEBUG
    tick5 = GetMsTicks();

    Debug(4, "video/vaapi: get=%2d get1=%2d get2=%d deint=%2d\n",
	tick2 - tick1, tick3 - tick2, tick4 - tick3, tick5 - tick4);
#endif
}

///
///	Vaapi software deinterlace.
///
///	@param decoder	VA-API decoder
///	@param surface	interlaced hardware surface
///
static void VaapiCpuPut(VaapiDecoder * decoder, VASurfaceID surface)
{
    //
    //	vaPutImage working
    //
#ifdef DEBUG
    uint32_t tick1;
    uint32_t tick2;
    uint32_t tick3;
    uint32_t tick4;
    uint32_t tick5;
#endif
    VAImage *img1;
    VAImage *img2;
    VAImage *img3;
    VASurfaceID out;
    VAStatus status;

    //
    //	Create deinterlace images.
    //
    if (decoder->DeintImages[0].image_id == VA_INVALID_ID) {
	VaapiCreateDeinterlaceImages(decoder);
    }

    if (0 && vaSyncSurface(decoder->VaDisplay, surface) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }

    img1 = decoder->DeintImages;
    img2 = decoder->DeintImages + 1;
    img3 = decoder->DeintImages + 2;

#ifdef DEBUG
    tick1 = GetMsTicks();
#endif
    if (vaGetImage(decoder->VaDisplay, surface, 0, 0, decoder->InputWidth,
	    decoder->InputHeight, img1->image_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't get source image\n"));
	VaapiQueueSurface(decoder, surface, 0);
	VaapiQueueSurface(decoder, surface, 0);
	return;
    }
#ifdef DEBUG
    tick2 = GetMsTicks();
#endif

    // FIXME: handle top_field_first

    switch (VideoDeinterlace[decoder->Resolution]) {
	case VideoDeinterlaceSoftBob:
	default:
	    VaapiBob(decoder, img1, img2, img3);
	    break;
	case VideoDeinterlaceSoftSpatial:
	    VaapiSpatial(decoder, img1, img2, img3);
	    break;
    }
#ifdef DEBUG
    tick3 = GetMsTicks();
#endif

    // get a free surface and upload the image
    out = VaapiGetSurface0(decoder);
    if (out == VA_INVALID_ID) {
	abort();
    }
    if ((status =
	    vaPutImage(VaDisplay, out, img2->image_id, 0, 0, img2->width,
		img2->height, 0, 0, img2->width,
		img2->height)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't put image: %d!\n"), status);
	abort();
    }
    VaapiQueueSurface(decoder, out, 1);
    if (0 && vaSyncSurface(decoder->VaDisplay, out) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }
#ifdef DEBUG
    tick4 = GetMsTicks();

    Debug(4, "video/vaapi: deint %d %#010x -> %#010x\n", decoder->SurfaceField,
	surface, out);
#endif

    // get a free surface and upload the image
    out = VaapiGetSurface0(decoder);
    if (out == VA_INVALID_ID) {
	abort();
    }
    if (vaPutImage(VaDisplay, out, img3->image_id, 0, 0, img3->width,
	    img3->height, 0, 0, img3->width,
	    img3->height) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't put image!\n"));
    }
    VaapiQueueSurface(decoder, out, 1);
    if (0 && vaSyncSurface(decoder->VaDisplay, out) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }
#ifdef DEBUG
    tick5 = GetMsTicks();

    Debug(4, "video/vaapi: get=%2d deint=%2d put1=%2d put2=%2d\n",
	tick2 - tick1, tick3 - tick2, tick4 - tick3, tick5 - tick4);
#endif
}

///
///	Vaapi software deinterlace.
///
///	@param decoder	VA-API decoder
///	@param surface	interlaced hardware surface
///
static void VaapiCpuDeinterlace(VaapiDecoder * decoder, VASurfaceID surface)
{
    if (decoder->GetPutImage) {
	VaapiCpuPut(decoder, surface);
    } else {
	VaapiCpuDerive(decoder, surface);
    }
    // FIXME: must release software input surface
}

///
///	Render a ffmpeg frame
///
///	@param decoder		VA-API decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VaapiRenderFrame(VaapiDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    VASurfaceID surface;
    int interlaced;
    AVRational aspect_ratio;
    enum AVColorSpace color;

    // FIXME: some tv-stations toggle interlace on/off
    // frame->interlaced_frame isn't always correct set
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
    interlaced = frame->interlaced_frame;
#else
    interlaced = frame->flags & AV_FRAME_FLAG_INTERLACED;
#endif
    //25, 30 - interlaced; 50, 60 - progressive; hevc - progressive allways
    if (video_ctx->framerate.num > 0) {
	if (video_ctx->framerate.num / video_ctx->framerate.den > 30) interlaced = 0;
	else interlaced = 1;
    }

    if (video_ctx->codec_id == AV_CODEC_ID_HEVC) interlaced = 0;

    // FIXME: should be done by init video_ctx->field_order
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != frame->top_field_first) {

	Debug(4, "video/vaapi: interlaced %d top-field-first %d num %d den %d\n", frame->interlaced_frame,
	    frame->top_field_first, video_ctx->framerate.num, video_ctx->framerate.den);

	decoder->TopFieldFirst = frame->top_field_first;
#else
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)) {

	Debug(4, "video/vaapi: interlaced %d top-field-first %d num %d den %d\n", frame->flags & AV_FRAME_FLAG_INTERLACED,
	    frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST, video_ctx->framerate.num, video_ctx->framerate.den);

	decoder->TopFieldFirst = frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST;
#endif
	decoder->SurfaceField = !decoder->Interlaced;
	decoder->Interlaced = interlaced;
    }
    //aspect correction
    aspect_ratio = frame->sample_aspect_ratio;
    if ((video_ctx->width >= 352  && video_ctx->width <= 720 && video_ctx->height == 288) ||
	(video_ctx->width > 1300 && video_ctx->width <= 1920 && video_ctx->height == 540)) {
	aspect_ratio.den *= 2;
    }
    // update aspect ratio changes
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(53,60,100)
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, aspect_ratio)) {
	Debug(3, "video/vaapi: aspect ratio changed\n");

	decoder->InputAspect = aspect_ratio;
	VaapiUpdateOutput(decoder);
    }
#else
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, aspect_ratio)) {
	Debug(3, "video/vaapi: aspect ratio changed\n");

	decoder->InputAspect = aspect_ratio;
	VaapiUpdateOutput(decoder);
    }
#endif

    color = frame->colorspace;
    if (color == AVCOL_SPC_UNSPECIFIED)   // if unknown
        color = AVCOL_SPC_BT709;
#ifdef USE_EGL
    decoder->ColorSpace = color;     // save colorspace
#endif
    //
    // Hardware render
    //
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,74,100)
    if (video_ctx->hwaccel_context) {
#else
    if (video_ctx->hw_frames_ctx) {
#endif
	if (video_ctx->height != decoder->InputHeight
	    || video_ctx->width != decoder->InputWidth) {
	    Error(_("video/vaapi: stream <-> surface size mismatch\n"));
	    return;
	}

	surface = (unsigned)(size_t) frame->data[3];
	Debug(4, "video/vaapi: hw render hw surface %#010x\n", surface);

	if (decoder->Interlaced
	    && VideoDeinterlace[decoder->Resolution] >=
	    VideoDeinterlaceSoftBob) {
	    VaapiCpuDeinterlace(decoder, surface);
	} else {
	    VaapiQueueSurface(decoder, surface, 0);
	}

	//
	// VAImage render
	//
    } else {
	void *va_image_data;
	int i;
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(55,50,100)
	AVPicture picture[1];
#else
	AVFrame picture[1];
#endif
	int width;
	int height;

	Debug(4, "video/vaapi: hw render sw surface\n");

	width = video_ctx->width;
	height = video_ctx->height;
	//
	//	Check image, format, size
	//
	if ((decoder->GetPutImage && decoder->Image->image_id == VA_INVALID_ID)
	    || decoder->PixFmt != video_ctx->pix_fmt
	    || width != decoder->InputWidth
	    || height != decoder->InputHeight) {

	    Debug(3,
		"video/vaapi: stream <-> surface size/interlace mismatch\n");

	    decoder->PixFmt = video_ctx->pix_fmt;
	    // FIXME: aspect done above!
	    decoder->InputWidth = width;
	    decoder->InputHeight = height;

	    VaapiSetup(decoder, video_ctx);
	}
	// FIXME: Need to insert software deinterlace here
	// FIXME: can/must insert auto-crop here (is done after upload)

	// get a free surface and upload the image
	surface = VaapiGetSurface0(decoder);
	Debug(4, "video/vaapi: video surface %#010x displayed\n", surface);

	if (!decoder->GetPutImage
	    && vaDeriveImage(decoder->VaDisplay, surface,
		decoder->Image) != VA_STATUS_SUCCESS) {
	    VAImageFormat format[1];

	    Error(_("video/vaapi: vaDeriveImage failed\n"));

	    decoder->GetPutImage = 1;
	    VaapiFindImageFormat(decoder, decoder->PixFmt, format);
	    if (vaCreateImage(VaDisplay, format, width, height,
		    decoder->Image) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't create image!\n"));
	    }
	}
	//
	//	Copy data from frame to image
	//
	if (vaMapBuffer(VaDisplay, decoder->Image->buf, &va_image_data)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't map the image!\n"));
	}
	// crazy: intel mixes YV12 and NV12 with mpeg
	if (decoder->Image->format.fourcc == VA_FOURCC_NV12) {
	    int x;

	    // intel NV12 convert YV12 to NV12
            if (decoder->PixFmt != AV_PIX_FMT_YUV420P10LE) { //8bit
	        // copy Y
	        for (i = 0; i < height; ++i) {
		    memcpy(va_image_data + decoder->Image->offsets[0] +
		        decoder->Image->pitches[0] * i,
		        frame->data[0] + frame->linesize[0] * i,
		        frame->linesize[0]);
	        }
	        // copy UV
	        for (i = 0; i < height / 2; ++i) {
		    for (x = 0; x < width / 2; ++x) {
		        ((uint8_t *) va_image_data)[decoder->Image->offsets[1]
			    + decoder->Image->pitches[1] * i + x * 2 + 0]
			    = frame->data[1][i * frame->linesize[1] + x];
		        ((uint8_t *) va_image_data)[decoder->Image->offsets[1]
			    + decoder->Image->pitches[1] * i + x * 2 + 1]
			    = frame->data[2][i * frame->linesize[2] + x];
		    }
	        }
            } else { //10bit
	        // copy Y
	        for (i = 0; i < height; ++i) {
		    for (x = 0; x < width; ++x) {
		        ((uint8_t *) va_image_data)[decoder->Image->offsets[0]
			    + decoder->Image->pitches[0] * i + x]
			    = (*((uint16_t*)frame->data[0] + i * frame->linesize[0]/2 + x) + 2) >> 2;
		    }
	        }
	        // copy UV
	        for (i = 0; i < height / 2; ++i) {
		    for (x = 0; x < width / 2; ++x) {
		        ((uint8_t *) va_image_data)[decoder->Image->offsets[1]
			    + decoder->Image->pitches[1] * i + x * 2 + 0]
			    = (*((uint16_t*)frame->data[1] + i * frame->linesize[1]/2 + x) + 2) >> 2;
		        ((uint8_t *) va_image_data)[decoder->Image->offsets[1]
			    + decoder->Image->pitches[1] * i + x * 2 + 1]
			    = (*((uint16_t*)frame->data[2] + i * frame->linesize[2]/2 + x) + 2) >> 2;
		    }
	        }
            }
	    // vdpau uses this
	} else if (decoder->Image->format.fourcc == VA_FOURCC('I', '4', '2',
		'0')) {
	    picture->data[0] = va_image_data + decoder->Image->offsets[0];
	    picture->linesize[0] = decoder->Image->pitches[0];
	    picture->data[1] = va_image_data + decoder->Image->offsets[1];
	    picture->linesize[1] = decoder->Image->pitches[2];
	    picture->data[2] = va_image_data + decoder->Image->offsets[2];
	    picture->linesize[2] = decoder->Image->pitches[1];
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(51,63,100)
	    av_picture_copy(picture, (AVPicture *) frame, video_ctx->pix_fmt, width, height);
#else
	    av_image_copy(picture->data, picture->linesize, (const uint8_t **)frame->data, frame->linesize,
	    video_ctx->pix_fmt, width, height);
#endif
	} else if (decoder->Image->num_planes == 3) {
	    picture->data[0] = va_image_data + decoder->Image->offsets[0];
	    picture->linesize[0] = decoder->Image->pitches[0];
	    picture->data[1] = va_image_data + decoder->Image->offsets[2];
	    picture->linesize[1] = decoder->Image->pitches[2];
	    picture->data[2] = va_image_data + decoder->Image->offsets[1];
	    picture->linesize[2] = decoder->Image->pitches[1];
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(51,63,100)
	    av_picture_copy(picture, (AVPicture *) frame, video_ctx->pix_fmt, width, height);
#else
	    av_image_copy(picture->data, picture->linesize, (const uint8_t **)frame->data, frame->linesize,
	    video_ctx->pix_fmt, width, height);
#endif
	}

	if (vaUnmapBuffer(VaDisplay, decoder->Image->buf)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't unmap the image!\n"));
	}

	Debug(4, "video/vaapi: buffer %dx%d <- %dx%d\n", decoder->Image->width,
	    decoder->Image->height, width, height);

	if (decoder->GetPutImage
	    && (i =
		vaPutImage(VaDisplay, surface, decoder->Image->image_id, 0, 0,
		    width, height, 0, 0, width,
		    height)) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't put image err:%d!\n"), i);
	}

	if (!decoder->GetPutImage) {
	    if (vaDestroyImage(VaDisplay, decoder->Image->image_id)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't destroy image!\n"));
	    }
	    decoder->Image->image_id = VA_INVALID_ID;
	}

	VaapiQueueSurface(decoder, surface, 1);
    }

    if (decoder->Interlaced) {
	++decoder->FrameCounter;
    }
}

///
///	Get hwaccel context for ffmpeg.
///
///	@param decoder	VA-API hw decoder
///
static void *VaapiGetHwAccelContext(VaapiDecoder * decoder)
{
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,74,100)
    return decoder->VaapiContext;
#else
    (void)decoder;
    return NULL;
#endif
}

///
///	Advance displayed frame of decoder.
///
///	@param decoder	VA-API hw decoder
///
static void VaapiAdvanceDecoderFrame(VaapiDecoder * decoder)
{
    VASurfaceID surface;
    int filled;

    filled = atomic_read(&decoder->SurfacesFilled);
    // FIXME: this should check the caller
    // check decoder, if new surface is available
    if (filled <= 1) {
        // keep use of last surface
        ++decoder->FramesDuped;
        // FIXME: don't warn after stream start, don't warn during pause
        Debug(4,"video: display buffer empty, duping frame (%d/%d) %d\n",
	    decoder->FramesDuped, decoder->FrameCounter,
	    VideoGetBuffers(decoder->Stream));
        return;
    }
    // wait for rendering finished
    surface = decoder->SurfacesRb[decoder->SurfaceRead];
    if (vaSyncSurface(decoder->VaDisplay, surface) != VA_STATUS_SUCCESS) {
        Error(_("video/vaapi: vaSyncSurface failed\n"));
    }

    decoder->SurfaceRead = (decoder->SurfaceRead + 1) % VIDEO_SURFACES_MAX;
    atomic_dec(&decoder->SurfacesFilled);
}

///
///	Display a video frame.
///
///	@todo FIXME: add detection of missed frames
///
static void VaapiDisplayFrame(void)
{
    struct timespec nowtime;
    static int timeSS;

#ifdef DEBUG
    uint32_t start;
    uint32_t put1;
    uint32_t put2;
#endif
    int i;
    VaapiDecoder *decoder;

    if (VideoSurfaceModesChanged) {	// handle changed modes
	VideoSurfaceModesChanged = 0;
	for (i = 0; i < VaapiDecoderN; ++i) {
	    VaapiInitSurfaceFlags(VaapiDecoders[i]);
	}
    }
#ifdef USE_GLX
	if (GlxEnabled) {
	    if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext)) {
		Error(_("video/glx: can't make glx context current\n"));
		return;
	    }
	    glClear(GL_COLOR_BUFFER_BIT);
	}
#endif
#ifdef USE_EGL
	if (EglEnabled) {
	    if (!eglMakeCurrent(EglDisplay, EglSurface, EglSurface, EglThreadContext)) {
		Error(_("video/egl: can't make egl context current\n"));
		return;
	    }
	    glClear(GL_COLOR_BUFFER_BIT);
	}
#endif
#ifdef USE_SCREENSAVER
    //get up screensaver every 20 sec
    if (DisableScreensaver) {
	clock_gettime(CLOCK_MONOTONIC, &nowtime);
	if (nowtime.tv_sec - timeSS > 20) {
	    xcb_force_screen_saver(Connection,XCB_SCREEN_SAVER_RESET);
	    timeSS = nowtime.tv_sec;
	}
    }
#endif
    // look if any stream have a new surface available
    for (i = 0; i < VaapiDecoderN; ++i) {
	VASurfaceID surface;
	int filled;

	decoder = VaapiDecoders[i];
	decoder->FramesDisplayed++;
	decoder->StartCounter++;

#ifdef VA_EXP
	// wait for display finished
	if (decoder->LastSurface != VA_INVALID_ID) {
	    if (vaSyncSurface(decoder->VaDisplay, decoder->LastSurface)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaSyncSurface failed\n"));
	    }
	}
#endif
	filled = atomic_read(&decoder->SurfacesFilled);
	// no surface availble show black with possible osd
	if (filled <= 1 && decoder->Closing) {
	    if ((VideoShowBlackPicture && !decoder->TrickSpeed)
		|| (VideoShowBlackPicture && decoder->Closing < -300)) {
		VaapiBlackSurface(decoder);
#ifdef VA_EXP
		decoder->LastSurface = decoder->BlackSurface;
#endif
		VaapiMessage(4, "video/vaapi: black surface displayed\n");
#ifdef USE_SCREENSAVER
		if (EnableDPMSatBlackScreen && DPMSDisabled) {
		    Debug(3, "Black surface, DPMS enabled");
		    X11DPMSReenable(Connection);
		    X11SuspendScreenSaver(Connection, 1);
		}
#endif
	    }
	    if (VideoShowBlackPicture)
		continue;
#ifdef USE_SCREENSAVER
	} else if (!DPMSDisabled) {	// always disable
	    Debug(3, "DPMS disabled");
	    X11DPMSDisable(Connection);
	    X11SuspendScreenSaver(Connection, 0);
#endif
	}

	surface = decoder->SurfacesRb[decoder->SurfaceRead];
#ifdef VA_EXP
	decoder->LastSurface = surface;
#endif
	if (surface == VA_INVALID_ID) {
	    Debug(3,"video/vaapi: invalid surface in ringbuffer\n");
	    continue;
	}
	Debug(4, "video/vaapi: yy video surface %#010x displayed\n", surface);
#ifdef DEBUG
	start = GetMsTicks();
#endif

	// VDPAU driver + INTEL driver does no v-sync with 1080
	if (0 && decoder->Interlaced
	    // FIXME: buggy libva-driver-vdpau, buggy libva-driver-intel
	    && (VaapiBuggyVdpau || (0 && VaapiBuggyIntel
		    && decoder->InputHeight == 1080))
	    && VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceWeave) {
	    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		decoder->Deinterlaced, decoder->TopFieldFirst, 0);
#ifdef DEBUG
	    put1 = GetMsTicks();
#endif

	    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		decoder->Deinterlaced, decoder->TopFieldFirst, 1);
#ifdef DEBUG
	    put2 = GetMsTicks();
#endif
	} else {
#ifdef USE_GLX
	    if (GlxEnabled) {
		VaapiPutSurfaceGLX(decoder, surface, decoder->Interlaced,
		    decoder->Deinterlaced, decoder->TopFieldFirst, decoder->SurfaceField);
	    } else
#endif
#ifdef USE_EGL
	    if (EglEnabled) {
		VaapiPutSurfaceEGL(decoder, surface, decoder->Interlaced,
		    decoder->Deinterlaced, decoder->TopFieldFirst, decoder->SurfaceField);
	    } else
#endif
	    {
		VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		    decoder->Deinterlaced, decoder->TopFieldFirst, decoder->SurfaceField);
	    }
#ifdef DEBUG
	    put1 = GetMsTicks();
	    put2 = put1;
#endif
	}
	clock_gettime(CLOCK_MONOTONIC, &nowtime);
	// FIXME: 31 only correct for 50Hz
	if (!decoder->Closing && ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
	    * 1000 * 1000 * 1000 + (nowtime.tv_nsec -
		decoder->FrameTime.tv_nsec) > 31 * 1000 * 1000)) {
	    // FIXME: ignore still-frame, trick-speed
	    Debug(4, "video/vaapi: time/frame too long %ldms\n",
		((nowtime.tv_sec - decoder->FrameTime.tv_sec)
		    * 1000 * 1000 * 1000 + (nowtime.tv_nsec -
			decoder->FrameTime.tv_nsec)) / (1000 * 1000));
	    Debug(4, "video/vaapi: put1 %2u put2 %2u\n", put1 - start,
		put2 - put1);
	}
#ifdef noDEBUG
	Debug(3, "video/vaapi: time/frame %ldms\n",
	    ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
		* 1000 * 1000 * 1000 + (nowtime.tv_nsec -
		    decoder->FrameTime.tv_nsec)) / (1000 * 1000));
	if (put2 > start + 20) {
	    Debug(3, "video/vaapi: putsurface too long %ums\n", put2 - start);
	}
	Debug(4, "video/vaapi: put1 %2u put2 %2u\n", put1 - start,
	    put2 - put1);
#endif

	decoder->FrameTime = nowtime;
    }

#ifdef USE_GLX
    if (GlxEnabled) {
	//
	//	add OSD
	//
	if (OsdShown) {
                glViewport(0, 0, VideoWindowWidth, VideoWindowHeight);
                glMatrixMode(GL_PROJECTION);
                glLoadIdentity();
                glOrtho(0.0, VideoWindowWidth, VideoWindowHeight, 0.0, -1.0, 1.0);
                GlxCheck();
#ifdef USE_OPENGLOSD
            if(!DisableOglOsd && OsdGlTexture) {
                GlxRenderTexture(OsdGlTexture, 0,0, VideoWindowWidth, VideoWindowHeight);
            } else
#endif
	    GlxRenderTexture(OsdGlTextures[OsdIndex], 0, 0, VideoWindowWidth, VideoWindowHeight);
	    // FIXME: toggle osd
	}

	glXSwapBuffers(XlibDisplay, VideoWindow);
	glXMakeCurrent(XlibDisplay, None, NULL);
	GlxCheck();
    }
#endif
#ifdef USE_EGL
    if (EglEnabled) {
	//
	//	add OSD
	//
	if (OsdShown) {
            glViewport(0, 0, VideoWindowWidth, VideoWindowHeight);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, VideoWindowWidth, VideoWindowHeight, 0.0, -1.0, 1.0);
            EglCheck();
#ifdef USE_OPENGLOSD
            if(!DisableOglOsd && OsdGlTexture) {
                EglRenderTexture(OsdGlTexture, 0,0, VideoWindowWidth, VideoWindowHeight);
            } else
#endif
	    EglRenderTexture(OsdGlTextures[OsdIndex], 0, 0, VideoWindowWidth, VideoWindowHeight);
	    // FIXME: toggle osd
	}

	eglSwapBuffers(EglDisplay, EglSurface);
	EglCheck();
	eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	EglCheck();
    }
#endif
	xcb_flush(Connection);
}

///
///	Set VA-API decoder video clock.
///
///	@param decoder	VA-API hardware decoder
///	@param pts	audio presentation timestamp
///
void VaapiSetClock(VaapiDecoder * decoder, int64_t pts)
{
    decoder->PTS = pts;
}

///
///	Get VA-API decoder video clock.
///
///	@param decoder	VA-API decoder
///
static int64_t VaapiGetClock(const VaapiDecoder * decoder)
{
    // pts is the timestamp of the latest decoded frame
    if (decoder->PTS == (int64_t) AV_NOPTS_VALUE) {
	return AV_NOPTS_VALUE;
    }
    // subtract buffered decoded frames
    if (decoder->Interlaced) {
	return decoder->PTS -
	    20 * 90 * (2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField);
    }
    return decoder->PTS - 20 * 90 * (atomic_read(&decoder->SurfacesFilled) + 2);
}

///
///	Set VA-API decoder closing stream flag.
///
///	@param decoder	VA-API decoder
///
static void VaapiSetClosing(VaapiDecoder * decoder)
{
    decoder->Closing = 1;
}

///
///	Reset start of frame counter.
///
///	@param decoder	VA-API decoder
///
static void VaapiResetStart(VaapiDecoder * decoder)
{
    decoder->StartCounter = 0;
}

///
///	Set trick play speed.
///
///	@param decoder		VA-API decoder
///	@param speed		trick speed (0 = normal)
///
static void VaapiSetTrickSpeed(VaapiDecoder * decoder, int speed)
{
    decoder->TrickSpeed = speed;
    decoder->TrickCounter = speed;
    if (speed) {
	decoder->Closing = 0;
    }
}

///
///	Get VA-API decoder statistics.
///
///	@param decoder		VA-API decoder
///	@param[out] missed	missed frames
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void VaapiGetStats(VaapiDecoder * decoder, int *missed, int *duped,
    int *dropped, int *counter, int * dec)
{
    *missed = decoder->FramesMissed;
    *duped = decoder->FramesDuped;
    *dropped = decoder->FramesDropped;
    *counter = decoder->FrameCounter;
    *dec = (decoder->PixFmt == AV_PIX_FMT_NV12) ? HWACCEL_VAAPI : HWACCEL_NONE;
}

///
///	Sync decoder output to audio.
///
///	trick-speed	show frame <n> times
///	still-picture	show frame until new frame arrives
///	60hz-mode	repeat every 5th picture
///	video>audio	slow down video by duplicating frames
///	video<audio	speed up video by skipping frames
///	soft-start	show every second frame
///
///	@param decoder	VDPAU hw decoder
///
static void VaapiSyncDecoder(VaapiDecoder * decoder)
{
    int err;
    int64_t audio_clock;
    int64_t video_clock;

    err = 0;
    video_clock = VaapiGetClock(decoder);
    audio_clock = AudioGetClock();

    if (!decoder->SyncOnAudio) {
        audio_clock = AV_NOPTS_VALUE;
        // FIXME: 60Hz Mode
        goto skip_sync;
    }

    EnoughVideo = (VideoGetBuffers(decoder->Stream) >= (VideoResolution == VideoResolution576i ? VideoStartThreshold_SD * !!VideoSoftStartSync : 0));
    if (!EnoughVideo && EnoughAudio && !AudioRunning && decoder->StartCounter > 200) {
	Debug (3,"video: force enough video\n");
	EnoughVideo = 1;
    }
    if (EnoughVideo && EnoughAudio && !AudioRunning && audio_clock != (int64_t) AV_NOPTS_VALUE) {
	Debug(3, "video: start audio after waiting for enough video: SurfacesFilled: %d, PacketsFilled: %d\n", atomic_read(&decoder->SurfacesFilled), VideoGetBuffers(decoder->Stream));
	AudioStarted = 1;
	AudioRunning = 1;
	pthread_cond_signal(&AudioStartCond);
    }

    // 60Hz: repeat every 5th field
    if (Video60HzMode && !(decoder->FramesDisplayed % 6)) {
	if (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock == (int64_t) AV_NOPTS_VALUE) {
	    goto out;
	}
	// both clocks are known
	if (audio_clock + VideoAudioDelay <= video_clock + 25 * 90) {
	    goto out;
	}
	// out of sync: audio before video
	if (!decoder->TrickSpeed) {
	    goto skip_sync;
	}
    }
    // TrickSpeed
    if (decoder->TrickSpeed) {
	if (decoder->TrickCounter--) {
	    goto out;
	}
	decoder->TrickCounter = decoder->TrickSpeed;
	goto skip_sync;
    }
    // at start of new video stream, soft or hard sync video to audio
    // FIXME: video waits for audio, audio for video
    if (!IsReplay() && VideoSoftStartSync < 2 && decoder->StartCounter < VideoSoftStartFrames
	&& video_clock != (int64_t) AV_NOPTS_VALUE
	&& (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock > audio_clock + VideoAudioDelay + 120 * 90)) {
	err =
	    VaapiMessage(3, "video: initial slow down video, frame %d\n",
	    decoder->StartCounter);
	goto out;
    }

    if (decoder->SyncCounter && decoder->SyncCounter-- && VideoSoftStartSync > 1) {
	goto skip_sync;
    }

    if (audio_clock != (int64_t) AV_NOPTS_VALUE
	&& video_clock != (int64_t) AV_NOPTS_VALUE) {
	// both clocks are known
	int diff;
	int lower_limit;
	if (IsReplay()) {
	    if((video_clock > audio_clock + VideoAudioDelay + 8000 * 90) && decoder->StartCounter < VideoSoftStartFrames) { // 8s
		Debug(3,"flush audio\n");
		AudioFlushBuffers();
		goto out;
	    }
	    if((video_clock < audio_clock + VideoAudioDelay - 120 * 90) && VideoSoftStartSync < 2) {
		if (atomic_read(&decoder->SurfacesFilled) > 1) {
		    Debug(3,"drop video\n");
		    VaapiAdvanceDecoderFrame(decoder);
		}
		goto skip_sync;
	    }
	}
	diff = video_clock - audio_clock - VideoAudioDelay;
	lower_limit = !IsReplay() ? -25 - (VideoResolution == VideoResolution576i ? 40 : 0) : 12;
	//diff = (decoder->LastAVDiff + diff) / 2;
	decoder->LastAVDiff = diff;
#ifdef DEBUG
	if (!decoder->SyncCounter && decoder->StartCounter < 1000)
	    Debug(3, "video/vaapi: diff %d %d lim %d fill %d\n", diff, diff/90, lower_limit, atomic_read(&decoder->SurfacesFilled));
#endif
	if (abs(diff) > 8000 * 90) {	// more than 8s
	    err = VaapiMessage(3, "video: audio/video difference too big\n");
	}
	if (diff > 100 * 90) {
	    // FIXME: this quicker sync step, did not work with new code!
	    err = VaapiMessage(3, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    if (VideoSoftStartSync > 1) {
		decoder->SyncCounter = diff > 100 * 90 ? diff % 2 : 1; //softsync :)
	    }
		goto out;
	} else if (diff > 55 * 90) {
	    err = VaapiMessage(3, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    decoder->SyncCounter = 1;
	    goto out;
	} else if (diff < lower_limit * 90 && atomic_read(&decoder->SurfacesFilled) > 2) { // double advance possible?
	    err = VaapiMessage(3, "video: speed up video, droping frame\n");
	    ++decoder->FramesDropped;
	    VaapiAdvanceDecoderFrame(decoder);
	    decoder->SyncCounter = 2;
	} else if (diff < lower_limit * 90 && atomic_read(&decoder->SurfacesFilled) <= 1 && !IsReplay()) {
	    err = VaapiMessage(3, "video: speed up audio, delay audio\n");
	    AudioDelayms(-diff / 90 + 55);
	}
#if defined(DEBUG) || defined(AV_INFO)
	if (!decoder->SyncCounter && decoder->StartCounter < 1000) {
#ifdef DEBUG
	    Debug(3, "video/vaapi: synced after %d frames %dms\n",
		decoder->StartCounter, GetMsTicks() - VideoSwitch);
#else
	    Info("video/vaapi: synced after %d frames\n",
		decoder->StartCounter);
#endif
	    decoder->StartCounter += 1000;
	}
#endif
    }

  skip_sync:
    // is it not possible, to advance the surface and/or the field?
    if (atomic_read(&decoder->SurfacesFilled) <= 1) {
	++decoder->FramesDuped;
	// FIXME: don't warn after stream start, don't warn during pause
	err =
	    VaapiMessage(3,
	    _("video: decoder buffer empty, "
		"duping frame (%d/%d) %d v-buf\n"), decoder->FramesDuped,
	    decoder->FrameCounter, VideoGetBuffers(decoder->Stream));
	// some time no new picture or black video configured
	if (decoder->Closing < -300 || (VideoShowBlackPicture
	    && decoder->Closing)) {
	    // clear ring buffer to trigger black picture
	    atomic_set(&decoder->SurfacesFilled, 0);
	}
	goto out;
    }

    VaapiAdvanceDecoderFrame(decoder);
  out:
#if defined(DEBUG) || defined(AV_INFO)
    // debug audio/video sync
    if (err || !(decoder->FramesDisplayed % AV_INFO_TIME)) {
	if (!err) {
	    VaapiMessage(0, NULL);
	}
	Info("video: %s%+5" PRId64 " %4" PRId64 " %3d/\\ms %3d%+d v-buf\n",
	    Timestamp2String(video_clock),
	    abs((int)(video_clock - audio_clock) / 90) <
	    8888 ? ((video_clock - audio_clock) / 90) : 8888,
	    AudioGetDelay() / 90, (int)VideoDeltaPTS / 90,
	    VideoGetBuffers(decoder->Stream),
	    decoder->Interlaced ? (2 * atomic_read(&decoder->SurfacesFilled)
		- decoder->SurfaceField)
	    : atomic_read(&decoder->SurfacesFilled));
	if (!(decoder->FramesDisplayed % (5 * 60 * 60))) {
	    VaapiPrintFrames(decoder);
	}
    }
#endif
    return;				// fix gcc bug!
}

///
///	Sync a video frame.
///
static void VaapiSyncFrame(void)
{
    int i;

    //
    //	Sync video decoder to audio
    //
    for (i = 0; i < VaapiDecoderN; ++i) {
	VaapiSyncDecoder(VaapiDecoders[i]);
    }
}

///
///	Sync and display surface.
///
static void VaapiSyncDisplayFrame(void)
{
    VaapiDisplayFrame();
    VaapiSyncFrame();
}

///
///	Sync and render a ffmpeg frame
///
///	@param decoder		VA-API decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VaapiSyncRenderFrame(VaapiDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
#ifdef DEBUG
    if (!atomic_read(&decoder->SurfacesFilled)) {
	Debug(3, "video: new stream frame %dms\n", GetMsTicks() - VideoSwitch);
    }
#endif

#if 1
#ifndef USE_PIP
#error	"-DUSE_PIP or #define USE_PIP is needed,"
#endif
    // if video output buffer is full, wait and display surface.
    // loop for interlace
    if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX - 1) {
	Info("video/vaapi: this code part shouldn't be used\n");
	return;
    }
#else
    // FIXME: this part code should be no longer be needed with new mpeg fix
    while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX - 1) {
	struct timespec abstime;

	pthread_mutex_unlock(&VideoLockMutex);

	abstime = decoder->FrameTime;
	abstime.tv_nsec += 14 * 1000 * 1000;
	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
	    // avoid overflow
	    abstime.tv_sec++;
	    abstime.tv_nsec -= 1000 * 1000 * 1000;
	}

	VideoPollEvent();

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_mutex_lock(&VideoLockMutex);
	// give osd some time slot
	while (pthread_cond_timedwait(&VideoWakeupCond, &VideoLockMutex,
		&abstime) != ETIMEDOUT) {
	    // SIGUSR1
	    Debug(3, "video/vaapi: pthread_cond_timedwait error\n");
	}
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	VaapiSyncDisplayFrame();
    }
#endif

    if (!decoder->Closing) {
	VideoSetPts(&decoder->PTS, decoder->Interlaced, video_ctx, frame);
    }
    VaapiRenderFrame(decoder, video_ctx, frame);
#ifdef USE_AUTOCROP
    VaapiCheckAutoCrop(decoder);
#endif
}

///
///	Set VA-API background color.
///
///	@param rgba	32 bit RGBA color.
///
static void VaapiSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
    Error(_("video/vaapi: FIXME: SetBackground not supported\n"));
}

///
///     Set VA-API video output position.
///
///     @param decoder  VA-API decoder
///     @param x        video output x coordinate inside the window
///     @param y        video output y coordinate inside the window
///     @param width    video output width
///     @param height   video output height
///
static void VaapiSetOutputPosition(VaapiDecoder * decoder, int x, int y,
    int width, int height)
{
    Debug(3, "video/vaapi: output %dx%d%+d%+d\n", width, height, x, y);

    decoder->VideoX = x;
    decoder->VideoY = y;
    decoder->VideoWidth = width;
    decoder->VideoHeight = height;
}

///
///	Set VA-API video mode.
///
static void VaapiSetVideoMode(void)
{
    int i;

    for (i = 0; i < VaapiDecoderN; ++i) {
	// reset video window, upper level needs to fix the positions
	VaapiDecoders[i]->VideoX = 0;
	VaapiDecoders[i]->VideoY = 0;
	VaapiDecoders[i]->VideoWidth = VideoWindowWidth;
	VaapiDecoders[i]->VideoHeight = VideoWindowHeight;
	// for window mode need restore scale
	if (VaapiDecoders[i]->ScaleX || VaapiDecoders[i]->ScaleY
	 || VaapiDecoders[i]->ScaleWidth || VaapiDecoders[i]->ScaleHeight) {
	    // convert OSD coordinates to window coordinates
	    int x = (VaapiDecoders[i]->ScaleX * VideoWindowWidth) / OsdWidth;
	    int width = (VaapiDecoders[i]->ScaleWidth * VideoWindowWidth) / OsdWidth;
	    int y = (VaapiDecoders[i]->ScaleY * VideoWindowHeight) / OsdHeight;
	    int height = (VaapiDecoders[i]->ScaleHeight * VideoWindowHeight) / OsdHeight;
	    VaapiSetOutputPosition(VaapiDecoders[i], x, y, width, height);
	}
	VaapiUpdateOutput(VaapiDecoders[i]);
    }
}

#ifdef USE_VIDEO_THREAD

///
///	Handle a va-api display.
///
static void VaapiDisplayHandlerThread(void)
{
    int i;
    int err;
    int allfull;
    int decoded;
    struct timespec nowtime;
    VaapiDecoder *decoder;

    allfull = 1;
    decoded = 0;
    pthread_mutex_lock(&VideoLockMutex);
    for (i = 0; i < VaapiDecoderN; ++i) {
	int filled;

	decoder = VaapiDecoders[i];

	//
	// fill frame output ring buffer
	//
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled < VIDEO_SURFACES_MAX - 1) {
	    // FIXME: hot polling
	    // fetch+decode or reopen
	    allfull = 0;
	    err = VideoDecodeInput(decoder->Stream);
	} else {
	    err = VideoPollInput(decoder->Stream);
	}
	// decoder can be invalid here
	if (err) {
	    // nothing buffered?
	    if (err == -1 && decoder->Closing) {
		decoder->Closing--;
		if (!decoder->Closing) {
		    Debug(3, "video/vaapi: closing eof\n");
		    decoder->Closing = -1;
		}
	    }
	    continue;
	}
	decoded = 1;
    }
    pthread_mutex_unlock(&VideoLockMutex);

    if (!decoded) {			// nothing decoded, sleep
	// FIXME: sleep on wakeup
	usleep(1 * 1000);
    }
    // all decoder buffers are full
    // speed up filling display queue, wait on display queue empty
    if (!allfull) {
	clock_gettime(CLOCK_MONOTONIC, &nowtime);
	// time for one frame over?
	if ((nowtime.tv_sec -
		VaapiDecoders[0]->FrameTime.tv_sec) * 1000 * 1000 * 1000 +
	    (nowtime.tv_nsec - VaapiDecoders[0]->FrameTime.tv_nsec) <
	    15 * 1000 * 1000) {
	    return;
	}
    }

    pthread_mutex_lock(&VideoLockMutex);
    VaapiSyncDisplayFrame();
    pthread_mutex_unlock(&VideoLockMutex);
}

#else

#define VaapiDisplayHandlerThread	NULL

#endif

//----------------------------------------------------------------------------
//	VA-API OSD
//----------------------------------------------------------------------------

///
///	Clear subpicture image.
///
///	@note looked by caller
///
static void VaapiOsdClear(void)
{
    void *image_buffer;

    // osd image available?
    if (VaOsdImage.image_id == VA_INVALID_ID) {
	return;
    }

    Debug(3, "video/vaapi: clear image\n");

    if (VaOsdImage.width < OsdDirtyWidth + OsdDirtyX || VaOsdImage.height < OsdDirtyHeight + OsdDirtyY) {
	Debug(3, "video/vaapi: OSD dirty area will not fit\n");
    }
    if (VaOsdImage.width < OsdDirtyX || VaOsdImage.height < OsdDirtyY)
	return;

    if (VaOsdImage.width < OsdDirtyWidth + OsdDirtyX)
	OsdDirtyWidth = VaOsdImage.width - OsdDirtyX;
    if (VaOsdImage.height < OsdDirtyHeight + OsdDirtyY)
	OsdDirtyHeight = VaOsdImage.height - OsdDirtyY;

    // map osd surface/image into memory.
    if (vaMapBuffer(VaDisplay, VaOsdImage.buf, &image_buffer)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't map osd image buffer\n"));
	return;
    }
    // have dirty area.
    if (OsdDirtyWidth && OsdDirtyHeight) {
	int o;

	for (o = 0; o < OsdDirtyHeight; ++o) {
	    memset(image_buffer + OsdDirtyX * 4 + (o +
			OsdDirtyY) * VaOsdImage.pitches[0], 0x00,
		OsdDirtyWidth * 4);
	}
    } else {
	// 100% transparent
	memset(image_buffer, 0x00, VaOsdImage.data_size);
    }

    if (vaUnmapBuffer(VaDisplay, VaOsdImage.buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap osd image buffer\n"));
    }
}

///
///	Upload ARGB to subpicture image.
///
///	@param xi	x-coordinate in argb image
///	@param yi	y-coordinate in argb image
///	@paran height	height in pixel in argb image
///	@paran width	width in pixel in argb image
///	@param pitch	pitch of argb image
///	@param argb	32bit ARGB image data
///	@param x	x-coordinate on screen of argb image
///	@param y	y-coordinate on screen of argb image
///
///	@note looked by caller
///
static void VaapiOsdDrawARGB(int xi, int yi, int width, int height, int pitch,
    const uint8_t * argb, int x, int y)
{
#ifdef DEBUG
    uint32_t start;
    uint32_t end;
#endif
    void *image_buffer;
    int o;
    int copywidth, copyheight;

    // osd image available?
    if (VaOsdImage.image_id == VA_INVALID_ID) {
	return;
    }

    if (VaOsdImage.width < width + x || VaOsdImage.height < height + y) {
	Error("video/vaapi: OSD will not fit (w: %d+%d, w-avail: %d, h: %d+%d, h-avail: %d\n",
	      width, x, VaOsdImage.width, height, y, VaOsdImage.height);
    }
    if (VaOsdImage.width < x || VaOsdImage.height < y)
	return;

    copywidth = width;
    copyheight = height;
    if (VaOsdImage.width < width + x)
	copywidth = VaOsdImage.width - x;
    if (VaOsdImage.height < height + y)
	copyheight = VaOsdImage.height - y;

#ifdef DEBUG
    start = GetMsTicks();
#endif
    // map osd surface/image into memory.
    if (vaMapBuffer(VaDisplay, VaOsdImage.buf, &image_buffer)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't map osd image buffer\n"));
	return;
    }
    // FIXME: convert image from ARGB to subpicture format, if not argb

    // copy argb to image
    for (o = 0; o < copyheight; ++o) {
	memcpy(image_buffer + x * 4 + (y + o) * VaOsdImage.pitches[0],
	    argb + xi * 4 + (o + yi) * pitch, copywidth * 4);
    }

    if (vaUnmapBuffer(VaDisplay, VaOsdImage.buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap osd image buffer\n"));
    }
#ifdef DEBUG
    end = GetMsTicks();

    Debug(3, "video/vaapi: osd upload %dx%d%+d%+d %dms %d\n", width, height, x,
	y, end - start, width * height * 4);
#endif
}

///
///	VA-API initialize OSD.
///
///	@param width	osd width
///	@param height	osd height
///
///	@note subpicture is unusable, it can be scaled with the video image.
///
static void VaapiOsdInit(int width, int height)
{
    VAImageFormat *formats;
    unsigned *flags;
    unsigned format_n;
    unsigned u;
    unsigned v;
    int i;
    static uint32_t wanted_formats[] =
#if VA_CHECK_VERSION(1,0,0)
	{ VA_FOURCC_BGRA, VA_FOURCC_RGBA };
#else
	{ VA_FOURCC('B', 'G', 'R', 'A'), VA_FOURCC_RGBA };
#endif

    if (VaOsdImage.image_id != VA_INVALID_ID) {
	Debug(3, "video/vaapi: osd already setup\n");
	return;
    }
    if (!VaDisplay) {
	Debug(3, "video/vaapi: va-api not setup\n");
	return;
    }
    //
    //	look through subpicture formats
    //
    format_n = vaMaxNumSubpictureFormats(VaDisplay);
    formats = alloca(format_n * sizeof(*formats));
    flags = alloca(format_n * sizeof(*flags));
    if (vaQuerySubpictureFormats(VaDisplay, formats, flags,
	    &format_n) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't get subpicture formats"));
	return;
    }
#ifdef DEBUG
    Debug(3, "video/vaapi: supported subpicture formats:\n");
    for (u = 0; u < format_n; ++u) {
	Debug(3, "video/vaapi:\t%c%c%c%c flags %#x %s - %#x\n", formats[u].fourcc,
	    formats[u].fourcc >> 8, formats[u].fourcc >> 16,
	    formats[u].fourcc >> 24, flags[u],
	    flags[u] & VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD ?
	    "screen coord" : "", formats[u].fourcc);
    }
#endif
    for (v = 0; v < sizeof(wanted_formats) / sizeof(*wanted_formats); ++v) {
	for (u = 0; u < format_n; ++u) {
	    if (formats[u].fourcc == wanted_formats[v]) {
		goto found;
	    }
	}
    }
    Error(_("video/vaapi: can't find a supported subpicture format"));
    return;

  found:
    Debug(3, "video/vaapi: use %c%c%c%c subpicture format with flags %#x\n",
	formats[u].fourcc, formats[u].fourcc >> 8, formats[u].fourcc >> 16,
	formats[u].fourcc >> 24, flags[u]);

    VaapiUnscaledOsd = 0;
    if (flags[u] & VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD) {
	Info(_("video/vaapi: supports unscaled osd\n"));
	VaapiUnscaledOsd = 1;
    }
    //VaapiUnscaledOsd = 0;
    //Info(_("video/vaapi: unscaled osd disabled\n"));

    if (vaCreateImage(VaDisplay, &formats[u], width, height,
	    &VaOsdImage) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create osd image\n"));
	return;
    }
    if (vaCreateSubpicture(VaDisplay, VaOsdImage.image_id,
	    &VaOsdSubpicture) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create subpicture\n"));

	if (vaDestroyImage(VaDisplay,
		VaOsdImage.image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	VaOsdImage.image_id = VA_INVALID_ID;

	return;
    }
    // FIXME: must store format, to convert ARGB to it.

    // restore osd association
    for (i = 0; i < VaapiDecoderN; ++i) {
	// only if input already setup
	if (VaapiDecoders[i]->InputWidth && VaapiDecoders[i]->InputHeight) {
	    VaapiAssociate(VaapiDecoders[i]);
	}
    }
}

///
///	VA-API cleanup osd.
///
static void VaapiOsdExit(void)
{
    if (VaOsdImage.image_id != VA_INVALID_ID) {
	if (vaDestroyImage(VaDisplay,
		VaOsdImage.image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	VaOsdImage.image_id = VA_INVALID_ID;
    }

    if (VaOsdSubpicture != VA_INVALID_ID) {
	int i;

	for (i = 0; i < VaapiDecoderN; ++i) {
	    VaapiDeassociate(VaapiDecoders[i]);
	}

	if (vaDestroySubpicture(VaDisplay, VaOsdSubpicture)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy subpicture\n"));
	}
	VaOsdSubpicture = VA_INVALID_ID;
    }
}

static int VaapiMaxPixmapSize (void)
{
	return MAX_PIXMAP_SIZE_VAAPI;
}

///
///	VA-API module.
///
static const VideoModule VaapiModule = {
    .Name = "va-api",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) VaapiNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))VaapiDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))VaapiGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))VaapiReleaseSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Vaapi_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))VaapiSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	VaapiGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))VaapiSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))VaapiGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))VaapiSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))VaapiResetStart,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))VaapiSetTrickSpeed,
    .GrabOutput = VaapiGrabOutputSurface,
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, int *))VaapiGetStats,
    .SetBackground = VaapiSetBackground,
    .SetVideoMode = VaapiSetVideoMode,
#ifdef USE_AUTOCROP
    .ResetAutoCrop = VaapiResetAutoCrop,
#endif
    .DisplayHandlerThread = VaapiDisplayHandlerThread,
    .OsdClear = VaapiOsdClear,
    .OsdDrawARGB = VaapiOsdDrawARGB,
    .OsdInit = VaapiOsdInit,
    .OsdExit = VaapiOsdExit,
    .MaxPixmapSize = VaapiMaxPixmapSize,
    .Init = VaapiInit,
    .Exit = VaapiExit,
};

#ifdef USE_GLX

#ifdef USE_OPENGLOSD
void *GetVaapiGlxOsdOutputTexture(GLuint texture) {
    OsdGlTexture = texture;
    return 0;
}

int VaapiInitGlx(void) {
    int a = 0;

    if (!GlxEnabled) {
        Debug(3,"video/osd: can't create glx context\n");
        return 0;
    }
    //after run an external player from time to time vdr not set playmode 1
    //then try start GLX forced.
    //is it a vdr bug or external player plugin???
    while (!GlxContext || !GlxThreadContext){
        usleep(1000);
        a++;
        if (a > 10 && a < 1000) {
            Debug(3,"Try start GLX forced\n");
            VideoDisplayWakeup();
            a = 1000;
        }
        if (a > 1010) return 0;
    }
    Debug(3,"Create OSD GLX context\n");
    glXMakeCurrent(XlibDisplay, None, NULL);
    glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext);
    return 1;
}
#endif

///
///	VA-API module.
///
static const VideoModule VaapiGlxModule = {
    .Name = "va-api-glx",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) VaapiNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))VaapiDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))VaapiGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))VaapiReleaseSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Vaapi_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))VaapiSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	VaapiGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))VaapiSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))VaapiGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))VaapiSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))VaapiResetStart,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))VaapiSetTrickSpeed,
    .GrabOutput = VaapiGrabOutputSurface,
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, int *))VaapiGetStats,
    .SetBackground = VaapiSetBackground,
    .SetVideoMode = VaapiSetVideoMode,
#ifdef USE_AUTOCROP
    .ResetAutoCrop = VaapiResetAutoCrop,
#endif
    .DisplayHandlerThread = VaapiDisplayHandlerThread,
    .OsdClear = GlxOsdClear,
    .OsdDrawARGB = GlxOsdDrawARGB,
    .OsdInit = GlxOsdInit,
    .OsdExit = GlxOsdExit,
    .MaxPixmapSize = GlxMaxPixmapSize,
    .Init = VaapiGlxInit,
    .Exit = VaapiExit,
};

#endif


#ifdef USE_EGL

#ifdef USE_OPENGLOSD
void *GetVaapiEglOsdOutputTexture(GLuint texture) {
    OsdGlTexture = texture;
    return 0;
}

int VaapiInitEgl(void) {
    int a = 0;

    if (!EglEnabled) {
        Debug(3,"video/osd: can't create egl context\n");
        return 0;
    }
    //after run an external player from time to time vdr not set playmode 1
    //then try start EGL forced.
    //is it a vdr bug or external player plugin???
    while (!EglContext || !EglThreadContext){
        usleep(1000);
        a++;
        if (a > 10 && a < 1000) {
            Debug(3,"Try start EGL forced\n");
            VideoDisplayWakeup();
            a = 1000;
        }
        if (a > 1010) return 0;
    }
    Debug(3,"Create OSD EGL context\n");
    eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglContext);
    return 1;
}
#endif

///
///	VA-API module.
///
static const VideoModule VaapiEglModule = {
    .Name = "va-api-egl",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) VaapiNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))VaapiDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))VaapiGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))VaapiReleaseSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Vaapi_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))VaapiSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	VaapiGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))VaapiSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))VaapiGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))VaapiSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))VaapiResetStart,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))VaapiSetTrickSpeed,
    .GrabOutput = VaapiGrabOutputSurface,
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, int *))VaapiGetStats,
    .SetBackground = VaapiSetBackground,
    .SetVideoMode = VaapiSetVideoMode,
#ifdef USE_AUTOCROP
    .ResetAutoCrop = VaapiResetAutoCrop,
#endif
    .DisplayHandlerThread = VaapiDisplayHandlerThread,
    .OsdClear = EglOsdClear,
    .OsdDrawARGB = EglOsdDrawARGB,
    .OsdInit = EglOsdInit,
    .OsdExit = EglOsdExit,
    .MaxPixmapSize = EglMaxPixmapSize,
    .Init = VaapiEglInit,
    .Exit = VaapiExit,
};

#endif

#endif //VAAPI

//----------------------------------------------------------------------------
//	VDPAU
//----------------------------------------------------------------------------

#ifdef USE_VDPAU

///
///	VDPAU decoder
///
typedef struct _vdpau_decoder_
{
    VdpDevice Device;			///< VDPAU device

    xcb_window_t Window;		///< output window

    int VideoX;				///< video base x coordinate
    int VideoY;				///< video base y coordinate
    int VideoWidth;			///< video base width
    int VideoHeight;			///< video base height

    int ScaleX;				///< video scale x coordinate
    int ScaleY;				///< video scale y coordinate
    int ScaleWidth;			///< video scale width
    int ScaleHeight;			///< video scale height

    int OutputX;			///< real video output x coordinate
    int OutputY;			///< real video output y coordinate
    int OutputWidth;			///< real video output width
    int OutputHeight;			///< real video output height

    enum AVPixelFormat PixFmt;		///< ffmpeg frame pixfmt
    int Interlaced;			///< ffmpeg interlaced flag
    int TopFieldFirst;			///< ffmpeg top field displayed first

    int InputWidth;			///< video input width
    int InputHeight;			///< video input height
    AVRational InputAspect;		///< video input aspect ratio
    VideoResolutions Resolution;	///< resolution group

    int CropX;				///< video crop x
    int CropY;				///< video crop y
    int CropWidth;			///< video crop width
    int CropHeight;			///< video crop height

#ifdef USE_AUTOCROP
    void *AutoCropBuffer;		///< auto-crop buffer cache
    unsigned AutoCropBufferSize;	///< auto-crop buffer size
    AutoCropCtx AutoCrop[1];		///< auto-crop variables
#endif
    VdpDecoderProfile Profile;		///< vdp decoder profile
    VdpDecoder VideoDecoder;		///< vdp video decoder
    VdpVideoMixer VideoMixer;		///< vdp video mixer
    VdpChromaType ChromaType;		///< vdp video surface chroma format
    VdpProcamp Procamp;			///< vdp procamp parameterization data

    int SurfacesNeeded;			///< number of surface to request
    int SurfaceUsedN;			///< number of used video surfaces
    /// used video surface ids
    VdpVideoSurface SurfacesUsed[CODEC_SURFACES_MAX];
    int SurfaceFreeN;			///< number of free video surfaces
    /// free video surface ids
    VdpVideoSurface SurfacesFree[CODEC_SURFACES_MAX];

    /// video surface ring buffer
    VdpVideoSurface SurfacesRb[VIDEO_SURFACES_MAX];
    int SurfaceWrite;			///< write pointer
    int SurfaceRead;			///< read pointer
    atomic_t SurfacesFilled;		///< how many of the buffer is used

    int SurfaceField;			///< current displayed field
    int TrickSpeed;			///< current trick speed
    int TrickCounter;			///< current trick speed counter
    struct timespec FrameTime;		///< time of last display
    VideoStream *Stream;		///< video stream
    int Closing;			///< flag about closing current stream
    int SyncOnAudio;			///< flag sync to audio
    int64_t PTS;			///< video PTS clock

    int LastAVDiff;			///< last audio - video difference
    int SyncCounter;			///< counter to sync frames
    int StartCounter;			///< counter for video start
    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
} VdpauDecoder;

static volatile char VdpauPreemption;	///< flag preemption happened.

static VdpauDecoder *VdpauDecoders[2];	///< open decoder streams
static int VdpauDecoderN;		///< number of decoder streams

static VdpDevice VdpauDevice;		///< VDPAU device
static VdpGetProcAddress *VdpauGetProcAddress;	///< entry point to use

    /// presentation queue target
static VdpPresentationQueueTarget VdpauQueueTarget;
static VdpPresentationQueue VdpauQueue;	///< presentation queue
static VdpColor VdpauQueueBackgroundColor[1];	///< queue background color

static int VdpauBackground;		///< background supported
static int VdpauHqScalingMax;		///< highest supported scaling level
static int VdpauTemporal;		///< temporal deinterlacer supported
static int VdpauTemporalSpatial;	///< temporal spatial deint. supported
static int VdpauInverseTelecine;	///< inverse telecine deint. supported
static int VdpauNoiseReduction;		///< noise reduction supported
static int VdpauSharpness;		///< sharpness supported
static int VdpauSkipChroma;		///< skip chroma deint. supported
static VdpChromaType VdpauChromaType;	///< best video surface chroma format

    /// display surface ring buffer
static VdpOutputSurface VdpauSurfacesRb[OUTPUT_SURFACES_MAX];
static int VdpauSurfaceIndex;		///< current display surface
static int VdpauSurfaceQueued;		///< number of display surfaces queued
static struct timespec VdpauFrameTime;	///< time of last display
#ifdef USE_GLX
GLuint GlTextures[OUTPUT_SURFACES_MAX][2];		///< gl texture for VDPAU
static GLvdpauSurfaceNV VdpauOutputSurfacesNV[OUTPUT_SURFACES_MAX];
#endif

#ifdef USE_BITMAP
    /// bitmap surfaces for osd
static VdpBitmapSurface VdpauOsdBitmapSurface[2] = {
    VDP_INVALID_HANDLE, VDP_INVALID_HANDLE
};
#else
    /// output surfaces for osd
static VdpOutputSurface VdpauOsdOutputSurface[2] = {
    VDP_INVALID_HANDLE, VDP_INVALID_HANDLE
};
#endif
static int VdpauOsdSurfaceIndex;	///< index into double buffered osd

    /// grab render output surface
static VdpOutputSurface VdpauGrabRenderSurface = VDP_INVALID_HANDLE;
static pthread_mutex_t VdpauGrabMutex;

///
///	Function pointer of the VDPAU device.
///
///@{
static VdpGetErrorString *VdpauGetErrorString;
static VdpDeviceDestroy *VdpauDeviceDestroy;
static VdpGenerateCSCMatrix *VdpauGenerateCSCMatrix;
static VdpVideoSurfaceQueryCapabilities *VdpauVideoSurfaceQueryCapabilities;
static VdpVideoSurfaceQueryGetPutBitsYCbCrCapabilities *
    VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities;
static VdpVideoSurfaceCreate *VdpauVideoSurfaceCreate;
static VdpVideoSurfaceDestroy *VdpauVideoSurfaceDestroy;
static VdpVideoSurfaceGetParameters *VdpauVideoSurfaceGetParameters;
static VdpVideoSurfaceGetBitsYCbCr *VdpauVideoSurfaceGetBitsYCbCr;
static VdpVideoSurfacePutBitsYCbCr *VdpauVideoSurfacePutBitsYCbCr;

static VdpOutputSurfaceQueryCapabilities *VdpauOutputSurfaceQueryCapabilities;

static VdpOutputSurfaceCreate *VdpauOutputSurfaceCreate;
static VdpOutputSurfaceDestroy *VdpauOutputSurfaceDestroy;
static VdpOutputSurfaceGetParameters *VdpauOutputSurfaceGetParameters;
static VdpOutputSurfaceGetBitsNative *VdpauOutputSurfaceGetBitsNative;
static VdpOutputSurfacePutBitsNative *VdpauOutputSurfacePutBitsNative;

static VdpBitmapSurfaceQueryCapabilities *VdpauBitmapSurfaceQueryCapabilities;
static VdpBitmapSurfaceCreate *VdpauBitmapSurfaceCreate;
static VdpBitmapSurfaceDestroy *VdpauBitmapSurfaceDestroy;

static VdpBitmapSurfacePutBitsNative *VdpauBitmapSurfacePutBitsNative;

static VdpOutputSurfaceRenderOutputSurface
    *VdpauOutputSurfaceRenderOutputSurface;
static VdpOutputSurfaceRenderBitmapSurface
    *VdpauOutputSurfaceRenderBitmapSurface;

static VdpDecoderQueryCapabilities *VdpauDecoderQueryCapabilities;
static VdpDecoderCreate *VdpauDecoderCreate;
static VdpDecoderDestroy *VdpauDecoderDestroy;

static VdpDecoderRender *VdpauDecoderRender;

static VdpVideoMixerQueryFeatureSupport *VdpauVideoMixerQueryFeatureSupport;
static VdpVideoMixerQueryAttributeSupport
    *VdpauVideoMixerQueryAttributeSupport;

static VdpVideoMixerCreate *VdpauVideoMixerCreate;
static VdpVideoMixerSetFeatureEnables *VdpauVideoMixerSetFeatureEnables;
static VdpVideoMixerSetAttributeValues *VdpauVideoMixerSetAttributeValues;

static VdpVideoMixerDestroy *VdpauVideoMixerDestroy;
static VdpVideoMixerRender *VdpauVideoMixerRender;
static VdpPresentationQueueTargetDestroy *VdpauPresentationQueueTargetDestroy;
static VdpPresentationQueueCreate *VdpauPresentationQueueCreate;
static VdpPresentationQueueDestroy *VdpauPresentationQueueDestroy;
static VdpPresentationQueueSetBackgroundColor *
    VdpauPresentationQueueSetBackgroundColor;

static VdpPresentationQueueGetTime *VdpauPresentationQueueGetTime;
static VdpPresentationQueueDisplay *VdpauPresentationQueueDisplay;
static VdpPresentationQueueBlockUntilSurfaceIdle
    *VdpauPresentationQueueBlockUntilSurfaceIdle;
static VdpPresentationQueueQuerySurfaceStatus
    *VdpauPresentationQueueQuerySurfaceStatus;
static VdpPreemptionCallbackRegister *VdpauPreemptionCallbackRegister;

static VdpPresentationQueueTargetCreateX11 *
    VdpauPresentationQueueTargetCreateX11;
///@}

static void VdpauOsdInit(int, int);	///< forward definition

//----------------------------------------------------------------------------

///
///	Output video messages.
///
///	Reduce output.
///
///	@param level	message level (Error, Warning, Info, Debug, ...)
///	@param format	printf format string (NULL to flush messages)
///	@param ...	printf arguments
///
///	@returns true, if message shown
///
static int VdpauMessage(int level, const char *format, ...)
{
    if (LogLevel > level || DebugLevel > level) {
	static const char *last_format;
	static char buf[256];
	va_list ap;

	va_start(ap, format);
	if (format != last_format) {	// don't repeat same message
	    if (buf[0]) {		// print last repeated message
		syslog(LOG_ERR, "%s", buf);
		buf[0] = '\0';
	    }

	    if (format) {
		last_format = format;
		vsyslog(LOG_ERR, format, ap);
	    }
	    va_end(ap);
	    return 1;
	}
	if (format)
	    vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
    }
    return 0;
}

//	Surfaces -------------------------------------------------------------

///
///	Create surfaces for VDPAU decoder.
///
///	@param decoder	VDPAU hw decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void VdpauCreateSurfaces(VdpauDecoder * decoder, int width, int height)
{
    int i;

#ifdef DEBUG
    if (!decoder->SurfacesNeeded) {
	Error(_("video/vdpau: surface needed not set\n"));
	decoder->SurfacesNeeded = 3 + VIDEO_SURFACES_MAX;
    }
#endif
    Debug(3, "video/vdpau: %s: %dx%d * %d\n", __FUNCTION__, width, height,
	decoder->SurfacesNeeded);

    // allocate only the number of needed surfaces
    decoder->SurfaceFreeN = decoder->SurfacesNeeded;
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	VdpStatus status;

	status =
	    VdpauVideoSurfaceCreate(decoder->Device, decoder->ChromaType,
	    width, height, decoder->SurfacesFree + i);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't create video surface: %s\n"),
		VdpauGetErrorString(status));
	    decoder->SurfacesFree[i] = VDP_INVALID_HANDLE;
	    // FIXME: better error handling
	}
	Debug(4, "video/vdpau: created video surface %dx%d with id 0x%08x\n",
	    width, height, decoder->SurfacesFree[i]);
    }
}

///
///	Destroy surfaces of VDPAU decoder.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauDestroySurfaces(VdpauDecoder * decoder)
{
    int i;
    VdpStatus status;

    Debug(3, "video/vdpau: %s\n", __FUNCTION__);

    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
#ifdef DEBUG
	if (decoder->SurfacesFree[i] == VDP_INVALID_HANDLE) {
	    Debug(3, "video/vdpau: invalid surface\n");
	}
#endif
	Debug(4, "video/vdpau: destroy video surface with id 0x%08x\n",
	    decoder->SurfacesFree[i]);
	status = VdpauVideoSurfaceDestroy(decoder->SurfacesFree[i]);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video surface: %s\n"),
		VdpauGetErrorString(status));
	}
	decoder->SurfacesFree[i] = VDP_INVALID_HANDLE;
    }
    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
#ifdef DEBUG
	if (decoder->SurfacesUsed[i] == VDP_INVALID_HANDLE) {
	    Debug(3, "video/vdpau: invalid surface\n");
	}
#endif
	Debug(4, "video/vdpau: destroy video surface with id 0x%08x\n",
	    decoder->SurfacesUsed[i]);
	status = VdpauVideoSurfaceDestroy(decoder->SurfacesUsed[i]);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video surface: %s\n"),
		VdpauGetErrorString(status));
	}
	decoder->SurfacesUsed[i] = VDP_INVALID_HANDLE;
    }
    decoder->SurfaceFreeN = 0;
    decoder->SurfaceUsedN = 0;
}

///
///	Get a free surface.
///
///	@param decoder	VDPAU hw decoder
///
///	@returns the oldest free surface
///
static unsigned VdpauGetSurface0(VdpauDecoder * decoder)
{
    VdpVideoSurface surface;
    int i;

    if (!decoder->SurfaceFreeN) {
	Error(_("video/vdpau: out of surfaces\n"));

	return VDP_INVALID_HANDLE;
    }
    // use oldest surface
    surface = decoder->SurfacesFree[0];

    decoder->SurfaceFreeN--;
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	decoder->SurfacesFree[i] = decoder->SurfacesFree[i + 1];
    }
    decoder->SurfacesFree[i] = VDP_INVALID_HANDLE;

    // save as used
    decoder->SurfacesUsed[decoder->SurfaceUsedN++] = surface;

    return surface;
}

///
///	Release a surface.
///
///	@param decoder	VDPAU hw decoder
///	@param surface	surface no longer used
///
static void VdpauReleaseSurface(VdpauDecoder * decoder, unsigned surface)
{
    int i;

    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	if (decoder->SurfacesUsed[i] == surface) {
	    // no problem, with last used
	    decoder->SurfacesUsed[i] =
		decoder->SurfacesUsed[--decoder->SurfaceUsedN];
	    decoder->SurfacesFree[decoder->SurfaceFreeN++] = surface;
	    return;
	}
    }
    Error(_("video/vdpau: release surface %#08x, which is not in use\n"),
	surface);
}

///
///	Debug VDPAU decoder frames drop...
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauPrintFrames(const VdpauDecoder * decoder)
{
    Debug(3, "video/vdpau: %d missed, %d duped, %d dropped frames of %d,%d\n",
	decoder->FramesMissed, decoder->FramesDuped, decoder->FramesDropped,
	decoder->FrameCounter, decoder->FramesDisplayed);
#ifndef DEBUG
    (void)decoder;
#endif
}

///
///	Create and setup VDPAU mixer.
///
///	@param decoder	VDPAU hw decoder
///
///	@note don't forget to update features, paramaters, attributes table
///	size, if more is add.
///
static void VdpauMixerSetup(VdpauDecoder * decoder)
{
    VdpStatus status;
    int i;
    VdpVideoMixerFeature features[15];
    VdpBool enables[15];
    int feature_n;
    VdpVideoMixerAttribute attributes[5];
    void const *attribute_value_ptrs[5];
    int attribute_n;
    VdpColor background_color[1];
    uint8_t skip_chroma_value;
    float noise_reduction_level;
    float sharpness_level;
    VdpColorStandard color_standard;
    VdpCSCMatrix csc_matrix;

    //
    //	Build enables table
    //
    feature_n = 0;
    if (VdpauTemporal) {
	enables[feature_n] =
	    (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceTemporal
	    || VideoDeinterlace[decoder->Resolution] ==
	    VideoDeinterlaceTemporalSpatial) ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL;
	Debug(3, "video/vdpau: temporal deinterlace %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauTemporalSpatial) {
	enables[feature_n] =
	    VideoDeinterlace[decoder->Resolution] ==
	    VideoDeinterlaceTemporalSpatial ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] =
	    VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL;
	Debug(3, "video/vdpau: temporal spatial deinterlace %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauInverseTelecine) {
	enables[feature_n] =
	    VideoInverseTelecine[decoder->Resolution] ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE;
	Debug(3, "video/vdpau: inverse telecine %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauNoiseReduction) {
	enables[feature_n] =
	    VideoDenoise[decoder->Resolution] ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION;
	Debug(3, "video/vdpau: noise reduction %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauSharpness) {
	enables[feature_n] =
	    VideoSharpen[decoder->Resolution] ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_SHARPNESS;
	Debug(3, "video/vdpau: sharpness %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    for (i = VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1;
	i <= VdpauHqScalingMax; ++i) {
	enables[feature_n] =
	    VideoScaling[decoder->Resolution] ==
	    VideoScalingHQ ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = i;
	Debug(3, "video/vdpau: high quality scaling %d %s\n",
	    1 + i - VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1,
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    status =
	VdpauVideoMixerSetFeatureEnables(decoder->VideoMixer, feature_n,
	features, enables);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't set mixer feature enables: %s\n"),
	    VdpauGetErrorString(status));
    }
    //
    //	build attributes table
    //

    /*
       FIXME:
       VDP_VIDEO_MIXER_ATTRIBUTE_LUMA_KEY_MIN_LUMA
       VDP_VIDEO_MIXER_ATTRIBUTE_LUMA_KEY_MAX_LUMA
     */

    attribute_n = 0;
    // none video-area background color
    if (VdpauBackground) {
	background_color->red = (VideoBackground >> 24) / 255.0;
	background_color->green = ((VideoBackground >> 16) & 0xFF) / 255.0;
	background_color->blue = ((VideoBackground >> 8) & 0xFF) / 255.0;
	background_color->alpha = (VideoBackground & 0xFF) / 255.0;
	attributes[attribute_n] = VDP_VIDEO_MIXER_ATTRIBUTE_BACKGROUND_COLOR;
	attribute_value_ptrs[attribute_n++] = background_color;
	Debug(3, "video/vdpau: background color %f/%f/%f/%f\n",
	    background_color->red, background_color->green,
	    background_color->blue, background_color->alpha);
    }
    if (VdpauSkipChroma) {
	skip_chroma_value = VideoSkipChromaDeinterlace[decoder->Resolution];
	attributes[attribute_n]
	    = VDP_VIDEO_MIXER_ATTRIBUTE_SKIP_CHROMA_DEINTERLACE;
	attribute_value_ptrs[attribute_n++] = &skip_chroma_value;
	Debug(3, "video/vdpau: skip chroma deinterlace %s\n",
	    skip_chroma_value ? "enabled" : "disabled");
    }
    if (VdpauNoiseReduction) {
	noise_reduction_level = VideoDenoise[decoder->Resolution] * VdpauConfigDenoise.scale;
	attributes[attribute_n]
	    = VDP_VIDEO_MIXER_ATTRIBUTE_NOISE_REDUCTION_LEVEL;
	attribute_value_ptrs[attribute_n++] = &noise_reduction_level;
	Debug(3, "video/vdpau: noise reduction level %1.3f\n",
	    noise_reduction_level);
    }
    if (VdpauSharpness) {
	sharpness_level = VideoSharpen[decoder->Resolution] * VdpauConfigSharpen.scale;
	attributes[attribute_n]
	    = VDP_VIDEO_MIXER_ATTRIBUTE_SHARPNESS_LEVEL;
	attribute_value_ptrs[attribute_n++] = &sharpness_level;
	Debug(3, "video/vdpau: sharpness level %+1.3f\n", sharpness_level);
    }
    switch (VideoColorSpaces[decoder->Resolution]) {
	case VideoColorSpaceNone:
	default:
	    color_standard = 0;
	    break;
	case VideoColorSpaceBt601:
	    color_standard = VDP_COLOR_STANDARD_ITUR_BT_601;
	    Debug(3, "video/vdpau: color space ITU-R BT.601\n");
	    break;
	case VideoColorSpaceBt709:
	    color_standard = VDP_COLOR_STANDARD_ITUR_BT_709;
	    Debug(3, "video/vdpau: color space ITU-R BT.709\n");
	    break;
	case VideoColorSpaceSmpte240:
	    color_standard = VDP_COLOR_STANDARD_SMPTE_240M;
	    Debug(3, "video/vdpau: color space SMPTE-240M\n");
	    break;
    }
    //
    //	Studio levels
    //
    //	based on www.nvnews.net forum thread
    //
    if (VideoStudioLevels) {
	static const float color_coeffs[][3] = {
	    {0.299, 0.587, 0.114},
	    {0.2125, 0.7154, 0.0721},
	    {0.2122, 0.7013, 0.0865}
	};
	float uvcos, uvsin;
	float uv_coeffs[3][2];
	float Kr, Kg, Kb;
	const int rgbmin = 16;
	const int rgbr = 235 - rgbmin;

	Kr = color_coeffs[color_standard][0];
	Kg = color_coeffs[color_standard][1];
	Kb = color_coeffs[color_standard][2];

	uv_coeffs[0][0] = 0.0;
	uv_coeffs[0][1] = (rgbr / 112.0) * (1.0 - Kr);
	uv_coeffs[1][0] = -(rgbr / 112.0) * (1.0 - Kb) * Kb / Kg;
	uv_coeffs[1][1] = -(rgbr / 112.0) * (1.0 - Kr) * Kr / Kg;
	uv_coeffs[2][0] = (rgbr / 112.0) * (1.0 - Kb);
	uv_coeffs[2][1] = 0.0;

	uvcos = decoder->Procamp.saturation * cos(decoder->Procamp.hue);
	uvsin = decoder->Procamp.saturation * sin(decoder->Procamp.hue);

	for (i = 0; i < 3; ++i) {
	    csc_matrix[i][3] = decoder->Procamp.brightness;
	    csc_matrix[i][0] = rgbr * decoder->Procamp.contrast / 219;
	    csc_matrix[i][3] += (-16 / 255.0) * csc_matrix[i][0];
	    csc_matrix[i][1] =
		uv_coeffs[i][0] * uvcos + uv_coeffs[i][1] * uvsin;
	    csc_matrix[i][3] += (-128 / 255.0) * csc_matrix[i][1];
	    csc_matrix[i][2] =
		uv_coeffs[i][0] * uvsin + uv_coeffs[i][1] * uvcos;
	    csc_matrix[i][3] += (-128 / 255.0) * csc_matrix[i][2];
	    csc_matrix[i][3] += rgbmin / 255.0;
	    csc_matrix[i][3] += 0.5 - decoder->Procamp.contrast / 2.0;
	}
    } else {
	status =
	    VdpauGenerateCSCMatrix(&decoder->Procamp, color_standard,
	    &csc_matrix);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't generate CSC matrix: %s\n"),
		VdpauGetErrorString(status));
	}
    }

    attributes[attribute_n] = VDP_VIDEO_MIXER_ATTRIBUTE_CSC_MATRIX;
    attribute_value_ptrs[attribute_n++] = &csc_matrix;

    status =
	VdpauVideoMixerSetAttributeValues(decoder->VideoMixer, attribute_n,
	attributes, attribute_value_ptrs);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't set mixer attribute values: %s\n"),
	    VdpauGetErrorString(status));
    }
}

///
///	Create and setup VDPAU mixer.
///
///	@param decoder	VDPAU hw decoder
///
///	@note don't forget to update features, paramaters, attributes table
///	size, if more is add.
///
static void VdpauMixerCreate(VdpauDecoder * decoder)
{
    VdpStatus status;
    int i;
    VdpVideoMixerFeature features[15];
    int feature_n;
    VdpVideoMixerParameter paramaters[4];
    void const *value_ptrs[4];
    int parameter_n;
    VdpChromaType chroma_type;
    int layers;

    //
    //	Build feature table
    //
    feature_n = 0;
    if (VdpauTemporal) {
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL;
    }
    if (VdpauTemporalSpatial) {
	features[feature_n++] =
	    VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL;
    }
    if (VdpauInverseTelecine) {
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE;
    }
    if (VdpauNoiseReduction) {
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION;
    }
    if (VdpauSharpness) {
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_SHARPNESS;
    }
    for (i = VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1;
	i <= VdpauHqScalingMax; ++i) {
	features[feature_n++] = i;
    }

    decoder->ChromaType = chroma_type = VdpauChromaType;

    //
    //	Setup parameter/value tables
    //
    paramaters[0] = VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH;
    value_ptrs[0] = &decoder->InputWidth;
    paramaters[1] = VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT;
    value_ptrs[1] = &decoder->InputHeight;
    paramaters[2] = VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE;
    value_ptrs[2] = &chroma_type;
    layers = 0;
    paramaters[3] = VDP_VIDEO_MIXER_PARAMETER_LAYERS;
    value_ptrs[3] = &layers;
    parameter_n = 4;

    status =
	VdpauVideoMixerCreate(VdpauDevice, feature_n, features, parameter_n,
	paramaters, value_ptrs, &decoder->VideoMixer);
    if (status != VDP_STATUS_OK) {
	Fatal(_("video/vdpau: can't create video mixer: %s\n"),
	    VdpauGetErrorString(status));
	// FIXME: no fatal errors
    }

    VdpauMixerSetup(decoder);
}

///
///	Allocate new VDPAU decoder.
///
///	@param stream	video stream
///
///	@returns a new prepared vdpau hardware decoder.
///
static VdpauDecoder *VdpauNewHwDecoder(VideoStream * stream)
{
    VdpauDecoder *decoder;
    int i;

    if ((unsigned)VdpauDecoderN >=
	sizeof(VdpauDecoders) / sizeof(*VdpauDecoders)) {
	Error(_("video/vdpau: out of decoders\n"));
	return NULL;
    }

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
	Error(_("video/vdpau: out of memory\n"));
	return NULL;
    }
    decoder->Device = VdpauDevice;
    decoder->Window = VideoWindow;
    //decoder->VideoX = 0;		// done by calloc
    //decoder->VideoY = 0;
    decoder->VideoWidth = VideoWindowWidth;
    decoder->VideoHeight = VideoWindowHeight;

    decoder->Profile = VDP_INVALID_HANDLE;
    decoder->VideoDecoder = VDP_INVALID_HANDLE;
    decoder->VideoMixer = VDP_INVALID_HANDLE;

    for (i = 0; i < CODEC_SURFACES_MAX; ++i) {
	decoder->SurfacesUsed[i] = VDP_INVALID_HANDLE;
	decoder->SurfacesFree[i] = VDP_INVALID_HANDLE;
    }

    //
    // setup video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VDP_INVALID_HANDLE;
    }

#ifdef DEBUG
    if (VIDEO_SURFACES_MAX < 1 + 1 + 1 + 1) {
	Error(_
	    ("video/vdpau: need 1 future, 1 current, 1 back and 1 work surface\n"));
    }
#endif

    // Procamp operation parameterization data
    decoder->Procamp.struct_version = VDP_PROCAMP_VERSION;
    decoder->Procamp.brightness = 0.0;
    decoder->Procamp.contrast = 1.0;
    decoder->Procamp.saturation = 1.0;
    decoder->Procamp.hue = 0.0;		// default values

    decoder->OutputWidth = VideoWindowWidth;
    decoder->OutputHeight = VideoWindowHeight;

    decoder->PixFmt = AV_PIX_FMT_NONE;

#ifdef USE_AUTOCROP
    //decoder->AutoCropBuffer = NULL;	// done by calloc
    //decoder->AutoCropBufferSize = 0;
#endif

    decoder->Stream = stream;
    if (!VdpauDecoderN) {		// FIXME: hack sync on audio
	decoder->SyncOnAudio = 1;
    }
    decoder->Closing = -300 - 1;

    decoder->PTS = AV_NOPTS_VALUE;

    VdpauDecoders[VdpauDecoderN++] = decoder;

    return decoder;
}

///
///	Cleanup VDPAU.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauCleanup(VdpauDecoder * decoder)
{
    VdpStatus status;
    int i;

    if (decoder->VideoDecoder != VDP_INVALID_HANDLE) {
	// hangs in lock
	status = VdpauDecoderDestroy(decoder->VideoDecoder);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video decoder: %s\n"),
		VdpauGetErrorString(status));
	}
	decoder->VideoDecoder = VDP_INVALID_HANDLE;
	// don't overwrite: decoder->Profile = VDP_INVALID_HANDLE;
    }

    if (decoder->VideoMixer != VDP_INVALID_HANDLE) {
	status = VdpauVideoMixerDestroy(decoder->VideoMixer);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video mixer: %s\n"),
		VdpauGetErrorString(status));
	}
	decoder->VideoMixer = VDP_INVALID_HANDLE;
    }

    if (decoder->SurfaceFreeN || decoder->SurfaceUsedN) {
	VdpauDestroySurfaces(decoder);
    }
    //
    // reset video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VDP_INVALID_HANDLE;
    }
    decoder->SurfaceRead = 0;
    decoder->SurfaceWrite = 0;
    decoder->SurfaceField = 0;
    decoder->Interlaced = 0;
    decoder->SyncCounter = 0;
    decoder->FrameCounter = 0;
    decoder->FramesDisplayed = 0;
    decoder->StartCounter = 0;
    decoder->Closing = 0;
    decoder->PTS = AV_NOPTS_VALUE;
    VideoDeltaPTS = 0;
}

///
///	Destroy a VDPAU decoder.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauDelHwDecoder(VdpauDecoder * decoder)
{
    int i;

    for (i = 0; i < VdpauDecoderN; ++i) {
	if (VdpauDecoders[i] == decoder) {
	    VdpauDecoders[i] = NULL;
	    // copy last slot into empty slot
	    if (i < --VdpauDecoderN) {
		VdpauDecoders[i] = VdpauDecoders[VdpauDecoderN];
	    }

	    VdpauCleanup(decoder);
	    VdpauPrintFrames(decoder);
#ifdef USE_AUTOCROP
	    free(decoder->AutoCropBuffer);
#endif
	    free(decoder);
	    return;
	}
    }
    Error(_("video/vdpau: decoder not in decoder list.\n"));
}

///
///	Get the proc address.
///
///	@param id		VDP function id
///	@param[out] addr	address of VDP function
///	@param name		name of function for error message
///
static inline void VdpauGetProc(const VdpFuncId id, void *addr,
    const char *name)
{
    VdpStatus status;

    status = VdpauGetProcAddress(VdpauDevice, id, addr);
    if (status != VDP_STATUS_OK) {
	Fatal(_("video/vdpau: Can't get function address of '%s': %s\n"), name,
	    VdpauGetErrorString(status));
	// FIXME: rewrite none fatal
    }
}

///
///	Initialize output queue.
///
static void VdpauInitOutputQueue(void)
{
    VdpStatus status;
    VdpRGBAFormat format;
    int i;

#ifdef USE_GLX
    if (GlxEnabled) goto output_surface;
#endif
    status =
	VdpauPresentationQueueTargetCreateX11(VdpauDevice, VideoWindow,
	&VdpauQueueTarget);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't create presentation queue target: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }

    status =
	VdpauPresentationQueueCreate(VdpauDevice, VdpauQueueTarget,
	&VdpauQueue);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't create presentation queue: %s\n"),
	    VdpauGetErrorString(status));
	VdpauPresentationQueueTargetDestroy(VdpauQueueTarget);
	VdpauQueueTarget = 0;
	return;
    }

    VdpauQueueBackgroundColor->red = 0.01;
    VdpauQueueBackgroundColor->green = 0.02;
    VdpauQueueBackgroundColor->blue = 0.03;
    VdpauQueueBackgroundColor->alpha = 1.00;
    VdpauPresentationQueueSetBackgroundColor(VdpauQueue,
	VdpauQueueBackgroundColor);

output_surface:
    //
    //	Create display output surfaces
    //
    format = VDP_RGBA_FORMAT_B8G8R8A8;
    // FIXME: does a 10bit rgba produce a better output?
    // format = VDP_RGBA_FORMAT_R10G10B10A2;
    for (i = 0; i < OUTPUT_SURFACES_MAX; ++i) {
	status =
	    VdpauOutputSurfaceCreate(VdpauDevice, format, VideoWindowWidth,
	    VideoWindowHeight, VdpauSurfacesRb + i);
	if (status != VDP_STATUS_OK) {
	    Fatal(_("video/vdpau: can't create output surface: %s\n"),
		VdpauGetErrorString(status));
	}
	Debug(3, "video/vdpau: created output surface %dx%d with id 0x%08x\n",
	    VideoWindowWidth, VideoWindowHeight, VdpauSurfacesRb[i]);
    }

    //
    //	 Create render output surface for grabbing
    //
    status =
	VdpauOutputSurfaceCreate(VdpauDevice, format, VideoWindowWidth,
	VideoWindowHeight, &VdpauGrabRenderSurface);
    if (status != VDP_STATUS_OK) {
	Fatal(_("video/vdpau: can't create grab render output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    Debug(3,
	"video/vdpau: created grab render output surface %dx%d with id 0x%08x\n",
	VideoWindowWidth, VideoWindowHeight, VdpauGrabRenderSurface);

#ifdef USE_GLX
    if (GlxEnabled) {
        if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext)) {
	    Error(_("video/glx: can't make glx context current\n"));
        }

	for (i = 0; i < OUTPUT_SURFACES_MAX; ++i) {
	GlxSetupDecoder(VideoWindowWidth, VideoWindowHeight,
	    GlTextures[i]);
	}

	for (i = 0; i < OUTPUT_SURFACES_MAX; ++i) {
	    VdpauOutputSurfacesNV[i] =
		glVDPAURegisterOutputSurfaceNV((void*)(uintptr_t)VdpauSurfacesRb[i], GL_TEXTURE_2D,
		    1, &GlTextures[i][0]);
	    GlxCheck();
	    Debug(3, "video/vdpau: created output GL surface %dx%d with id 0x%08x\n",
		VideoWindowWidth, VideoWindowHeight, VdpauSurfacesRb[i]);

	    glVDPAUSurfaceAccessNV(VdpauOutputSurfacesNV[i], GL_READ_ONLY);
	    GlxCheck();
	}
    }
#endif
}

///
///	Cleanup output queue.
///
static void VdpauExitOutputQueue(void)
{
    int i;
    VdpStatus status;

    if (VdpauQueue) {
	VdpauPresentationQueueDestroy(VdpauQueue);
	VdpauQueue = 0;
    }
    if (VdpauQueueTarget) {
	VdpauPresentationQueueTargetDestroy(VdpauQueueTarget);
	VdpauQueueTarget = 0;
    }
#ifdef USE_GLX
    if (GlxEnabled) {
        if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext)) {
	    Error(_("video/glx: can't make glx context current\n"));
        }
	for (i = 0; i < OUTPUT_SURFACES_MAX; ++i) {
	    glVDPAUUnregisterSurfaceNV(VdpauOutputSurfacesNV[i]);
	    GlxCheck();
	    glDeleteTextures(2, GlTextures[i]);
	}
    }
#endif
    //
    //	destroy display output surfaces
    //
    for (i = 0; i < OUTPUT_SURFACES_MAX; ++i) {
	Debug(4, "video/vdpau: destroy output surface with id 0x%08x\n",
	    VdpauSurfacesRb[i]);
	if (VdpauSurfacesRb[i] != VDP_INVALID_HANDLE) {
	    status = VdpauOutputSurfaceDestroy(VdpauSurfacesRb[i]);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't destroy output surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    VdpauSurfacesRb[i] = VDP_INVALID_HANDLE;
	}
    }
    if (VdpauGrabRenderSurface != VDP_INVALID_HANDLE) {
	status = VdpauOutputSurfaceDestroy(VdpauGrabRenderSurface);
	if (status != VDP_STATUS_OK) {
	    Error(_
		("video/vdpau: can't destroy grab render output surface: %s\n"),
		VdpauGetErrorString(status));
	}
	VdpauGrabRenderSurface = VDP_INVALID_HANDLE;
    }
}

///
///	Display preemption callback.
///
///	@param device	device that had its display preempted
///	@param context	client-supplied callback context
///
static void VdpauPreemptionCallback(VdpDevice device, __attribute__ ((unused))
    void *context)
{
    Debug(3, "video/vdpau: display preemption\n");
    if (device != VdpauDevice) {
	Error(_("video/vdpau preemption device not our device\n"));
	return;
    }
    VdpauPreemption = 1;		// set flag for video thread
}

static int VdpauPreemptionRecover(void);

///
///	VDPAU setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns true if VDPAU could be initialized, false otherwise.
///
static int VdpauInit(const char *display_name)
{
    VdpStatus status;
    VdpGetApiVersion *get_api_version;
    uint32_t api_version;
    VdpGetInformationString *get_information_string;
    const char *information_string;
    int i;
    VdpBool flag;
    uint32_t max_width;
    uint32_t max_height;

    pthread_mutex_init(&VdpauGrabMutex, NULL);

    status =
	vdp_device_create_x11(XlibDisplay, DefaultScreen(XlibDisplay),
	&VdpauDevice, &VdpauGetProcAddress);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: Can't create vdp device on display '%s'\n"),
	    display_name);
	return 0;
    }
    // get error function first, for better error messages
    status =
	VdpauGetProcAddress(VdpauDevice, VDP_FUNC_ID_GET_ERROR_STRING,
	(void **)&VdpauGetErrorString);
    if (status != VDP_STATUS_OK) {
	Error(_
	    ("video/vdpau: Can't get function address of 'GetErrorString'\n"));
	// FIXME: destroy_x11 VdpauDeviceDestroy
	return 0;
    }
    // get destroy device next, for cleaning up
    VdpauGetProc(VDP_FUNC_ID_DEVICE_DESTROY, &VdpauDeviceDestroy,
	"DeviceDestroy");

    // get version
    VdpauGetProc(VDP_FUNC_ID_GET_API_VERSION, &get_api_version,
	"GetApiVersion");
    VdpauGetProc(VDP_FUNC_ID_GET_INFORMATION_STRING, &get_information_string,
	"VdpauGetProc");
    status = get_api_version(&api_version);
    // FIXME: check status
    status = get_information_string(&information_string);
    // FIXME: check status

    Info(_("video/vdpau: VDPAU API version: %u\n"), api_version);
    Info(_("video/vdpau: VDPAU information: %s\n"), information_string);
    // FIXME: check if needed capabilities are available

    VdpauGetProc(VDP_FUNC_ID_GENERATE_CSC_MATRIX, &VdpauGenerateCSCMatrix,
	"GenerateCSCMatrix");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_QUERY_CAPABILITIES,
	&VdpauVideoSurfaceQueryCapabilities, "VideoSurfaceQueryCapabilities");
    VdpauGetProc
	(VDP_FUNC_ID_VIDEO_SURFACE_QUERY_GET_PUT_BITS_Y_CB_CR_CAPABILITIES,
	&VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities,
	"VideoSurfaceQueryGetPutBitsYCbCrCapabilities");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_CREATE, &VdpauVideoSurfaceCreate,
	"VideoSurfaceCreate");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_DESTROY, &VdpauVideoSurfaceDestroy,
	"VideoSurfaceDestroy");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_GET_PARAMETERS,
	&VdpauVideoSurfaceGetParameters, "VideoSurfaceGetParameters");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_GET_BITS_Y_CB_CR,
	&VdpauVideoSurfaceGetBitsYCbCr, "VideoSurfaceGetBitsYCbCr");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_PUT_BITS_Y_CB_CR,
	&VdpauVideoSurfacePutBitsYCbCr, "VideoSurfacePutBitsYCbCr");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_CAPABILITIES,
	&VdpauOutputSurfaceQueryCapabilities,
	"OutputSurfaceQueryCapabilities");
#if 0
    VdpauGetProc
	(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_GET_PUT_BITS_NATIVE_CAPABILITIES, &,
	"");
    VdpauGetProc
	(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_PUT_BITS_INDEXED_CAPABILITIES, &,
	"");
    VdpauGetProc
	(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_PUT_BITS_Y_CB_CR_CAPABILITIES, &,
	"");
#endif
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_CREATE, &VdpauOutputSurfaceCreate,
	"OutputSurfaceCreate");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_DESTROY,
	&VdpauOutputSurfaceDestroy, "OutputSurfaceDestroy");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_GET_PARAMETERS,
	&VdpauOutputSurfaceGetParameters, "OutputSurfaceGetParameters");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_GET_BITS_NATIVE,
	&VdpauOutputSurfaceGetBitsNative, "OutputSurfaceGetBitsNative");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_NATIVE,
	&VdpauOutputSurfacePutBitsNative, "OutputSurfacePutBitsNative");
#if 0
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_INDEXED, &, "");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_Y_CB_CR, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_QUERY_CAPABILITIES,
	&VdpauBitmapSurfaceQueryCapabilities,
	"BitmapSurfaceQueryCapabilities");
    VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_CREATE, &VdpauBitmapSurfaceCreate,
	"BitmapSurfaceCreate");
    VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_DESTROY,
	&VdpauBitmapSurfaceDestroy, "BitmapSurfaceDestroy");
    // VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_GET_PARAMETERS, &VdpauBitmapSurfaceGetParameters, "BitmapSurfaceGetParameters");
    VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_PUT_BITS_NATIVE,
	&VdpauBitmapSurfacePutBitsNative, "BitmapSurfacePutBitsNative");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_OUTPUT_SURFACE,
	&VdpauOutputSurfaceRenderOutputSurface,
	"OutputSurfaceRenderOutputSurface");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_BITMAP_SURFACE,
	&VdpauOutputSurfaceRenderBitmapSurface,
	"OutputSurfaceRenderBitmapSurface");
#if 0
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_VIDEO_SURFACE_LUMA, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_DECODER_QUERY_CAPABILITIES,
	&VdpauDecoderQueryCapabilities, "DecoderQueryCapabilities");
    VdpauGetProc(VDP_FUNC_ID_DECODER_CREATE, &VdpauDecoderCreate,
	"DecoderCreate");
    VdpauGetProc(VDP_FUNC_ID_DECODER_DESTROY, &VdpauDecoderDestroy,
	"DecoderDestroy");
#if 0
    VdpauGetProc(VDP_FUNC_ID_DECODER_GET_PARAMETERS,
	&VdpauDecoderGetParameters, "DecoderGetParameters");
#endif
    VdpauGetProc(VDP_FUNC_ID_DECODER_RENDER, &VdpauDecoderRender,
	"DecoderRender");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_FEATURE_SUPPORT,
	&VdpauVideoMixerQueryFeatureSupport, "VideoMixerQueryFeatureSupport");
#if 0
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_PARAMETER_SUPPORT, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_ATTRIBUTE_SUPPORT,
	&VdpauVideoMixerQueryAttributeSupport,
	"VideoMixerQueryAttributeSupport");
#if 0
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_PARAMETER_VALUE_RANGE, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_ATTRIBUTE_VALUE_RANGE, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_CREATE, &VdpauVideoMixerCreate,
	"VideoMixerCreate");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_SET_FEATURE_ENABLES,
	&VdpauVideoMixerSetFeatureEnables, "VideoMixerSetFeatureEnables");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_SET_ATTRIBUTE_VALUES,
	&VdpauVideoMixerSetAttributeValues, "VideoMixerSetAttributeValues");
#if 0
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_GET_FEATURE_SUPPORT, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_GET_FEATURE_ENABLES, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_GET_PARAMETER_VALUES, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_GET_ATTRIBUTE_VALUES, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_DESTROY, &VdpauVideoMixerDestroy,
	"VideoMixerDestroy");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_RENDER, &VdpauVideoMixerRender,
	"VideoMixerRender");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_DESTROY,
	&VdpauPresentationQueueTargetDestroy,
	"PresentationQueueTargetDestroy");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_CREATE,
	&VdpauPresentationQueueCreate, "PresentationQueueCreate");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_DESTROY,
	&VdpauPresentationQueueDestroy, "PresentationQueueDestroy");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_SET_BACKGROUND_COLOR,
	&VdpauPresentationQueueSetBackgroundColor,
	"PresentationQueueSetBackgroundColor");
#if 0
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_GET_BACKGROUND_COLOR,
	&VdpauPresentationQueueGetBackgroundColor,
	"PresentationQueueGetBackgroundColor");
#endif
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_GET_TIME,
	&VdpauPresentationQueueGetTime, "PresentationQueueGetTime");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_DISPLAY,
	&VdpauPresentationQueueDisplay, "PresentationQueueDisplay");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_BLOCK_UNTIL_SURFACE_IDLE,
	&VdpauPresentationQueueBlockUntilSurfaceIdle,
	"PresentationQueueBlockUntilSurfaceIdle");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_QUERY_SURFACE_STATUS,
	&VdpauPresentationQueueQuerySurfaceStatus,
	"PresentationQueueQuerySurfaceStatus");
    VdpauGetProc(VDP_FUNC_ID_PREEMPTION_CALLBACK_REGISTER,
	&VdpauPreemptionCallbackRegister, "PreemptionCallbackRegister");

    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_CREATE_X11,
	&VdpauPresentationQueueTargetCreateX11,
	"PresentationQueueTargetCreateX11");

    status =
	VdpauPreemptionCallbackRegister(VdpauDevice, VdpauPreemptionCallback,
	NULL);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't register preemption callback: %s\n"),
	    VdpauGetErrorString(status));
    }
    //
    //	Look which levels of high quality scaling are supported
    //
    for (i = 0; i < 9; ++i) {
	status =
	    VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	    VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + i, &flag);
	if (status != VDP_STATUS_OK) {
	    Warning(_("video/vdpau: can't query feature '%s': %s\n"),
		"high-quality-scaling", VdpauGetErrorString(status));
	    break;
	}
	if (!flag) {
	    break;
	}
	VdpauHqScalingMax =
	    VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + i;
    }

    //
    //	Cache some features
    //
    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_ATTRIBUTE_BACKGROUND_COLOR, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "background-color", VdpauGetErrorString(status));
    } else {
	VdpauBackground = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "deinterlace-temporal", VdpauGetErrorString(status));
    } else {
	VdpauTemporal = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "deinterlace-temporal-spatial", VdpauGetErrorString(status));
    } else {
	VdpauTemporalSpatial = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "inverse-telecine", VdpauGetErrorString(status));
    } else {
	VdpauInverseTelecine = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "noise-reduction", VdpauGetErrorString(status));
    } else {
	VdpauNoiseReduction = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_SHARPNESS, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"), "sharpness",
	    VdpauGetErrorString(status));
    } else {
	VdpauSharpness = flag;
    }

    status =
	VdpauVideoMixerQueryAttributeSupport(VdpauDevice,
	VDP_VIDEO_MIXER_ATTRIBUTE_SKIP_CHROMA_DEINTERLACE, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "skip-chroma-deinterlace", VdpauGetErrorString(status));
    } else {
	VdpauSkipChroma = flag;
    }

    if (VdpauHqScalingMax) {
	Info(_("video/vdpau: highest supported high quality scaling %d\n"),
	    VdpauHqScalingMax -
	    VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + 1);
    } else {
	Info(_("video/vdpau: high quality scaling unsupported\n"));
    }
    Info(_("video/vdpau: feature deinterlace temporal %s\n"),
	VdpauTemporal ? _("supported") : _("unsupported"));
    Info(_("video/vdpau: feature deinterlace temporal spatial %s\n"),
	VdpauTemporalSpatial ? _("supported") : _("unsupported"));
    Info(_("video/vdpau: attribute skip chroma deinterlace %s\n"),
	VdpauSkipChroma ? _("supported") : _("unsupported"));

    //
    //	video formats
    //
    flag = VDP_FALSE;
    status =
	VdpauVideoSurfaceQueryCapabilities(VdpauDevice, VDP_CHROMA_TYPE_420,
	&flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query video surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 4:2:0 chroma format with %dx%d supported\n"),
	    max_width, max_height);
	VdpauChromaType = VDP_CHROMA_TYPE_420;
    }
    flag = VDP_FALSE;
    status =
	VdpauVideoSurfaceQueryCapabilities(VdpauDevice, VDP_CHROMA_TYPE_422,
	&flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query video surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 4:2:2 chroma format with %dx%d supported\n"),
	    max_width, max_height);
	VdpauChromaType = VDP_CHROMA_TYPE_422;
    }
    flag = VDP_FALSE;
    status =
	VdpauVideoSurfaceQueryCapabilities(VdpauDevice, VDP_CHROMA_TYPE_444,
	&flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query video surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 4:4:4 chroma format with %dx%d supported\n"),
	    max_width, max_height);
	VdpauChromaType = VDP_CHROMA_TYPE_444;
    }
    // FIXME: check if all chroma-types failed
    // FIXME: vdpau didn't support decode of other chroma types
    VdpauChromaType = VDP_CHROMA_TYPE_420;

    // FIXME: does only check for chroma formats, but no action
    status =
	VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities(VdpauDevice,
	VDP_CHROMA_TYPE_422, VDP_YCBCR_FORMAT_YUYV, &flag);
    if (status != VDP_STATUS_OK || !flag) {
	Error(_("video/vdpau: doesn't support yuvy video surface\n"));
    }
    status =
	VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities(VdpauDevice,
	VDP_CHROMA_TYPE_420, VDP_YCBCR_FORMAT_YV12, &flag);
    if (status != VDP_STATUS_OK || !flag) {
	Error(_("video/vdpau: doesn't support yv12 video surface\n"));
    }

    flag = VDP_FALSE;
    status =
	VdpauOutputSurfaceQueryCapabilities(VdpauDevice,
	VDP_RGBA_FORMAT_B8G8R8A8, &flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 8bit BGRA format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauOutputSurfaceQueryCapabilities(VdpauDevice,
	VDP_RGBA_FORMAT_R8G8B8A8, &flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 8bit RGBA format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauOutputSurfaceQueryCapabilities(VdpauDevice,
	VDP_RGBA_FORMAT_R10G10B10A2, &flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 10bit RGBA format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauOutputSurfaceQueryCapabilities(VdpauDevice,
	VDP_RGBA_FORMAT_B10G10R10A2, &flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 8bit BRGA format with %dx%d supported\n"),
	    max_width, max_height);
    }
#ifdef USE_GLX
    if (GlxEnabled) {
        void *vdpDevice = GetVDPAUDevice();
        void *procAdress = GetVDPAUProcAdress();

        if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext)) {
	    Error(_("video/glx: can't make glx context current\n"));
        }

        glVDPAUInitNV(vdpDevice, procAdress);
	GlxCheck();
    }
#endif
    // FIXME: does only check for rgba formats, but no action

    // FIXME: what if preemption happens during setup?

    //
    //	Create presentation queue, only one queue pro window
    //
   if (VdpauPreemption) {		// Restore if preempted
	VdpauPreemptionRecover();
    }
    else
	VdpauInitOutputQueue();

    return 1;
}

#ifdef USE_GLX
///
///	VDPAU GLX setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns true if VDPAU could be initialized, false otherwise.
///
static int VdpauGlxInit(const char *display_name)
{
    GlxEnabled = 1;

    GlxInit();
    if (GlxEnabled) {
	GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight,
	    GlxSharedContext);
    }
    if (!GlxEnabled) {
	Error(_("video/glx: glx error\n"));
    }

    return VdpauInit(display_name);
}
#endif

///
///	VDPAU cleanup.
///
static void VdpauExit(void)
{
    int i;

    for (i = 0; i < VdpauDecoderN; ++i) {
	if (VdpauDecoders[i]) {
	    VdpauDelHwDecoder(VdpauDecoders[i]);
	    VdpauDecoders[i] = NULL;
	}
    }
    VdpauDecoderN = 0;

    if (VdpauDevice) {
	VdpauExitOutputQueue();

	// FIXME: more VDPAU cleanups...

	if (VdpauDeviceDestroy) {
	    VdpauDeviceDestroy(VdpauDevice);
	}
	VdpauDevice = 0;
    }
#ifdef USE_GLX
    if (GlxEnabled) {
        glVDPAUFiniNV();
        GlxCheck();
    }
#endif
    pthread_mutex_destroy(&VdpauGrabMutex);
}

///
///	Update output for new size or aspect ratio.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauUpdateOutput(VdpauDecoder * decoder)
{
    VideoUpdateOutput(decoder->InputAspect, decoder->InputWidth,
	decoder->InputHeight, decoder->Resolution, decoder->VideoX,
	decoder->VideoY, decoder->VideoWidth, decoder->VideoHeight,
	&decoder->OutputX, &decoder->OutputY, &decoder->OutputWidth,
	&decoder->OutputHeight, &decoder->CropX, &decoder->CropY,
	&decoder->CropWidth, &decoder->CropHeight);
#ifdef USE_AUTOCROP
    decoder->AutoCrop->State = 0;
    decoder->AutoCrop->Count = AutoCropDelay;
#endif
}

///
///	Configure VDPAU for new video format.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauSetupOutput(VdpauDecoder * decoder)
{
    VdpStatus status;
    VdpChromaType chroma_type;
    uint32_t width;
    uint32_t height;

    // FIXME: need only to create and destroy surfaces for size changes
    //		or when number of needed surfaces changed!
    decoder->Resolution =
	VideoResolutionGroup(decoder->InputWidth, decoder->InputHeight,
	decoder->Interlaced);
    VideoResolution = decoder->Resolution;
    VdpauCreateSurfaces(decoder, decoder->InputWidth, decoder->InputHeight);

    VdpauMixerCreate(decoder);

    VdpauUpdateOutput(decoder);		// update aspect/scaling

    //	get real surface size
    status =
	VdpauVideoSurfaceGetParameters(decoder->SurfacesFree[0], &chroma_type,
	&width, &height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface parameters: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }
    // vdpau can choose different sizes, must use them for putbits
    if (chroma_type != decoder->ChromaType) {
	// I request 422 if supported, but get only 420
	Warning(_("video/vdpau: video surface chroma type mismatch\n"));
    }
    if (width != (uint32_t) decoder->InputWidth
	|| height != (uint32_t) decoder->InputHeight) {
	// FIXME: must rewrite the code to support this case
	Warning(_("video/vdpau: video surface size mismatch\n"));
    }
}

///
///	Get a free surface.  Called from ffmpeg.
///
///	@param decoder		VDPAU hw decoder
///	@param video_ctx	ffmpeg video codec context
///
///	@returns the oldest free surface
///
static unsigned VdpauGetSurface(VdpauDecoder * decoder,
    const AVCodecContext * video_ctx)
{
#ifdef FFMPEG_BUG1_WORKAROUND
    // get_format not called with valid informations.
    if (video_ctx->width != decoder->InputWidth
	|| video_ctx->height != decoder->InputHeight) {
	VdpStatus status;

	VdpauCleanup(decoder);

	status =
	    VdpauDecoderCreate(VdpauDevice, decoder->Profile, video_ctx->width,
	    video_ctx->height,
	    decoder->SurfacesNeeded - VIDEO_SURFACES_MAX - 1,
	    &decoder->VideoDecoder);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't create decoder: %s\n"),
		VdpauGetErrorString(status));
	}

	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;
	VdpauSetupOutput(decoder);
    }
#else
    (void)video_ctx;
#endif
    return VdpauGetSurface0(decoder);
}

///
///	Check profile supported.
///
///	@param decoder		VDPAU hw decoder
///	@param profile		VDPAU profile requested
///
static VdpDecoderProfile VdpauCheckProfile(VdpauDecoder * decoder,
    VdpDecoderProfile profile)
{
    VdpStatus status;
    VdpBool is_supported;
    uint32_t max_level;
    uint32_t max_macroblocks;
    uint32_t max_width;
    uint32_t max_height;

    status =
	VdpauDecoderQueryCapabilities(decoder->Device, profile, &is_supported,
	&max_level, &max_macroblocks, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query decoder capabilities: %s\n"),
	    VdpauGetErrorString(status));
	return VDP_INVALID_HANDLE;
    }
    Debug(3,
	"video/vdpau: profile %d with level %d, macro blocks %d, width %d, height %d %ssupported\n",
	profile, max_level, max_macroblocks, max_width, max_height,
	is_supported ? "" : "not ");
    return is_supported ? profile : VDP_INVALID_HANDLE;
}

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,80,100)
typedef struct VDPAUContext {
    AVBufferRef *hw_frames_ctx;
    AVFrame *tmp_frame;
} VDPAUContext;
#endif

void vdpau_uninit(AVCodecContext *s)
{
    VideoDecoder *ist = s->opaque;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,80,100)
    VDPAUContext *ctx = ist->hwaccel_ctx;

    av_buffer_unref(&ctx->hw_frames_ctx);
    av_frame_free(&ctx->tmp_frame);
    av_freep(&ist->hwaccel_ctx);
#endif
    av_freep(&s->hwaccel_context);

    ist->hwaccel_uninit = NULL;
    ist->hwaccel_get_buffer = NULL;
    ist->hwaccel_retrieve_data = NULL;
}
/*
static int vdpau_get_buffer(AVCodecContext *s, AVFrame *frame, int flags)
{
    VideoDecoder *ist = s->opaque;
    VDPAUContext *ctx = ist->hwaccel_ctx;
    int ret;

    ret = av_hwframe_get_buffer(ctx->hw_frames_ctx, frame, 0);
    Debug(4, "hwframe got buffer %#08x \n", frame->data[3]);
    return ret;
}
*/
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,80,100)
static int vdpau_retrieve_data(AVCodecContext *s, AVFrame *frame)
{
    VideoDecoder *ist = s->opaque;
    VDPAUContext *ctx = ist->hwaccel_ctx;
    int ret;

    Debug(3, "vdpau_retrieve data\n");

    ret = av_hwframe_transfer_data(ctx->tmp_frame, frame, 0);
    Debug(3, "vdpau_retrieve data %d\n", ret);
    if (ret < 0)
	return ret;

    ret = av_frame_copy_props(ctx->tmp_frame, frame);
    Debug(3, "vdpau_retrieve data %d\n", ret);
    if (ret < 0) {
	av_frame_unref(ctx->tmp_frame);
	return ret;
    }

    av_frame_unref(frame);
    av_frame_move_ref(frame, ctx->tmp_frame);

    return 0;
}
#endif

static int vdpau_alloc(AVCodecContext *s)
{
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,85,101)
    VideoDecoder *ist = s->opaque;
    VDPAUContext *ctx;
#endif
    int ret;

    AVHWDeviceContext *device_ctx;
    AVVDPAUDeviceContext *device_hwctx;
    AVHWFramesContext *frames_ctx;
    AVBufferRef *hw_frames_ctx;
    AVBufferRef *hw_device_ctx;

    Debug(3, "vdpau_alloc\n");
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,85,101)
    ctx = av_mallocz(sizeof(*ctx));
    if (!ctx) {
	Debug(3, "VDPAU init failed for av_malloccz\n");
	return AVERROR(ENOMEM);
    }

    ist->hwaccel_ctx = ctx;
    ist->hwaccel_uninit = vdpau_uninit;
//    ist->hwaccel_get_buffer = vdpau_get_buffer;
    ist->hwaccel_retrieve_data = vdpau_retrieve_data;

    ctx->tmp_frame = av_frame_alloc();
    if (!ctx->tmp_frame) {
	Debug(3, "VDPAU init failed for av_frame_alloc\n");
	goto fail;
    }
#endif
    hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VDPAU);
    if (!hw_device_ctx) {
	Debug(3, "VDPAU init failed for av_hwdevice_ctx_alloc\n");
	goto fail;
    }

    device_ctx = (AVHWDeviceContext*)hw_device_ctx->data;
    device_hwctx = device_ctx->hwctx;
    device_hwctx->device  = VdpauDevice;
    device_hwctx->get_proc_address = VdpauGetProcAddress;

    ret = av_hwdevice_ctx_init(hw_device_ctx);
    if (ret < 0) {
	Debug(3, "VDPAU init failed for av_hwdevice_ctx_init\n");
	goto fail;
    }

    hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
    if (!hw_frames_ctx) {
	Debug(3, "VDPAU init failed for av_hwframe_ctx_alloc\n");
	goto fail;
    }

    frames_ctx = (AVHWFramesContext*)hw_frames_ctx->data;
    frames_ctx->format = AV_PIX_FMT_VDPAU;
    frames_ctx->sw_format = s->sw_pix_fmt;
    frames_ctx->width = s->width;
    frames_ctx->height = s->height;
    frames_ctx->initial_pool_size = 16;

    ret = av_hwframe_ctx_init(hw_frames_ctx);

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,85,101)
    ctx->hw_device_ctx = hw_device_ctx;
    ctx->hw_frames_ctx = hw_frames_ctx;
#else
    s->hw_device_ctx = hw_device_ctx;
    s->hw_frames_ctx = hw_frames_ctx;
#endif
    if (av_vdpau_bind_context(s, VdpauDevice, VdpauGetProcAddress, AV_HWACCEL_FLAG_ALLOW_HIGH_DEPTH | AV_HWACCEL_FLAG_IGNORE_LEVEL)) {
	Debug(3, "VDPAU init failed for av_bind_context\n");
	goto fail;
    }
    return 0;
fail:
    Debug(3, "VDPAU init failed for stream #\n");
    vdpau_uninit(s);
    return AVERROR(EINVAL);
}

int vdpau_init(AVCodecContext *s)
{
    VideoDecoder *ist = s->opaque;

    if (ist->hwaccel_ctx)
	vdpau_uninit(s);
    if (!ist->hwaccel_ctx) {
	int ret = vdpau_alloc(s);
	if (ret < 0)
	    return ret;
    }
    return 0;
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param fmt	is the list of formats which are supported by the codec,
///			it is terminated by -1 as 0 is a valid format, the
///			formats are ordered by quality.
///
static enum AVPixelFormat Vdpau_get_format(VdpauDecoder * decoder,
    AVCodecContext * video_ctx, const enum AVPixelFormat *fmt)
{
    const enum AVPixelFormat *fmt_idx;
    VdpDecoderProfile profile;
    int max_refs;
    VideoDecoder *ist = video_ctx->opaque;

    Debug(3,"get format  %dx%d\n",video_ctx->width,video_ctx->height);

    if (!VideoHardwareDecoder || (video_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO
	    && VideoHardwareDecoder == HWmpeg2Off)
	) {				// hardware disabled by config
	Debug(3, "codec: hardware acceleration disabled\n");
	goto slow_path;
    }
    //
    //	look through formats
    //
    Debug(3, "%s: codec %d fmts:\n", __FUNCTION__, video_ctx->codec_id);
    for (fmt_idx = fmt; *fmt_idx != AV_PIX_FMT_NONE; fmt_idx++) {
        Debug(3, "\t%#010x %s\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx));
    }

    for (fmt_idx = fmt; *fmt_idx != AV_PIX_FMT_NONE; fmt_idx++) {
	// check supported pixel format with entry point
	switch (*fmt_idx) {
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,89,100)
	    case AV_PIX_FMT_VDPAU_H264:
	    case AV_PIX_FMT_VDPAU_MPEG1:
	    case AV_PIX_FMT_VDPAU_MPEG2:
	    case AV_PIX_FMT_VDPAU_WMV3:
	    case AV_PIX_FMT_VDPAU_VC1:
	    case AV_PIX_FMT_VDPAU_MPEG4:
#endif
	    case AV_PIX_FMT_VDPAU:
		break;
	    default:
		continue;
	}
	break;
    }
    Debug(3, "%s: selected codec:\n", __FUNCTION__);
    Debug(3, "\t%#010x %s\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx));

    if (*fmt_idx == AV_PIX_FMT_NONE) {
	Error(_("video/vdpau: no valid vdpau pixfmt found\n"));
	goto slow_path;
    }

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,85,101)
    if (ist->GetFormatDone) return *fmt_idx;
#else
    //VDPAU free hw_frames_ctx then avcodec_flush_buffers
    if (ist->GetFormatDone && video_ctx->hw_frames_ctx) return *fmt_idx;
#endif
    max_refs = CODEC_SURFACES_DEFAULT;
    // check profile

    switch (video_ctx->codec_id) {
	case AV_CODEC_ID_MPEG1VIDEO:
	    max_refs = CODEC_SURFACES_MPEG2;
	    profile = VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_MPEG1);
	    break;
	case AV_CODEC_ID_MPEG2VIDEO:
	    max_refs = CODEC_SURFACES_MPEG2;
	    profile = VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_MPEG2_MAIN);
	    break;
	case AV_CODEC_ID_MPEG4:
	case AV_CODEC_ID_H263:
	    /*
	       p = VaapiFindProfile(profiles, profile_n,
	       VAProfileMPEG4AdvancedSimple);
	     */
	    goto slow_path;
	case AV_CODEC_ID_H264:
	    // FIXME: can calculate level 4.1 limits
	    // vdpau supports only 16 references
	    max_refs = 16;
	    // try more simple formats, fallback to better
	    if (video_ctx->profile == FF_PROFILE_H264_BASELINE) {
		profile =
		    VdpauCheckProfile(decoder,
		    VDP_DECODER_PROFILE_H264_BASELINE);
		if (profile == VDP_INVALID_HANDLE) {
		    profile =
			VdpauCheckProfile(decoder,
			VDP_DECODER_PROFILE_H264_MAIN);
		}
		if (profile == VDP_INVALID_HANDLE) {
		    profile =
			VdpauCheckProfile(decoder,
			VDP_DECODER_PROFILE_H264_HIGH);
		}
	    } else if (video_ctx->profile == FF_PROFILE_H264_MAIN) {
		profile =
		    VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_H264_MAIN);
		if (profile == VDP_INVALID_HANDLE) {
		    profile =
			VdpauCheckProfile(decoder,
			VDP_DECODER_PROFILE_H264_HIGH);
		}
	    } else {
		profile =
		    VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_H264_MAIN);
	    }
	    break;
        case AV_CODEC_ID_HEVC:
            max_refs = 16;
            if (video_ctx->profile == FF_PROFILE_HEVC_MAIN_10) {
                Debug(3,"HEVC Profile Main 10 detected\n");
                profile =
                    VdpauCheckProfile(decoder,
                    VDP_DECODER_PROFILE_HEVC_MAIN_10);
            }
            else if (video_ctx->profile == FF_PROFILE_HEVC_MAIN) {
                Debug(3,"HEVC Profile Main detected\n");
                profile =
                    VdpauCheckProfile(decoder,
                    VDP_DECODER_PROFILE_HEVC_MAIN);
            }
            else {
                goto slow_path;
            }

            break;
	case AV_CODEC_ID_WMV3:
	    /*
	       p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Main);
	     */
	    goto slow_path;
	case AV_CODEC_ID_VC1:
	    /*
	       p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Advanced);
	     */
	    goto slow_path;
	default:
	    goto slow_path;
    }

    if (profile == VDP_INVALID_HANDLE) {
	Error(_("video/vdpau: no valid profile found\n"));
	goto slow_path;
    }

    Debug(3, "video/vdpau: create decoder profile=%d %dx%d #%d refs\n",
	profile, video_ctx->width, video_ctx->height, max_refs);

    decoder->Profile = profile;
    decoder->SurfacesNeeded = max_refs + VIDEO_SURFACES_MAX + 1;
    decoder->PixFmt = *fmt_idx;
    decoder->InputWidth = 0;
    decoder->InputHeight = 0;

    if (*fmt_idx == AV_PIX_FMT_VDPAU) { // HWACCEL used
	int ret;

	decoder->PixFmt = AV_PIX_FMT_VDPAU;
	ist->active_hwaccel_id = HWACCEL_VDPAU;
	ist->hwaccel_pix_fmt = AV_PIX_FMT_VDPAU;
	ist->hwaccel_output_format = AV_PIX_FMT_VDPAU;

	VdpauChromaType = VDP_CHROMA_TYPE_420;

	video_ctx->draw_horiz_band = NULL;
	video_ctx->slice_flags = 0;
	if (video_ctx->width && video_ctx->height) {
	    VdpStatus status;

	    VdpauCleanup(decoder);
	    status =
		VdpauDecoderCreate(VdpauDevice, profile, video_ctx->coded_width,
		video_ctx->coded_height, max_refs, &decoder->VideoDecoder);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't create decoder: %s\n"),
		    VdpauGetErrorString(status));
		goto slow_path;
	    }
	    ret = vdpau_init(video_ctx);  // init HWACCEL
	    if (ret < 0) {
		Debug(3, "vdpau_init failed\n");
		goto slow_path;
	    }
	    decoder->InputWidth = video_ctx->coded_width;
	    decoder->InputHeight = video_ctx->coded_height;
	    decoder->InputAspect = video_ctx->sample_aspect_ratio;
	    if(video_ctx->coded_height == 1088) {                                                // fix 1080i/1088i
		if((unsigned)decoder->VideoHeight == VideoWindowHeight)
		    decoder->VideoHeight += 8 * decoder->VideoHeight / 1080;
	    } else if ((unsigned)decoder->VideoHeight > VideoWindowHeight) decoder->VideoHeight = VideoWindowHeight;
	    VdpauSetupOutput(decoder);
	}

	Debug(3, "HWACCEL init ok\n");
        ist->GetFormatDone = 1;
	return AV_PIX_FMT_VDPAU;
    }
    else {

	VdpauChromaType = VDP_CHROMA_TYPE_420;
	ist->hwaccel_pix_fmt = AV_PIX_FMT_NONE;
	ist->hwaccel_get_buffer = NULL;
	ist->hwaccel_uninit = NULL;
	ist->hwaccel_retrieve_data = NULL;
	ist->active_hwaccel_id = HWACCEL_NONE;

#ifndef FFMPEG_BUG1_WORKAROUND
    if (video_ctx->width && video_ctx->height) {
	VdpStatus status;

	VdpauCleanup(decoder);

	status =
	    VdpauDecoderCreate(VdpauDevice, profile, video_ctx->width,
	    video_ctx->height, max_refs, &decoder->VideoDecoder);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't create decoder: %s\n"),
		VdpauGetErrorString(status));
	    goto slow_path;
	}

	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;

	VdpauSetupOutput(decoder);

    }
#endif
    }
    ist->GetFormatDone = 1;
    Debug(3, "\t%#010x %s\n", fmt_idx[0], av_get_pix_fmt_name(fmt_idx[0]));
    return *fmt_idx;

  slow_path:
    // no accelerated format found
    ist->hwaccel_get_buffer = NULL;
    decoder->Profile = VDP_INVALID_HANDLE;
    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX + 2;
    decoder->PixFmt = AV_PIX_FMT_NONE;
    video_ctx->draw_horiz_band = NULL;

    decoder->InputWidth = 0;
    decoder->InputHeight = 0;
    video_ctx->hwaccel_context = NULL;
    ist->GetFormatDone = 1;
    return avcodec_default_get_format(video_ctx, fmt);
}

#ifdef USE_GRAB

#ifdef DEBUG				// function not used

///
///	Grab video surface.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauGrabVideoSurface(VdpauDecoder * decoder)
{
    VdpVideoSurface surface;
    VdpStatus status;
    VdpChromaType chroma_type;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[3];
    uint32_t pitches[3];
    VdpYCbCrFormat format;

    // FIXME: test function to grab output surface content
    // for screen shots, atom light and auto crop.

    surface = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
	% VIDEO_SURFACES_MAX];

    //	get real surface size
    status =
	VdpauVideoSurfaceGetParameters(surface, &chroma_type, &width, &height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface parameters: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }
    switch (chroma_type) {
	case VDP_CHROMA_TYPE_420:
	case VDP_CHROMA_TYPE_422:
	case VDP_CHROMA_TYPE_444:
	    size = width * height + ((width + 1) / 2) * ((height + 1) / 2)
		+ ((width + 1) / 2) * ((height + 1) / 2);
	    // FIXME: can use auto-crop buffer cache
	    base = malloc(size);
	    if (!base) {
		Error(_("video/vdpau: out of memory\n"));
		return;
	    }
	    pitches[0] = width;
	    pitches[1] = width / 2;
	    pitches[2] = width / 2;
	    data[0] = base;
	    data[1] = base + width * height;
	    data[2] = base + width * height + width * height / 4;
	    format = VDP_YCBCR_FORMAT_YV12;
	    break;
	default:
	    Error(_("video/vdpau: unsupported chroma type %d\n"), chroma_type);
	    return;
    }
    status = VdpauVideoSurfaceGetBitsYCbCr(surface, format, data, pitches);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface bits: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }

    free(base);
}

#endif

///
///	Grab output surface already locked.
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *VdpauGrabOutputSurfaceLocked(int *ret_size, int *ret_width,
    int *ret_height)
{
    VdpOutputSurface surface;
    VdpStatus status;
    VdpRGBAFormat rgba_format;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[1];
    uint32_t pitches[1];
    VdpRect source_rect;
    VdpRect output_rect;

    surface = VdpauSurfacesRb[VdpauSurfaceIndex];

    //	get real surface size
    status =
	VdpauOutputSurfaceGetParameters(surface, &rgba_format, &width,
	&height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get output surface parameters: %s\n"),
	    VdpauGetErrorString(status));
	return NULL;
    }

    Debug(3, "video/vdpau: grab %dx%d format %d\n", width, height,
	rgba_format);

    source_rect.x0 = 0;
    source_rect.y0 = 0;
    source_rect.x1 = width;
    source_rect.y1 = height;

    if (ret_width && ret_height) {
	if (*ret_width <= -64) {	// this is an Atmo grab service request
	    int overscan;

	    // calculate aspect correct size of analyze image
	    width = *ret_width * -1;
	    height = (width * source_rect.y1) / source_rect.x1;

	    // calculate size of grab (sub) window
	    overscan = *ret_height;

	    if (overscan > 0 && overscan <= 200) {
		source_rect.x0 = source_rect.x1 * overscan / 1000;
		source_rect.x1 -= source_rect.x0;
		source_rect.y0 = source_rect.y1 * overscan / 1000;
		source_rect.y1 -= source_rect.y0;
	    }
	} else {
	    if (*ret_width > 0 && (unsigned)*ret_width < width) {
		width = *ret_width;
	    }
	    if (*ret_height > 0 && (unsigned)*ret_height < height) {
		height = *ret_height;
	    }
	}

	Debug(3, "video/vdpau: grab source rect %d,%d:%d,%d dest dim %dx%d\n",
	    source_rect.x0, source_rect.y0, source_rect.x1, source_rect.y1,
	    width, height);

	if ((source_rect.x1 - source_rect.x0) != width
	    || (source_rect.y1 - source_rect.y0) != height) {
	    output_rect.x0 = 0;
	    output_rect.y0 = 0;
	    output_rect.x1 = width;
	    output_rect.y1 = height;

	    status =
		VdpauOutputSurfaceRenderOutputSurface(VdpauGrabRenderSurface,
		&output_rect, surface, &source_rect, NULL, NULL,
		VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't render output surface: %s\n"),
		    VdpauGetErrorString(status));
		return NULL;
	    }

	    surface = VdpauGrabRenderSurface;
	    source_rect = output_rect;
#if 0
	    // FIXME: what if VdpauGrabRenderSurface has different sizes
	    //	get real surface size
	    status =
		VdpauOutputSurfaceGetParameters(surface, &rgba_format, &width,
		&height);
	    if (status != VDP_STATUS_OK) {
		Error(_
		    ("video/vdpau: can't get output surface parameters: %s\n"),
		    VdpauGetErrorString(status));
		return NULL;
	    }
	    if (width != output_rect.x1 || height != output_rect.y1) {
		// FIXME: this warning can be removed, is now for debug only
		Warning(_("video/vdpau: video surface size mismatch\n"));
	    }
#endif
	}
    }

    switch (rgba_format) {
	case VDP_RGBA_FORMAT_B8G8R8A8:
	case VDP_RGBA_FORMAT_R8G8B8A8:
	    size = width * height * sizeof(uint32_t);
	    base = malloc(size);
	    if (!base) {
		Error(_("video/vdpau: out of memory\n"));
		return NULL;
	    }
	    pitches[0] = width * sizeof(uint32_t);
	    data[0] = base;
	    break;
	case VDP_RGBA_FORMAT_R10G10B10A2:
	case VDP_RGBA_FORMAT_B10G10R10A2:
	case VDP_RGBA_FORMAT_A8:
	default:
	    Error(_("video/vdpau: unsupported rgba format %d\n"), rgba_format);
	    return NULL;
    }

    status =
	VdpauOutputSurfaceGetBitsNative(surface, &source_rect, data, pitches);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface bits native: %s\n"),
	    VdpauGetErrorString(status));
	free(base);
	return NULL;
    }

    if (ret_size) {
	*ret_size = size;
    }
    if (ret_width) {
	*ret_width = width;
    }
    if (ret_height) {
	*ret_height = height;
    }

    return base;
}

///
///	Grab output surface.
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *VdpauGrabOutputSurface(int *ret_size, int *ret_width,
    int *ret_height)
{
    uint8_t *img;

    if (VdpauGrabRenderSurface == VDP_INVALID_HANDLE) {
	return NULL;			// vdpau video module not yet initialized
    }

    pthread_mutex_lock(&VdpauGrabMutex);
    img = VdpauGrabOutputSurfaceLocked(ret_size, ret_width, ret_height);
    pthread_mutex_unlock(&VdpauGrabMutex);
    return img;
}

#endif

#ifdef USE_AUTOCROP

///
///	VDPAU auto-crop support.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauAutoCrop(VdpauDecoder * decoder)
{
    VdpVideoSurface surface;
    VdpStatus status;
    VdpChromaType chroma_type;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[3];
    uint32_t pitches[3];
    int crop14;
    int crop16;
    int next_state;
    VdpYCbCrFormat format;

    surface = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
	% VIDEO_SURFACES_MAX];

    //	get real surface size (can be different)
    status =
	VdpauVideoSurfaceGetParameters(surface, &chroma_type, &width, &height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface parameters: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }
    switch (chroma_type) {
	case VDP_CHROMA_TYPE_420:
	case VDP_CHROMA_TYPE_422:
	case VDP_CHROMA_TYPE_444:
	    size = width * height + ((width + 1) / 2) * ((height + 1) / 2)
		+ ((width + 1) / 2) * ((height + 1) / 2);
	    // cache buffer for reuse
	    base = decoder->AutoCropBuffer;
	    if (size > decoder->AutoCropBufferSize) {
		free(base);
		decoder->AutoCropBuffer = malloc(size);
		base = decoder->AutoCropBuffer;
		decoder->AutoCropBufferSize = size;
	    }
	    if (!base) {
		Error(_("video/vdpau: out of memory\n"));
		return;
	    }
	    pitches[0] = width;
	    pitches[1] = width / 2;
	    pitches[2] = width / 2;
	    data[0] = base;
	    data[1] = base + width * height;
	    data[2] = base + width * height + width * height / 4;
	    format = VDP_YCBCR_FORMAT_YV12;
	    break;
	default:
	    Error(_("video/vdpau: unsupported chroma type %d\n"), chroma_type);
	    return;
    }
    status = VdpauVideoSurfaceGetBitsYCbCr(surface, format, data, pitches);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface bits: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }

    AutoCropDetect(decoder->AutoCrop, width, height, data, pitches);

    // ignore black frames
    if (decoder->AutoCrop->Y1 >= decoder->AutoCrop->Y2) {
	return;
    }

    crop14 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 14);
    crop14 = (decoder->InputHeight - crop14) / 2;
    crop16 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 16);
    crop16 = (decoder->InputHeight - crop16) / 2;

    if (decoder->AutoCrop->Y1 >= crop16 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop16 - AutoCropTolerance) {
	next_state = 16;
    } else if (decoder->AutoCrop->Y1 >= crop14 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop14 - AutoCropTolerance) {
	next_state = 14;
    } else {
	next_state = 0;
    }

    if (decoder->AutoCrop->State == next_state) {
	return;
    }

    Debug(3, "video: crop aspect %d:%d %d/%d %d%+d\n",
	decoder->InputAspect.num, decoder->InputAspect.den, crop14, crop16,
	decoder->AutoCrop->Y1, decoder->InputHeight - decoder->AutoCrop->Y2);

    Debug(3, "video: crop aspect %d -> %d\n", decoder->AutoCrop->State,
	next_state);

    switch (decoder->AutoCrop->State) {
	case 16:
	case 14:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay / 2) {
		return;
	    }
	    break;
	case 0:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay) {
		return;
	    }
	    break;
    }

    decoder->AutoCrop->State = next_state;
    if (next_state) {
	decoder->CropX = VideoCutLeftRight[decoder->Resolution];
	decoder->CropY =
	    (next_state ==
	    16 ? crop16 : crop14) + VideoCutTopBottom[decoder->Resolution];
	decoder->CropWidth = decoder->InputWidth - decoder->CropX * 2;
	decoder->CropHeight = decoder->InputHeight - decoder->CropY * 2;

	// FIXME: this overwrites user choosen output position
	// FIXME: resize kills the auto crop values
	// FIXME: support other 4:3 zoom modes
	decoder->OutputX = decoder->VideoX;
	decoder->OutputY = decoder->VideoY;
	decoder->OutputWidth = (decoder->VideoHeight * next_state) / 9;
	decoder->OutputHeight = (decoder->VideoWidth * 9) / next_state;
	if (decoder->OutputWidth > decoder->VideoWidth) {
	    decoder->OutputWidth = decoder->VideoWidth;
	    decoder->OutputY +=
		(decoder->VideoHeight - decoder->OutputHeight) / 2;
	} else if (decoder->OutputHeight > decoder->VideoHeight) {
	    decoder->OutputHeight = decoder->VideoHeight;
	    decoder->OutputX +=
		(decoder->VideoWidth - decoder->OutputWidth) / 2;
	}
	Debug(3, "video: aspect output %dx%d %dx%d%+d%+d\n",
	    decoder->InputWidth, decoder->InputHeight, decoder->OutputWidth,
	    decoder->OutputHeight, decoder->OutputX, decoder->OutputY);
    } else {
	// sets AutoCrop->Count
	VdpauUpdateOutput(decoder);
    }
    decoder->AutoCrop->Count = 0;
}

///
///	VDPAU check if auto-crop todo.
///
///	@param decoder	VDPAU hw decoder
///
///	@note a copy of VaapiCheckAutoCrop
///	@note auto-crop only supported with normal 4:3 display mode
///
static void VdpauCheckAutoCrop(VdpauDecoder * decoder)
{
    // reduce load, check only n frames
    if (Video4to3ZoomMode == VideoNormal && AutoCropInterval
	&& !(decoder->FrameCounter % AutoCropInterval)) {
	AVRational input_aspect_ratio;
	AVRational tmp_ratio;

	av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den,
	    decoder->InputWidth * decoder->InputAspect.num,
	    decoder->InputHeight * decoder->InputAspect.den, 1024 * 1024);

	tmp_ratio.num = 4;
	tmp_ratio.den = 3;
	// 4:3 with 16:9/14:9 inside
	if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	    VdpauAutoCrop(decoder);
	} else {
	// 15:11 with 16:9/14:9 inside
	    tmp_ratio.num = 15;
	    tmp_ratio.den = 11;
	    if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	        VdpauAutoCrop(decoder);
	    } else {
	        decoder->AutoCrop->Count = 0;
	        decoder->AutoCrop->State = 0;
	    }
	}
    }
}

///
///	VDPAU reset auto-crop.
///
static void VdpauResetAutoCrop(void)
{
    int i;

    for (i = 0; i < VdpauDecoderN; ++i) {
	VdpauDecoders[i]->AutoCrop->State = 0;
	VdpauDecoders[i]->AutoCrop->Count = 0;
    }
}

#endif

///
///	Queue output surface.
///
///	@param decoder	VDPAU hw decoder
///	@param surface	output surface
///	@param softdec	software decoder
///
///	@note we can't mix software and hardware decoder surfaces
///
static void VdpauQueueSurface(VdpauDecoder * decoder, VdpVideoSurface surface,
    int softdec)
{
    VdpVideoSurface old;

    ++decoder->FrameCounter;

    if (1) {				// can't wait for output queue empty
	if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    Warning(_
		("video/vdpau: output buffer full, dropping frame (%d/%d)\n"),
		++decoder->FramesDropped, decoder->FrameCounter);
	    if (!(decoder->FramesDisplayed % 300)) {
		VdpauPrintFrames(decoder);
	    }
	    // software surfaces only
	    if (softdec) {
		VdpauReleaseSurface(decoder, surface);
	    }
	    return;
	}
#if 0
    } else {				// wait for output queue empty
	while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    VideoDisplayHandler();
	}
#endif
    }

    //
    //	    Check and release, old surface
    //
    if ((old = decoder->SurfacesRb[decoder->SurfaceWrite])
	!= VDP_INVALID_HANDLE) {

	// now we can release the surface, software surfaces only
	if (softdec) {
	    VdpauReleaseSurface(decoder, old);
	}
    }

    Debug(4, "video/vdpau: yy video surface %#08x@%d ready\n", surface,
	decoder->SurfaceWrite);

    decoder->SurfacesRb[decoder->SurfaceWrite] = surface;
    decoder->SurfaceWrite = (decoder->SurfaceWrite + 1)
	% VIDEO_SURFACES_MAX;
    atomic_inc(&decoder->SurfacesFilled);
}

///
///	Render a ffmpeg frame.
///
///	@param decoder		VDPAU hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VdpauRenderFrame(VdpauDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    VdpStatus status;
    VdpVideoSurface surface;
//    VideoDecoder        *ist = video_ctx->opaque;
//    AVFrame *output;
    int interlaced;
    AVRational aspect_ratio;

    // FIXME: some tv-stations toggle interlace on/off
    // frame->interlaced_frame isn't always correct set
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
    interlaced = frame->interlaced_frame;
#else
    interlaced = frame->flags & AV_FRAME_FLAG_INTERLACED;
#endif
    //25, 30 - interlaced; 50, 60 - progressive; hevc - progressive allways
    if (video_ctx->framerate.num > 0) {
	if (video_ctx->framerate.num / video_ctx->framerate.den > 30) interlaced = 0;
	else interlaced = 1;
    }

    if (video_ctx->codec_id == AV_CODEC_ID_HEVC) interlaced = 0;

    // FIXME: should be done by init video_ctx->field_order
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != frame->top_field_first) {

	Debug(4, "video/vdpau: interlaced %d top-field-first %d num %d den %d\n", frame->interlaced_frame,
	    frame->top_field_first, video_ctx->framerate.num, video_ctx->framerate.den);

	decoder->TopFieldFirst = frame->top_field_first;
#else
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)) {

	Debug(4, "video/vdpau: interlaced %d top-field-first %d num %d den %d\n", frame->flags & AV_FRAME_FLAG_INTERLACED,
	    frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST, video_ctx->framerate.num, video_ctx->framerate.den);

	decoder->TopFieldFirst = frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST;
#endif
	decoder->SurfaceField = !decoder->Interlaced;
	decoder->Interlaced = interlaced;
    }
    //aspect correction
    aspect_ratio = frame->sample_aspect_ratio;
    if ((video_ctx->width >= 352  && video_ctx->width <= 720 && video_ctx->height == 288) ||
	(video_ctx->width > 1300 && video_ctx->width <= 1920 && video_ctx->height == 540)) {
	aspect_ratio.den *= 2;
    }
    // update aspect ratio changes
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(53,60,100)
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, aspect_ratio)) {
	Debug(3, "video/vdpau: aspect ratio changed\n");

	decoder->InputAspect = aspect_ratio;
	VdpauUpdateOutput(decoder);
    }
#else
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, aspect_ratio)) {
	Debug(3, "video/vdpau: aspect ratio changed\n");

	decoder->InputAspect = aspect_ratio;
	VdpauUpdateOutput(decoder);
    }
#endif

    //
    // Hardware render
    //
    // VDPAU: AV_PIX_FMT_VDPAU_H264 .. AV_PIX_FMT_VDPAU_VC1 AV_PIX_FMT_VDPAU_MPEG4
#if LIBAVUTIL_VERSION_MAJOR < 56
    if ((AV_PIX_FMT_VDPAU_H264 <= video_ctx->pix_fmt
	    && video_ctx->pix_fmt <= AV_PIX_FMT_VDPAU_VC1)
	|| video_ctx->pix_fmt == AV_PIX_FMT_VDPAU
	|| video_ctx->pix_fmt == AV_PIX_FMT_VDPAU_MPEG4) {
	struct vdpau_render_state *vrs;
#endif
	if (video_ctx->pix_fmt == AV_PIX_FMT_VDPAU) {
	    surface = (VdpVideoSurface)(uintptr_t)frame->data[3];
	    Debug(4, "video/vdpau: hw render VDPAU surface from frame %#08x\n", surface);
#if LIBAVUTIL_VERSION_MAJOR < 56
	} else {
	    vrs = (struct vdpau_render_state *)frame->data[0];
	    surface = vrs->surface;
	    Debug(4, "video/vdpau: hw render hw surface from frame %#08x from buf%#08x\n", surface, vrs->surface);
	}
#endif
	if (interlaced
	    && VideoDeinterlace[decoder->Resolution] >=
	    VideoDeinterlaceSoftBob) {
	    // FIXME: software deinterlace avpicture_deinterlace
	    // FIXME: VdpauCpuDeinterlace(decoder, surface);
	    VdpauQueueSurface(decoder, surface, 0);
	} else {
	    VdpauQueueSurface(decoder, surface, 0);
	}
	//
	// PutBitsYCbCr render
	//
    } else {
	void const *data[3];
	uint32_t pitches[3];
//	uint8_t  *data[4],*p;
//	uint32_t pitches[4];

	//
	//	Check image, format, size
	//
	if (decoder->PixFmt != video_ctx->pix_fmt
	    || video_ctx->width != decoder->InputWidth
	    || video_ctx->height != decoder->InputHeight) {

	    decoder->PixFmt = video_ctx->pix_fmt;
	    decoder->InputWidth = video_ctx->width;
	    decoder->InputHeight = video_ctx->height;

	    VdpauCleanup(decoder);
	    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX + 2;

	    VdpauSetupOutput(decoder);
	}
	//
	//	Copy data from frame to image
	//
	switch (video_ctx->pix_fmt) {
	    case AV_PIX_FMT_YUV420P:
	    case AV_PIX_FMT_YUVJ420P:	// some streams produce this
	    case AV_PIX_FMT_YUV420P10LE:  // for softdecode of HEVC 10 Bit
	    case AV_PIX_FMT_YUV422P:
		break;
	    case AV_PIX_FMT_YUV444P:
	    default:
		Fatal(_("video/vdpau: pixel format %d not supported\n"),
		    video_ctx->pix_fmt);
		// FIXME: no fatals!
	}

	// convert ffmpeg order to vdpau
	data[0] = frame->data[0];
	data[1] = frame->data[2];
	data[2] = frame->data[1];
	pitches[0] = frame->linesize[0];
	pitches[1] = frame->linesize[2];
	pitches[2] = frame->linesize[1];

#ifdef USE_SWSCALE
	// Convert the image into YUV420 format
	if (video_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE) {
	    struct SwsContext *img_convert_ctx;
	    int w = video_ctx->width;
	    int h = video_ctx->height;
	    data[1] = frame->data[1];
	    data[2] = frame->data[2];
	    img_convert_ctx = sws_getContext(w, h,
		video_ctx->pix_fmt,
		w, h,  AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR,
		NULL, NULL, NULL);
	    if (img_convert_ctx == NULL) {
		Fatal(_("Cannot initialize the conversion context!\n"));
		exit(1);
	    }
	    sws_scale(img_convert_ctx, (const uint8_t * const*)frame->data,
		frame->linesize, 0,
		h,
		(uint8_t * const*)data, (const int *)pitches);
	    data[1] = frame->data[2];
	    data[2] = frame->data[1];
	    sws_freeContext(img_convert_ctx);
	}
#endif

	surface = VdpauGetSurface0(decoder);
	status =
	    VdpauVideoSurfacePutBitsYCbCr(surface, VDP_YCBCR_FORMAT_YV12, data,
	    pitches);
	if (status != VDP_STATUS_OK) {
	    Error(_("YV12 video/vdpau: can't put video surface bits: %s\n"),
		VdpauGetErrorString(status));
	}

	Debug(4, "video/vdpau: sw render hw surface %#08x\n", surface);

	VdpauQueueSurface(decoder, surface, 1);
    }

    if (decoder->Interlaced) {
	++decoder->FrameCounter;
    }
}

///
///	Get hwaccel context for ffmpeg.
///
///	@param decoder	VDPAU hw decoder
///
static void *VdpauGetHwAccelContext(VdpauDecoder * decoder)
{
    (void)decoder;

    // FIXME: new ffmpeg versions supports struct AVVDPAUContext
    Error(_("video: get hwaccel context, not supported\n"));
    return NULL;
}

///
///	Render osd surface to output surface.
///
static void VdpauMixOsd(void)
{
    VdpOutputSurfaceRenderBlendState blend_state;
    VdpRect source_rect;
    VdpRect output_rect;
    VdpRect output_double_rect;
    VdpStatus status;

    //uint32_t start;
    //uint32_t end;

    //
    //	blend overlay over output
    //
    blend_state.struct_version = VDP_OUTPUT_SURFACE_RENDER_BLEND_STATE_VERSION;
    blend_state.blend_factor_source_color =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_SRC_ALPHA;
    blend_state.blend_factor_source_alpha =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE;
    blend_state.blend_factor_destination_color =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_state.blend_factor_destination_alpha =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_state.blend_equation_color =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD;
    blend_state.blend_equation_alpha =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD;

    // use dirty area
    if (OsdDirtyWidth && OsdDirtyHeight) {
	source_rect.x0 = OsdDirtyX;
	source_rect.y0 = OsdDirtyY;
	source_rect.x1 = source_rect.x0 + OsdDirtyWidth;
	source_rect.y1 = source_rect.y0 + OsdDirtyHeight;

	output_rect.x0 = (OsdDirtyX * VideoWindowWidth) / OsdWidth;
	output_rect.y0 = (OsdDirtyY * VideoWindowHeight) / OsdHeight;
	output_rect.x1 =
	    output_rect.x0 + (OsdDirtyWidth * VideoWindowWidth) / OsdWidth;
	output_rect.y1 =
	    output_rect.y0 + (OsdDirtyHeight * VideoWindowHeight) / OsdHeight;
    } else {
	source_rect.x0 = 0;
	source_rect.y0 = 0;
	source_rect.x1 = OsdWidth;
	source_rect.y1 = OsdHeight;

	output_rect.x0 = 0;
	output_rect.y0 = 0;
	output_rect.x1 = VideoWindowWidth;
	output_rect.y1 = VideoWindowHeight;
    }

    output_double_rect = output_rect;

    switch (Osd3DMode) {
	case 1:
	    output_rect.x0 = output_rect.x0 / 2;
	    output_rect.x1 = output_rect.x1 / 2;
	    output_double_rect.x0 = output_rect.x0 + (VideoWindowWidth / 2);
	    output_double_rect.x1 = output_rect.x1 + (VideoWindowWidth / 2);
	    break;
	case 2:
	    output_rect.y0 = output_rect.y0 / 2;
	    output_rect.y1 = output_rect.y1 / 2;
	    output_double_rect.y0 = output_rect.y0 + (VideoWindowHeight / 2);
	    output_double_rect.y1 = output_rect.y1 + (VideoWindowHeight / 2);
	    break;
	default:
	    break;
    }

    //start = GetMsTicks();

    // FIXME: double buffered osd disabled
    VdpauOsdSurfaceIndex = 1;
#ifdef USE_BITMAP
    status =
	VdpauOutputSurfaceRenderBitmapSurface(VdpauSurfacesRb
	[VdpauSurfaceIndex], &output_rect,
	VdpauOsdBitmapSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL,
	VideoTransparentOsd ? &blend_state : NULL,
	VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render bitmap surface: %s\n"),
	    VdpauGetErrorString(status));
    }

    if (Osd3DMode > 0) {
	status =
	    VdpauOutputSurfaceRenderBitmapSurface(VdpauSurfacesRb
	    [VdpauSurfaceIndex], &output_double_rect,
	    VdpauOsdBitmapSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL,
	    VideoTransparentOsd ? &blend_state : NULL,
	    VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't render output surface: %s\n"),
		VdpauGetErrorString(status));
	}
    }
#else
    status =
	VdpauOutputSurfaceRenderOutputSurface(VdpauSurfacesRb
	[VdpauSurfaceIndex], &output_rect,
	VdpauOsdOutputSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL,
	VideoTransparentOsd ? &blend_state : NULL,
	VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render output surface: %s\n"),
	    VdpauGetErrorString(status));
    }

    if (Osd3DMode > 0) {
	status =
	    VdpauOutputSurfaceRenderOutputSurface(VdpauSurfacesRb
	    [VdpauSurfaceIndex], &output_double_rect,
	    VdpauOsdOutputSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL,
	    VideoTransparentOsd ? &blend_state : NULL,
	    VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't render output surface: %s\n"),
		VdpauGetErrorString(status));
	}
    }
#endif
    //end = GetMsTicks();
    /*
       Debug(4, "video:/vdpau: osd render %d %dms\n", VdpauOsdSurfaceIndex,
       end - start);
     */

    VdpauOsdSurfaceIndex = !VdpauOsdSurfaceIndex;
}

#ifdef USE_GLX
static void VdpauMixVideoGlx(VdpauDecoder * decoder, int level)
{
    glViewport(0, 0, VideoWindowWidth, VideoWindowHeight);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, VideoWindowWidth, VideoWindowHeight, 0.0, -1.0, 1.0);

    glVDPAUMapSurfacesNV (1,  &VdpauOutputSurfacesNV[VdpauSurfaceIndex]);
    GlxCheck();

    GlxRenderTexture(GlTextures[VdpauSurfaceIndex][0], decoder->OutputX,
	decoder->OutputY, decoder->OutputWidth, decoder->OutputHeight);
    GlxCheck();

    glVDPAUUnmapSurfacesNV (1,  &VdpauOutputSurfacesNV[VdpauSurfaceIndex]);
    GlxCheck();

    Debug(4,"video/vdpau: yy glx video surface %d displayed\n", VdpauSurfaceIndex);
    (void)level;
}
#endif

///
///	Render video surface to output surface.
///
///	@param decoder	VDPAU hw decoder
///	@param level	video surface level 0 = bottom
///
static void VdpauMixVideo(VdpauDecoder * decoder, int level)
{
    VdpVideoSurface current;
    VdpRect video_src_rect;
    VdpRect dst_rect;
    VdpRect dst_video_rect;
    VdpStatus status;

#ifdef USE_AUTOCROP
    // FIXME: can move to render frame
    VdpauCheckAutoCrop(decoder);
#endif
#ifdef USE_GLX
    if (GlxEnabled) {
	dst_rect.x0 = 0;
	dst_rect.y0 = 0;
	dst_rect.x1 = VideoWindowWidth;
	dst_rect.y1 = VideoWindowHeight;
	video_src_rect.x0 = decoder->CropX;
	video_src_rect.y0 = decoder->CropY;
	video_src_rect.x1 = decoder->CropX + decoder->CropWidth;
	video_src_rect.y1 = decoder->CropY + decoder->CropHeight;
	dst_video_rect.x0 = 0;
	dst_video_rect.y0 = 0;
	dst_video_rect.x1 = VideoWindowWidth;
	dst_video_rect.y1 = VideoWindowHeight;
    } else
#endif
    {
	if (level) {
	    dst_rect.x0 = decoder->VideoX;	// video window output (clip)
	    dst_rect.y0 = decoder->VideoY;
	    dst_rect.x1 = decoder->VideoX + decoder->VideoWidth;
	    dst_rect.y1 = decoder->VideoY + decoder->VideoHeight;
	} else {
	    dst_rect.x0 = 0;		// complete window (clip)
	    dst_rect.y0 = 0;
	    dst_rect.x1 = VideoWindowWidth;
	    dst_rect.y1 = VideoWindowHeight;
	}

	video_src_rect.x0 = decoder->CropX;	// video source (crop)
	video_src_rect.y0 = decoder->CropY;
	video_src_rect.x1 = decoder->CropX + decoder->CropWidth;
	video_src_rect.y1 = decoder->CropY + decoder->CropHeight;

	dst_video_rect.x0 = decoder->OutputX;	// video output (scale)
	dst_video_rect.y0 = decoder->OutputY;
	dst_video_rect.x1 = decoder->OutputX + decoder->OutputWidth;
	dst_video_rect.y1 = decoder->OutputY + decoder->OutputHeight;
    }

    if (decoder->Interlaced
	&& VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceWeave) {
	//
	//	Build deinterlace structures
	//
	VdpVideoMixerPictureStructure cps;
	VdpVideoSurface past[3];
	int past_n;
	VdpVideoSurface future[3];
	int future_n;

#ifdef DEBUG
	if (atomic_read(&decoder->SurfacesFilled) < 3) {
	    Debug(3, "only %d\n", atomic_read(&decoder->SurfacesFilled));
	}
#endif
	// FIXME: can use VDP_INVALID_HANDLE to support less surface on start
	if (VideoDeinterlaceSurfaces == 5) {
	    past_n = 2;
	    future_n = 2;

	    // FIXME: wrong for bottom-field first
	    // read: past: B0 T0 current T1 future B1 T2 (0 1 2)
	    // read: past: T1 B0 current B1 future T2 B2 (0 1 2)
	    if (decoder->TopFieldFirst != decoder->SurfaceField) {
		cps = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD;

		past[1] = decoder->SurfacesRb[decoder->SurfaceRead];
		past[0] = past[1];
		current = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX];
		future[0] = current;
		future[1] = decoder->SurfacesRb[(decoder->SurfaceRead + 2)
		    % VIDEO_SURFACES_MAX];
		// FIXME: can support 1 future more
	    } else {
		cps = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_BOTTOM_FIELD;

		// FIXME: can support 1 past more
		past[1] = decoder->SurfacesRb[decoder->SurfaceRead];
		past[0] = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX];
		current = past[0];
		future[0] = decoder->SurfacesRb[(decoder->SurfaceRead + 2)
		    % VIDEO_SURFACES_MAX];
		future[1] = future[0];
	    }

	} else if (VideoDeinterlaceSurfaces == 4) {
	    past_n = 2;
	    future_n = 1;

	    // FIXME: wrong for bottom-field first
	    // read: past: B0 T0 current T1 future B1 (0 1 2)
	    // read: past: T1 B0 current B1 future T2 (0 1 2)
	    if (decoder->TopFieldFirst != decoder->SurfaceField) {
		cps = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD;

		past[1] = decoder->SurfacesRb[decoder->SurfaceRead];
		past[0] = past[1];
		current = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX];
		future[0] = current;
	    } else {
		cps = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_BOTTOM_FIELD;

		past[1] = decoder->SurfacesRb[decoder->SurfaceRead];
		past[0] = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX];
		current = past[0];
		future[0] = decoder->SurfacesRb[(decoder->SurfaceRead + 2)
		    % VIDEO_SURFACES_MAX];
	    }

	} else {
	    Error(_("video/vdpau: %d surface deinterlace unsupported\n"),
		VideoDeinterlaceSurfaces);
	}

	// FIXME: past_n, future_n here:
	Debug(4, " %02d	 %02d(%c%02d) %02d  %02d\n", past[1], past[0],
	    cps == VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD ? 'T' : 'B',
	    current, future[0], future[1]);

	if ((int)current < 0) goto end;
	// Render complex interlaced
	status =
	    VdpauVideoMixerRender(decoder->VideoMixer, VDP_INVALID_HANDLE,
	    NULL, cps, past_n, past, current, future_n, future,
	    &video_src_rect, VdpauSurfacesRb[VdpauSurfaceIndex], &dst_rect,
	    &dst_video_rect, 0, NULL);

    } else {
	if (decoder->Interlaced) {
	    current = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		% VIDEO_SURFACES_MAX];
	} else {
	    current = decoder->SurfacesRb[decoder->SurfaceRead];
	}

	if ((int)current < 0) goto end;
	// Render Progressive frame and simple interlaced
	status =
	    VdpauVideoMixerRender(decoder->VideoMixer, VDP_INVALID_HANDLE,
	    NULL, VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME, 0, NULL, current, 0,
	    NULL, &video_src_rect, VdpauSurfacesRb[VdpauSurfaceIndex],
	    &dst_rect, &dst_video_rect, 0, NULL);
    }
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render mixer: %s\n"),
	    VdpauGetErrorString(status));
    }
end:
    Debug(4, "video/vdpau: yy video surface %#08x@%d displayed\n", current,
	decoder->SurfaceRead);

#ifdef USE_GLX
    if (GlxEnabled) {
        VdpauMixVideoGlx(decoder, level);
    }
#endif
}

///
///	Create and display a black empty surface.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauBlackSurface(VdpauDecoder * decoder)
{
    VdpStatus status;
    VdpRect source_rect;
    VdpRect output_rect;

    source_rect.x0 = 0;
    source_rect.y0 = 0;
    source_rect.x1 = 0;
    source_rect.y1 = 0;

    if(((char*)decoder->Stream)[0]) {      //isPipStream == 1 PIP video
	output_rect.x0 = decoder->VideoX;
	output_rect.y0 = decoder->VideoY;
	output_rect.x1 = decoder->VideoWidth + decoder->VideoX;
	output_rect.y1 = decoder->VideoHeight + decoder->VideoY;
    } else {                                                 //Main video
	output_rect.x0 = 0;
	output_rect.y0 = 0;
	output_rect.x1 = VideoWindowWidth;
	output_rect.y1 = VideoWindowHeight;
    }
    // FIXME: double buffered osd disabled
    // VdpauOsdSurfaceIndex always 0 and only 0 valid
#ifdef USE_BITMAP
    status =
	VdpauOutputSurfaceRenderBitmapSurface(VdpauSurfacesRb
	[VdpauSurfaceIndex], &output_rect,
	VdpauOsdBitmapSurface[VdpauOsdSurfaceIndex], &source_rect, NULL, NULL,
	VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
#else
    status =
	VdpauOutputSurfaceRenderOutputSurface(VdpauSurfacesRb
	[VdpauSurfaceIndex], &output_rect,
	VdpauOsdOutputSurface[VdpauOsdSurfaceIndex], &source_rect, NULL, NULL,
	VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
#endif
}

///
///	Advance displayed frame of decoder.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauAdvanceDecoderFrame(VdpauDecoder * decoder)
{
    // next surface, if complete frame is displayed (1 -> 0)
    if (decoder->SurfaceField) {
	int filled;

	// FIXME: this should check the caller
	// check decoder, if new surface is available
	// need 2 frames for progressive
	// need 4 frames for interlaced
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled <  1 + 2 * decoder->Interlaced) {
	    // keep use of last surface
	    ++decoder->FramesDuped;
	    // FIXME: don't warn after stream start, don't warn during pause
	    Debug(4,"video: display buffer empty, duping frame (%d/%d) %d\n",
		decoder->FramesDuped, decoder->FrameCounter,
		VideoGetBuffers(decoder->Stream));
	    return;
	}
	decoder->SurfaceRead = (decoder->SurfaceRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&decoder->SurfacesFilled);
	decoder->SurfaceField = !decoder->Interlaced;
	return;
    }
    // next field
    decoder->SurfaceField = 1;
}

///
///	Display a video frame.
///
static void VdpauDisplayFrame(void)
{
    VdpStatus status;
    VdpTime first_time;
    static VdpTime last_time;
    static int timeSS;
    int i;

    if (VideoSurfaceModesChanged) {	// handle changed modes
	VideoSurfaceModesChanged = 0;
	for (i = 0; i < VdpauDecoderN; ++i) {
	    if (VdpauDecoders[i]->VideoMixer != VDP_INVALID_HANDLE) {
		VdpauMixerSetup(VdpauDecoders[i]);
	    }
	}
    }
#ifdef USE_GLX
    if (GlxEnabled) {
	glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext);
	glClear(GL_COLOR_BUFFER_BIT);
	goto skip_query;
    }
#endif
    //
    //	check how many surfaces are queued
    //
    VdpauSurfaceQueued = 0;
    for (i = 0; i < OUTPUT_SURFACES_MAX; ++i) {
	VdpPresentationQueueStatus qstatus;

	status =
	    VdpauPresentationQueueQuerySurfaceStatus(VdpauQueue,
	    VdpauSurfacesRb[(VdpauSurfaceIndex + i) % OUTPUT_SURFACES_MAX],
	    &qstatus, &first_time);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't query status: %s\n"),
		VdpauGetErrorString(status));
	    break;
	}
	if (qstatus == VDP_PRESENTATION_QUEUE_STATUS_IDLE) {
	    continue;
	}
	// STATUS_QUEUED | STATUS_VISIBLE
	VdpauSurfaceQueued++;
    }
    //
    //	wait for surface no longer visible (blocks max ~5ms)
    //
    status =
	VdpauPresentationQueueBlockUntilSurfaceIdle(VdpauQueue,
	VdpauSurfacesRb[VdpauSurfaceIndex], &first_time);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't block queue: %s\n"),
	    VdpauGetErrorString(status));
    }

skip_query:
    // check if surface was displayed for more than 1 frame
    // FIXME: 21 only correct for 50Hz
    if (last_time && first_time > last_time + 21 * 1000 * 1000) {
	// FIXME: ignore still-frame, trick-speed
	Debug(3, "video/vdpau: %" PRId64 " display time %" PRId64 "\n",
	    first_time / 1000, (first_time - last_time) / 1000);
	// FIXME: can be more than 1 frame long shown
	for (i = 0; i < VdpauDecoderN; ++i) {
	    VdpauDecoders[i]->FramesMissed++;
	    VdpauMessage(2, _("video/vdpau: missed frame (%d/%d)\n"),
		VdpauDecoders[i]->FramesMissed,
		VdpauDecoders[i]->FrameCounter);
	}
    }

    last_time = first_time;
#ifdef USE_SCREENSAVER
    //get up screensaver every 20 sec
    if (DisableScreensaver) {
	if (first_time / 1000000000  - timeSS > 20) {
	    xcb_force_screen_saver(Connection,XCB_SCREEN_SAVER_RESET);
	    timeSS = first_time / 1000000000;
	}
    }
#endif
    //
    //	Render videos into output
    //
    for (i = 0; i < VdpauDecoderN; ++i) {
	int filled;
	VdpauDecoder *decoder;
#ifdef USE_GLX
	if (GlxEnabled)
	    VdpauSurfaceIndex = i;
#endif
	decoder = VdpauDecoders[i];
	decoder->FramesDisplayed++;
	decoder->StartCounter++;

	filled = atomic_read(&decoder->SurfacesFilled);
	// need 1 frame for progressive, 3 frames for interlaced
	if (filled < 1 + 2 * decoder->Interlaced && decoder->Closing) {
	    // FIXME: rewrite MixVideo to support less surfaces
	    if ((VideoShowBlackPicture && !decoder->TrickSpeed)
		|| (VideoShowBlackPicture && decoder->Closing < -300)) {
		VdpauBlackSurface(decoder);
		VdpauMessage(4, "video/vdpau: black surface displayed\n");
#ifdef USE_SCREENSAVER
		if (EnableDPMSatBlackScreen && DPMSDisabled) {
		    VdpauMessage(3, "Black surface, DPMS enabled\n");
		    X11DPMSReenable(Connection);
		    X11SuspendScreenSaver(Connection, 1);
		}
#endif
	    }
	    if (VideoShowBlackPicture)
		continue;
#ifdef USE_SCREENSAVER
	} else if (!DPMSDisabled) {	// always disable
	    VdpauMessage(3, "DPMS disabled\n");
	    X11DPMSDisable(Connection);
	    X11SuspendScreenSaver(Connection, 0);
#endif
	}
	VdpauMixVideo(decoder, i);
    }
#ifdef USE_GLX
    if (GlxEnabled) {
	//
	//	add OSD
	//
	if (OsdShown) {
            glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);

            glViewport(0, 0, VideoWindowWidth, VideoWindowHeight);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, VideoWindowWidth, VideoWindowHeight, 0.0, -1.0, 1.0);
            GlxCheck();
#ifdef USE_OPENGLOSD
            if(!DisableOglOsd && OsdGlTexture) {
                GlxRenderTexture(OsdGlTexture, 0,0, VideoWindowWidth, VideoWindowHeight);
            } else
#endif
	    GlxRenderTexture(OsdGlTextures[OsdIndex], 0, 0, VideoWindowWidth, VideoWindowHeight);
	    // FIXME: toggle osd
	}

	glXSwapBuffers(XlibDisplay, VideoWindow);

	glXMakeCurrent(XlibDisplay, None, NULL);
	GlxCheck();
	goto skip_queue;
    } else
#endif
    {
	//
	//	add osd to surface
	//
	if (OsdShown) {			// showing costs performance
	    VdpauMixOsd();
	}
   }
	//
	//	place surface in presentation queue
	//

	status =
	    VdpauPresentationQueueDisplay(VdpauQueue,
	    VdpauSurfacesRb[VdpauSurfaceIndex], 0, 0, 0);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't queue display: %s\n"),
		VdpauGetErrorString(status));
	}
skip_queue:
	// FIXME: CLOCK_MONOTONIC_RAW
	clock_gettime(CLOCK_MONOTONIC, &VdpauFrameTime);
	for (i = 0; i < VdpauDecoderN; ++i) {
	    // remember time of last shown surface
	    VdpauDecoders[i]->FrameTime = VdpauFrameTime;
	}

    VdpauSurfaceIndex = (VdpauSurfaceIndex + 1) % OUTPUT_SURFACES_MAX;

    xcb_flush(Connection);
}

///
///	Set VDPAU decoder video clock.
///
///	@param decoder	VDPAU hardware decoder
///	@param pts	audio presentation timestamp
///
void VdpauSetClock(VdpauDecoder * decoder, int64_t pts)
{
    decoder->PTS = pts;
}

///
///	Get VDPAU decoder video clock.
///
///	@param decoder	VDPAU hw decoder
///
///	FIXME: 20 wrong for 60hz dvb streams
///
static int64_t VdpauGetClock(const VdpauDecoder * decoder)
{
    // pts is the timestamp of the latest decoded frame
    if (decoder->PTS == (int64_t) AV_NOPTS_VALUE) {
	return AV_NOPTS_VALUE;
    }
    // subtract buffered decoded frames
    if (decoder->Interlaced) {
	/*
	   Info("video: %s =pts field%d #%d\n",
	   Timestamp2String(decoder->PTS),
	   decoder->SurfaceField,
	   atomic_read(&decoder->SurfacesFilled));
	 */
	// 1 field is future, 2 fields are past, + 2 in driver queue
	return decoder->PTS -
	    20 * 90 * (2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField - 2 + 2);
    }
    // + 2 in driver queue
    return decoder->PTS - 20 * 90 * (atomic_read(&decoder->SurfacesFilled) + 2);
}

///
///	Set VDPAU decoder closing stream flag.
///
///	@param decoder	VDPAU decoder
///
static void VdpauSetClosing(VdpauDecoder * decoder)
{
    decoder->Closing = 1;
}

///
///	Reset start of frame counter.
///
///	@param decoder	VDPAU decoder
///
static void VdpauResetStart(VdpauDecoder * decoder)
{
    decoder->StartCounter = 0;
}

///
///	Set trick play speed.
///
///	@param decoder	VDPAU decoder
///	@param speed	trick speed (0 = normal)
///
static void VdpauSetTrickSpeed(VdpauDecoder * decoder, int speed)
{
    decoder->TrickSpeed = speed;
    decoder->TrickCounter = speed;
    if (speed) {
	decoder->Closing = 0;
    }
}

///
///	Get VDPAU decoder statistics.
///
///	@param decoder		VDPAU decoder
///	@param[out] missed	missed frames
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void VdpauGetStats(VdpauDecoder * decoder, int *missed, int *duped,
    int *dropped, int *counter, int *dec)
{
    *missed = decoder->FramesMissed;
    *duped = decoder->FramesDuped;
    *dropped = decoder->FramesDropped;
    *counter = decoder->FrameCounter;
    *dec = (decoder->VideoDecoder != VDP_INVALID_HANDLE) ? HWACCEL_VDPAU : HWACCEL_NONE;
}

///
///	Sync decoder output to audio.
///
///	trick-speed	show frame <n> times
///	still-picture	show frame until new frame arrives
///	60hz-mode	repeat every 5th picture
///	video>audio	slow down video by duplicating frames
///	video<audio	speed up video by skipping frames
///	soft-start	show every second frame
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauSyncDecoder(VdpauDecoder * decoder)
{
    int err;
    int64_t audio_clock;
    int64_t video_clock;

    err = 0;
    video_clock = VdpauGetClock(decoder);
    audio_clock = AudioGetClock();

    if (!decoder->SyncOnAudio) {
	audio_clock = AV_NOPTS_VALUE;
	// FIXME: 60Hz Mode
	goto skip_sync;
    }

    EnoughVideo = (VideoGetBuffers(decoder->Stream) >= (VideoResolution == VideoResolution576i ? VideoStartThreshold_SD * !!VideoSoftStartSync : 0));
    if (!EnoughVideo && EnoughAudio && !AudioRunning && decoder->StartCounter > 200) {
	Debug (3,"video: force enough video\n");
	EnoughVideo = 1;
    }
    if (EnoughVideo && EnoughAudio && !AudioRunning && audio_clock != (int64_t) AV_NOPTS_VALUE) {
	Debug(3, "video: start audio after waiting for enough video: SurfacesFilled: %d, PacketsFilled: %d\n", atomic_read(&decoder->SurfacesFilled), VideoGetBuffers(decoder->Stream));
	AudioStarted = 1;
	AudioRunning = 1;
	pthread_cond_signal(&AudioStartCond);
    }

    // 60Hz: repeat every 5th field
    if (Video60HzMode && !(decoder->FramesDisplayed % 6)) {
	if (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock == (int64_t) AV_NOPTS_VALUE) {
	    goto out;
	}
	// both clocks are known
	if (audio_clock + VideoAudioDelay <= video_clock + 25 * 90) {
	    goto out;
	}
	// out of sync: audio before video
	if (!decoder->TrickSpeed) {
	    goto skip_sync;
	}
    }
    // TrickSpeed
    if (decoder->TrickSpeed) {
	if (decoder->TrickCounter--) {
	    goto out;
	}
	decoder->TrickCounter = decoder->TrickSpeed;
	goto skip_sync;
    }
    // at start of new video stream, soft or hard sync video to audio
    if (!IsReplay() && VideoSoftStartSync < 2 && decoder->StartCounter < VideoSoftStartFrames
	&& video_clock != (int64_t) AV_NOPTS_VALUE
	&& (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock > audio_clock + VideoAudioDelay + 120 * 90)) {
	err =
	    VdpauMessage(3, "video: initial slow down video, frame %d\n",
	    decoder->StartCounter);
	goto out;
    }

    if (decoder->SyncCounter && decoder->SyncCounter-- && VideoSoftStartSync > 1) {
	goto skip_sync;
    }

    if (audio_clock != (int64_t) AV_NOPTS_VALUE
	&& video_clock != (int64_t) AV_NOPTS_VALUE) {
	// both clocks are known
	int diff;
	int lower_limit;
	if (IsReplay()) {
	    if((video_clock > audio_clock + VideoAudioDelay + 8000 * 90) && decoder->StartCounter < VideoSoftStartFrames) { // 8s
		Debug(3,"flush audio\n");
		AudioFlushBuffers();
		goto out;
	    }
	    if((video_clock < audio_clock + VideoAudioDelay - 120 * 90) && VideoSoftStartSync < 2) {
		if (!(decoder->SurfaceField && atomic_read(&decoder->SurfacesFilled) < 1 + 2 * decoder->Interlaced)) {
		    Debug(3,"drop video\n");
		    VdpauAdvanceDecoderFrame(decoder);
		}
		goto skip_sync;
	    }
	}
	diff = video_clock - audio_clock - VideoAudioDelay;
	lower_limit = !IsReplay() ? -25 - (VideoResolution == VideoResolution576i ? 40 : 0) : 32;
	//diff = (decoder->LastAVDiff + diff) / 2;
	decoder->LastAVDiff = diff;
#ifdef DEBUG
	if (!decoder->SyncCounter && decoder->StartCounter < 1000)
	    Debug(3, "video/vdpau: diff %d %d lim %d fill %d\n", diff, diff/90, lower_limit, atomic_read(&decoder->SurfacesFilled));
#endif
	if (abs(diff) > 8000 * 90) {	// more than 8s
	    err = VdpauMessage(3, "video: audio/video difference too big\n");
	}
	if (diff > 100 * 90) {
	    // FIXME: this quicker sync step, did not work with new code!
	    err = VdpauMessage(3, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    if (VideoSoftStartSync > 1) {
		decoder->SyncCounter = diff > 100 * 90 ? diff % 2 : 1; //softsync :)
	    }
	    goto out;
	} else if (diff > 55 * 90) {
	    err = VdpauMessage(3, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    decoder->SyncCounter = 1;
	    goto out;
	} else if (diff < lower_limit * 90 && atomic_read(&decoder->SurfacesFilled) > 1 + decoder->Interlaced) { // double advance possible?
	    err = VdpauMessage(3, "video: speed up video, droping frame\n");
	    ++decoder->FramesDropped;
	    VdpauAdvanceDecoderFrame(decoder);
	    decoder->SyncCounter = 2;
	} else if (diff < lower_limit * 90 && atomic_read(&decoder->SurfacesFilled) < 1 + 2 * decoder->Interlaced && !IsReplay()) {
	    err = VdpauMessage(3, "video: speed up audio, delay audio\n");
	    AudioDelayms(-diff / 90 + 55);
	}
#if defined(DEBUG) || defined(AV_INFO)
	if (!decoder->SyncCounter && decoder->StartCounter < 1000) {
#ifdef DEBUG
	    Debug(3, "video/vdpau: synced after %d frames %dms\n",
		decoder->StartCounter, GetMsTicks() - VideoSwitch);
#else
	    Info("video/vdpau: synced after %d frames\n",
		decoder->StartCounter);
#endif
	    decoder->StartCounter += 1000;
	}
#endif
    }

  skip_sync:
    // is it not possible, to advance the surface and/or the field?
    if (decoder->SurfaceField && atomic_read(&decoder->SurfacesFilled) < 1 + 2 * decoder->Interlaced + 1 * !IsReplay()) {
	++decoder->FramesDuped;
	// FIXME: don't warn after stream start, don't warn during pause
	err =
	    VdpauMessage(3,
	    _("video: decoder buffer empty, "
	    "duping frame (%d/%d) %d v-buf closing %d\n"), decoder->FramesDuped,
	    decoder->FrameCounter, VideoGetBuffers(decoder->Stream), decoder->Closing);
	// some time no new picture or black video configured
	if (decoder->Closing < -300 || (VideoShowBlackPicture
	    && decoder->Closing)) {
	    // clear ring buffer to trigger black picture
	    atomic_set(&decoder->SurfacesFilled, 0);
	}
	goto out;
    }

    VdpauAdvanceDecoderFrame(decoder);
  out:
#if defined(DEBUG) || defined(AV_INFO)
    // debug audio/video sync
    if (err || !(decoder->FramesDisplayed % AV_INFO_TIME)) {
	if (!err) {
	    VdpauMessage(0, NULL);
	}
	Info("video: %s%+5" PRId64 " %4" PRId64 " %3d/\\ms %3d%+d%+d v-buf\n",
	    Timestamp2String(video_clock),
	    abs((int)(video_clock - audio_clock) / 90) <
	    8888 ? ((video_clock - audio_clock) / 90) : 8888,
	    AudioGetDelay() / 90, (int)VideoDeltaPTS / 90,
	    VideoGetBuffers(decoder->Stream),
	    decoder->Interlaced ? 2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField : atomic_read(&decoder->SurfacesFilled),
	    VdpauSurfaceQueued);
	if (!(decoder->FramesDisplayed % (5 * 60 * 60))) {
	    VdpauPrintFrames(decoder);
	}
    }
#endif
    return;				// fix gcc bug!
}

///
///	Sync a video frame.
///
static void VdpauSyncFrame(void)
{
    int i;

    //
    //	Sync video decoder to audio
    //
    for (i = 0; i < VdpauDecoderN; ++i) {
	VdpauSyncDecoder(VdpauDecoders[i]);
    }
}

///
///	Sync and display surface.
///
static void VdpauSyncDisplayFrame(void)
{
    VdpauDisplayFrame();
    VdpauSyncFrame();
}

///
///	Sync and render a ffmpeg frame
///
///	@param decoder		VDPAU hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VdpauSyncRenderFrame(VdpauDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    // FIXME: temp debug
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,61,100)
    if (0 && frame->pkt_pts != (int64_t) AV_NOPTS_VALUE) {
	Debug(3, "video: render frame pts %s\n",
	    Timestamp2String(frame->pkt_pts));
#else
    if (0 && frame->pts != (int64_t) AV_NOPTS_VALUE) {
	Debug(3, "video: render frame pts %s\n",
	    Timestamp2String(frame->pts));
#endif
    }
#ifdef DEBUG
    if (!atomic_read(&decoder->SurfacesFilled)) {
	Debug(3, "video: new stream frame %dms\n", GetMsTicks() - VideoSwitch);
    }
#endif

    if (VdpauPreemption) {		// display preempted
	if (!decoder->Closing) {
	    VideoSetPts(&decoder->PTS, decoder->Interlaced, video_ctx, frame);
	}
	return;
    }
#if 1
#ifndef USE_PIP
#error	"-DUSE_PIP or #define USE_PIP is needed,"
#endif
    // if video output buffer is full, wait and display surface.
    // loop for interlace
    if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	Info("video/vdpau: this code part shouldn't be used\n");
	return;
    }
#else
    // FIXME: disabled for remove
    // FIXME: wrong for multiple streams
    // FIXME: this part code should be no longer be needed with new mpeg fix
    while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	struct timespec abstime;

	pthread_mutex_unlock(&VideoLockMutex);

	abstime = decoder->FrameTime;
	abstime.tv_nsec += 14 * 1000 * 1000;
	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
	    // avoid overflow
	    abstime.tv_sec++;
	    abstime.tv_nsec -= 1000 * 1000 * 1000;
	}

	VideoPollEvent();

	// fix dead-lock with VdpauExit
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_mutex_lock(&VideoLockMutex);
	// give osd some time slot
	while (pthread_cond_timedwait(&VideoWakeupCond, &VideoLockMutex,
		&abstime) != ETIMEDOUT) {
	    // SIGUSR1
	    Debug(3, "video/vdpau: pthread_cond_timedwait error\n");
	}
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	if (VdpauPreemption) {		// display become preempted
	    return;
	}
	VdpauSyncDisplayFrame();
    }
#endif

    if (!decoder->Closing) {
	VideoSetPts(&decoder->PTS, decoder->Interlaced, video_ctx, frame);
    }
    VdpauRenderFrame(decoder, video_ctx, frame);
}

///
///	Recover from preemption.
///
static int VdpauPreemptionRecover(void)
{
    VdpStatus status;
    int i;

#ifdef USE_GLX
    if (GlxEnabled) goto skip_callback;
#endif
    status =
	vdp_device_create_x11(XlibDisplay, DefaultScreen(XlibDisplay),
	&VdpauDevice, &VdpauGetProcAddress);
    if (status != VDP_STATUS_OK) {
	VdpauPreemption = 1;
	return -1;
    }
    // VDPAU seems to loose the callback during preemption
    status =
	VdpauPreemptionCallbackRegister(VdpauDevice, VdpauPreemptionCallback,
	NULL);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't register preemption callback: %s\n"),
	    VdpauGetErrorString(status));
    }

skip_callback:
    VdpauPreemption = 0;
    Debug(3, "video/vdpau: display preemption recovery\n");

    VdpauInitOutputQueue();

    // mixer
    for (i = 0; i < VdpauDecoderN; ++i) {
	VdpauDecoders[i]->VideoDecoder = VDP_INVALID_HANDLE;
	VdpauDecoders[i]->VideoMixer = VDP_INVALID_HANDLE;
	VdpauDecoders[i]->SurfaceFreeN = 0;
	VdpauDecoders[i]->SurfaceUsedN = 0;
    }

    // FIXME: codec has still some surfaces used

    //
    //	invalid osd bitmap/output surfaces
    //
    for (i = 0; i < 1; ++i) {
#ifdef USE_BITMAP
	VdpauOsdBitmapSurface[i] = VDP_INVALID_HANDLE;
#else
	VdpauOsdOutputSurface[i] = VDP_INVALID_HANDLE;
#endif
    }

    VdpauOsdInit(OsdWidth, OsdHeight);

    return 1;
}

///
///	Set VDPAU background color.
///
///	@param rgba	32 bit RGBA color.
///
static void VdpauSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
}

///
///	Set video output position.
///
///	@param decoder	VDPAU hw decoder
///	@param x	video output x coordinate inside the window
///	@param y	video output y coordinate inside the window
///	@param width	video output width
///	@param height	video output height
///
///	@note FIXME: need to know which stream.
///
static void VdpauSetOutputPosition(VdpauDecoder * decoder, int x, int y,
    int width, int height)
{
    Debug(3, "video/vdpau: output %dx%d%+d%+d\n", width, height, x, y);

    decoder->VideoX = x;
    decoder->VideoY = y;
    decoder->VideoWidth = width;
    decoder->VideoHeight = height;

    // next video pictures are automatic rendered to correct position
}

///
///	Set VDPAU video mode.
///
static void VdpauSetVideoMode(void)
{
    int i;

    VdpauExitOutputQueue();

    VdpauInitOutputQueue();
    for (i = 0; i < VdpauDecoderN; ++i) {
	// reset video window, upper level needs to fix the positions
	VdpauDecoders[i]->VideoX = 0;
	VdpauDecoders[i]->VideoY = 0;
	VdpauDecoders[i]->VideoWidth = VideoWindowWidth;
	VdpauDecoders[i]->VideoHeight = VideoWindowHeight;
	// for window mode need restore scale
	if (VdpauDecoders[i]->ScaleX || VdpauDecoders[i]->ScaleY
	 || VdpauDecoders[i]->ScaleWidth || VdpauDecoders[i]->ScaleHeight) {
	    // convert OSD coordinates to window coordinates
	    int x = (VdpauDecoders[i]->ScaleX * VideoWindowWidth) / OsdWidth;
	    int width = (VdpauDecoders[i]->ScaleWidth * VideoWindowWidth) / OsdWidth;
	    int y = (VdpauDecoders[i]->ScaleY * VideoWindowHeight) / OsdHeight;
	    int height = (VdpauDecoders[i]->ScaleHeight * VideoWindowHeight) / OsdHeight;
	    VdpauSetOutputPosition(VdpauDecoders[i], x, y, width, height);
	}
	VdpauUpdateOutput(VdpauDecoders[i]);
    }
}

#ifdef USE_VIDEO_THREAD2

#else

#ifdef USE_VIDEO_THREAD

///
///	Handle a VDPAU display.
///
static void VdpauDisplayHandlerThread(void)
{
    int i;
    int err;
    int allfull;
    int decoded;
    struct timespec nowtime;
    VdpauDecoder *decoder;

    allfull = 1;
    decoded = 0;
    pthread_mutex_lock(&VideoLockMutex);
    for (i = 0; i < VdpauDecoderN; ++i) {
	int filled;

	decoder = VdpauDecoders[i];

	//
	// fill frame output ring buffer
	//
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled <= 1 + 2 * decoder->Interlaced) {
	    // FIXME: hot polling
	    // fetch+decode or reopen
	    allfull = 0;
	    err = VideoDecodeInput(decoder->Stream);
	} else {
	    err = VideoPollInput(decoder->Stream);
	}
	// decoder can be invalid here
	if (err) {
	    // nothing buffered?
	    if (err == -1 && decoder->Closing) {
		decoder->Closing--;
		if (!decoder->Closing) {
		    Debug(3, "video/vdpau: closing eof\n");
		    decoder->Closing = -1;
		}
	    }
	    continue;
	}
	decoded = 1;
    }
    pthread_mutex_unlock(&VideoLockMutex);

    if (!decoded) {			// nothing decoded, sleep
	// FIXME: sleep on wakeup
	usleep(1 * 1000);
    }
    // all decoder buffers are full
    // and display is not preempted
    // speed up filling display queue, wait on display queue empty
    if (!allfull || VdpauPreemption) {
	clock_gettime(CLOCK_MONOTONIC, &nowtime);
	// time for one frame over?
	if ((nowtime.tv_sec - VdpauFrameTime.tv_sec) * 1000 * 1000 * 1000 +
	    (nowtime.tv_nsec - VdpauFrameTime.tv_nsec) < 15 * 1000 * 1000) {
	    return;
	}
    }

    pthread_mutex_lock(&VideoLockMutex);
    VdpauSyncDisplayFrame();
    pthread_mutex_unlock(&VideoLockMutex);
}

#else

#define VdpauDisplayHandlerThread	NULL

#endif

#endif

//----------------------------------------------------------------------------
//	VDPAU OSD
//----------------------------------------------------------------------------

static const uint8_t OsdZeros[4096 * 2160 * 4];	///< 0 for clear osd

///
///	Clear subpicture image.
///
///	@note looked by caller
///
static void VdpauOsdClear(void)
{
    VdpStatus status;
    void const *data[1];
    uint32_t pitches[1];
    VdpRect dst_rect;

    if (VdpauPreemption) {		// display preempted
	return;
    }
    // osd image available?
#ifdef USE_BITMAP
    if (VdpauOsdBitmapSurface[VdpauOsdSurfaceIndex] == VDP_INVALID_HANDLE) {
	return;
    }
#else
    if (VdpauOsdOutputSurface[VdpauOsdSurfaceIndex] == VDP_INVALID_HANDLE) {
	return;
    }
#endif

    if (OsdWidth * OsdHeight > 4096 * 2160) {
	Error(_("video/vdpau: osd too big: unsupported\n"));
	return;
    }
    // have dirty area.
    if (OsdDirtyWidth && OsdDirtyHeight) {
	Debug(3, "video/vdpau: osd clear dirty %dx%d%+d%+d\n", OsdDirtyWidth,
	    OsdDirtyHeight, OsdDirtyX, OsdDirtyY);
	dst_rect.x0 = OsdDirtyX;
	dst_rect.y0 = OsdDirtyY;
	dst_rect.x1 = dst_rect.x0 + OsdDirtyWidth;
	dst_rect.y1 = dst_rect.y0 + OsdDirtyHeight;
    } else {
	Debug(3, "video/vdpau: osd clear image\n");
	dst_rect.x0 = 0;
	dst_rect.y0 = 0;
	dst_rect.x1 = dst_rect.x0 + OsdWidth;
	dst_rect.y1 = dst_rect.y0 + OsdHeight;
    }
    data[0] = OsdZeros;
    pitches[0] = OsdWidth * 4;

#ifdef USE_BITMAP
    status =
	VdpauBitmapSurfacePutBitsNative(VdpauOsdBitmapSurface
	[VdpauOsdSurfaceIndex], data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: bitmap surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }
#else
    status =
	VdpauOutputSurfacePutBitsNative(VdpauOsdOutputSurface
	[VdpauOsdSurfaceIndex], data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: output surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }
#endif
}

///
///	Upload ARGB to subpicture image.
///
///	@param xi	x-coordinate in argb image
///	@param yi	y-coordinate in argb image
///	@paran height	height in pixel in argb image
///	@paran width	width in pixel in argb image
///	@param pitch	pitch of argb image
///	@param argb	32bit ARGB image data
///	@param x	x-coordinate on screen of argb image
///	@param y	y-coordinate on screen of argb image
///
///	@note looked by caller
///
static void VdpauOsdDrawARGB(int xi, int yi, int width, int height, int pitch,
    const uint8_t * argb, int x, int y)
{
    VdpStatus status;
    void const *data[1];
    uint32_t pitches[1];
    VdpRect dst_rect;

#ifdef DEBUG
    uint32_t start;
    uint32_t end;
#endif

    if (VdpauPreemption) {		// display preempted
	return;
    }
    // osd image available?
#ifdef USE_BITMAP
    if (VdpauOsdBitmapSurface[VdpauOsdSurfaceIndex] == VDP_INVALID_HANDLE) {
	return;
    }
#else
    if (VdpauOsdOutputSurface[VdpauOsdSurfaceIndex] == VDP_INVALID_HANDLE) {
	return;
    }
#endif

#ifdef DEBUG
    start = GetMsTicks();
#endif

    dst_rect.x0 = x;
    dst_rect.y0 = y;
    dst_rect.x1 = dst_rect.x0 + width;
    dst_rect.y1 = dst_rect.y0 + height;
    data[0] = argb + xi * 4 + yi * pitch;
    pitches[0] = pitch;

#ifdef USE_BITMAP
    status =
	VdpauBitmapSurfacePutBitsNative(VdpauOsdBitmapSurface
	[VdpauOsdSurfaceIndex], data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: bitmap surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }
#else
    status =
	VdpauOutputSurfacePutBitsNative(VdpauOsdOutputSurface
	[VdpauOsdSurfaceIndex], data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: output surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }
#endif
#ifdef DEBUG
    end = GetMsTicks();

    Debug(3, "video/vdpau: osd upload %dx%d%+d%+d %dms %d\n", width, height, x,
	y, end - start, width * height * 4);
#endif
}

///
///	VDPAU initialize OSD.
///
///	@param width	osd width
///	@param height	osd height
///
static void VdpauOsdInit(int width, int height)
{
    int i;
    VdpStatus status;

    if (!VdpauDevice) {
	Debug(3, "video/vdpau: vdpau not setup\n");
	return;
    }

    //
    //	create bitmap/surface for osd
    //
#ifdef USE_BITMAP
    if (VdpauOsdBitmapSurface[0] == VDP_INVALID_HANDLE) {
	for (i = 0; i < 1; ++i) {
	    status =
		VdpauBitmapSurfaceCreate(VdpauDevice, VDP_RGBA_FORMAT_B8G8R8A8,
		width, height, VDP_TRUE, VdpauOsdBitmapSurface + i);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't create bitmap surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    Debug(4,
		"video/vdpau: created bitmap surface %dx%d with id 0x%08x\n",
		width, height, VdpauOsdBitmapSurface[i]);
	}
    }
#else
    if (VdpauOsdOutputSurface[0] == VDP_INVALID_HANDLE) {
	for (i = 0; i < 1; ++i) {
	    status =
		VdpauOutputSurfaceCreate(VdpauDevice, VDP_RGBA_FORMAT_B8G8R8A8,
		width, height, VdpauOsdOutputSurface + i);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't create output surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    Debug(4,
		"video/vdpau: created osd output surface %dx%d with id 0x%08x\n",
		width, height, VdpauOsdOutputSurface[i]);
	    Error(_("video/vdpau: created osd output surface %dx%d with id 0x%08x\n"),width, height, VdpauOsdOutputSurface[i]);
	}
    }
#endif
    Debug(3, "video/vdpau: osd surfaces created\n");
}

///
///	Cleanup osd.
///
static void VdpauOsdExit(void)
{
    int i;

    //
    //	destroy osd bitmap/output surfaces
    //
#ifdef USE_BITMAP
    for (i = 0; i < 1; ++i) {
	VdpStatus status;

	if (VdpauOsdBitmapSurface[i] != VDP_INVALID_HANDLE) {
	    status = VdpauBitmapSurfaceDestroy(VdpauOsdBitmapSurface[i]);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't destroy bitmap surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    VdpauOsdBitmapSurface[i] = VDP_INVALID_HANDLE;
	}
    }
#else
    for (i = 0; i < 1; ++i) {
	VdpStatus status;

	if (VdpauOsdOutputSurface[i] != VDP_INVALID_HANDLE) {
	    status = VdpauOutputSurfaceDestroy(VdpauOsdOutputSurface[i]);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't destroy output surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    VdpauOsdOutputSurface[i] = VDP_INVALID_HANDLE;
	}
    }
#endif
#ifdef USE_OPENGLOSD
    OsdNeedRestart = 1;
#endif
}

static int VdpauMaxPixmapSize (void)
{
	return MAX_PIXMAP_SIZE_VDPAU;
}

void *GetVDPAUDevice(void) {
    return (void*)(uintptr_t)VdpauDevice;
}
void *GetVDPAUProcAdress(void) {
    return (void*)(uintptr_t)VdpauGetProcAddress;
}

void *GetVDPAUOsdOutputSurface(void) {
#ifdef USE_OPENGLOSD
    OsdNeedRestart = 0;
#endif
#ifndef USE_BITMAP
    return (void*)(uintptr_t)VdpauOsdOutputSurface[VdpauOsdSurfaceIndex];
#else
    return NULL;
#endif
}

///
///	VDPAU module.
///
static const VideoModule VdpauModule = {
    .Name = "vdpau",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) VdpauNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))VdpauDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))VdpauGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))VdpauReleaseSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Vdpau_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))VdpauSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	VdpauGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))VdpauSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))VdpauGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))VdpauSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))VdpauResetStart,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))VdpauSetTrickSpeed,
#ifdef USE_GRAB
    .GrabOutput = VdpauGrabOutputSurface,
#endif
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, int *))VdpauGetStats,
    .SetBackground = VdpauSetBackground,
    .SetVideoMode = VdpauSetVideoMode,
#ifdef USE_AUTOCROP
    .ResetAutoCrop = VdpauResetAutoCrop,
#endif
    .DisplayHandlerThread = VdpauDisplayHandlerThread,
    .OsdClear = VdpauOsdClear,
    .OsdDrawARGB = VdpauOsdDrawARGB,
    .OsdInit = VdpauOsdInit,
    .OsdExit = VdpauOsdExit,
    .MaxPixmapSize = VdpauMaxPixmapSize,
    .Init = VdpauInit,
    .Exit = VdpauExit,
};

#ifdef USE_GLX

#ifdef USE_OPENGLOSD
void *GetVdpauGlxOsdOutputTexture(GLuint texture) {
    OsdGlTexture = texture;
    return 0;
}

int VdpauInitGlx(void) {
    int a = 0;

    if (!GlxEnabled) {
        Debug(3,"video/osd: can't create glx context\n");
        return 0;
    }
    //after run an external player from time to time vdr not set playmode 1
    //then try start GLX forced.
    //is it a vdr bug or external player plugin???
    while (!GlxSharedContext || !GlxThreadContext){
        usleep(1000);
        a++;
        if (a > 10 && a < 1000) {
            Debug(3,"Try start GLX forced\n");
            VideoDisplayWakeup();
            a = 1000;
        }
        if (a > 1010) return 0;
    }
    Debug(3,"Create OSD GLX context\n");
    glXMakeCurrent(XlibDisplay, None, NULL);
    glXMakeCurrent(XlibDisplay, VideoWindow, GlxSharedContext);
    return 1;
}
#endif

///
///	VDPAU GLX module.
///
static const VideoModule VdpauGlxModule = {
    .Name = "vdpau-glx",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) VdpauNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))VdpauDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))VdpauGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))VdpauReleaseSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Vdpau_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))VdpauSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	VdpauGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))VdpauSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))VdpauGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))VdpauSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))VdpauResetStart,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))VdpauSetTrickSpeed,
#ifdef USE_GRAB
    .GrabOutput = VdpauGrabOutputSurface,
#endif
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, int *))VdpauGetStats,
    .SetBackground = VdpauSetBackground,
    .SetVideoMode = VdpauSetVideoMode,
#ifdef USE_AUTOCROP
    .ResetAutoCrop = VdpauResetAutoCrop,
#endif
    .DisplayHandlerThread = VdpauDisplayHandlerThread,
    .OsdClear = GlxOsdClear,
    .OsdDrawARGB = GlxOsdDrawARGB,
    .OsdInit = GlxOsdInit,
    .OsdExit = GlxOsdExit,
    .MaxPixmapSize = GlxMaxPixmapSize,
    .Init = VdpauGlxInit,
    .Exit = VdpauExit,

};
#endif //GLX

#endif //VDPAU

//----------------------------------------------------------------------------
//	CUVID
//----------------------------------------------------------------------------

#ifdef USE_CUVID

///
///	CUVID decoder
///
typedef struct _cuvid_decoder_
{
     xcb_window_t Window;		///< output window

    int VideoX;				///< video base x coordinate
    int VideoY;				///< video base y coordinate
    int VideoWidth;			///< video base width
    int VideoHeight;			///< video base height

    int ScaleX;				///< video scale x coordinate
    int ScaleY;				///< video scale y coordinate
    int ScaleWidth;			///< video scale width
    int ScaleHeight;			///< video scale height

    int OutputX;			///< real video output x coordinate
    int OutputY;			///< real video output y coordinate
    int OutputWidth;			///< real video output width
    int OutputHeight;			///< real video output height

    enum AVPixelFormat PixFmt;		///< ffmpeg frame pixfmt
    enum AVColorSpace  ColorSpace;	/// ffmpeg ColorSpace
    enum AVColorTransferCharacteristic  trc;  // 
    enum AVColorPrimaries color_primaries;

    int Interlaced;			///< ffmpeg interlaced flag
    int TopFieldFirst;			///< ffmpeg top field displayed first

    int InputWidth;			///< video input width
    int InputHeight;			///< video input height
    AVRational InputAspect;		///< video input aspect ratio
    VideoResolutions Resolution;	///< resolution group

    int CropX;				///< video crop x
    int CropY;				///< video crop y
    int CropWidth;			///< video crop width
    int CropHeight;			///< video crop height

#ifdef USE_AUTOCROP
    void *AutoCropBuffer;		///< auto-crop buffer cache
    unsigned AutoCropBufferSize;	///< auto-crop buffer size
    AutoCropCtx AutoCrop[1];		///< auto-crop variables
#endif
    int SurfacesNeeded;			///< number of surface to request
    int SurfaceUsedN;			///< number of used video surfaces
    /// used video surface ids
    int SurfacesUsed[CODEC_SURFACES_MAX];
    int SurfaceFreeN;			///< number of free video surfaces
    /// free video surface ids
    int SurfacesFree[CODEC_SURFACES_MAX];
    /// video surface ring buffer
    int SurfacesRb[VIDEO_SURFACES_MAX *2];

    int SurfaceWrite;			///< write pointer
    int SurfaceRead;			///< read pointer
    atomic_t SurfacesFilled;		///< how many of the buffer is used

    CUarray cu_array[CODEC_SURFACES_MAX][2];
    CUgraphicsResource cu_res[CODEC_SURFACES_MAX][2];
    GLuint gl_textures[CODEC_SURFACES_MAX][2];  // where we will copy the CUDA result
    CUcontext cu_ctx;			///cuda context

    AVCodecContext *video_ctx;

    int SurfaceField;			///< current displayed field
    int TrickSpeed;			///< current trick speed
    int TrickCounter;			///< current trick speed counter
    struct timespec FrameTime;		///< time of last display
    VideoStream *Stream;		///< video stream
    int Closing;			///< flag about closing current stream
    int SyncOnAudio;			///< flag sync to audio
    int64_t PTS;			///< video PTS clock

    int LastAVDiff;			///< last audio - video difference
    int SyncCounter;			///< counter to sync frames
    int StartCounter;			///< counter for video start
    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
} CuvidDecoder;

static CuvidDecoder *CuvidDecoders[2];	///< open decoder streams
static int CuvidDecoderN;		///< number of decoder streams
static CudaFunctions *cu;
static CUdevice CuvidDevice;
static int CuvidSurfaceQueued;          ///< number of display surfaces queued

static struct timespec CuvidFrameTime;	///< time of last display

static pthread_mutex_t CuvidGrabMutex;

unsigned int size_tex_data;
unsigned int num_texels;
unsigned int num_values;

//----------------------------------------------------------------------------

///
///	Output video messages.
///
///	Reduce output.
///
///	@param level	message level (Error, Warning, Info, Debug, ...)
///	@param format	printf format string (NULL to flush messages)
///	@param ...	printf arguments
///
///	@returns true, if message shown
///
int CuvidMessage(int level, const char *format, ...)
{
    if (LogLevel > level || DebugLevel > level) {
	static const char *last_format;
	static char buf[256];
	va_list ap;

	va_start(ap, format);
	if (format != last_format) {	// don't repeat same message
	    if (buf[0]) {		// print last repeated message
		syslog(LOG_ERR, "%s", buf);
		buf[0] = '\0';
	    }

	    if (format) {
		last_format = format;
		vsyslog(LOG_ERR, format, ap);
	    }
	    va_end(ap);
	    return 1;
	}
	if (format)
	    vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
    }
    return 0;
}

//	Check CUDA errors ----------------------------------------------------

int CUStatus(CUresult err)
{
    if (CUDA_SUCCESS != err)
    {
        Error(_("Cuvid Driver API error = %04d"), err );
        return -1;
    }
    return 0;
}


//	Surfaces -------------------------------------------------------------

void CuvidCreateGlTexture(CuvidDecoder * decoder, unsigned int size_x, unsigned int size_y)
{
    int n, i, ret;

    if (GlxEnabled) {
        glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
        GlxCheck();
    }
#ifdef USE_EGL
    if (EglEnabled) {
        eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglThreadContext);
        EglCheck();;
    }
#endif
    // create texture planes
    for (n = 0; n < 2; n++) {       // number of planes
        glGenTextures(CODEC_SURFACES_MAX, decoder->gl_textures[n]);
        GlCheck();
    }
    Debug(3,"video/cuvid: create %d Textures Format %s w %d h %d \n",
        decoder->SurfacesNeeded, decoder->PixFmt != AV_PIX_FMT_P010LE && decoder->PixFmt != AV_PIX_FMT_YUV420P10LE ? "NV12/YV12" : "P010", size_x, size_y);

    for (i = 0; i < decoder->SurfacesNeeded; i++) {
        for (n = 0; n < 2; n++) {   // number of planes

            glBindTexture(GL_TEXTURE_2D, decoder->gl_textures[i][n]);
            GlCheck();
            // set basic parameters
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            if (decoder->PixFmt != AV_PIX_FMT_P010LE && decoder->PixFmt != AV_PIX_FMT_YUV420P10LE)
                glTexImage2D(GL_TEXTURE_2D, 0, n == 0 ? GL_R8 : GL_RG8, n == 0 ? size_x : size_x/2, n == 0 ? size_y : size_y/2, 0,
                     n == 0 ? GL_RED : GL_RG, GL_UNSIGNED_BYTE, NULL);
            else
                glTexImage2D(GL_TEXTURE_2D, 0, n == 0 ? GL_R16 : GL_RG16, n == 0 ? size_x : size_x/2, n == 0 ? size_y : size_y/2, 0,
                    n == 0 ? GL_RED : GL_RG, GL_UNSIGNED_SHORT, NULL);

            GlCheck();
            // register this texture with CUDA, not need for software decoder YV12
            if (decoder->PixFmt == AV_PIX_FMT_NV12 || decoder->PixFmt == AV_PIX_FMT_P010LE) {
                ret = CUStatus(cu->cuGraphicsGLRegisterImage(&decoder->cu_res[i][n], decoder->gl_textures[i][n],
                    GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD));
                if (ret < 0)
                    Fatal(_("video/cuvid: cuGraphicsGLRegisterImage failed \n"));
                ret = CUStatus(cu->cuGraphicsMapResources(1, &decoder->cu_res[i][n], 0));
                if (ret < 0)
                    Fatal(_("video/cuvid: cuGraphicsMapResources failed \n"));
                ret = CUStatus(cu->cuGraphicsSubResourceGetMappedArray(&decoder->cu_array[i][n], decoder->cu_res[i][n], 0, 0));
                if (ret < 0)
                    Fatal(_("video/cuvid: cuGraphicsSubResourceGetMappedArray failed \n"));
                ret = CUStatus(cu->cuGraphicsUnmapResources(1, &decoder->cu_res[i][n], 0));
                if (ret < 0)
                    Fatal(_("video/cuvid: cuGraphicsUnmapResources failed \n"));
            }
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    GlCheck();
}

///
///	Create surfaces for CUVID decoder.
///
///	@param decoder	CUVID hw decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void CuvidCreateSurfaces(CuvidDecoder * decoder, int width, int height)
{
    int i;

#ifdef DEBUG
    if (!decoder->SurfacesNeeded) {
	Error(_("video/cuvid: surface needed not set\n"));
	decoder->SurfacesNeeded = VIDEO_SURFACES_MAX * 2;
    }
#endif
    Debug(3, "video/cuvid: %s: %dx%d * %d\n", __FUNCTION__, width, height,
	decoder->SurfacesNeeded);

    // allocate only the number of needed surfaces
    decoder->SurfaceFreeN = decoder->SurfacesNeeded;

    CuvidCreateGlTexture(decoder, width, height);

    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	    decoder->SurfacesFree[i] = i;
	}
	Debug(4, "video/cuvid: created video surface %dx%d with id 0x%08x\n",
	    width, height, decoder->SurfacesFree[i]);
}

///
///	You need to unregister the CUVID resource before closing the codec
//	otherwise you will get a video memory leak
///
static void CuvidUnregisterSurface(CuvidDecoder * decoder)
{
    int i,n;

    //not need for softdecoder
    if (decoder->PixFmt != AV_PIX_FMT_NV12 && decoder->PixFmt != AV_PIX_FMT_P010LE) return;

    if(GlxEnabled) {
        if(GlxThreadContext){
            glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
            GlxCheck();
        }
    }
#ifdef USE_EGL
    if (EglEnabled) {
        if(EglThreadContext){
            eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglThreadContext);
            EglCheck();
        }
    }
#endif
    for (i = 0; i < decoder->SurfacesNeeded; i++) {
        for (n = 0; n < 2; n++) {   // number of planes
            if (decoder->cu_res[i][n]) {
                CUStatus(cu->cuGraphicsUnregisterResource(decoder->cu_res[i][n]));
                decoder->cu_res[i][n] = NULL;
            }
        }
    }
}

///
///	Destroy surfaces of CUVID decoder.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidDestroySurfaces(CuvidDecoder * decoder)
{
    int i;

    if(GlxEnabled) {
        if(GlxThreadContext){
            glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
            GlxCheck();
        }
    }
#ifdef USE_EGL
    if (EglEnabled) {
        if(EglThreadContext){
            eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglThreadContext);
            EglCheck();;
        }
    }
#endif
    for (i = 0; i < 2; i++) {       // number of planes
        glDeleteTextures(CODEC_SURFACES_MAX, decoder->gl_textures[i]);
        GlCheck();
    }
    if (decoder == CuvidDecoders[0]) {   // only when last decoder closes
        Debug(3,"Last decoder closes\n");
        if (gl_prog)
            glDeleteProgram(gl_prog);
        gl_prog = 0;
    }

    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	Debug(4, "video/cuvid: destroy video surface with id 0x%08x\n",
	    decoder->SurfacesFree[i]);
	decoder->SurfacesFree[i] = -1;
    }
    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	Debug(4, "video/cuvid: destroy video surface with id 0x%08x\n",
	    decoder->SurfacesUsed[i]);
	decoder->SurfacesUsed[i] = -1;
    }
    decoder->SurfaceFreeN = 0;
    decoder->SurfaceUsedN = 0;
}

///
///	Get a free surface.
///
///	@param decoder	CUVID hw decoder
///
///	@returns the oldest free surface
///
static unsigned CuvidGetSurface0(CuvidDecoder * decoder)
{
    int surface;
    int i;

    if (!decoder->SurfaceFreeN) {
	Error(_("video/cuvid: out of surfaces\n"));
	return -1;
    }
    // use oldest surface
    surface = decoder->SurfacesFree[0];

    decoder->SurfaceFreeN--;
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	decoder->SurfacesFree[i] = decoder->SurfacesFree[i + 1];
    }
    decoder->SurfacesFree[i] = -1;

    // save as used
    decoder->SurfacesUsed[decoder->SurfaceUsedN++] = surface;

    return surface;
}

///
///	Release a surface.
///
///	@param decoder	CUVID hw decoder
///	@param surface	surface no longer used
///
static void CuvidReleaseSurface(CuvidDecoder * decoder, int surface)
{
    int i;

    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	if (decoder->SurfacesUsed[i] == surface) {
	    // no problem, with last used
	    decoder->SurfacesUsed[i] =
		decoder->SurfacesUsed[--decoder->SurfaceUsedN];
	    decoder->SurfacesFree[decoder->SurfaceFreeN++] = surface;
	    return;
	}
    }
    Error(_("video/cuvid: release surface %#08x, which is not in use\n"),
	surface);
}

///
///	Debug CUVID decoder frames drop...
///
///	@param decoder	CUVID hw decoder
///
static void CuvidPrintFrames(const CuvidDecoder * decoder)
{
    Debug(3, "video/cuvid: %d missed, %d duped, %d dropped frames of %d,%d\n",
	decoder->FramesMissed, decoder->FramesDuped, decoder->FramesDropped,
	decoder->FrameCounter, decoder->FramesDisplayed);
#ifndef DEBUG
    (void)decoder;
#endif
}

static void CuvidMixerSetup(CuvidDecoder * decoder)
{
    if (decoder->video_ctx) {
        VideoDecoder *ist = decoder->video_ctx->opaque;

        if (ist->hwaccel_pix_fmt == AV_PIX_FMT_CUDA) {
            int mode = 0;
            int drop = 0;

            if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceWeave) {
                Debug(3, "video/cuvid: set weave");
                mode = 0;
                drop = 0;
            } else if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceBob) {
                Debug(3, "video/cuvid: set bob");
                mode = 1;
                drop = 0;
            } else if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceTemporal) {
                Debug(3, "video/cuvid: set adap");
                mode = 2;
                drop = 0;
            }
            if (av_opt_set_int(decoder->video_ctx->priv_data, "deint", mode, 0) < 0)
                Error(_("Can't set deinterlace mode\n"));
            if (av_opt_set(decoder->video_ctx->priv_data, "drop_second_field", drop ? "true" : "false", 0) < 0)
                Error(_("Can't set drop second field\n"));
        } else { //soft deinterlace
#ifdef USE_AVFILTER
            if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceWeave) {
                Debug(3, "video/cuvid: set weave");
            } else if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceBob) {
                Debug(3, "video/cuvid: set yadif");
                if (decoder->video_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO)
                    CodecVideoInitFilter(ist, "yadif=1:-1:0");
                else
                    CodecVideoInitFilter(ist, "yadif=1:-1:1");
            } else if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceTemporal) {
                Debug(3, "video/cuvid: set bwdif");
                if (decoder->video_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO)
                    CodecVideoInitFilter(ist, "bwdif=1:-1:0");
                else
                    CodecVideoInitFilter(ist, "bwdif=1:-1:1");
            }
#endif
        }
    }
}

///
///	Allocate new CUVID decoder.
///
///	@param stream	video stream
///
///	@returns a new prepared cuvid hardware decoder.
///
static CuvidDecoder *CuvidNewHwDecoder(VideoStream * stream)
{
    CuvidDecoder *decoder;
    int i;

    Debug(3, "video/cuvid: %s\n", __FUNCTION__);
    if ((unsigned)CuvidDecoderN >=
	sizeof(CuvidDecoders) / sizeof(*CuvidDecoders)) {
    	Error(_("video/cuvid: out of decoders\n"));
	return NULL;
    }

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
	Error(_("video/cuvid: out of memory\n"));
	return NULL;
    }
    decoder->Window = VideoWindow;
    decoder->VideoWidth = VideoWindowWidth;
    decoder->VideoHeight = VideoWindowHeight;

    for (i = 0; i < CODEC_SURFACES_MAX; ++i) {
	decoder->SurfacesUsed[i] = -1;
	decoder->SurfacesFree[i] = -1;
    }

    //
    // setup video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < (VIDEO_SURFACES_MAX * 2); ++i) {
	decoder->SurfacesRb[i] = -1;
    }

#ifdef DEBUG
    if ((VIDEO_SURFACES_MAX) < 1 + 1 + 1 + 1) {
	Error(_
	    ("video/cuvid: need 1 future, 1 current, 1 back and 1 work surface\n"));
    }
#endif

    // Procamp operation parameterization data
//    decoder->Procamp.struct_version = VDP_PROCAMP_VERSION;
//    decoder->Procamp.brightness = 0.0;
//    decoder->Procamp.contrast = 1.0;
//    decoder->Procamp.saturation = 1.0;
//    decoder->Procamp.hue = 0.0;		// default values

    decoder->OutputWidth = VideoWindowWidth;
    decoder->OutputHeight = VideoWindowHeight;

    decoder->PixFmt = AV_PIX_FMT_NONE;

#ifdef USE_AUTOCROP
    //decoder->AutoCropBuffer = NULL;	// done by calloc
    //decoder->AutoCropBufferSize = 0;
#endif

    decoder->Stream = stream;
    if (!CuvidDecoderN) {		// FIXME: hack sync on audio
	decoder->SyncOnAudio = 1;
    }
    decoder->Closing = -300 - 1;

    decoder->PTS = AV_NOPTS_VALUE;

    CuvidDecoders[CuvidDecoderN++] = decoder;

    decoder->SurfaceUsedN = 1; // for correct cleanup after resume

    if (CUStatus(cu->cuCtxCreate(&decoder->cu_ctx, (unsigned int) CU_CTX_SCHED_BLOCKING_SYNC, CuvidDevice)))
        Error(_("video/cuvid: CUDA context create failed"));

    Debug(3, "CUDA API Version %d\n", CUDA_VERSION);

    return decoder;
}

///
///	Cleanup CUVID.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidCleanup(CuvidDecoder * decoder)
{
    int i;
    Debug(3, "video/cuvid: %s\n", __FUNCTION__);
    if (decoder->SurfaceFreeN || decoder->SurfaceUsedN) {
	CuvidDestroySurfaces(decoder);
    }
    //
    // reset video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX * 2; ++i) {
	decoder->SurfacesRb[i] = -1;
    }

    decoder->SurfaceRead = 0;
    decoder->SurfaceWrite = 0;
    decoder->SurfaceField = 0;
    decoder->SyncCounter = 0;
    decoder->Interlaced = 0;
    decoder->FrameCounter = 0;
    decoder->FramesDisplayed = 0;
    decoder->StartCounter = 0;
    decoder->Closing = 0;
    decoder->PTS = AV_NOPTS_VALUE;
    VideoDeltaPTS = 0;
}

///
///	Destroy a CUVID decoder.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidDelHwDecoder(CuvidDecoder * decoder)
{
    int i;
    Debug(3, "video/cuvid: %s\n", __FUNCTION__);
    for (i = 0; i < CuvidDecoderN; ++i) {
	if (CuvidDecoders[i] == decoder) {
	    CuvidDecoders[i] = NULL;
	    // copy last slot into empty slot
	    if (i < --CuvidDecoderN) {
		CuvidDecoders[i] = CuvidDecoders[CuvidDecoderN];
	    }

	    CuvidCleanup(decoder);
	    CuvidPrintFrames(decoder);

            if (decoder->cu_ctx) {
                cu->cuCtxDestroy (decoder->cu_ctx);
                decoder->cu_ctx = NULL;
            }
#ifdef USE_AUTOCROP
	    free(decoder->AutoCropBuffer);
#endif
	    free(decoder);

	    return;
	}
    }
    Error(_("video/cuvid: decoder not in decoder list.\n"));
}

///
///	CUVID setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns true if CUVID could be initialized, false otherwise.
///
static int CuvidInit(const char *display_name)
{
    int ret;

    Info(_("video/cuvid: Start CUVID\n"));

    GlxEnabled = 1;

    GlxInit();
    if (GlxEnabled) {
        GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight, GlxSharedContext);
    }
    if (!GlxEnabled) {
        Error(_("video/cuvid: GLX error\n"));
        return 0;
    }

    glGenBuffers(1,&vao_buffer);
    GlxCheck();

    if (!cu) {
        ret = cuda_load_functions(&cu, NULL);
        if (ret < 0) {
            Error(_("Could not dynamically load CUDA\n"));
            return 0;
        }
    }
    ret = CUStatus(cu->cuInit(0));
    if (ret < 0) {
        Error(_("Could not init CUDA device\n"));
        return 0;
    }
    ret = CUStatus(cu->cuDeviceGet(&CuvidDevice, 0));
    if (ret < 0) {
        Error(_("Could not get CUDA device\n"));
        return 0;
    }
    pthread_mutex_init(&CuvidGrabMutex, NULL);

    Info(_("video/cuvid: Start CUVID ok\n"));

    (void)display_name;
    return 1;
}

#ifdef USE_EGL
static int CuvidEglInit(const char *display_name)
{
    int ret;

    Info(_("video/cuvid: Start CUVID\n"));

    EglEnabled = 1;

    EglInit();
    if (EglEnabled) {
	EglSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight,
	    EglSharedContext);
    }
    if (!EglEnabled) {
        Error(_("video/cuvid: EGL error\n"));
        return 0;
    }
    if (!cu) {
        ret = cuda_load_functions(&cu, NULL);
        if (ret < 0) {
            Error(_("Could not dynamically load CUDA\n"));
            return 0;
        }
    }
    ret = CUStatus(cu->cuInit(0));
    if (ret < 0) {
        Error(_("Could not init CUDA device\n"));
        return 0;
    }
    ret = CUStatus(cu->cuDeviceGet(&CuvidDevice, 0));
    if (ret < 0) {
        Error(_("Could not get CUDA device\n"));
        return 0;
    }
    pthread_mutex_init(&CuvidGrabMutex, NULL);

    Info(_("video/cuvid: Start CUVID ok\n"));

    (void)display_name;
    return 1;
}
#endif


///
///	CUVID cleanup.
///
static void CuvidExit(void)
{
    int i;
    Debug(3, "video/cuvid: %s\n", __FUNCTION__);

    for (i = 0; i < CuvidDecoderN; ++i) {
	if (CuvidDecoders[i]) {
	    CuvidDelHwDecoder(CuvidDecoders[i]);
	    CuvidDecoders[i] = NULL;
	}
    }
    CuvidDecoderN = 0;
    pthread_mutex_destroy(&CuvidGrabMutex);

    glDeleteBuffers(1, &vao_buffer);
    vao_buffer = 0;

    CuvidDevice = (CUdevice)(uintptr_t)NULL;
    cuda_free_functions(&cu);
}

///
///	Update output for new size or aspect ratio.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidUpdateOutput(CuvidDecoder * decoder)
{
    Debug(3, "video/cuvid: %s\n", __FUNCTION__);

    VideoUpdateOutput(decoder->InputAspect, decoder->InputWidth,
	decoder->InputHeight, decoder->Resolution, decoder->VideoX,
	decoder->VideoY, decoder->VideoWidth, decoder->VideoHeight,
	&decoder->OutputX, &decoder->OutputY, &decoder->OutputWidth,
	&decoder->OutputHeight, &decoder->CropX, &decoder->CropY,
	&decoder->CropWidth, &decoder->CropHeight);
#ifdef USE_AUTOCROP
    decoder->AutoCrop->State = 0;
    decoder->AutoCrop->Count = AutoCropDelay;
#endif
}

///
///	Configure CUVID for new video format.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidSetupOutput(CuvidDecoder * decoder)
{
    // FIXME: need only to create and destroy surfaces for size changes
    //		or when number of needed surfaces changed!
    decoder->Resolution =
	VideoResolutionGroup(decoder->InputWidth, decoder->InputHeight,
	decoder->Interlaced);
    VideoResolution = decoder->Resolution;

    CuvidMixerSetup(decoder);

    CuvidCreateSurfaces(decoder, decoder->InputWidth, decoder->InputHeight);

    CuvidUpdateOutput(decoder);		// update aspect/scaling
}

///
///	Get a free surface.  Called from ffmpeg.
///
///	@param decoder		CUVID hw decoder
///	@param video_ctx	ffmpeg video codec context
///
///	@returns the oldest free surface
///
static unsigned CuvidGetSurface(CuvidDecoder * decoder,
    const AVCodecContext * video_ctx)
{

#ifdef FFMPEG_BUG1_WORKAROUND
    // get_format not called with valid informations.
    if (video_ctx->width != decoder->InputWidth
	|| video_ctx->height != decoder->InputHeight) {

	CuvidCleanup(decoder);

	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;
	CuvidSetupOutput(decoder);
    }
#else
    (void)video_ctx;
#endif
    return CuvidGetSurface0(decoder);
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param fmt	is the list of formats which are supported by the codec,
///			it is terminated by -1 as 0 is a valid format, the
///			formats are ordered by quality.
///
static enum AVPixelFormat Cuvid_get_format(CuvidDecoder * decoder,
    AVCodecContext * video_ctx, const enum AVPixelFormat *fmt)
{
    const enum AVPixelFormat *fmt_idx;
    CUcontext dummy;
    VideoDecoder *ist = video_ctx->opaque;

    Debug(3,"get format  %dx%d\n",video_ctx->width,video_ctx->height);

    if (!VideoHardwareDecoder || (video_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO
	    && VideoHardwareDecoder == HWmpeg2Off)
	) {				// hardware disabled by config
	Debug(3, "codec: hardware acceleration disabled\n");
	goto slow_path;
    }
    //
    //	look through formats
    //
    Debug(3, "%s: codec %d fmts:\n", __FUNCTION__, video_ctx->codec_id);
    for (fmt_idx = fmt; *fmt_idx != AV_PIX_FMT_NONE; fmt_idx++) {
        Debug(3, "\t%#010x %s\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx));
    }

    for (fmt_idx = fmt; *fmt_idx != AV_PIX_FMT_NONE; fmt_idx++) {
	// check supported pixel format with entry point
	switch (*fmt_idx) {
	    case AV_PIX_FMT_P010LE:
	    case AV_PIX_FMT_NV12:
		break;
	    default:
		continue;
	}
	break;
    }
    Debug(3, "%s: selected codec:\n", __FUNCTION__);
    Debug(3, "\t%#010x %s\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx));

    if (*fmt_idx == AV_PIX_FMT_NONE) {
	Error(_("video/cuvid: no valid cuvid pixfmt found\n"));
	goto slow_path;
    }

    if (ist->GetFormatDone) return AV_PIX_FMT_CUDA;

    decoder->PixFmt = *fmt_idx;
    ist->active_hwaccel_id = HWACCEL_CUVID;
    ist->hwaccel_pix_fmt = AV_PIX_FMT_CUDA;
    ist->hwaccel_get_buffer = NULL;
    video_ctx->draw_horiz_band = NULL;
    video_ctx->slice_flags = 0;
    decoder->video_ctx = video_ctx;

    if (video_ctx->width && video_ctx->height) {
        CuvidCleanup(decoder);
	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;
        decoder->SurfacesNeeded = VIDEO_SURFACES_MAX * 2 + 1;
	CuvidSetupOutput(decoder);
    }

    Debug(3,"CUVID Init ok %dx%d\n",video_ctx->width,video_ctx->height);
    ist->GetFormatDone = 1;
    return AV_PIX_FMT_CUDA;

  slow_path:
    // no accelerated format found

    ist->active_hwaccel_id = HWACCEL_NONE;
    ist->hwaccel_pix_fmt   = AV_PIX_FMT_NONE;
    ist->hwaccel_get_buffer = NULL;
    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX * 2 + 2;
    decoder->PixFmt = AV_PIX_FMT_NONE;
    video_ctx->thread_count = 0;
    decoder->InputWidth = 0;
    decoder->InputHeight = 0;
    video_ctx->hwaccel_context = NULL;
    video_ctx->draw_horiz_band = NULL;

    cu->cuCtxPopCurrent(&dummy);
    ist->GetFormatDone = 1;
    return avcodec_default_get_format(video_ctx, fmt);
}

#ifdef USE_GRAB
///
///	Grab output surface already locked.
//	Grab openGL video + osd (screenshot)
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *CuvidGrabOutputSurfaceLocked(int *ret_size, int *ret_width, int *ret_height)
{
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint8_t *base;
    size_t i, j, k, cur_gl, cur_rgb;
    GLubyte *pixels;
    double scalew, scaleh;

    typedef struct {
        uint32_t x0;
        uint32_t y0;
        uint32_t x1;
        uint32_t y1;
    } CuRect;
    CuRect source_rect;

    CuvidDecoder *decoder;

    decoder = CuvidDecoders[0];
    if (decoder == NULL)   // no video aktiv
	return NULL;

    //get real surface size
    width = VideoWindowWidth;
    height = VideoWindowHeight;


    Debug(3, "video/cuvid: grab %dx%d\n", width, height);

    source_rect.x0 = 0;
    source_rect.y0 = 0;
    source_rect.x1 = width;
    source_rect.y1 = height;

    if (ret_width && ret_height) {
	if (*ret_width <= -64) {	// this is an Atmo grab service request
	    int overscan;

	    // calculate aspect correct size of analyze image
	    width = *ret_width * -1;
	    height = (width * source_rect.y1) / source_rect.x1;

	    // calculate size of grab (sub) window
	    overscan = *ret_height;

	    if (overscan > 0 && overscan <= 200) {
		source_rect.x0 = source_rect.x1 * overscan / 1000;
		source_rect.x1 -= source_rect.x0;
		source_rect.y0 = source_rect.y1 * overscan / 1000;
		source_rect.y1 -= source_rect.y0;
	    }
	} else {
	    if (*ret_width > 0 && (unsigned)*ret_width < width) {
		width = *ret_width;
	    }
	    if (*ret_height > 0 && (unsigned)*ret_height < height) {
		height = *ret_height;
	    }
	}

	Debug(3, "video/cuvid: grab source rect %d,%d:%d,%d dest dim %dx%d\n",
	source_rect.x0, source_rect.y0, source_rect.x1, source_rect.y1, width, height);

	size = width * height * 4;;

	base = malloc(size*sizeof(uint8_t));
	if (!base) {
	    Error(_("video/cuvid: grab out of memory\n"));
	    return NULL;
	}

	pixels = malloc(VideoWindowWidth * VideoWindowHeight * 4 * sizeof(GLubyte));
	if (!pixels) {
	    Error(_("video/cuvid: grab out of memory\n"));
	    free(base);
	    return NULL;
	}
	pthread_mutex_lock(&VideoLockMutex);
        if (GlxEnabled) {
	    glXMakeCurrent(XlibDisplay, VideoWindow, GlxSharedContext);
	    GlxCheck();
        }
#ifdef USE_EGL
        if (EglEnabled) {
            eglMakeCurrent(EglDisplay, EglSurface, EglSurface, EglSharedContext);
            EglCheck();
        }
#endif
	/* Get BGRA to align to 32 bits instead of just 24 for RGB */
	glReadPixels(source_rect.x0, source_rect.y0, VideoWindowWidth, VideoWindowHeight, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        GlCheck();

        if (GlxEnabled) {
	    glXMakeCurrent(XlibDisplay, None, NULL);
	    GlxCheck();
        }
#ifdef USE_EGL
        if (EglEnabled) {
            eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            EglCheck();
        }
#endif
	pthread_mutex_unlock(&VideoLockMutex);
	scalew = (double)VideoWindowWidth / width;
	scaleh = (double)VideoWindowHeight / height;

	// convert 32 -> 24 and simple scale
	for (i = 0; i < height; i++) {
	    for (j = 0; j < width; j++) {
	        cur_gl  = 4 * (VideoWindowWidth * (int)(scaleh * (height - i - 1)) + (int)(scalew * j));
	        cur_rgb = 4 * (width * i + j);
	            for (k = 0; k < 4; k++)
	                (base)[cur_rgb + k] = (pixels)[cur_gl + k];
	    }
	}

	free(pixels);

	Debug(3,"got grab data\n");

	*ret_size = size;
	*ret_width = width;
	*ret_height = height;
	return base;
    }

    return NULL;
}

///
///	Grab output surface.
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *CuvidGrabOutputSurface(int *ret_size, int *ret_width,  int *ret_height)
{
    uint8_t *img;

    if (!gl_prog) {
        return NULL;			// cuvid video module not yet initialized
    }

    pthread_mutex_lock(&CuvidGrabMutex);
//	pthread_mutex_lock(&VideoLockMutex);
    img = CuvidGrabOutputSurfaceLocked(ret_size, ret_width, ret_height);
//	pthread_mutex_unlock(&VideoLockMutex);
    pthread_mutex_unlock(&CuvidGrabMutex);
    return img;
}

#endif

#ifdef USE_AUTOCROP

///
///	CUVID auto-crop support.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidAutoCrop(CuvidDecoder * decoder)
{
    int surface;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[3];
    uint32_t pitches[3];
    int crop14;
    int crop16;
    int next_state;

    surface = decoder->SurfacesRb[(decoder->SurfaceRead + 1) %  (VIDEO_SURFACES_MAX * 2)];

    width = decoder->InputWidth;
    height = decoder->InputHeight;

    size = width * height + ((width + 1) / 2) * ((height + 1) / 2)
	+ ((width + 1) / 2) * ((height + 1) / 2);
    // cache buffer for reuse
    base = decoder->AutoCropBuffer;
    if (size > decoder->AutoCropBufferSize) {
	free(base);
	decoder->AutoCropBuffer = malloc(size);
	base = decoder->AutoCropBuffer;
	decoder->AutoCropBufferSize = size;
    }
    if (!base) {
	Error(_("video/cuvid: out of memory\n"));
	return;
    }
    pitches[0] = width;
    pitches[1] = width / 2;
    pitches[2] = width / 2;
    data[0] = base;
    data[1] = base + width * height;
    data[2] = base + width * height + width * height / 4;

    //we need Y in data[0] only
    glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][0]);
    glGetTexImage(GL_TEXTURE_2D,0,GL_RED, GL_UNSIGNED_BYTE,base);
    //glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][1]);
    //glGetTexImage(GL_TEXTURE_2D,0,GL_RG, GL_UNSIGNED_BYTE,base + width * height);

    AutoCropDetect(decoder->AutoCrop, width, height, data, pitches);

    glBindTexture(GL_TEXTURE_2D, 0);

    // ignore black frames
    if (decoder->AutoCrop->Y1 >= decoder->AutoCrop->Y2) {
	return;
    }

    crop14 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 14);
    crop14 = (decoder->InputHeight - crop14) / 2;
    crop16 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 16);
    crop16 = (decoder->InputHeight - crop16) / 2;

    if (decoder->AutoCrop->Y1 >= crop16 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop16 - AutoCropTolerance) {
	next_state = 16;
    } else if (decoder->AutoCrop->Y1 >= crop14 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop14 - AutoCropTolerance) {
	next_state = 14;
    } else {
	next_state = 0;
    }

    if (decoder->AutoCrop->State == next_state) {
	return;
    }

    Debug(3, "video: crop aspect %d:%d %d/%d %d%+d\n",
	decoder->InputAspect.num, decoder->InputAspect.den, crop14, crop16,
	decoder->AutoCrop->Y1, decoder->InputHeight - decoder->AutoCrop->Y2);

    Debug(3, "video: crop aspect %d -> %d\n", decoder->AutoCrop->State,	next_state);

    switch (decoder->AutoCrop->State) {
	case 16:
	case 14:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay / 2) {
		return;
	    }
	    break;
	case 0:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay) {
		return;
	    }
	    break;
    }

    decoder->AutoCrop->State = next_state;
    if (next_state) {
		decoder->CropX = VideoCutLeftRight[decoder->Resolution];
		decoder->CropY =
			(next_state ==
			16 ? crop16 : crop14) + VideoCutTopBottom[decoder->Resolution];
		decoder->CropWidth = decoder->InputWidth - decoder->CropX * 2;
		decoder->CropHeight = decoder->InputHeight - decoder->CropY * 2;

		// FIXME: this overwrites user choosen output position
		// FIXME: resize kills the auto crop values
		// FIXME: support other 4:3 zoom modes
		decoder->OutputX = decoder->VideoX;
		decoder->OutputY = decoder->VideoY;
		decoder->OutputWidth = (decoder->VideoHeight * next_state) / 9;
		decoder->OutputHeight = (decoder->VideoWidth * 9) / next_state;
		if (decoder->OutputWidth > decoder->VideoWidth) {
			decoder->OutputWidth = decoder->VideoWidth;
			decoder->OutputY +=
			(decoder->VideoHeight - decoder->OutputHeight) / 2;
		} else if (decoder->OutputHeight > decoder->VideoHeight) {
			decoder->OutputHeight = decoder->VideoHeight;
			decoder->OutputX +=
			(decoder->VideoWidth - decoder->OutputWidth) / 2;
		}
		Debug(3, "video: aspect output %dx%d %dx%d%+d%+d\n",
			decoder->InputWidth, decoder->InputHeight, decoder->OutputWidth,
			decoder->OutputHeight, decoder->OutputX, decoder->OutputY);
    } else {
		// sets AutoCrop->Count
		CuvidUpdateOutput(decoder);
    }
    decoder->AutoCrop->Count = 0;
}

///
///	CUVID check if auto-crop todo.
///
///	@param decoder	CUVID hw decoder
///
///	@note a copy of VaapiCheckAutoCrop
///	@note auto-crop only supported with normal 4:3 display mode
///
static void CuvidCheckAutoCrop(CuvidDecoder * decoder)
{
    // reduce load, check only n frames
    if (Video4to3ZoomMode == VideoNormal && AutoCropInterval
	&& !(decoder->FrameCounter % AutoCropInterval)) {
	AVRational input_aspect_ratio;
	AVRational tmp_ratio;
	av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den,
	    decoder->InputWidth * decoder->InputAspect.num,
	    decoder->InputHeight * decoder->InputAspect.den, 1024 * 1024);

	tmp_ratio.num = 4;
	tmp_ratio.den = 3;
	// 4:3 with 16:9/14:9 inside
	if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	    CuvidAutoCrop(decoder);
	} else {
	// 15:11 with 16:9/14:9 inside
	    tmp_ratio.num = 15;
	    tmp_ratio.den = 11;
	    if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	        CuvidAutoCrop(decoder);
	    } else {
	        decoder->AutoCrop->Count = 0;
	        decoder->AutoCrop->State = 0;
	    }
	}
    }
}

///
///	CUVID reset auto-crop.
///
static void CuvidResetAutoCrop(void)
{
    int i;

    for (i = 0; i < CuvidDecoderN; ++i) {
	CuvidDecoders[i]->AutoCrop->State = 0;
	CuvidDecoders[i]->AutoCrop->Count = 0;
    }
}

#endif

///
///	Queue output surface.
///
///	@param decoder	CUVID hw decoder
///	@param surface	output surface
///	@param softdec	software decoder
///
///	@note we can't mix software and hardware decoder surfaces
///
static void CuvidQueueSurface(CuvidDecoder * decoder, int surface,
    int softdec)
{
    int old;

    ++decoder->FrameCounter;

    if (1) {				// can't wait for output queue empty
	if (atomic_read(&decoder->SurfacesFilled) >= (VIDEO_SURFACES_MAX * 2)) {
	    Warning(_
		("video/cuvid: output buffer full, dropping frame (%d/%d)\n"),
		++decoder->FramesDropped, decoder->FrameCounter);
	    if (!(decoder->FramesDisplayed % 300)) {
		CuvidPrintFrames(decoder);
	    }
	    // software surfaces only
	    if (softdec) {
		CuvidReleaseSurface(decoder, surface);
	    }
	    return;
	}
#if 0
    } else {				// wait for output queue empty
	while (atomic_read(&decoder->SurfacesFilled) >= (VIDEO_SURFACES_MAX * 2)) {
	    VideoDisplayHandler();
	}
#endif
    }

    //
    //	    Check and release, old surface
    //
    if ((old = decoder->SurfacesRb[decoder->SurfaceWrite])
	!= -1) {

	// now we can release the surface, software surfaces only
	if (softdec) {
	    CuvidReleaseSurface(decoder, old);
	}
    }

    Debug(4, "video/cuvid: yy video surface %#08x@%d ready\n", surface,
	decoder->SurfaceWrite);

    decoder->SurfacesRb[decoder->SurfaceWrite] = surface;
    decoder->SurfaceWrite = (decoder->SurfaceWrite + 1)
	% (VIDEO_SURFACES_MAX * 2);
    atomic_inc(&decoder->SurfacesFilled);
}

///
///	Render a ffmpeg frame.
///
///	@param decoder		CUVID hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void CuvidRenderFrame(CuvidDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    int surface;
    VideoDecoder        *ist = video_ctx->opaque;
    int interlaced;
    enum AVColorSpace color;
    int n;
    AVRational aspect_ratio;

    // FIXME: some tv-stations toggle interlace on/off
    // frame->interlaced_frame isn't always correct set
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
    interlaced = frame->interlaced_frame;
#else
    interlaced = frame->flags & AV_FRAME_FLAG_INTERLACED;
#endif
    //25, 30 - interlaced; 50, 60 - progressive; hevc - progressive allways
    if (video_ctx->framerate.num > 0) {
	if (video_ctx->framerate.num / video_ctx->framerate.den > 30) interlaced = 0;
	else interlaced = 1;
    }

    if (video_ctx->codec_id == AV_CODEC_ID_HEVC) interlaced = 0;

    // FIXME: should be done by init video_ctx->field_order
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != frame->top_field_first) {

	Debug(4, "video/cuvid: interlaced %d top-field-first %d num %d den %d\n", frame->interlaced_frame,
	    frame->top_field_first, video_ctx->framerate.num, video_ctx->framerate.den);

	decoder->TopFieldFirst = frame->top_field_first;
#else
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)) {

	Debug(4, "video/cuvid: interlaced %d top-field-first %d num %d den %d\n", frame->flags & AV_FRAME_FLAG_INTERLACED,
	    frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST, video_ctx->framerate.num, video_ctx->framerate.den);

	decoder->TopFieldFirst = frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST;
#endif
	decoder->SurfaceField = !decoder->Interlaced;
	decoder->Interlaced = interlaced;
    }
    //aspect correction
    aspect_ratio = frame->sample_aspect_ratio;
    if ((video_ctx->width >= 352  && video_ctx->width <= 720 && video_ctx->height == 288) ||
	(video_ctx->width > 1300 && video_ctx->width <= 1920 && video_ctx->height == 540)) {
	aspect_ratio.den *= 2;
    }
    // update aspect ratio changes
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, aspect_ratio)) {
	Debug(3, "video/cuvid: aspect ratio changed\n");
	decoder->InputAspect = aspect_ratio;
	CuvidUpdateOutput(decoder);
    }

    color = frame->colorspace;
    if (color == AVCOL_SPC_UNSPECIFIED)   // if unknown
        color = AVCOL_SPC_BT709;

    //
    // render
    //
    //
    // Check image, format, size
    //

    if (ist->hwaccel_pix_fmt != video_ctx->pix_fmt
        || video_ctx->width != decoder->InputWidth
        || video_ctx->height != decoder->InputHeight) {
        if (video_ctx->pix_fmt != AV_PIX_FMT_CUDA) { // for softdecode
            decoder->PixFmt = video_ctx->pix_fmt;
            ist->hwaccel_pix_fmt = video_ctx->pix_fmt;
            ist->active_hwaccel_id = HWACCEL_NONE;
        }
        decoder->InputWidth = video_ctx->width;
        decoder->InputHeight = video_ctx->height;
        decoder->video_ctx = (AVCodecContext *)video_ctx;

        CuvidCleanup(decoder);
        decoder->SurfacesNeeded = VIDEO_SURFACES_MAX * 2 + 1;
        CuvidSetupOutput(decoder);
    }
    //
    // Copy data from frame to image
    //
    switch (video_ctx->pix_fmt) {
        case AV_PIX_FMT_CUDA:
//        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_YUV420P:      // for softdecode
//        case AV_PIX_FMT_YUVJ420P:     // some streams produce this
        case AV_PIX_FMT_YUV420P10LE:  // for softdecode of 10 Bit
        break;
//        case AV_PIX_FMT_YUV422P:
//        case AV_PIX_FMT_YUV444P:
        default:
            Error(_("video/cuvid: pixel format %d not supported\n"),
            video_ctx->pix_fmt);
            // FIXME: no fatals!
    }
    decoder->ColorSpace = color;     // save colorspace
    decoder->trc = frame->color_trc;
    decoder->color_primaries = frame->color_primaries;

    surface = CuvidGetSurface0(decoder);

    if (surface == -1)     // no free surfaces
        return;
    if (video_ctx->pix_fmt == AV_PIX_FMT_CUDA) { //hardware

        for (n = 0; n < 2; n++) { //.
            // widthInBytes must account for the chroma plane
            // elements being two samples wide.
            CUDA_MEMCPY2D cpy = {
                .srcMemoryType = CU_MEMORYTYPE_DEVICE,
                .dstMemoryType = CU_MEMORYTYPE_ARRAY,
                .srcDevice     = (CUdeviceptr)frame->data[n],
                .srcPitch      = frame->linesize[n],
                .srcY          = 0,
                .dstArray      = decoder->cu_array[surface][n],
                .WidthInBytes  = decoder->InputWidth * (decoder->PixFmt == AV_PIX_FMT_NV12 ? 1 : 2),
                .Height        = n == 0 ? decoder->InputHeight : decoder->InputHeight / 2,
            };
            if (CUStatus(cu->cuMemcpy2D(&cpy))) Fatal(_("video/cuvid: render frame error\n"));
        }
        CuvidQueueSurface(decoder, surface, 1);

    } else {       //software
        uint8_t *outY = NULL;
        uint8_t *outUV = NULL;

        if(GlxEnabled) {
            glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
            GlxCheck();
        }
#ifdef USE_EGL
        if (EglEnabled) {
            eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglThreadContext);
            EglCheck();
        }
#endif
        if (decoder->PixFmt != AV_PIX_FMT_YUV420P10LE) { //8bit
            //YV12 -> NV12
            outUV = (uint8_t*) malloc(frame->linesize[1] * decoder->InputHeight * sizeof(uint8_t));

            if (!outUV) {
                Error(_("video/cuvid: out of memory\n"));
                return;
            }
            //TODO use shader or direct yuv render?
            for (int i = 0; i < (frame->linesize[1] * decoder->InputHeight / 2); i++) {
                memcpy(outUV + i * 2, frame->data[1] + i, 1); // For NV12, U first
                memcpy(outUV + i * 2 + 1, frame->data[2] + i, 1); // For NV12, V second
            }

            //Y
            glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][0]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[0]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decoder->InputWidth,
                decoder->InputHeight, GL_RED, GL_UNSIGNED_BYTE, frame->data[0]);
            GlCheck();
            //UV
            glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][1]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[1]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decoder->InputWidth/2,
                decoder->InputHeight/2, GL_RG, GL_UNSIGNED_BYTE, outUV);
            GlCheck();
            glBindTexture(GL_TEXTURE_2D, 0);

        } else { //10bit
            //yuv420ple -> p010le + 10bit -> 8bit
            outY = (uint8_t*) malloc(frame->linesize[0] * decoder->InputHeight / 2 * sizeof(uint8_t));

            if (!outY) {
                Error(_("video/cuvid: out of memory\n"));
                return;
            }

            outUV = (uint8_t*) malloc(frame->linesize[1] * decoder->InputHeight / 2 * sizeof(uint8_t));

            if (!outUV) {
                Error(_("video/cuvid: out of memory\n"));
                return;
            }

            //TODO use shader or direct yuv render?
            for (int i = 0; i < (frame->linesize[0] / 2 * decoder->InputHeight); i++) {
                outY[i] = (*((uint16_t*)frame->data[0] + i) + 2) >> 2; // Y
            }

            for (int i = 0; i < (frame->linesize[1] * decoder->InputHeight / 4); i++) {
                outUV[i * 2] = (*((uint16_t*)frame->data[1] + i) + 2) >> 2; // For p010le, U first
                outUV[i * 2 + 1] = (*((uint16_t*)frame->data[2] + i) + 2) >> 2; // For p010le, V second
            }

            //Y
            glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][0]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[0]/2);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decoder->InputWidth,
                decoder->InputHeight,  GL_RED, GL_UNSIGNED_BYTE, outY);
            GlCheck();
            //UV
            glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][1]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[1]/2);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decoder->InputWidth/2,
                decoder->InputHeight/2, GL_RG, GL_UNSIGNED_BYTE, outUV);
            GlCheck();
            glBindTexture(GL_TEXTURE_2D, 0);

        }
        Debug(4, "video/cuvid: sw render hw surface %#08x\n", surface);

        CuvidQueueSurface(decoder, surface, 1);
        if (outY) free(outY);
        if (outUV) free(outUV);
    }
    if (decoder->Interlaced) {
	++decoder->FrameCounter;
    }
}

///
///	Get hwaccel context for ffmpeg.
///
///	@param decoder	CUVID hw decoder
///
static void *CuvidGetHwAccelContext(CuvidDecoder * decoder)
{
    Debug(3, "Initializing cuvid hwaccel\n");
    cu->cuCtxPushCurrent(decoder->cu_ctx);
    (void)decoder;

    return decoder->cu_ctx;
}

static void CuvidMixVideo(CuvidDecoder * decoder, int level)
{
    int current;
    int y;
    float xcropf, ycropf;
    GLint texLoc;

#ifdef USE_AUTOCROP
    // FIXME: can move to render frame
    CuvidCheckAutoCrop(decoder);
#endif

    xcropf = (float) decoder->CropX / (float) decoder->InputWidth;
    ycropf = (float) decoder->CropY / (float) decoder->InputHeight;

    current = decoder->SurfacesRb[decoder->SurfaceRead];

    if (!decoder->gl_textures[current][0] || !decoder->gl_textures[current][1]) return;
    // Render Progressive frame and simple interlaced
    y = VideoWindowHeight - decoder->OutputY - decoder->OutputHeight;
    if (y < 0)
        y = 0;
    glViewport(decoder->OutputX, y, decoder->OutputWidth, decoder->OutputHeight);

    if (gl_prog == 0)
        gl_prog = sc_generate(gl_prog, decoder->ColorSpace);    // generate shader programm
    if (!gl_prog) return;

    glUseProgram(gl_prog);
    texLoc = glGetUniformLocation(gl_prog, "texture0");
    glUniform1i(texLoc, 0);
    texLoc = glGetUniformLocation(gl_prog, "texture1");
    glUniform1i(texLoc, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[current][0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[current][1]);

    render_pass_quad(0, xcropf, ycropf);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);

    Debug(4,"video/cuvid: yy video surface %d displayed\n", current);
    (void)level;
}

///
///	Create and display a black empty surface.
///
///	@param decoder	CUVID hw decoder
///
///	@FIXME: render only video area, not fullscreen!
///	decoder->Output.. isn't correct setup for radio stations
///
static void CuvidBlackSurface(CuvidDecoder * decoder)
{
    glClear(GL_COLOR_BUFFER_BIT);
    (void)decoder;
    return;
}

///
///	Advance displayed frame of decoder.
///
///	@param decoder	CUVID hw decoder
///
static void CuvidAdvanceDecoderFrame(CuvidDecoder * decoder)
{
    // next surface, if complete frame is displayed (1 -> 0)
    if (decoder->SurfaceField) {
	int filled;

	// FIXME: this should check the caller
	// check decoder, if new surface is available
	// need 2 frames for progressive
	// need 4 frames for interlaced
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled < 1 + 2 * decoder->Interlaced) {
	    // keep use of last surface
	    ++decoder->FramesDuped;
	    // FIXME: don't warn after stream start, don't warn during pause
	    Debug(4,"video: display buffer empty, duping frame (%d/%d) %d\n",
		decoder->FramesDuped, decoder->FrameCounter,
		VideoGetBuffers(decoder->Stream));
	    return;
	}
	decoder->SurfaceRead = (decoder->SurfaceRead + 1) % (VIDEO_SURFACES_MAX * 2);
	atomic_dec(&decoder->SurfacesFilled);
	decoder->SurfaceField = !decoder->Interlaced;
	return;
    }
    // next field
    decoder->SurfaceField = 1;
    (void)decoder;
}

///
///	Display a video frame.
///
static void CuvidDisplayFrame(void)
{
    uint64_t first_time = 0;
    static uint64_t last_time;
    static int timeSS;
    int i;

    if (VideoSurfaceModesChanged) {	// handle changed modes
	VideoSurfaceModesChanged = 0;
	for (i = 0; i < CuvidDecoderN; ++i) {
		CuvidMixerSetup(CuvidDecoders[i]);
	}
    }

    if (GlxEnabled) {
	glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
	glClear(GL_COLOR_BUFFER_BIT);
    }
#ifdef USE_EGL
    if (EglEnabled) {
	eglMakeCurrent(EglDisplay, EglSurface, EglSurface, EglThreadContext);
	glClear(GL_COLOR_BUFFER_BIT);
    }
#endif
    // check if surface was displayed for more than 1 frame
	first_time = GetUsTicks();
    // FIXME: 21 only correct for 50Hz
    if (last_time && first_time > last_time + 21 * 1000 * 1000) {
	// FIXME: ignore still-frame, trick-speed
	Debug(3, "video/cuvid: %" PRId64 " display time %" PRId64 "\n",
	    first_time / 1000, (first_time - last_time) / 1000);
	// FIXME: can be more than 1 frame long shown
	for (i = 0; i < CuvidDecoderN; ++i) {
	    CuvidDecoders[i]->FramesMissed++;
	    CuvidMessage(2, _("video/cuvid: missed frame (%d/%d)\n"),
		CuvidDecoders[i]->FramesMissed,
		CuvidDecoders[i]->FrameCounter);
	}
    }

    last_time = first_time;
#ifdef USE_SCREENSAVER
    //get up screensaver every 20 sec
    if (DisableScreensaver) {
	if (first_time / 1000000 - timeSS > 20) {
	    xcb_force_screen_saver(Connection,XCB_SCREEN_SAVER_RESET);
	    timeSS = first_time / 1000000;
	}
    }
#endif
    //
    //	Render videos into output
    //
    for (i = 0; i < CuvidDecoderN; ++i) {
	int filled;
	CuvidDecoder *decoder;

	decoder = CuvidDecoders[i];
	decoder->FramesDisplayed++;
	decoder->StartCounter++;

	filled = atomic_read(&decoder->SurfacesFilled);
	// need 1 frame for progressive, 3 frames for interlaced
	if (filled < 1 + 2 * decoder->Interlaced && decoder->Closing) {
	    // FIXME: rewrite MixVideo to support less surfaces
	    if ((VideoShowBlackPicture && !decoder->TrickSpeed)
		|| (VideoShowBlackPicture && decoder->Closing < -300)) {
		CuvidBlackSurface(decoder);
		CuvidMessage(4, "video/cuvid: black surface displayed\n");
#ifdef USE_SCREENSAVER
		if (EnableDPMSatBlackScreen && DPMSDisabled) {
		    CuvidMessage(4, "Black surface, DPMS enabled\n");
		    X11DPMSReenable(Connection);
		    X11SuspendScreenSaver(Connection, 1);
		}
#endif
	    }
	    if (VideoShowBlackPicture)
		continue;
#ifdef USE_SCREENSAVER
	} else if (!DPMSDisabled) {	// always disable
	    CuvidMessage(4, "DPMS disabled\n");
	    X11DPMSDisable(Connection);
	    X11SuspendScreenSaver(Connection, 0);
#endif
	}

	CuvidMixVideo(decoder, i);
    }

    //
    //	add osd to surface
    //
    if(GlxEnabled) {
        if (OsdShown) {
            glViewport(0, 0, VideoWindowWidth, VideoWindowHeight);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, VideoWindowWidth, VideoWindowHeight, 0.0, -1.0, 1.0);
            GlxCheck();
#ifdef USE_OPENGLOSD
            if(!DisableOglOsd && OsdGlTexture) {
                GlxRenderTexture(OsdGlTexture, 0,0, VideoWindowWidth, VideoWindowHeight);
            } else
#endif
                GlxRenderTexture(OsdGlTextures[OsdIndex], 0,0, VideoWindowWidth, VideoWindowHeight);
        }

        glXSwapBuffers(XlibDisplay, VideoWindow);
        glXMakeCurrent(XlibDisplay, None, NULL);
    }
#ifdef USE_EGL
    if (EglEnabled) {
	if (OsdShown) {
            glViewport(0, 0, VideoWindowWidth, VideoWindowHeight);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, VideoWindowWidth, VideoWindowHeight, 0.0, -1.0, 1.0);
            EglCheck();
#ifdef USE_OPENGLOSD
            if(!DisableOglOsd && OsdGlTexture) {
                EglRenderTexture(OsdGlTexture, 0,0, VideoWindowWidth, VideoWindowHeight);
            } else
#endif
	    EglRenderTexture(OsdGlTextures[OsdIndex], 0, 0, VideoWindowWidth, VideoWindowHeight);
	}

	eglSwapBuffers(EglDisplay, EglSurface);
	eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	EglCheck();
    }
#endif
    // FIXME: CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC, &CuvidFrameTime);
    for (i = 0; i < CuvidDecoderN; ++i) {
	// remember time of last shown surface
	CuvidDecoders[i]->FrameTime = CuvidFrameTime;
    }

    xcb_flush(Connection);
}


///
///	Set CUVID decoder video clock.
///
///	@param decoder	CUVID hardware decoder
///	@param pts	audio presentation timestamp
///
void CuvidSetClock(CuvidDecoder * decoder, int64_t pts)
{
    decoder->PTS = pts;
}

///
///	Get CUVID decoder video clock.
///
///	@param decoder	CUVID hw decoder
///
///	FIXME: 20 wrong for 60hz dvb streams
///
static int64_t CuvidGetClock(const CuvidDecoder * decoder)
{
    // pts is the timestamp of the latest decoded frame
    if (decoder->PTS == (int64_t) AV_NOPTS_VALUE) {
	return AV_NOPTS_VALUE;
    }
    // subtract buffered decoded frames
    if (decoder->Interlaced) {
	/*
	   Info("video: %s =pts field%d #%d\n",
	   Timestamp2String(decoder->PTS),
	   decoder->SurfaceField,
	   atomic_read(&decoder->SurfacesFilled));
	 */
	// 1 field is future, 2 fields are past, + 2 in driver queue
	return decoder->PTS -
	    20 * 90 * (2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField - 2 + 2);
    }
    // + 2 in driver queue
    return decoder->PTS - 20 * 90 * (atomic_read(&decoder->SurfacesFilled) + 2);
}

///
///	Set CUVID decoder closing stream flag.
///
///	@param decoder	CUVID decoder
///
static void CuvidSetClosing(CuvidDecoder * decoder)
{
    decoder->Closing = 1;
}

///
///	Reset start of frame counter.
///
///	@param decoder	CUVID decoder
///
static void CuvidResetStart(CuvidDecoder * decoder)
{
    decoder->StartCounter = 0;
}

///
///	Set trick play speed.
///
///	@param decoder	CUVID decoder
///	@param speed	trick speed (0 = normal)
///
static void CuvidSetTrickSpeed(CuvidDecoder * decoder, int speed)
{
    decoder->TrickSpeed = speed;
    decoder->TrickCounter = speed;
    if (speed) {
	decoder->Closing = 0;
    }
}

///
///	Get CUVID decoder statistics.
///
///	@param decoder		CUVID decoder
///	@param[out] missed	missed frames
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void CuvidGetStats(CuvidDecoder * decoder, int *missed, int *duped,
    int *dropped, int *counter, int *dec)
{
    *missed = decoder->FramesMissed;
    *duped = decoder->FramesDuped;
    *dropped = decoder->FramesDropped;
    *counter = decoder->FrameCounter;
    *dec = (decoder->PixFmt == AV_PIX_FMT_NV12 || decoder->PixFmt == AV_PIX_FMT_P010LE)
        ? HWACCEL_CUVID : HWACCEL_NONE;
}

///
///	Sync decoder output to audio.
///
///	trick-speed	show frame <n> times
///	still-picture	show frame until new frame arrives
///	60hz-mode	repeat every 5th picture
///	video>audio	slow down video by duplicating frames
///	video<audio	speed up video by skipping frames
///	soft-start	show every second frame
///
///	@param decoder	CUVID hw decoder
///
static void CuvidSyncDecoder(CuvidDecoder * decoder)
{
    int err;
    int64_t audio_clock;
    int64_t video_clock;

    err = 0;
    video_clock = CuvidGetClock(decoder);
    audio_clock = AudioGetClock();

    if (!decoder->SyncOnAudio) {
	audio_clock = AV_NOPTS_VALUE;
	// FIXME: 60Hz Mode
	goto skip_sync;
    }

    EnoughVideo = (VideoGetBuffers(decoder->Stream) >= (VideoResolution == VideoResolution576i ? VideoStartThreshold_SD * !!VideoSoftStartSync : 0));
    if (!EnoughVideo && EnoughAudio && !AudioRunning && decoder->StartCounter > 200) {
	Debug (3,"video: force enough video\n");
	EnoughVideo = 1;
    }
    if (EnoughVideo && EnoughAudio && !AudioRunning && audio_clock != (int64_t) AV_NOPTS_VALUE) {
	Debug(3, "video: start audio after waiting for enough video: SurfacesFilled: %d, PacketsFilled: %d\n", atomic_read(&decoder->SurfacesFilled), VideoGetBuffers(decoder->Stream));
	AudioStarted = 1;
	AudioRunning = 1;
	pthread_cond_signal(&AudioStartCond);
    }

    // 60Hz: repeat every 5th field
    if (Video60HzMode && !(decoder->FramesDisplayed % 6)) {
	if (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock == (int64_t) AV_NOPTS_VALUE) {
	    goto out;
	}
	// both clocks are known
	if (audio_clock + VideoAudioDelay <= video_clock + 25 * 90) {
	    goto out;
	}
	// out of sync: audio before video
	if (!decoder->TrickSpeed) {
	    goto skip_sync;
	}
    }
    // TrickSpeed
    if (decoder->TrickSpeed) {
	if (decoder->TrickCounter--) {
	    goto out;
	}
	decoder->TrickCounter = decoder->TrickSpeed;
	goto skip_sync;
    }
    // at start of new video stream, soft or hard sync video to audio
    if (!IsReplay() && VideoSoftStartSync < 2 && decoder->StartCounter < VideoSoftStartFrames
	&& video_clock != (int64_t) AV_NOPTS_VALUE
	&& (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock > audio_clock + VideoAudioDelay + 120 * 90)) {
	err =
	    CuvidMessage(3, "video: initial slow down video, frame %d\n",
	    decoder->StartCounter);
	goto out;
    }

    if (decoder->SyncCounter && decoder->SyncCounter-- && VideoSoftStartSync > 1) {
	goto skip_sync;
    }

    if (audio_clock != (int64_t) AV_NOPTS_VALUE
	&& video_clock != (int64_t) AV_NOPTS_VALUE) {
	// both clocks are known
	int diff;
	int lower_limit;
	if (IsReplay()) {
	    if((video_clock > audio_clock + VideoAudioDelay + 8000 * 90) && decoder->StartCounter < VideoSoftStartFrames) { // 8s
		Debug(3,"flush audio\n");
		AudioFlushBuffers();
		goto out;
	    }
	    if((video_clock < audio_clock + VideoAudioDelay - 120 * 90) && VideoSoftStartSync < 2) {
		if (!(decoder->SurfaceField && atomic_read(&decoder->SurfacesFilled) < 1 + 2 * decoder->Interlaced)) {
		    Debug(3,"drop video\n");
		    CuvidAdvanceDecoderFrame(decoder);
		}
		goto skip_sync;
	    }
	}
	diff = video_clock - audio_clock - VideoAudioDelay;
	lower_limit = !IsReplay() ? -25 - (VideoResolution == VideoResolution576i ? 40 : 0) : 32;
	//diff = (decoder->LastAVDiff + diff) / 2;
	decoder->LastAVDiff = diff;
#ifdef DEBUG
	if (!decoder->SyncCounter && decoder->StartCounter < 1000)
	    Debug(3, "video/cuvid: diff %d %d lim %d fill %d\n", diff, diff/90, lower_limit, atomic_read(&decoder->SurfacesFilled));
#endif
	if (abs(diff) > 8000 * 90) {	// more than 8s
	    err = CuvidMessage(3, "video: audio/video difference too big\n");
	}
	if (diff > 100 * 90) {
	    // FIXME: this quicker sync step, did not work with new code!
	    err = CuvidMessage(3, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    if (VideoSoftStartSync > 1) {
		decoder->SyncCounter = diff > 100 * 90 ? diff % 2 : 1; //softsync :)
	    }
	    goto out;
	} else if (diff > 55 * 90) {
	    err = CuvidMessage(3, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    decoder->SyncCounter = 1;
	    goto out;
	} else if (diff < lower_limit * 90 && atomic_read(&decoder->SurfacesFilled) > 1 + decoder->Interlaced) { // double advance possible?
	    err = CuvidMessage(3, "video: speed up video, droping frame\n");
	    ++decoder->FramesDropped;
	    CuvidAdvanceDecoderFrame(decoder);
	    decoder->SyncCounter = 2;
	} else if (diff < lower_limit * 90 && atomic_read(&decoder->SurfacesFilled) < 1 + 2 * decoder->Interlaced && !IsReplay()) {
	    err = CuvidMessage(3, "video: speed up audio, delay audio\n");
	    AudioDelayms(-diff / 90 + 55);
	}
#if defined(DEBUG) || defined(AV_INFO)
	if (!decoder->SyncCounter && decoder->StartCounter < 1000) {
#ifdef DEBUG
	    Debug(3, "video/cuvid: synced after %d frames %dms\n",
		decoder->StartCounter, GetMsTicks() - VideoSwitch);
#else
	    Info("video/cuvid: synced after %d frames\n",
		decoder->StartCounter);
#endif
	    decoder->StartCounter += 1000;
	}
#endif
    }

  skip_sync:
    // is it not possible, to advance the surface and/or the field?
    if (decoder->SurfaceField && atomic_read(&decoder->SurfacesFilled) < 1 + 2 * decoder->Interlaced + 1 * !IsReplay()) {
	++decoder->FramesDuped;
	// FIXME: don't warn after stream start, don't warn during pause
	err =
	    CuvidMessage(3,
	    _("video: decoder buffer empty, "
	    "duping frame (%d/%d) %d v-buf closing %d\n"), decoder->FramesDuped,
	    decoder->FrameCounter, VideoGetBuffers(decoder->Stream), decoder->Closing);
	// some time no new picture or black video configured
	if (decoder->Closing < -300 || (VideoShowBlackPicture
	    && decoder->Closing)) {
	    // clear ring buffer to trigger black picture
	    atomic_set(&decoder->SurfacesFilled, 0);
	}
	goto out;
    }

    CuvidAdvanceDecoderFrame(decoder);
  out:
#if defined(DEBUG) || defined(AV_INFO)
    // debug audio/video sync
    if (err || !(decoder->FramesDisplayed % AV_INFO_TIME)) {
	if (!err) {
	    CuvidMessage(0, NULL);
	}
	Info("video: %s%+5" PRId64 " %4" PRId64 " %3d/\\ms %3d%+d%+d v-buf\n",
	    Timestamp2String(video_clock),
	    abs((int)(video_clock - audio_clock) / 90) <
	    8888 ? ((video_clock - audio_clock) / 90) : 8888,
	    AudioGetDelay() / 90, (int)VideoDeltaPTS / 90,
	    VideoGetBuffers(decoder->Stream),
	    decoder->Interlaced ? 2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField : atomic_read(&decoder->SurfacesFilled),
	    CuvidSurfaceQueued);
	if (!(decoder->FramesDisplayed % (5 * 60 * 60))) {
	    CuvidPrintFrames(decoder);
	}
    }
#endif
    return;				// fix gcc bug!
}

///
///	Sync a video frame.
///
static void CuvidSyncFrame(void)
{
    int i;

    //
    //	Sync video decoder to audio
    //
    for (i = 0; i < CuvidDecoderN; ++i) {
	CuvidSyncDecoder(CuvidDecoders[i]);
    }
}

///
///	Sync and display surface.
///
static void CuvidSyncDisplayFrame(void)
{
    CuvidDisplayFrame();
    CuvidSyncFrame();
}

///
///	Sync and render a ffmpeg frame
///
///	@param decoder		CUVID hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void CuvidSyncRenderFrame(CuvidDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    // FIXME: temp debug
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,61,100)
    if (0 && frame->pkt_pts != (int64_t) AV_NOPTS_VALUE) {
	Debug(3, "video: render frame pts %s\n",
	    Timestamp2String(frame->pkt_pts));
#else
    if (0 && frame->pts != (int64_t) AV_NOPTS_VALUE) {
	Debug(3, "video: render frame pts %s\n",
	    Timestamp2String(frame->pts));
#endif
    }
#ifdef DEBUG
    if (!atomic_read(&decoder->SurfacesFilled)) {
	Debug(3, "video: new stream frame %dms\n", GetMsTicks() - VideoSwitch);
    }
#endif

#if 1
#ifndef USE_PIP
#error	"-DUSE_PIP or #define USE_PIP is needed,"
#endif
    // if video output buffer is full, wait and display surface.
    // loop for interlace
    if (atomic_read(&decoder->SurfacesFilled) >= (VIDEO_SURFACES_MAX * 2)) {
	Info("video/cuvid: this code part shouldn't be used\n");
	return;
    }
#else
    // FIXME: disabled for remove
    // FIXME: wrong for multiple streams
    // FIXME: this part code should be no longer be needed with new mpeg fix
    while (atomic_read(&decoder->SurfacesFilled) >= (VIDEO_SURFACES_MAX * 2)) {
	struct timespec abstime;

	pthread_mutex_unlock(&VideoLockMutex);

	abstime = decoder->FrameTime;
	abstime.tv_nsec += 14 * 1000 * 1000;
	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
	    // avoid overflow
	    abstime.tv_sec++;
	    abstime.tv_nsec -= 1000 * 1000 * 1000;
	}

	VideoPollEvent();

	// fix dead-lock with VdpauExit
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_mutex_lock(&VideoLockMutex);
	// give osd some time slot
	while (pthread_cond_timedwait(&VideoWakeupCond, &VideoLockMutex,
		&abstime) != ETIMEDOUT) {
	    // SIGUSR1
	    Debug(3, "video/cuvid: pthread_cond_timedwait error\n");
	}
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	CuvidSyncDisplayFrame();
    }
#endif

    if (!decoder->Closing) {
	VideoSetPts(&decoder->PTS, decoder->Interlaced, video_ctx, frame);
    }
    CuvidRenderFrame(decoder, video_ctx, frame);
}

///
///	Set CUVID background color.
///
///	@param rgba	32 bit RGBA color.
///
static void CuvidSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
}

///
///	Set video output position.
///
///	@param decoder	CUVID hw decoder
///	@param x	video output x coordinate inside the window
///	@param y	video output y coordinate inside the window
///	@param width	video output width
///	@param height	video output height
///
///	@note FIXME: need to know which stream.
///
static void CuvidSetOutputPosition(CuvidDecoder * decoder, int x, int y,
    int width, int height)
{
    Debug(3, "video/cuvid: output %dx%d%+d%+d\n", width, height, x, y);

    decoder->VideoX = x;
    decoder->VideoY = y;
    decoder->VideoWidth = width;
    decoder->VideoHeight = height;

    // next video pictures are automatic rendered to correct position
}

///
///	Set CUVID video mode.
///
static void CuvidSetVideoMode(void)
{
    int i;

    for (i = 0; i < CuvidDecoderN; ++i) {
	// reset video window, upper level needs to fix the positions
	CuvidDecoders[i]->VideoX = 0;
	CuvidDecoders[i]->VideoY = 0;
	CuvidDecoders[i]->VideoWidth = VideoWindowWidth;
	CuvidDecoders[i]->VideoHeight = VideoWindowHeight;
	// for window mode need restore scale
	if (CuvidDecoders[i]->ScaleX || CuvidDecoders[i]->ScaleY
	 || CuvidDecoders[i]->ScaleWidth || CuvidDecoders[i]->ScaleHeight) {
	    // convert OSD coordinates to window coordinates
	    int x = (CuvidDecoders[i]->ScaleX * VideoWindowWidth) / OsdWidth;
	    int width = (CuvidDecoders[i]->ScaleWidth * VideoWindowWidth) / OsdWidth;
	    int y = (CuvidDecoders[i]->ScaleY * VideoWindowHeight) / OsdHeight;
	    int height = (CuvidDecoders[i]->ScaleHeight * VideoWindowHeight) / OsdHeight;
	    CuvidSetOutputPosition(CuvidDecoders[i], x, y, width, height);
	}
	CuvidUpdateOutput(CuvidDecoders[i]);
    }
}

#ifdef USE_VIDEO_THREAD2

#else

#ifdef USE_VIDEO_THREAD

///
///	Handle a CUVID display.
///
static void CuvidDisplayHandlerThread(void)
{
    int i;
    int err;
    int allfull;
    int decoded;
    struct timespec nowtime;
    CuvidDecoder *decoder;

    allfull = 1;
    decoded = 0;
    pthread_mutex_lock(&VideoLockMutex);
    for (i = 0; i < CuvidDecoderN; ++i) {
	int filled;

	decoder = CuvidDecoders[i];

	//
	// fill frame output ring buffer
	//
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled <= 1 + 2 * decoder->Interlaced) {
	    // FIXME: hot polling
	    // fetch+decode or reopen
	    allfull = 0;
	    err = VideoDecodeInput(decoder->Stream);
	} else {
	    err = VideoPollInput(decoder->Stream);
	}
	// decoder can be invalid here
	if (err) {
	    // nothing buffered?
	    if (err == -1 && decoder->Closing) {
		decoder->Closing--;
		if (!decoder->Closing) {
		    Debug(3, "video/cuvid: closing eof\n");
		    decoder->Closing = -1;
		}
	    }
	    continue;
	}
	decoded = 1;
    }
    pthread_mutex_unlock(&VideoLockMutex);

    if (!decoded) {			// nothing decoded, sleep
	// FIXME: sleep on wakeup
	usleep(1 * 1000);
    }
    // all decoder buffers are full
    // and display is not preempted
    // speed up filling display queue, wait on display queue empty
    if (!allfull) {
	clock_gettime(CLOCK_MONOTONIC, &nowtime);
	// time for one frame over?
	if ((nowtime.tv_sec - CuvidFrameTime.tv_sec) * 1000 * 1000 * 1000 +
	    (nowtime.tv_nsec - CuvidFrameTime.tv_nsec) < 15 * 1000 * 1000) {
	    return;
	}
    }

    pthread_mutex_lock(&VideoLockMutex);
    CuvidSyncDisplayFrame();
    pthread_mutex_unlock(&VideoLockMutex);
}

#else

#define CuvidDisplayHandlerThread	NULL

#endif

#endif

//----------------------------------------------------------------------------
//	CUVID OSD USE GLX
//----------------------------------------------------------------------------
#ifdef USE_OPENGLOSD
void *GetCuvidOsdOutputTexture(GLuint texture) {
    OsdGlTexture = texture;
    return 0;
}

int CuvidInitGlx(void) {
    int a = 0;

    if (!GlxEnabled) {
        Debug(3,"video/osd: can't create glx context\n");
        return 0;
    }
    //after run an external player from time to time vdr not set playmode 1
    //then try start GLX forced.
    //is it a vdr bug or external player plugin???
    while (!GlxContext || !GlxThreadContext){
        usleep(1000);
        a++;
        if (a > 10 && a < 1000) {
            Debug(3,"Try start GLX forced\n");
            VideoDisplayWakeup();
            a = 1000;
        }
        if (a > 1010) return 0;
    }
    Debug(3,"Create OSD GLX context\n");
    glXMakeCurrent(XlibDisplay, None, NULL);
    glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext);
    return 1;
}
#endif

///
///	CUVID module.
///
static const VideoModule CuvidModule = {
    .Name = "cuvid",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) CuvidNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))CuvidDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))CuvidGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))CuvidReleaseSurface,
    .UnregisterSurface =
	(void (*const) (VideoHwDecoder *))CuvidUnregisterSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Cuvid_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))CuvidSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	CuvidGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))CuvidSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))CuvidGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))CuvidSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))CuvidResetStart,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))CuvidSetTrickSpeed,
#ifdef USE_GRAB
    .GrabOutput = CuvidGrabOutputSurface,
#endif
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, int *))CuvidGetStats,
    .SetBackground = CuvidSetBackground,
    .SetVideoMode = CuvidSetVideoMode,
#ifdef USE_AUTOCROP
    .ResetAutoCrop = CuvidResetAutoCrop,
#endif
    .DisplayHandlerThread = CuvidDisplayHandlerThread,
    .OsdClear = GlxOsdClear,
    .OsdDrawARGB = GlxOsdDrawARGB,
    .OsdInit = GlxOsdInit,
    .OsdExit = GlxOsdExit,
    .MaxPixmapSize = GlxMaxPixmapSize,
    .Init = CuvidInit,
    .Exit = CuvidExit,
};

//----------------------------------------------------------------------------
//	CUVID OSD USE EGL
//----------------------------------------------------------------------------
#ifdef USE_EGL

#ifdef USE_OPENGLOSD
void *GetCuvidEglOsdOutputTexture(GLuint texture) {
    OsdGlTexture = texture;
    return 0;
}

int CuvidInitEgl(void) {
    int a = 0;

    if (!EglEnabled) {
        Debug(3,"video/osd: can't create egl context\n");
        return 0;
    }
    //after run an external player from time to time vdr not set playmode 1
    //then try start EGL forced.
    //is it a vdr bug or external player plugin???
    while (!EglContext || !EglThreadContext){
        usleep(1000);
        a++;
        if (a > 10 && a < 1000) {
            Debug(3,"Try start EGL forced\n");
            VideoDisplayWakeup();
            a = 1000;
        }
        if (a > 1010) return 0;
    }
    Debug(3,"Create OSD EGL context\n");
    eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglContext);
    return 1;
}
#endif

///
///	CUVID EGL module.
///
static const VideoModule CuvidEglModule = {
    .Name = "cuvid-egl",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) CuvidNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))CuvidDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))CuvidGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))CuvidReleaseSurface,
    .UnregisterSurface =
	(void (*const) (VideoHwDecoder *))CuvidUnregisterSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Cuvid_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))CuvidSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	CuvidGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))CuvidSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))CuvidGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))CuvidSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))CuvidResetStart,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))CuvidSetTrickSpeed,
#ifdef USE_GRAB
    .GrabOutput = CuvidGrabOutputSurface,
#endif
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, int *))CuvidGetStats,
    .SetBackground = CuvidSetBackground,
    .SetVideoMode = CuvidSetVideoMode,
#ifdef USE_AUTOCROP
    .ResetAutoCrop = CuvidResetAutoCrop,
#endif
    .DisplayHandlerThread = CuvidDisplayHandlerThread,
    .OsdClear = EglOsdClear,
    .OsdDrawARGB = EglOsdDrawARGB,
    .OsdInit = EglOsdInit,
    .OsdExit = EglOsdExit,
    .MaxPixmapSize = EglMaxPixmapSize,
    .Init = CuvidEglInit,
    .Exit = CuvidExit,
};
#endif

//----------------------------------------------------------------------------
//	NVDEC
//----------------------------------------------------------------------------

///
///	NVDEC decoder
///
typedef struct _nvdec_decoder_
{
     xcb_window_t Window;		///< output window

    int VideoX;				///< video base x coordinate
    int VideoY;				///< video base y coordinate
    int VideoWidth;			///< video base width
    int VideoHeight;			///< video base height

    int ScaleX;				///< video scale x coordinate
    int ScaleY;				///< video scale y coordinate
    int ScaleWidth;			///< video scale width
    int ScaleHeight;			///< video scale height

    int OutputX;			///< real video output x coordinate
    int OutputY;			///< real video output y coordinate
    int OutputWidth;			///< real video output width
    int OutputHeight;			///< real video output height

    enum AVPixelFormat PixFmt;		///< ffmpeg frame pixfmt
    enum AVColorSpace  ColorSpace;	/// ffmpeg ColorSpace
    enum AVColorTransferCharacteristic  trc;  // 
    enum AVColorPrimaries color_primaries;

    int Interlaced;			///< ffmpeg interlaced flag
    int TopFieldFirst;			///< ffmpeg top field displayed first

    int InputWidth;			///< video input width
    int InputHeight;			///< video input height
    AVRational InputAspect;		///< video input aspect ratio
    VideoResolutions Resolution;	///< resolution group

    int CropX;				///< video crop x
    int CropY;				///< video crop y
    int CropWidth;			///< video crop width
    int CropHeight;			///< video crop height

#ifdef USE_AUTOCROP
    void *AutoCropBuffer;		///< auto-crop buffer cache
    unsigned AutoCropBufferSize;	///< auto-crop buffer size
    AutoCropCtx AutoCrop[1];		///< auto-crop variables
#endif
    int SurfacesNeeded;			///< number of surface to request
    int SurfaceUsedN;			///< number of used video surfaces
    /// used video surface ids
    int SurfacesUsed[CODEC_SURFACES_MAX];
    int SurfaceFreeN;			///< number of free video surfaces
    /// free video surface ids
    int SurfacesFree[CODEC_SURFACES_MAX];
    /// video surface ring buffer
    int SurfacesRb[VIDEO_SURFACES_MAX *2];

    int SurfaceWrite;			///< write pointer
    int SurfaceRead;			///< read pointer
    atomic_t SurfacesFilled;		///< how many of the buffer is used

    CUarray cu_array[CODEC_SURFACES_MAX][2];
    CUgraphicsResource cu_res[CODEC_SURFACES_MAX][2];
    GLuint gl_textures[CODEC_SURFACES_MAX][2];  // where we will copy the CUDA result
    CUcontext cu_ctx;			///cuda context

    AVCodecContext *video_ctx;

    int SurfaceField;			///< current displayed field
    int TrickSpeed;			///< current trick speed
    int TrickCounter;			///< current trick speed counter
    struct timespec FrameTime;		///< time of last display
    VideoStream *Stream;		///< video stream
    int Closing;			///< flag about closing current stream
    int SyncOnAudio;			///< flag sync to audio
    int64_t PTS;			///< video PTS clock

    int LastAVDiff;			///< last audio - video difference
    int SyncCounter;			///< counter to sync frames
    int StartCounter;			///< counter for video start
    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
} NVdecDecoder;

static NVdecDecoder *NVdecDecoders[2];	///< open decoder streams
static int NVdecDecoderN;		///< number of decoder streams
static CudaFunctions *cu;
static CUdevice NVdecDevice;
static int NVdecSurfaceQueued;          ///< number of display surfaces queued

static struct timespec NVdecFrameTime;	///< time of last display

static pthread_mutex_t NVdecGrabMutex;

unsigned int size_tex_data;
unsigned int num_texels;
unsigned int num_values;

//----------------------------------------------------------------------------

///
///	Output video messages.
///
///	Reduce output.
///
///	@param level	message level (Error, Warning, Info, Debug, ...)
///	@param format	printf format string (NULL to flush messages)
///	@param ...	printf arguments
///
///	@returns true, if message shown
///
int NVdecMessage(int level, const char *format, ...)
{
    if (LogLevel > level || DebugLevel > level) {
	static const char *last_format;
	static char buf[256];
	va_list ap;

	va_start(ap, format);
	if (format != last_format) {	// don't repeat same message
	    if (buf[0]) {		// print last repeated message
		syslog(LOG_ERR, "%s", buf);
		buf[0] = '\0';
	    }

	    if (format) {
		last_format = format;
		vsyslog(LOG_ERR, format, ap);
	    }
	    va_end(ap);
	    return 1;
	}
	if (format)
	    vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
    }
    return 0;
}

//	Surfaces -------------------------------------------------------------

void NVdecCreateGlTexture(NVdecDecoder * decoder, unsigned int size_x, unsigned int size_y)
{
    int n, i, ret;


    if (GlxEnabled) {
        glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
        GlxCheck();
    }
#ifdef USE_EGL
    if (EglEnabled) {
        eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglThreadContext);
        EglCheck();;
    }
#endif
    // create texture planes
    for (n = 0; n < 2; n++) {       // number of planes
        glGenTextures(CODEC_SURFACES_MAX, decoder->gl_textures[n]);
        GlCheck();
    }
    Debug(3,"video/nvdec: create %d Textures Format %s w %d h %d \n",
        decoder->SurfacesNeeded, decoder->PixFmt != AV_PIX_FMT_P010LE && decoder->PixFmt != AV_PIX_FMT_YUV420P10LE ? "NV12/YV12" : "P010", size_x, size_y);

    for (i = 0; i < decoder->SurfacesNeeded; i++) {
        for (n = 0; n < 2; n++) {   // number of planes

            glBindTexture(GL_TEXTURE_2D, decoder->gl_textures[i][n]);
            GlCheck();
            // set basic parameters
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            if (decoder->PixFmt != AV_PIX_FMT_P010LE && decoder->PixFmt != AV_PIX_FMT_YUV420P10LE)
                glTexImage2D(GL_TEXTURE_2D, 0, n == 0 ? GL_R8 : GL_RG8, n == 0 ? size_x : size_x/2, n == 0 ? size_y : size_y/2, 0,
                     n == 0 ? GL_RED : GL_RG, GL_UNSIGNED_BYTE, NULL);
            else
                glTexImage2D(GL_TEXTURE_2D, 0, n == 0 ? GL_R16 : GL_RG16, n == 0 ? size_x : size_x/2, n == 0 ? size_y : size_y/2, 0,
                    n == 0 ? GL_RED : GL_RG, GL_UNSIGNED_SHORT, NULL);

            GlCheck();
            // register this texture with CUDA, not need for software decoder YV12
            if (decoder->PixFmt == AV_PIX_FMT_NV12 || decoder->PixFmt == AV_PIX_FMT_P010LE) {
                ret = CUStatus(cu->cuGraphicsGLRegisterImage(&decoder->cu_res[i][n], decoder->gl_textures[i][n],
                    GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD));
                if (ret < 0)
                    Fatal(_("video/nvdec: cuGraphicsGLRegisterImage failed \n"));
                ret = CUStatus(cu->cuGraphicsMapResources(1, &decoder->cu_res[i][n], 0));
                if (ret < 0)
                    Fatal(_("video/nvdec: cuGraphicsMapResources failed \n"));
                ret = CUStatus(cu->cuGraphicsSubResourceGetMappedArray(&decoder->cu_array[i][n], decoder->cu_res[i][n], 0, 0));
                if (ret < 0)
                    Fatal(_("video/nvdec: cuGraphicsSubResourceGetMappedArray failed \n"));
                ret = CUStatus(cu->cuGraphicsUnmapResources(1, &decoder->cu_res[i][n], 0));
                if (ret < 0)
                    Fatal(_("video/nvdec: cuGraphicsUnmapResources failed \n"));
            }
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    GlCheck();
}

///
///	Create surfaces for NVDEC decoder.
///
///	@param decoder	NVDEC hw decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void NVdecCreateSurfaces(NVdecDecoder * decoder, int width, int height)
{
    int i;

#ifdef DEBUG
    if (!decoder->SurfacesNeeded) {
	Error(_("video/nvdec: surface needed not set\n"));
	decoder->SurfacesNeeded = VIDEO_SURFACES_MAX * 2;
    }
#endif
    Debug(3, "video/nvdec: %s: %dx%d * %d\n", __FUNCTION__, width, height,
	decoder->SurfacesNeeded);

    // allocate only the number of needed surfaces
    decoder->SurfaceFreeN = decoder->SurfacesNeeded;

    NVdecCreateGlTexture(decoder, width, height);

    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	    decoder->SurfacesFree[i] = i;
	}
	Debug(4, "video/nvdec: created video surface %dx%d with id 0x%08x\n",
	    width, height, decoder->SurfacesFree[i]);
}

///
///	You need to unregister the NVDEC resource before closing the codec
//	otherwise you will get a video memory leak
///
static void NVdecUnregisterSurface(NVdecDecoder * decoder)
{
    int i,n;

    //not need for softdecoder
    if (decoder->PixFmt != AV_PIX_FMT_NV12 && decoder->PixFmt != AV_PIX_FMT_P010LE) return;

    if(GlxEnabled) {
        if(GlxThreadContext){
            glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
            GlxCheck();
        }
    }
#ifdef USE_EGL
    if (EglEnabled) {
        if(EglThreadContext){
            eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglThreadContext);
            EglCheck();
        }
    }
#endif
    for (i = 0; i < decoder->SurfacesNeeded; i++) {
        for (n = 0; n < 2; n++) {   // number of planes
            if (decoder->cu_res[i][n]) {
                CUStatus(cu->cuGraphicsUnregisterResource(decoder->cu_res[i][n]));
                decoder->cu_res[i][n] = NULL;
            }
        }
    }
}

///
///	Destroy surfaces of NVDEC decoder.
///
///	@param decoder	NVDEC hw decoder
///
static void NVdecDestroySurfaces(NVdecDecoder * decoder)
{
    int i;

    if(GlxEnabled) {
        if(GlxThreadContext){
            glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
            GlxCheck();
        }
    }
#ifdef USE_EGL
    if (EglEnabled) {
        if(EglThreadContext){
            eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglThreadContext);
            EglCheck();;
        }
    }
#endif
    for (i = 0; i < 2; i++) {       // number of planes
        glDeleteTextures(CODEC_SURFACES_MAX, decoder->gl_textures[i]);
        GlCheck();
    }
    if (decoder == NVdecDecoders[0]) {   // only when last decoder closes
        Debug(3,"Last decoder closes\n");
        if (gl_prog)
            glDeleteProgram(gl_prog);
        gl_prog = 0;
    }

    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	Debug(4, "video/nvdec: destroy video surface with id 0x%08x\n",
	    decoder->SurfacesFree[i]);
	decoder->SurfacesFree[i] = -1;
    }
    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	Debug(4, "video/nvdec: destroy video surface with id 0x%08x\n",
	    decoder->SurfacesUsed[i]);
	decoder->SurfacesUsed[i] = -1;
    }
    decoder->SurfaceFreeN = 0;
    decoder->SurfaceUsedN = 0;
}

///
///	Get a free surface.
///
///	@param decoder	NVDEC hw decoder
///
///	@returns the oldest free surface
///
static unsigned NVdecGetSurface0(NVdecDecoder * decoder)
{
    int surface;
    int i;

    if (!decoder->SurfaceFreeN) {
	Error(_("video/nvdec: out of surfaces\n"));
	return -1;
    }
    // use oldest surface
    surface = decoder->SurfacesFree[0];

    decoder->SurfaceFreeN--;
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	decoder->SurfacesFree[i] = decoder->SurfacesFree[i + 1];
    }
    decoder->SurfacesFree[i] = -1;

    // save as used
    decoder->SurfacesUsed[decoder->SurfaceUsedN++] = surface;

    return surface;
}

///
///	Release a surface.
///
///	@param decoder	NVDEC hw decoder
///	@param surface	surface no longer used
///
static void NVdecReleaseSurface(NVdecDecoder * decoder, int surface)
{
    int i;

    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	if (decoder->SurfacesUsed[i] == surface) {
	    // no problem, with last used
	    decoder->SurfacesUsed[i] =
		decoder->SurfacesUsed[--decoder->SurfaceUsedN];
	    decoder->SurfacesFree[decoder->SurfaceFreeN++] = surface;
	    return;
	}
    }
    Error(_("video/nvdec: release surface %#08x, which is not in use\n"),
	surface);
}

///
///	Debug NVDEC decoder frames drop...
///
///	@param decoder	NVDEC hw decoder
///
static void NVdecPrintFrames(const NVdecDecoder * decoder)
{
    Debug(3, "video/nvdec: %d missed, %d duped, %d dropped frames of %d,%d\n",
	decoder->FramesMissed, decoder->FramesDuped, decoder->FramesDropped,
	decoder->FrameCounter, decoder->FramesDisplayed);
#ifndef DEBUG
    (void)decoder;
#endif
}

static void NVdecMixerSetup(NVdecDecoder * decoder)
{
    if (decoder->video_ctx) {
#ifdef USE_AVFILTER
        VideoDecoder *ist = decoder->video_ctx->opaque;

        if (ist->hwaccel_pix_fmt == AV_PIX_FMT_CUDA) {
            if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceWeave) {
                Debug(3, "video/nvdec: set weave");
            } else if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceBob) {
                Debug(3, "video/nvdec: set yadif");
                if (decoder->video_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO)
                    CodecVideoInitFilter(ist, "yadif_cuda=1:-1:0");
                else
                    CodecVideoInitFilter(ist, "yadif_cuda=1:-1:1");
            } else if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceTemporal) {
                Debug(3, "video/nvdec: set bwdif");
                if (decoder->video_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO)
                    CodecVideoInitFilter(ist, "bwdif_cuda=1:-1:0");
                else
                    CodecVideoInitFilter(ist, "bwdif_cuda=1:-1:1");
            }
        } else { //software
            if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceWeave) {
                Debug(3, "video/nvdec: set weave");
            } else if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceBob) {
                Debug(3, "video/nvdec: set yadif");
                if (decoder->video_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO)
                    CodecVideoInitFilter(ist, "yadif=1:-1:0");
                else
                    CodecVideoInitFilter(ist, "yadif=1:-1:1");
            } else if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceTemporal) {
                Debug(3, "video/nvdec: set bwdif");
                if (decoder->video_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO)
                    CodecVideoInitFilter(ist, "bwdif=1:-1:0");
                else
                    CodecVideoInitFilter(ist, "bwdif=1:-1:1");
            }
        }
#endif
    }
}

///
///	Allocate new NVDEC decoder.
///
///	@param stream	video stream
///
///	@returns a new prepared nvdec hardware decoder.
///
static NVdecDecoder *NVdecNewHwDecoder(VideoStream * stream)
{
    NVdecDecoder *decoder;
    int i;

    Debug(3, "video/nvdec: %s\n", __FUNCTION__);
    if ((unsigned)NVdecDecoderN >=
	sizeof(NVdecDecoders) / sizeof(*NVdecDecoders)) {
    	Error(_("video/nvdec: out of decoders\n"));
	return NULL;
    }

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
	Error(_("video/nvdec: out of memory\n"));
	return NULL;
    }
    decoder->Window = VideoWindow;
    decoder->VideoWidth = VideoWindowWidth;
    decoder->VideoHeight = VideoWindowHeight;

    for (i = 0; i < CODEC_SURFACES_MAX; ++i) {
	decoder->SurfacesUsed[i] = -1;
	decoder->SurfacesFree[i] = -1;
    }

    //
    // setup video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < (VIDEO_SURFACES_MAX * 2); ++i) {
	decoder->SurfacesRb[i] = -1;
    }

#ifdef DEBUG
    if ((VIDEO_SURFACES_MAX) < 1 + 1 + 1 + 1) {
	Error(_
	    ("video/nvdec: need 1 future, 1 current, 1 back and 1 work surface\n"));
    }
#endif

    // Procamp operation parameterization data
//    decoder->Procamp.struct_version = VDP_PROCAMP_VERSION;
//    decoder->Procamp.brightness = 0.0;
//    decoder->Procamp.contrast = 1.0;
//    decoder->Procamp.saturation = 1.0;
//    decoder->Procamp.hue = 0.0;		// default values

    decoder->OutputWidth = VideoWindowWidth;
    decoder->OutputHeight = VideoWindowHeight;

    decoder->PixFmt = AV_PIX_FMT_NONE;

#ifdef USE_AUTOCROP
    //decoder->AutoCropBuffer = NULL;	// done by calloc
    //decoder->AutoCropBufferSize = 0;
#endif

    decoder->Stream = stream;
    if (!NVdecDecoderN) {		// FIXME: hack sync on audio
	decoder->SyncOnAudio = 1;
    }
    decoder->Closing = -300 - 1;

    decoder->PTS = AV_NOPTS_VALUE;

    NVdecDecoders[NVdecDecoderN++] = decoder;

    decoder->SurfaceUsedN = 1; // for correct cleanup after resume
    if (CUStatus(cu->cuCtxCreate(&decoder->cu_ctx, (unsigned int) CU_CTX_SCHED_BLOCKING_SYNC, NVdecDevice)))
        Fatal(_("video/nvdec: CUDA context create failed"));

    Debug(3, "CUDA API Version %d\n", CUDA_VERSION);

    return decoder;
}

///
///	Cleanup NVDEC.
///
///	@param decoder	NVDEC hw decoder
///
static void NVdecCleanup(NVdecDecoder * decoder)
{
    int i;
    Debug(3, "video/nvdec: %s\n", __FUNCTION__);
    if (decoder->SurfaceFreeN || decoder->SurfaceUsedN) {
	NVdecDestroySurfaces(decoder);
    }
    //
    // reset video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX * 2; ++i) {
	decoder->SurfacesRb[i] = -1;
    }

    decoder->SurfaceRead = 0;
    decoder->SurfaceWrite = 0;
    decoder->SurfaceField = 0;
    decoder->SyncCounter = 0;
    decoder->Interlaced = 0;
    decoder->FrameCounter = 0;
    decoder->FramesDisplayed = 0;
    decoder->StartCounter = 0;
    decoder->Closing = 0;
    decoder->PTS = AV_NOPTS_VALUE;
    VideoDeltaPTS = 0;
}

///
///	Destroy a NVDEC decoder.
///
///	@param decoder	NVDEC hw decoder
///
static void NVdecDelHwDecoder(NVdecDecoder * decoder)
{
    int i;
    Debug(3, "video/nvdec: %s\n", __FUNCTION__);
    for (i = 0; i < NVdecDecoderN; ++i) {
	if (NVdecDecoders[i] == decoder) {
	    NVdecDecoders[i] = NULL;
	    // copy last slot into empty slot
	    if (i < --NVdecDecoderN) {
		NVdecDecoders[i] = NVdecDecoders[NVdecDecoderN];
	    }

	    NVdecCleanup(decoder);
	    NVdecPrintFrames(decoder);

            if (decoder->cu_ctx) {
                cu->cuCtxDestroy (decoder->cu_ctx);
                decoder->cu_ctx = NULL;
            }
#ifdef USE_AUTOCROP
	    free(decoder->AutoCropBuffer);
#endif
	    free(decoder);

	    return;
	}
    }
    Error(_("video/nvdec: decoder not in decoder list.\n"));
}

///
///	NVDEC setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns true if NVDEC could be initialized, false otherwise.
///
static int NVdecInit(const char *display_name)
{
    int ret;

    Info(_("video/nvdec: Start NVDEC\n"));

    GlxEnabled = 1;

    GlxInit();
    if (GlxEnabled) {
        GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight, GlxSharedContext);
    }
    if (!GlxEnabled) {
        Error(_("video/nvdec: GLX error\n"));
        return 0;
    }

    glGenBuffers(1,&vao_buffer);
    GlxCheck();

    if (!cu) {
        ret = cuda_load_functions(&cu, NULL);
        if (ret < 0) {
            Error(_("Could not dynamically load CUDA\n"));
            return 0;
        }
    }
    ret = CUStatus(cu->cuInit(0));
    if (ret < 0) {
        Error(_("Could not init CUDA device\n"));
        return 0;
    }

    ret = CUStatus(cu->cuDeviceGet(&NVdecDevice, 0));
    if (ret < 0) {
        Error(_("Could not get CUDA device\n"));
        return 0;
    }

    pthread_mutex_init(&NVdecGrabMutex, NULL);

    Info(_("video/nvdec: Start NVdec ok\n"));

    (void)display_name;
    return 1;
}

#ifdef USE_EGL
static int NVdecEglInit(const char *display_name)
{
    int ret;

    Info(_("video/nvdec: Start NVDEC\n"));

    EglEnabled = 1;

    EglInit();
    if (EglEnabled) {
	EglSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight,
	    EglSharedContext);
    }
    if (!EglEnabled) {
        Error(_("video/nvdec: EGL error\n"));
        return 0;
    }
    if (!cu) {
        ret = cuda_load_functions(&cu, NULL);
        if (ret < 0) {
            Error(_("Could not dynamically load CUDA\n"));
            return 0;
        }
    }
    ret = CUStatus(cu->cuInit(0));
    if (ret < 0) {
        Error(_("Could not init CUDA device\n"));
        return 0;
    }

    ret = CUStatus(cu->cuDeviceGet(&NVdecDevice, 0));
    if (ret < 0) {
        Error(_("Could not get CUDA device\n"));
        return 0;
    }
    pthread_mutex_init(&NVdecGrabMutex, NULL);

    Info(_("video/nvdec: Start NVDEC ok\n"));

    (void)display_name;
    return 1;
}
#endif


///
///	NVDEC cleanup.
///
static void NVdecExit(void)
{
    int i;
    Debug(3, "video/nvdec: %s\n", __FUNCTION__);

    for (i = 0; i < NVdecDecoderN; ++i) {
	if (NVdecDecoders[i]) {
	    NVdecDelHwDecoder(NVdecDecoders[i]);
	    NVdecDecoders[i] = NULL;
	}
    }
    NVdecDecoderN = 0;
    pthread_mutex_destroy(&NVdecGrabMutex);

    glDeleteBuffers(1, &vao_buffer);
    vao_buffer = 0;

    NVdecDevice = (CUdevice)(uintptr_t)NULL;
    cuda_free_functions(&cu);
}

///
///	Update output for new size or aspect ratio.
///
///	@param decoder	NVDEC hw decoder
///
static void NVdecUpdateOutput(NVdecDecoder * decoder)
{
    Debug(3, "video/nvdec: %s\n", __FUNCTION__);

    VideoUpdateOutput(decoder->InputAspect, decoder->InputWidth,
	decoder->InputHeight, decoder->Resolution, decoder->VideoX,
	decoder->VideoY, decoder->VideoWidth, decoder->VideoHeight,
	&decoder->OutputX, &decoder->OutputY, &decoder->OutputWidth,
	&decoder->OutputHeight, &decoder->CropX, &decoder->CropY,
	&decoder->CropWidth, &decoder->CropHeight);
#ifdef USE_AUTOCROP
    decoder->AutoCrop->State = 0;
    decoder->AutoCrop->Count = AutoCropDelay;
#endif
}

///
///	Configure NVDEC for new video format.
///
///	@param decoder	NVDEC hw decoder
///
static void NVdecSetupOutput(NVdecDecoder * decoder)
{
    // FIXME: need only to create and destroy surfaces for size changes
    //		or when number of needed surfaces changed!
    decoder->Resolution =
	VideoResolutionGroup(decoder->InputWidth, decoder->InputHeight,
	decoder->Interlaced);
    VideoResolution = decoder->Resolution;

    NVdecMixerSetup(decoder);

    NVdecCreateSurfaces(decoder, decoder->InputWidth, decoder->InputHeight);

    NVdecUpdateOutput(decoder);		// update aspect/scaling
}

///
///	Get a free surface.  Called from ffmpeg.
///
///	@param decoder		NVDEC hw decoder
///	@param video_ctx	ffmpeg video codec context
///
///	@returns the oldest free surface
///
static unsigned NVdecGetSurface(NVdecDecoder * decoder,
    const AVCodecContext * video_ctx)
{

#ifdef FFMPEG_BUG1_WORKAROUND
    // get_format not called with valid informations.
    if (video_ctx->width != decoder->InputWidth
	|| video_ctx->height != decoder->InputHeight) {

	NVdecCleanup(decoder);

	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;
	NVdecSetupOutput(decoder);
    }
#else
    (void)video_ctx;
#endif
    return NVdecGetSurface0(decoder);
}

void nvdec_uninit(AVCodecContext *s)
{
    VideoDecoder *ist = s->opaque;

    ist->hwaccel_uninit = NULL;
    av_buffer_unref(&s->hw_frames_ctx);
    s->hw_frames_ctx = NULL;
}

static int nvdec_alloc(AVCodecContext *s, NVdecDecoder * decoder)
{
    VideoDecoder *ist = s->opaque;
    int ret;

    AVHWDeviceContext *device_ctx;
    AVCUDADeviceContext *device_hwctx;
    AVHWFramesContext *frames_ctx;
    AVBufferRef *hw_frames_ctx = NULL;
    AVBufferRef *hw_device_ctx;

    Debug(3, "nvdec_alloc\n");

    ist->hwaccel_uninit = nvdec_uninit;

    hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if (!hw_device_ctx) {
	Debug(3, "NVDEC init failed for av_hwdevice_ctx_alloc\n");
	goto fail;
    }

    device_ctx = (AVHWDeviceContext*)hw_device_ctx->data;
    device_hwctx = device_ctx->hwctx;
    device_hwctx->cuda_ctx = decoder->cu_ctx;

    ret = av_hwdevice_ctx_init(hw_device_ctx);
    if (ret < 0) {
	Debug(3, "NVDEC init failed for av_hwdevice_ctx_init\n");
	goto fail;
    }

    hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
    if (!hw_frames_ctx) {
	Debug(3, "NVDEC init failed for av_hwframe_ctx_alloc\n");
	goto fail;
    }

    frames_ctx = (AVHWFramesContext*)hw_frames_ctx->data;
    frames_ctx->format = AV_PIX_FMT_CUDA;
    frames_ctx->sw_format = decoder->PixFmt;
    frames_ctx->width = s->width;
    frames_ctx->height = s->height;
    frames_ctx->initial_pool_size = 15;

    ret = av_hwframe_ctx_init(hw_frames_ctx);
    if (ret < 0) {
	Debug(3, "NVDEC init failed for av_hwframe_ctx_init\n");
	goto fail;
    }

    s->hw_device_ctx = hw_device_ctx;
    s->hw_frames_ctx = hw_frames_ctx;

    return 0;
fail:
    Debug(3, "NVDEC init failed for stream #\n");
    nvdec_uninit(s);
    return AVERROR(EINVAL);
}

int nvdec_init(AVCodecContext *s, NVdecDecoder * decoder)
{
    if (s->hw_frames_ctx)
	nvdec_uninit(s);
    if (!s->hw_frames_ctx) {
	int ret = nvdec_alloc(s, decoder);
	if (ret < 0)
	    return ret;
    }
    return 0;
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param fmt	is the list of formats which are supported by the codec,
///			it is terminated by -1 as 0 is a valid format, the
///			formats are ordered by quality.
///
static enum AVPixelFormat NVdec_get_format(NVdecDecoder * decoder,
    AVCodecContext * video_ctx, const enum AVPixelFormat *fmt)
{
    const enum AVPixelFormat *fmt_idx;
    CUcontext dummy;
    VideoDecoder *ist = video_ctx->opaque;
    int bit10 = 0;

    Debug(3,"get format  %dx%d\n",video_ctx->width,video_ctx->height);

    if (!VideoHardwareDecoder || (video_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO
	    && VideoHardwareDecoder == HWmpeg2Off)
	) {				// hardware disabled by config
	Debug(3, "codec: hardware acceleration disabled\n");
	goto slow_path;
    }
    //
    //	look through formats
    //
    Debug(3, "%s: codec %d fmts:\n", __FUNCTION__, video_ctx->codec_id);
    for (fmt_idx = fmt; *fmt_idx != AV_PIX_FMT_NONE; fmt_idx++) {
        Debug(3, "\t%#010x %s\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx));
        if (*fmt_idx == AV_PIX_FMT_YUV420P10LE) bit10 = 1;
    }

    for (fmt_idx = fmt; *fmt_idx != AV_PIX_FMT_NONE; fmt_idx++) {
	// check supported pixel format with entry point
	switch (*fmt_idx) {
	    case AV_PIX_FMT_CUDA:
		break;
	    default:
		continue;
	}
	break;
    }
    Debug(3, "%s: selected codec:\n", __FUNCTION__);
    Debug(3, "\t%#010x %s bit %d\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx), bit10 ? 10 : 8);

    if (*fmt_idx == AV_PIX_FMT_NONE) {
	Error(_("video/nvdec: no valid nvdec pixfmt found\n"));
	goto slow_path;
    }

    if (ist->GetFormatDone) return AV_PIX_FMT_CUDA;

    decoder->PixFmt = bit10 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    ist->active_hwaccel_id = HWACCEL_CUVID;
    ist->hwaccel_pix_fmt = AV_PIX_FMT_CUDA;
    ist->hwaccel_get_buffer = NULL;
    video_ctx->draw_horiz_band = NULL;
    video_ctx->slice_flags = 0;
    video_ctx->pix_fmt = AV_PIX_FMT_CUDA;
    decoder->video_ctx = video_ctx;

    if (video_ctx->width && video_ctx->height) {
        int ret;
        NVdecCleanup(decoder);
        ret = nvdec_init(video_ctx, decoder);
        if (ret < 0) {
            Debug(3, "nvdec_init failed\n");
            goto slow_path;
        }
	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;
        decoder->SurfacesNeeded = VIDEO_SURFACES_MAX * 2 + 1;
	NVdecSetupOutput(decoder);
    }

    Debug(3,"NVDEC Init ok %dx%d\n",video_ctx->width,video_ctx->height);
    ist->GetFormatDone = 1;
    return AV_PIX_FMT_CUDA;

  slow_path:
    // no accelerated format found

    ist->active_hwaccel_id = HWACCEL_NONE;
    ist->hwaccel_pix_fmt   = AV_PIX_FMT_NONE;
    ist->hwaccel_get_buffer = NULL;
    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX * 2 + 2;
    decoder->PixFmt = AV_PIX_FMT_NONE;
    video_ctx->thread_count = 0;
    decoder->InputWidth = 0;
    decoder->InputHeight = 0;
    video_ctx->hwaccel_context = NULL;
    video_ctx->draw_horiz_band = NULL;

    cu->cuCtxPopCurrent(&dummy);
    ist->GetFormatDone = 1;
    return avcodec_default_get_format(video_ctx, fmt);
}

#ifdef USE_GRAB
///
///	Grab output surface already locked.
//	Grab openGL video + osd (screenshot)
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *NVdecGrabOutputSurfaceLocked(int *ret_size, int *ret_width, int *ret_height)
{
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint8_t *base;
    size_t i, j, k, cur_gl, cur_rgb;
    GLubyte *pixels;
    double scalew, scaleh;

    typedef struct {
        uint32_t x0;
        uint32_t y0;
        uint32_t x1;
        uint32_t y1;
    } CuRect;
    CuRect source_rect;

    NVdecDecoder *decoder;

    decoder = NVdecDecoders[0];
    if (decoder == NULL)   // no video aktiv
	return NULL;

    //get real surface size
    width = VideoWindowWidth;
    height = VideoWindowHeight;


    Debug(3, "video/nvdec: grab %dx%d\n", width, height);

    source_rect.x0 = 0;
    source_rect.y0 = 0;
    source_rect.x1 = width;
    source_rect.y1 = height;

    if (ret_width && ret_height) {
	if (*ret_width <= -64) {	// this is an Atmo grab service request
	    int overscan;

	    // calculate aspect correct size of analyze image
	    width = *ret_width * -1;
	    height = (width * source_rect.y1) / source_rect.x1;

	    // calculate size of grab (sub) window
	    overscan = *ret_height;

	    if (overscan > 0 && overscan <= 200) {
		source_rect.x0 = source_rect.x1 * overscan / 1000;
		source_rect.x1 -= source_rect.x0;
		source_rect.y0 = source_rect.y1 * overscan / 1000;
		source_rect.y1 -= source_rect.y0;
	    }
	} else {
	    if (*ret_width > 0 && (unsigned)*ret_width < width) {
		width = *ret_width;
	    }
	    if (*ret_height > 0 && (unsigned)*ret_height < height) {
		height = *ret_height;
	    }
	}

	Debug(3, "video/nvdec: grab source rect %d,%d:%d,%d dest dim %dx%d\n",
	source_rect.x0, source_rect.y0, source_rect.x1, source_rect.y1, width, height);

	size = width * height * 4;;

	base = malloc(size*sizeof(uint8_t));
	if (!base) {
	    Error(_("video/nvdec: grab out of memory\n"));
	    return NULL;
	}

	pixels = malloc(VideoWindowWidth * VideoWindowHeight * 4 * sizeof(GLubyte));
	if (!pixels) {
	    Error(_("video/nvdec: grab out of memory\n"));
	    free(base);
	    return NULL;
	}
	pthread_mutex_lock(&VideoLockMutex);
        if (GlxEnabled) {
	    glXMakeCurrent(XlibDisplay, VideoWindow, GlxSharedContext);
	    GlxCheck();
        }
#ifdef USE_EGL
        if (EglEnabled) {
            eglMakeCurrent(EglDisplay, EglSurface, EglSurface, EglSharedContext);
            EglCheck();
        }
#endif
	/* Get BGRA to align to 32 bits instead of just 24 for RGB */
	glReadPixels(source_rect.x0, source_rect.y0, VideoWindowWidth, VideoWindowHeight, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        GlCheck();

        if (GlxEnabled) {
	    glXMakeCurrent(XlibDisplay, None, NULL);
	    GlxCheck();
        }
#ifdef USE_EGL
        if (EglEnabled) {
            eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            EglCheck();
        }
#endif
	pthread_mutex_unlock(&VideoLockMutex);
	scalew = (double)VideoWindowWidth / width;
	scaleh = (double)VideoWindowHeight / height;

	// convert 32 -> 24 and simple scale
	for (i = 0; i < height; i++) {
	    for (j = 0; j < width; j++) {
	        cur_gl  = 4 * (VideoWindowWidth * (int)(scaleh * (height - i - 1)) + (int)(scalew * j));
	        cur_rgb = 4 * (width * i + j);
	            for (k = 0; k < 4; k++)
	                (base)[cur_rgb + k] = (pixels)[cur_gl + k];
	    }
	}

	free(pixels);

	Debug(3,"got grab data\n");

	*ret_size = size;
	*ret_width = width;
	*ret_height = height;
	return base;
    }

    return NULL;
}

///
///	Grab output surface.
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *NVdecGrabOutputSurface(int *ret_size, int *ret_width,  int *ret_height)
{
    uint8_t *img;

    if (!gl_prog) {
        return NULL;			// nvdec video module not yet initialized
    }

    pthread_mutex_lock(&NVdecGrabMutex);
//	pthread_mutex_lock(&VideoLockMutex);
    img = NVdecGrabOutputSurfaceLocked(ret_size, ret_width, ret_height);
//	pthread_mutex_unlock(&VideoLockMutex);
    pthread_mutex_unlock(&NVdecGrabMutex);
    return img;
}

#endif

#ifdef USE_AUTOCROP

///
///	NVDEC auto-crop support.
///
///	@param decoder	NVDEC hw decoder
///
static void NVdecAutoCrop(NVdecDecoder * decoder)
{
    int surface;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[3];
    uint32_t pitches[3];
    int crop14;
    int crop16;
    int next_state;

    surface = decoder->SurfacesRb[(decoder->SurfaceRead + 1) %  (VIDEO_SURFACES_MAX * 2)];

    width = decoder->InputWidth;
    height = decoder->InputHeight;

    size = width * height + ((width + 1) / 2) * ((height + 1) / 2)
	+ ((width + 1) / 2) * ((height + 1) / 2);
    // cache buffer for reuse
    base = decoder->AutoCropBuffer;
    if (size > decoder->AutoCropBufferSize) {
	free(base);
	decoder->AutoCropBuffer = malloc(size);
	base = decoder->AutoCropBuffer;
	decoder->AutoCropBufferSize = size;
    }
    if (!base) {
	Error(_("video/nvdec: out of memory\n"));
	return;
    }
    pitches[0] = width;
    pitches[1] = width / 2;
    pitches[2] = width / 2;
    data[0] = base;
    data[1] = base + width * height;
    data[2] = base + width * height + width * height / 4;

    //we need Y in data[0] only
    glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][0]);
    glGetTexImage(GL_TEXTURE_2D,0,GL_RED, GL_UNSIGNED_BYTE,base);
    //glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][1]);
    //glGetTexImage(GL_TEXTURE_2D,0,GL_RG, GL_UNSIGNED_BYTE,base + width * height);

    AutoCropDetect(decoder->AutoCrop, width, height, data, pitches);

    glBindTexture(GL_TEXTURE_2D, 0);

    // ignore black frames
    if (decoder->AutoCrop->Y1 >= decoder->AutoCrop->Y2) {
	return;
    }

    crop14 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 14);
    crop14 = (decoder->InputHeight - crop14) / 2;
    crop16 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 16);
    crop16 = (decoder->InputHeight - crop16) / 2;

    if (decoder->AutoCrop->Y1 >= crop16 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop16 - AutoCropTolerance) {
	next_state = 16;
    } else if (decoder->AutoCrop->Y1 >= crop14 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop14 - AutoCropTolerance) {
	next_state = 14;
    } else {
	next_state = 0;
    }

    if (decoder->AutoCrop->State == next_state) {
	return;
    }

    Debug(3, "video: crop aspect %d:%d %d/%d %d%+d\n",
	decoder->InputAspect.num, decoder->InputAspect.den, crop14, crop16,
	decoder->AutoCrop->Y1, decoder->InputHeight - decoder->AutoCrop->Y2);

    Debug(3, "video: crop aspect %d -> %d\n", decoder->AutoCrop->State,	next_state);

    switch (decoder->AutoCrop->State) {
	case 16:
	case 14:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay / 2) {
		return;
	    }
	    break;
	case 0:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay) {
		return;
	    }
	    break;
    }

    decoder->AutoCrop->State = next_state;
    if (next_state) {
		decoder->CropX = VideoCutLeftRight[decoder->Resolution];
		decoder->CropY =
			(next_state ==
			16 ? crop16 : crop14) + VideoCutTopBottom[decoder->Resolution];
		decoder->CropWidth = decoder->InputWidth - decoder->CropX * 2;
		decoder->CropHeight = decoder->InputHeight - decoder->CropY * 2;

		// FIXME: this overwrites user choosen output position
		// FIXME: resize kills the auto crop values
		// FIXME: support other 4:3 zoom modes
		decoder->OutputX = decoder->VideoX;
		decoder->OutputY = decoder->VideoY;
		decoder->OutputWidth = (decoder->VideoHeight * next_state) / 9;
		decoder->OutputHeight = (decoder->VideoWidth * 9) / next_state;
		if (decoder->OutputWidth > decoder->VideoWidth) {
			decoder->OutputWidth = decoder->VideoWidth;
			decoder->OutputY +=
			(decoder->VideoHeight - decoder->OutputHeight) / 2;
		} else if (decoder->OutputHeight > decoder->VideoHeight) {
			decoder->OutputHeight = decoder->VideoHeight;
			decoder->OutputX +=
			(decoder->VideoWidth - decoder->OutputWidth) / 2;
		}
		Debug(3, "video: aspect output %dx%d %dx%d%+d%+d\n",
			decoder->InputWidth, decoder->InputHeight, decoder->OutputWidth,
			decoder->OutputHeight, decoder->OutputX, decoder->OutputY);
    } else {
		// sets AutoCrop->Count
		NVdecUpdateOutput(decoder);
    }
    decoder->AutoCrop->Count = 0;
}

///
///	NVDEC check if auto-crop todo.
///
///	@param decoder	NVDEC hw decoder
///
///	@note a copy of VaapiCheckAutoCrop
///	@note auto-crop only supported with normal 4:3 display mode
///
static void NVdecCheckAutoCrop(NVdecDecoder * decoder)
{
    // reduce load, check only n frames
    if (Video4to3ZoomMode == VideoNormal && AutoCropInterval
	&& !(decoder->FrameCounter % AutoCropInterval)) {
	AVRational input_aspect_ratio;
	AVRational tmp_ratio;
	av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den,
	    decoder->InputWidth * decoder->InputAspect.num,
	    decoder->InputHeight * decoder->InputAspect.den, 1024 * 1024);

	tmp_ratio.num = 4;
	tmp_ratio.den = 3;
	// 4:3 with 16:9/14:9 inside
	if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	    NVdecAutoCrop(decoder);
	} else {
	// 15:11 with 16:9/14:9 inside
	    tmp_ratio.num = 15;
	    tmp_ratio.den = 11;
	    if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	        NVdecAutoCrop(decoder);
	    } else {
	        decoder->AutoCrop->Count = 0;
	        decoder->AutoCrop->State = 0;
	    }
	}
    }
}

///
///	NVDEC reset auto-crop.
///
static void NVdecResetAutoCrop(void)
{
    int i;

    for (i = 0; i < NVdecDecoderN; ++i) {
	NVdecDecoders[i]->AutoCrop->State = 0;
	NVdecDecoders[i]->AutoCrop->Count = 0;
    }
}

#endif

///
///	Queue output surface.
///
///	@param decoder	NVDEC hw decoder
///	@param surface	output surface
///	@param softdec	software decoder
///
///	@note we can't mix software and hardware decoder surfaces
///
static void NVdecQueueSurface(NVdecDecoder * decoder, int surface,
    int softdec)
{
    int old;

    ++decoder->FrameCounter;

    if (1) {				// can't wait for output queue empty
	if (atomic_read(&decoder->SurfacesFilled) >= (VIDEO_SURFACES_MAX * 2)) {
	    Warning(_
		("video/nvdec: output buffer full, dropping frame (%d/%d)\n"),
		++decoder->FramesDropped, decoder->FrameCounter);
	    if (!(decoder->FramesDisplayed % 300)) {
		NVdecPrintFrames(decoder);
	    }
	    // software surfaces only
	    if (softdec) {
		NVdecReleaseSurface(decoder, surface);
	    }
	    return;
	}
#if 0
    } else {				// wait for output queue empty
	while (atomic_read(&decoder->SurfacesFilled) >= (VIDEO_SURFACES_MAX * 2)) {
	    VideoDisplayHandler();
	}
#endif
    }

    //
    //	    Check and release, old surface
    //
    if ((old = decoder->SurfacesRb[decoder->SurfaceWrite])
	!= -1) {

	// now we can release the surface, software surfaces only
	if (softdec) {
	    NVdecReleaseSurface(decoder, old);
	}
    }

    Debug(4, "video/nvdec: yy video surface %#08x@%d ready\n", surface,
	decoder->SurfaceWrite);

    decoder->SurfacesRb[decoder->SurfaceWrite] = surface;
    decoder->SurfaceWrite = (decoder->SurfaceWrite + 1)
	% (VIDEO_SURFACES_MAX * 2);
    atomic_inc(&decoder->SurfacesFilled);
}

///
///	Render a ffmpeg frame.
///
///	@param decoder		NVDEC hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void NVdecRenderFrame(NVdecDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    int surface;
    VideoDecoder        *ist = video_ctx->opaque;
    int interlaced;
    enum AVColorSpace color;
    int n;
    AVRational aspect_ratio;

    // FIXME: some tv-stations toggle interlace on/off
    // frame->interlaced_frame isn't always correct set
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
    interlaced = frame->interlaced_frame;
#else
    interlaced = frame->flags & AV_FRAME_FLAG_INTERLACED;
#endif
    //25, 30 - interlaced; 50, 60 - progressive; hevc - progressive allways
    if (video_ctx->framerate.num > 0) {
	if (video_ctx->framerate.num / video_ctx->framerate.den > 30) interlaced = 0;
	else interlaced = 1;
    }

    if (video_ctx->codec_id == AV_CODEC_ID_HEVC) interlaced = 0;

    // FIXME: should be done by init video_ctx->field_order
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != frame->top_field_first) {

	Debug(4, "video/nvdec: interlaced %d top-field-first %d num %d den %d\n", frame->interlaced_frame,
	    frame->top_field_first, video_ctx->framerate.num, video_ctx->framerate.den);

	decoder->TopFieldFirst = frame->top_field_first;
#else
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)) {

	Debug(4, "video/nvdec: interlaced %d top-field-first %d num %d den %d\n", frame->flags & AV_FRAME_FLAG_INTERLACED,
	    frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST, video_ctx->framerate.num, video_ctx->framerate.den);

	decoder->TopFieldFirst = frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST;
#endif
	decoder->SurfaceField = !decoder->Interlaced;
	decoder->Interlaced = interlaced;
    }
    //aspect correction
    aspect_ratio = frame->sample_aspect_ratio;
    if ((video_ctx->width >= 352  && video_ctx->width <= 720 && video_ctx->height == 288) ||
	(video_ctx->width > 1300 && video_ctx->width <= 1920 && video_ctx->height == 540)) {
	aspect_ratio.den *= 2;
    }
    // update aspect ratio changes
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, aspect_ratio)) {
	Debug(3, "video/nvdec: aspect ratio changed\n");
	decoder->InputAspect = aspect_ratio;
	NVdecUpdateOutput(decoder);
    }

    color = frame->colorspace;
    if (color == AVCOL_SPC_UNSPECIFIED)   // if unknown
        color = AVCOL_SPC_BT709;

    //
    // render
    //
    //
    // Check image, format, size
    //

    if (ist->hwaccel_pix_fmt != video_ctx->pix_fmt
        || video_ctx->width != decoder->InputWidth
        || video_ctx->height != decoder->InputHeight) {
        if (video_ctx->pix_fmt != AV_PIX_FMT_CUDA) { // for softdecode
            decoder->PixFmt = video_ctx->pix_fmt;
            ist->hwaccel_pix_fmt = video_ctx->pix_fmt;
            ist->active_hwaccel_id = HWACCEL_NONE;
        }
        decoder->InputWidth = video_ctx->width;
        decoder->InputHeight = video_ctx->height;
        decoder->video_ctx = (AVCodecContext *)video_ctx;

        NVdecCleanup(decoder);
        decoder->SurfacesNeeded = VIDEO_SURFACES_MAX * 2 + 1;
        NVdecSetupOutput(decoder);
    }
    //
    // Copy data from frame to image
    //
    switch (video_ctx->pix_fmt) {
        case AV_PIX_FMT_CUDA:
//        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_YUV420P:      // for softdecode
//        case AV_PIX_FMT_YUVJ420P:     // some streams produce this
        case AV_PIX_FMT_YUV420P10LE:  // for softdecode of HEVC 10 Bit
        break;
        case AV_PIX_FMT_YUV422P:
//        case AV_PIX_FMT_YUV444P:
        break;
        default:
            Error(_("video/nvdec: pixel format %d not supported\n"),
            video_ctx->pix_fmt);
            // FIXME: no fatals!
    }
    decoder->ColorSpace = color;     // save colorspace
    decoder->trc = frame->color_trc;
    decoder->color_primaries = frame->color_primaries;

    surface = NVdecGetSurface0(decoder);

    if (surface == -1)     // no free surfaces
        return;
    if (video_ctx->pix_fmt == AV_PIX_FMT_CUDA) { //hardware

        cu->cuCtxPushCurrent(decoder->cu_ctx);
        for (n = 0; n < 2; n++) { //.
            // widthInBytes must account for the chroma plane
            // elements being two samples wide.
            CUDA_MEMCPY2D cpy = {
                .srcMemoryType = CU_MEMORYTYPE_DEVICE,
                .dstMemoryType = CU_MEMORYTYPE_ARRAY,
                .srcDevice     = (CUdeviceptr)frame->data[n],
                .srcPitch      = frame->linesize[n],
                .srcY          = 0,
                .dstArray      = decoder->cu_array[surface][n],
                .WidthInBytes  = decoder->InputWidth * (decoder->PixFmt == AV_PIX_FMT_NV12 ? 1 : 2),
                .Height        = n == 0 ? decoder->InputHeight : decoder->InputHeight / 2,
            };
            if (CUStatus(cu->cuMemcpy2D(&cpy))) Fatal(_("video/nvdec: render frame error\n"));
        }
        NVdecQueueSurface(decoder, surface, 1);

    } else {       //software
        uint8_t *outY = NULL;
        uint8_t *outUV = NULL;

        if(GlxEnabled) {
            glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
            GlxCheck();
        }
#ifdef USE_EGL
        if (EglEnabled) {
            eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglThreadContext);
            EglCheck();
        }
#endif
        if (decoder->PixFmt != AV_PIX_FMT_YUV420P10LE) { //8bit
            //YV12 -> NV12
            outUV = (uint8_t*) malloc(frame->linesize[1] * decoder->InputHeight * sizeof(uint8_t));

            if (!outUV) {
                Error(_("video/ndec: out of memory\n"));
                return;
            }
            //TODO use shader or direct yuv render?
            for (int i = 0; i < (frame->linesize[1] * decoder->InputHeight / 2); i++) {
                memcpy(outUV + i * 2, frame->data[1] + i, 1); // For NV12, U first
                memcpy(outUV + i * 2 + 1, frame->data[2] + i, 1); // For NV12, V second
            }

            //Y
            glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][0]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[0]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decoder->InputWidth,
                decoder->InputHeight, GL_RED, GL_UNSIGNED_BYTE, frame->data[0]);
            GlCheck();
            //UV
            glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][1]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[1]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decoder->InputWidth/2,
                decoder->InputHeight/2, GL_RG, GL_UNSIGNED_BYTE, outUV);
            GlCheck();
            glBindTexture(GL_TEXTURE_2D, 0);

        } else { //10bit
            //yuv420ple -> p010le + 10bit -> 8bit
            outY = (uint8_t*) malloc(frame->linesize[0] * decoder->InputHeight / 2 * sizeof(uint8_t));

            if (!outY) {
                Error(_("video/ndec: out of memory\n"));
                return;
            }

            outUV = (uint8_t*) malloc(frame->linesize[1] * decoder->InputHeight / 2 * sizeof(uint8_t));

            if (!outUV) {
                Error(_("video/ndec: out of memory\n"));
                return;
            }

            //TODO use shader or direct yuv render?
            for (int i = 0; i < (frame->linesize[0] / 2 * decoder->InputHeight); i++) {
                outY[i] = (*((uint16_t*)frame->data[0] + i) + 2) >> 2; // Y
            }

            for (int i = 0; i < (frame->linesize[1] * decoder->InputHeight / 4); i++) {
                outUV[i * 2] = (*((uint16_t*)frame->data[1] + i) + 2) >> 2; // For p010le, U first
                outUV[i * 2 + 1] = (*((uint16_t*)frame->data[2] + i) + 2) >> 2; // For p010le, V second
            }

            //Y
            glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][0]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[0]/2);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decoder->InputWidth,
                decoder->InputHeight,  GL_RED, GL_UNSIGNED_BYTE, outY);
            GlCheck();
            //UV
            glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][1]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[1]/2);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decoder->InputWidth/2,
                decoder->InputHeight/2, GL_RG, GL_UNSIGNED_BYTE, outUV);
            GlCheck();
            glBindTexture(GL_TEXTURE_2D, 0);

        }
        Debug(4, "video/nvdec: sw render hw surface %#08x\n", surface);

        NVdecQueueSurface(decoder, surface, 1);
        if (outY) free(outY);
        if (outUV) free(outUV);
    }
    if (decoder->Interlaced) {
	++decoder->FrameCounter;
    }
}

///
///	Get hwaccel context for ffmpeg.
///
///	@param decoder	NVDEC hw decoder
///
static void *NVdecGetHwAccelContext(NVdecDecoder * decoder)
{
    Debug(3, "Initializing nvdec hwaccel\n");
    cu->cuCtxPushCurrent(decoder->cu_ctx);
    (void)decoder;

    return decoder->cu_ctx;
}

static void NVdecMixVideo(NVdecDecoder * decoder, int level)
{
    int current;
    int y;
    float xcropf, ycropf;
    GLint texLoc;

#ifdef USE_AUTOCROP
    // FIXME: can move to render frame
    NVdecCheckAutoCrop(decoder);
#endif

    xcropf = (float) decoder->CropX / (float) decoder->InputWidth;
    ycropf = (float) decoder->CropY / (float) decoder->InputHeight;

    current = decoder->SurfacesRb[decoder->SurfaceRead];

    if (!decoder->gl_textures[current][0] || !decoder->gl_textures[current][1]) return;
    // Render Progressive frame and simple interlaced
    y = VideoWindowHeight - decoder->OutputY - decoder->OutputHeight;
    if (y < 0)
        y = 0;
    glViewport(decoder->OutputX, y, decoder->OutputWidth, decoder->OutputHeight);

    if (gl_prog == 0)
        gl_prog = sc_generate(gl_prog, decoder->ColorSpace);    // generate shader programm
    if (!gl_prog) return;

    glUseProgram(gl_prog);
    texLoc = glGetUniformLocation(gl_prog, "texture0");
    glUniform1i(texLoc, 0);
    texLoc = glGetUniformLocation(gl_prog, "texture1");
    glUniform1i(texLoc, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[current][0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[current][1]);

    render_pass_quad(0, xcropf, ycropf);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);

    Debug(4,"video/nvdec: yy video surface %d displayed\n", current);
    (void)level;
}

///
///	Create and display a black empty surface.
///
///	@param decoder	NVDEC hw decoder
///
///	@FIXME: render only video area, not fullscreen!
///	decoder->Output.. isn't correct setup for radio stations
///
static void NVdecBlackSurface(NVdecDecoder * decoder)
{
    glClear(GL_COLOR_BUFFER_BIT);
    (void)decoder;
    return;
}

///
///	Advance displayed frame of decoder.
///
///	@param decoder	NVdec hw decoder
///
static void NVdecAdvanceDecoderFrame(NVdecDecoder * decoder)
{
    // next surface, if complete frame is displayed (1 -> 0)
    if (decoder->SurfaceField) {
	int filled;

	// FIXME: this should check the caller
	// check decoder, if new surface is available
	// need 2 frames for progressive
	// need 4 frames for interlaced
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled < 1 + 2 * decoder->Interlaced) {
	    // keep use of last surface
	    ++decoder->FramesDuped;
	    // FIXME: don't warn after stream start, don't warn during pause
	    Debug(4,"video: display buffer empty, duping frame (%d/%d) %d\n",
		decoder->FramesDuped, decoder->FrameCounter,
		VideoGetBuffers(decoder->Stream));
	    return;
	}
	decoder->SurfaceRead = (decoder->SurfaceRead + 1) % (VIDEO_SURFACES_MAX * 2);
	atomic_dec(&decoder->SurfacesFilled);
	decoder->SurfaceField = !decoder->Interlaced;
	return;
    }
    // next field
    decoder->SurfaceField = 1;
    (void)decoder;
}

///
///	Display a video frame.
///
static void NVdecDisplayFrame(void)
{
    uint64_t first_time = 0;
    static uint64_t last_time;
    static int timeSS;
    int i;

    if (VideoSurfaceModesChanged) {	// handle changed modes
	VideoSurfaceModesChanged = 0;
	for (i = 0; i < NVdecDecoderN; ++i) {
		NVdecMixerSetup(NVdecDecoders[i]);
	}
    }

    if (GlxEnabled) {
	glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
	glClear(GL_COLOR_BUFFER_BIT);
    }
#ifdef USE_EGL
    if (EglEnabled) {
	eglMakeCurrent(EglDisplay, EglSurface, EglSurface, EglThreadContext);
	glClear(GL_COLOR_BUFFER_BIT);
    }
#endif
    // check if surface was displayed for more than 1 frame
	first_time = GetUsTicks();
    // FIXME: 21 only correct for 50Hz
    if (last_time && first_time > last_time + 21 * 1000 * 1000) {
	// FIXME: ignore still-frame, trick-speed
	Debug(3, "video/nvdec: %" PRId64 " display time %" PRId64 "\n",
	    first_time / 1000, (first_time - last_time) / 1000);
	// FIXME: can be more than 1 frame long shown
	for (i = 0; i < NVdecDecoderN; ++i) {
	    NVdecDecoders[i]->FramesMissed++;
	    NVdecMessage(2, _("video/nvdec: missed frame (%d/%d)\n"),
		NVdecDecoders[i]->FramesMissed,
		NVdecDecoders[i]->FrameCounter);
	}
    }

    last_time = first_time;
#ifdef USE_SCREENSAVER
    //get up screensaver every 20 sec
    if (DisableScreensaver) {
	if (first_time / 1000000 - timeSS > 20) {
	    xcb_force_screen_saver(Connection,XCB_SCREEN_SAVER_RESET);
	    timeSS = first_time / 1000000;
	}
    }
#endif
    //
    //	Render videos into output
    //
    for (i = 0; i < NVdecDecoderN; ++i) {
	int filled;
	NVdecDecoder *decoder;

	decoder = NVdecDecoders[i];
	decoder->FramesDisplayed++;
	decoder->StartCounter++;

	filled = atomic_read(&decoder->SurfacesFilled);
	// need 1 frame for progressive, 3 frames for interlaced
	if (filled < 1 + 2 * decoder->Interlaced && decoder->Closing) {
	    // FIXME: rewrite MixVideo to support less surfaces
	    if ((VideoShowBlackPicture && !decoder->TrickSpeed)
		|| (VideoShowBlackPicture && decoder->Closing < -300)) {
		NVdecBlackSurface(decoder);
		NVdecMessage(4, "video/nvdec: black surface displayed\n");
#ifdef USE_SCREENSAVER
		if (EnableDPMSatBlackScreen && DPMSDisabled) {
		    NVdecMessage(4, "Black surface, DPMS enabled\n");
		    X11DPMSReenable(Connection);
		    X11SuspendScreenSaver(Connection, 1);
		}
#endif
	    }
	    if (VideoShowBlackPicture)
		continue;
#ifdef USE_SCREENSAVER
	} else if (!DPMSDisabled) {	// always disable
	    NVdecMessage(4, "DPMS disabled\n");
	    X11DPMSDisable(Connection);
	    X11SuspendScreenSaver(Connection, 0);
#endif
	}

	NVdecMixVideo(decoder, i);
    }

    //
    //	add osd to surface
    //
    if(GlxEnabled) {
        if (OsdShown) {
            glViewport(0, 0, VideoWindowWidth, VideoWindowHeight);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, VideoWindowWidth, VideoWindowHeight, 0.0, -1.0, 1.0);
            GlxCheck();
#ifdef USE_OPENGLOSD
            if(!DisableOglOsd && OsdGlTexture) {
                GlxRenderTexture(OsdGlTexture, 0,0, VideoWindowWidth, VideoWindowHeight);
            } else
#endif
                GlxRenderTexture(OsdGlTextures[OsdIndex], 0,0, VideoWindowWidth, VideoWindowHeight);
        }

        glXSwapBuffers(XlibDisplay, VideoWindow);
        glXMakeCurrent(XlibDisplay, None, NULL);
    }
#ifdef USE_EGL
    if (EglEnabled) {
	if (OsdShown) {
            glViewport(0, 0, VideoWindowWidth, VideoWindowHeight);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, VideoWindowWidth, VideoWindowHeight, 0.0, -1.0, 1.0);
            EglCheck();
#ifdef USE_OPENGLOSD
            if(!DisableOglOsd && OsdGlTexture) {
                EglRenderTexture(OsdGlTexture, 0,0, VideoWindowWidth, VideoWindowHeight);
            } else
#endif
	    EglRenderTexture(OsdGlTextures[OsdIndex], 0, 0, VideoWindowWidth, VideoWindowHeight);
	}

	eglSwapBuffers(EglDisplay, EglSurface);
	eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	EglCheck();
    }
#endif
    // FIXME: CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC, &NVdecFrameTime);
    for (i = 0; i < NVdecDecoderN; ++i) {
	// remember time of last shown surface
	NVdecDecoders[i]->FrameTime = NVdecFrameTime;
    }

    xcb_flush(Connection);
}


///
///	Set NVDEC decoder video clock.
///
///	@param decoder	NVDEC hardware decoder
///	@param pts	audio presentation timestamp
///
void NVdecSetClock(NVdecDecoder * decoder, int64_t pts)
{
    decoder->PTS = pts;
}

///
///	Get NVDEC decoder video clock.
///
///	@param decoder	NVDEC hw decoder
///
///	FIXME: 20 wrong for 60hz dvb streams
///
static int64_t NVdecGetClock(const NVdecDecoder * decoder)
{
    // pts is the timestamp of the latest decoded frame
    if (decoder->PTS == (int64_t) AV_NOPTS_VALUE) {
	return AV_NOPTS_VALUE;
    }
    // subtract buffered decoded frames
    if (decoder->Interlaced) {
	/*
	   Info("video: %s =pts field%d #%d\n",
	   Timestamp2String(decoder->PTS),
	   decoder->SurfaceField,
	   atomic_read(&decoder->SurfacesFilled));
	 */
	// 1 field is future, 2 fields are past, + 2 in driver queue
	return decoder->PTS -
	    20 * 90 * (2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField - 2 + 2);
    }
    // + 2 in driver queue
    return decoder->PTS - 20 * 90 * (atomic_read(&decoder->SurfacesFilled) + 2);
}

///
///	Set NVDEC decoder closing stream flag.
///
///	@param decoder	NVDEC decoder
///
static void NVdecSetClosing(NVdecDecoder * decoder)
{
    decoder->Closing = 1;
}

///
///	Reset start of frame counter.
///
///	@param decoder	NVDEC decoder
///
static void NVdecResetStart(NVdecDecoder * decoder)
{
    decoder->StartCounter = 0;
}

///
///	Set trick play speed.
///
///	@param decoder	NVDEC decoder
///	@param speed	trick speed (0 = normal)
///
static void NVdecSetTrickSpeed(NVdecDecoder * decoder, int speed)
{
    decoder->TrickSpeed = speed;
    decoder->TrickCounter = speed;
    if (speed) {
	decoder->Closing = 0;
    }
}

///
///	Get NVDEC decoder statistics.
///
///	@param decoder		NVDEC decoder
///	@param[out] missed	missed frames
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void NVdecGetStats(NVdecDecoder * decoder, int *missed, int *duped,
    int *dropped, int *counter, int *dec)
{
    *missed = decoder->FramesMissed;
    *duped = decoder->FramesDuped;
    *dropped = decoder->FramesDropped;
    *counter = decoder->FrameCounter;
    *dec = (decoder->PixFmt == AV_PIX_FMT_NV12 || decoder->PixFmt == AV_PIX_FMT_P010LE)
        ? HWACCEL_NVDEC : HWACCEL_NONE;
}

///
///	Sync decoder output to audio.
///
///	trick-speed	show frame <n> times
///	still-picture	show frame until new frame arrives
///	60hz-mode	repeat every 5th picture
///	video>audio	slow down video by duplicating frames
///	video<audio	speed up video by skipping frames
///	soft-start	show every second frame
///
///	@param decoder	NVDEC hw decoder
///
static void NVdecSyncDecoder(NVdecDecoder * decoder)
{
    int err;
    int64_t audio_clock;
    int64_t video_clock;

    err = 0;
    video_clock = NVdecGetClock(decoder);
    audio_clock = AudioGetClock();

    if (!decoder->SyncOnAudio) {
	audio_clock = AV_NOPTS_VALUE;
	// FIXME: 60Hz Mode
	goto skip_sync;
    }

    EnoughVideo = (VideoGetBuffers(decoder->Stream) >= (VideoResolution == VideoResolution576i ? VideoStartThreshold_SD * !!VideoSoftStartSync : 0));
    if (!EnoughVideo && EnoughAudio && !AudioRunning && decoder->StartCounter > 200) {
	Debug (3,"video: force enough video\n");
	EnoughVideo = 1;
    }
    if (EnoughVideo && EnoughAudio && !AudioRunning && audio_clock != (int64_t) AV_NOPTS_VALUE) {
        Debug(3, "video: start audio after waiting for enough video: SurfacesFilled: %d, PacketsFilled: %d\n", atomic_read(&decoder->SurfacesFilled), VideoGetBuffers(decoder->Stream));
        AudioStarted = 1;
        AudioRunning = 1;
        pthread_cond_signal(&AudioStartCond);
    }

    // 60Hz: repeat every 5th field
    if (Video60HzMode && !(decoder->FramesDisplayed % 6)) {
	if (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock == (int64_t) AV_NOPTS_VALUE) {
	    goto out;
	}
	// both clocks are known
	if (audio_clock + VideoAudioDelay <= video_clock + 25 * 90) {
	    goto out;
	}
	// out of sync: audio before video
	if (!decoder->TrickSpeed) {
	    goto skip_sync;
	}
    }
    // TrickSpeed
    if (decoder->TrickSpeed) {
	if (decoder->TrickCounter--) {
	    goto out;
	}
	decoder->TrickCounter = decoder->TrickSpeed;
	goto skip_sync;
    }
    // at start of new video stream, soft or hard sync video to audio
    if (!IsReplay() && VideoSoftStartSync < 2 && decoder->StartCounter < VideoSoftStartFrames
	&& video_clock != (int64_t) AV_NOPTS_VALUE
	&& (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock > audio_clock + VideoAudioDelay + 120 * 90)) {
	err =
	    NVdecMessage(3, "video: initial slow down video, frame %d\n",
	    decoder->StartCounter);
	goto out;
    }

    if (decoder->SyncCounter && decoder->SyncCounter-- && VideoSoftStartSync > 1) {
	goto skip_sync;
    }

    if (audio_clock != (int64_t) AV_NOPTS_VALUE
	&& video_clock != (int64_t) AV_NOPTS_VALUE) {
	// both clocks are known
	int diff;
	int lower_limit;
	if (IsReplay()) {
	    if((video_clock > audio_clock + VideoAudioDelay + 8000 * 90) && decoder->StartCounter < VideoSoftStartFrames) { // 8s
		Debug(3,"flush audio\n");
		AudioFlushBuffers();
		goto out;
	    }
	    if((video_clock < audio_clock + VideoAudioDelay - 120 * 90) && VideoSoftStartSync < 2) {
		if (!(decoder->SurfaceField && atomic_read(&decoder->SurfacesFilled) < 1 + 2 * decoder->Interlaced)) {
		    Debug(3,"drop video\n");
		    NVdecAdvanceDecoderFrame(decoder);
		}
		goto skip_sync;
	    }
	}
	diff = video_clock - audio_clock - VideoAudioDelay;
	lower_limit = !IsReplay() ? -25 - (VideoResolution == VideoResolution576i ? 40 : 0) : 32;
	//diff = (decoder->LastAVDiff + diff) / 2;
	decoder->LastAVDiff = diff;
#ifdef DEBUG
	if (!decoder->SyncCounter && decoder->StartCounter < 1000)
	    Debug(3, "video/nvdec: diff %d %d lim %d fill %d\n", diff, diff/90, lower_limit, atomic_read(&decoder->SurfacesFilled));
#endif
	if (abs(diff) > 8000 * 90) {	// more than 8s
	    err = NVdecMessage(3, "video: audio/video difference too big\n");
	}
	if (diff > 100 * 90) {
	    // FIXME: this quicker sync step, did not work with new code!
	    err = NVdecMessage(3, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    if (VideoSoftStartSync > 1) {
		decoder->SyncCounter = diff > 100 * 90 ? diff % 2 : 1; //softsync :)
	    }
	    goto out;
	} else if (diff > 55 * 90) {
	    err = NVdecMessage(3, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    decoder->SyncCounter = 1;
	    goto out;
	} else if (diff < lower_limit * 90 && atomic_read(&decoder->SurfacesFilled) > 1 + decoder->Interlaced) { // double advance possible?
	    err = NVdecMessage(3, "video: speed up video, droping frame\n");
	    ++decoder->FramesDropped;
	    NVdecAdvanceDecoderFrame(decoder);
	    decoder->SyncCounter = 2;
	} else if (diff < lower_limit * 90 && atomic_read(&decoder->SurfacesFilled) < 1 + 2 * decoder->Interlaced && !IsReplay()) {
	    err = NVdecMessage(3, "video: speed up audio, delay audio\n");
	    AudioDelayms(-diff / 90 + 55);
	}
#if defined(DEBUG) || defined(AV_INFO)
	if (!decoder->SyncCounter && decoder->StartCounter < 1000) {
#ifdef DEBUG
	    Debug(3, "video/nvdec: synced after %d frames %dms\n",
		decoder->StartCounter, GetMsTicks() - VideoSwitch);
#else
	    Info("video/nvdec: synced after %d frames\n",
		decoder->StartCounter);
#endif
	    decoder->StartCounter += 1000;
	}
#endif
    }

  skip_sync:
    // is it not possible, to advance the surface and/or the field?
    if (decoder->SurfaceField && atomic_read(&decoder->SurfacesFilled) < 1 + 2 * decoder->Interlaced + 1 * !IsReplay()) {
        ++decoder->FramesDuped;
        // FIXME: don't warn after stream start, don't warn during pause
        err =
            NVdecMessage(3,
            _("video: decoder buffer empty, "
            "duping frame (%d/%d) %d v-buf closing %d\n"), decoder->FramesDuped,
            decoder->FrameCounter, VideoGetBuffers(decoder->Stream), decoder->Closing);
        // some time no new picture or black video configured
        if (decoder->Closing < -300 || (VideoShowBlackPicture
            && decoder->Closing)) {
            // clear ring buffer to trigger black picture
            atomic_set(&decoder->SurfacesFilled, 0);
	}
	goto out;
    }

    NVdecAdvanceDecoderFrame(decoder);
  out:
#if defined(DEBUG) || defined(AV_INFO)
    // debug audio/video sync
    if (err || !(decoder->FramesDisplayed % AV_INFO_TIME)) {
	if (!err) {
	    NVdecMessage(0, NULL);
	}
	Info("video: %s%+5" PRId64 " %4" PRId64 " %3d/\\ms %3d%+d%+d v-buf\n",
	    Timestamp2String(video_clock),
	    abs((int)(video_clock - audio_clock) / 90) <
	    8888 ? ((video_clock - audio_clock) / 90) : 8888,
	    AudioGetDelay() / 90, (int)VideoDeltaPTS / 90,
	    VideoGetBuffers(decoder->Stream),
	    decoder->Interlaced ? 2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField : atomic_read(&decoder->SurfacesFilled),
	    NVdecSurfaceQueued);
	if (!(decoder->FramesDisplayed % (5 * 60 * 60))) {
	    NVdecPrintFrames(decoder);
	}
    }
#endif
    return;				// fix gcc bug!
}

///
///	Sync a video frame.
///
static void NVdecSyncFrame(void)
{
    int i;

    //
    //	Sync video decoder to audio
    //
    for (i = 0; i < NVdecDecoderN; ++i) {
	NVdecSyncDecoder(NVdecDecoders[i]);
    }
}

///
///	Sync and display surface.
///
static void NVdecSyncDisplayFrame(void)
{
    NVdecDisplayFrame();
    NVdecSyncFrame();
}

///
///	Sync and render a ffmpeg frame
///
///	@param decoder		NVDEC hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void NVdecSyncRenderFrame(NVdecDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    // FIXME: temp debug
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,61,100)
    if (0 && frame->pkt_pts != (int64_t) AV_NOPTS_VALUE) {
	Debug(3, "video: render frame pts %s\n",
	    Timestamp2String(frame->pkt_pts));
#else
    if (0 && frame->pts != (int64_t) AV_NOPTS_VALUE) {
	Debug(3, "video: render frame pts %s\n",
	    Timestamp2String(frame->pts));
#endif
    }
#ifdef DEBUG
    if (!atomic_read(&decoder->SurfacesFilled)) {
	Debug(3, "video: new stream frame %dms\n", GetMsTicks() - VideoSwitch);
    }
#endif

#if 1
#ifndef USE_PIP
#error	"-DUSE_PIP or #define USE_PIP is needed,"
#endif
    // if video output buffer is full, wait and display surface.
    // loop for interlace
    if (atomic_read(&decoder->SurfacesFilled) >= (VIDEO_SURFACES_MAX * 2)) {
	Info("video/nvdec: this code part shouldn't be used\n");
	return;
    }
#else
    // FIXME: disabled for remove
    // FIXME: wrong for multiple streams
    // FIXME: this part code should be no longer be needed with new mpeg fix
    while (atomic_read(&decoder->SurfacesFilled) >= (VIDEO_SURFACES_MAX * 2)) {
	struct timespec abstime;

	pthread_mutex_unlock(&VideoLockMutex);

	abstime = decoder->FrameTime;
	abstime.tv_nsec += 14 * 1000 * 1000;
	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
	    // avoid overflow
	    abstime.tv_sec++;
	    abstime.tv_nsec -= 1000 * 1000 * 1000;
	}

	VideoPollEvent();

	// fix dead-lock with VdpauExit
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_mutex_lock(&VideoLockMutex);
	// give osd some time slot
	while (pthread_cond_timedwait(&VideoWakeupCond, &VideoLockMutex,
		&abstime) != ETIMEDOUT) {
	    // SIGUSR1
	    Debug(3, "video/nvdec: pthread_cond_timedwait error\n");
	}
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	NVdecSyncDisplayFrame();
    }
#endif

    if (!decoder->Closing) {
	VideoSetPts(&decoder->PTS, decoder->Interlaced, video_ctx, frame);
    }
    NVdecRenderFrame(decoder, video_ctx, frame);
}

///
///	Set NVDEC background color.
///
///	@param rgba	32 bit RGBA color.
///
static void NVdecSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
}

///
///	Set video output position.
///
///	@param decoder	NVDEC hw decoder
///	@param x	video output x coordinate inside the window
///	@param y	video output y coordinate inside the window
///	@param width	video output width
///	@param height	video output height
///
///	@note FIXME: need to know which stream.
///
static void NVdecSetOutputPosition(NVdecDecoder * decoder, int x, int y,
    int width, int height)
{
    Debug(3, "video/nvdec: output %dx%d%+d%+d\n", width, height, x, y);

    decoder->VideoX = x;
    decoder->VideoY = y;
    decoder->VideoWidth = width;
    decoder->VideoHeight = height;

    // next video pictures are automatic rendered to correct position
}

///
///	Set NVDEC video mode.
///
static void NVdecSetVideoMode(void)
{
    int i;

    for (i = 0; i < NVdecDecoderN; ++i) {
	// reset video window, upper level needs to fix the positions
	NVdecDecoders[i]->VideoX = 0;
	NVdecDecoders[i]->VideoY = 0;
	NVdecDecoders[i]->VideoWidth = VideoWindowWidth;
	NVdecDecoders[i]->VideoHeight = VideoWindowHeight;
	// for window mode need restore scale
	if (NVdecDecoders[i]->ScaleX || NVdecDecoders[i]->ScaleY
	 || NVdecDecoders[i]->ScaleWidth || NVdecDecoders[i]->ScaleHeight) {
	    // convert OSD coordinates to window coordinates
	    int x = (NVdecDecoders[i]->ScaleX * VideoWindowWidth) / OsdWidth;
	    int width = (NVdecDecoders[i]->ScaleWidth * VideoWindowWidth) / OsdWidth;
	    int y = (NVdecDecoders[i]->ScaleY * VideoWindowHeight) / OsdHeight;
	    int height = (NVdecDecoders[i]->ScaleHeight * VideoWindowHeight) / OsdHeight;
	    NVdecSetOutputPosition(NVdecDecoders[i], x, y, width, height);
	}
	NVdecUpdateOutput(NVdecDecoders[i]);
    }
}

#ifdef USE_VIDEO_THREAD2

#else

#ifdef USE_VIDEO_THREAD

///
///	Handle a NVDEC display.
///
static void NVdecDisplayHandlerThread(void)
{
    int i;
    int err;
    int allfull;
    int decoded;
    struct timespec nowtime;
    NVdecDecoder *decoder;

    allfull = 1;
    decoded = 0;
    pthread_mutex_lock(&VideoLockMutex);
    for (i = 0; i < NVdecDecoderN; ++i) {
	int filled;

	decoder = NVdecDecoders[i];

	//
	// fill frame output ring buffer
	//
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled <= 1 + 2 * decoder->Interlaced) {
	    // FIXME: hot polling
	    // fetch+decode or reopen
	    allfull = 0;
	    err = VideoDecodeInput(decoder->Stream);
	} else {
	    err = VideoPollInput(decoder->Stream);
	}
	// decoder can be invalid here
	if (err) {
	    // nothing buffered?
	    if (err == -1 && decoder->Closing) {
		decoder->Closing--;
		if (!decoder->Closing) {
		    Debug(3, "video/nvdec: closing eof\n");
		    decoder->Closing = -1;
		}
	    }
	    continue;
	}
	decoded = 1;
    }
    pthread_mutex_unlock(&VideoLockMutex);

    if (!decoded) {			// nothing decoded, sleep
	// FIXME: sleep on wakeup
	usleep(1 * 1000);
    }
    // all decoder buffers are full
    // and display is not preempted
    // speed up filling display queue, wait on display queue empty
    if (!allfull) {
	clock_gettime(CLOCK_MONOTONIC, &nowtime);
	// time for one frame over?
	if ((nowtime.tv_sec - NVdecFrameTime.tv_sec) * 1000 * 1000 * 1000 +
	    (nowtime.tv_nsec - NVdecFrameTime.tv_nsec) < 15 * 1000 * 1000) {
	    return;
	}
    }

    pthread_mutex_lock(&VideoLockMutex);
    NVdecSyncDisplayFrame();
    pthread_mutex_unlock(&VideoLockMutex);
}

#else

#define NVdecDisplayHandlerThread	NULL

#endif

#endif

//----------------------------------------------------------------------------
//	NVDEC OSD USE GLX
//----------------------------------------------------------------------------
#ifdef USE_OPENGLOSD
void *GetNVdecOsdOutputTexture(GLuint texture) {
    OsdGlTexture = texture;
    return 0;
}

int NVdecInitGlx(void) {
    int a = 0;

    if (!GlxEnabled) {
        Debug(3,"video/osd: can't create glx context\n");
        return 0;
    }
    //after run an external player from time to time vdr not set playmode 1
    //then try start GLX forced.
    //is it a vdr bug or external player plugin???
    while (!GlxContext || !GlxThreadContext){
        usleep(1000);
        a++;
        if (a > 10 && a < 1000) {
            Debug(3,"Try start GLX forced\n");
            VideoDisplayWakeup();
            a = 1000;
        }
        if (a > 1010) return 0;
    }
    Debug(3,"Create OSD GLX context\n");
    glXMakeCurrent(XlibDisplay, None, NULL);
    glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext);
    return 1;
}
#endif

///
///	NVDEC module.
///
static const VideoModule NVdecModule = {
    .Name = "nvdec",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) NVdecNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))NVdecDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))NVdecGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))NVdecReleaseSurface,
    .UnregisterSurface =
	(void (*const) (VideoHwDecoder *))NVdecUnregisterSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))NVdec_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))NVdecSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	NVdecGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))NVdecSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))NVdecGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))NVdecSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))NVdecResetStart,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))NVdecSetTrickSpeed,
#ifdef USE_GRAB
    .GrabOutput = NVdecGrabOutputSurface,
#endif
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, int *))NVdecGetStats,
    .SetBackground = NVdecSetBackground,
    .SetVideoMode = NVdecSetVideoMode,
#ifdef USE_AUTOCROP
    .ResetAutoCrop = NVdecResetAutoCrop,
#endif
    .DisplayHandlerThread = NVdecDisplayHandlerThread,
    .OsdClear = GlxOsdClear,
    .OsdDrawARGB = GlxOsdDrawARGB,
    .OsdInit = GlxOsdInit,
    .OsdExit = GlxOsdExit,
    .MaxPixmapSize = GlxMaxPixmapSize,
    .Init = NVdecInit,
    .Exit = NVdecExit,
};

//----------------------------------------------------------------------------
//	NVDEC OSD USE EGL
//----------------------------------------------------------------------------
#ifdef USE_EGL

#ifdef USE_OPENGLOSD
void *GetNVdecEglOsdOutputTexture(GLuint texture) {
    OsdGlTexture = texture;
    return 0;
}

int NVdecInitEgl(void) {
    int a = 0;

    if (!EglEnabled) {
        Debug(3,"video/osd: can't create egl context\n");
        return 0;
    }
    //after run an external player from time to time vdr not set playmode 1
    //then try start EGL forced.
    //is it a vdr bug or external player plugin???
    while (!EglContext || !EglThreadContext){
        usleep(1000);
        a++;
        if (a > 10 && a < 1000) {
            Debug(3,"Try start EGL forced\n");
            VideoDisplayWakeup();
            a = 1000;
        }
        if (a > 1010) return 0;
    }
    Debug(3,"Create OSD EGL context\n");
    eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglContext);
    return 1;
}
#endif

///
///	NVDEC EGL module.
///
static const VideoModule NVdecEglModule = {
    .Name = "nvdec-egl",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) NVdecNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))NVdecDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))NVdecGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))NVdecReleaseSurface,
    .UnregisterSurface =
	(void (*const) (VideoHwDecoder *))NVdecUnregisterSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))NVdec_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))NVdecSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	NVdecGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))NVdecSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))NVdecGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))NVdecSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))NVdecResetStart,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))NVdecSetTrickSpeed,
#ifdef USE_GRAB
    .GrabOutput = NVdecGrabOutputSurface,
#endif
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, int *))NVdecGetStats,
    .SetBackground = NVdecSetBackground,
    .SetVideoMode = NVdecSetVideoMode,
#ifdef USE_AUTOCROP
    .ResetAutoCrop = NVdecResetAutoCrop,
#endif
    .DisplayHandlerThread = NVdecDisplayHandlerThread,
    .OsdClear = EglOsdClear,
    .OsdDrawARGB = EglOsdDrawARGB,
    .OsdInit = EglOsdInit,
    .OsdExit = EglOsdExit,
    .MaxPixmapSize = EglMaxPixmapSize,
    .Init = NVdecEglInit,
    .Exit = NVdecExit,
};
#endif

#endif

//----------------------------------------------------------------------------
//	CPU
//----------------------------------------------------------------------------

#if defined USE_GLX || defined USE_EGL

///
///	CPU decoder
///
typedef struct _cpudecoder_
{
     xcb_window_t Window;		///< output window

    int VideoX;				///< video base x coordinate
    int VideoY;				///< video base y coordinate
    int VideoWidth;			///< video base width
    int VideoHeight;			///< video base height

    int ScaleX;				///< video scale x coordinate
    int ScaleY;				///< video scale y coordinate
    int ScaleWidth;			///< video scale width
    int ScaleHeight;			///< video scale height

    int OutputX;			///< real video output x coordinate
    int OutputY;			///< real video output y coordinate
    int OutputWidth;			///< real video output width
    int OutputHeight;			///< real video output height

    enum AVPixelFormat PixFmt;		///< ffmpeg frame pixfmt
    enum AVColorSpace  ColorSpace;	/// ffmpeg ColorSpace
    enum AVColorTransferCharacteristic  trc;  // 
    enum AVColorPrimaries color_primaries;

    int Interlaced;			///< ffmpeg interlaced flag
    int TopFieldFirst;			///< ffmpeg top field displayed first

    int InputWidth;			///< video input width
    int InputHeight;			///< video input height
    AVRational InputAspect;		///< video input aspect ratio
    VideoResolutions Resolution;	///< resolution group

    int CropX;				///< video crop x
    int CropY;				///< video crop y
    int CropWidth;			///< video crop width
    int CropHeight;			///< video crop height

#ifdef USE_AUTOCROP
    void *AutoCropBuffer;		///< auto-crop buffer cache
    unsigned AutoCropBufferSize;	///< auto-crop buffer size
    AutoCropCtx AutoCrop[1];		///< auto-crop variables
#endif
    int SurfacesNeeded;			///< number of surface to request
    int SurfaceUsedN;			///< number of used video surfaces
    /// used video surface ids
    int SurfacesUsed[CODEC_SURFACES_MAX];
    int SurfaceFreeN;			///< number of free video surfaces
    /// free video surface ids
    int SurfacesFree[CODEC_SURFACES_MAX];
    /// video surface ring buffer
    int SurfacesRb[VIDEO_SURFACES_MAX *2];

    int SurfaceWrite;			///< write pointer
    int SurfaceRead;			///< read pointer
    atomic_t SurfacesFilled;		///< how many of the buffer is used

    GLuint gl_textures[CODEC_SURFACES_MAX][2];  // where we will copy the CPU result

    AVCodecContext *video_ctx;

    int SurfaceField;			///< current displayed field
    int TrickSpeed;			///< current trick speed
    int TrickCounter;			///< current trick speed counter
    struct timespec FrameTime;		///< time of last display
    VideoStream *Stream;		///< video stream
    int Closing;			///< flag about closing current stream
    int SyncOnAudio;			///< flag sync to audio
    int64_t PTS;			///< video PTS clock

    int LastAVDiff;			///< last audio - video difference
    int SyncCounter;			///< counter to sync frames
    int StartCounter;			///< counter for video start
    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
} CpuDecoder;

static CpuDecoder *CpuDecoders[2];	///< open decoder streams
static int CpuDecoderN;		///< number of decoder streams
static int CpuSurfaceQueued;          ///< number of display surfaces queued

static struct timespec CpuFrameTime;	///< time of last display

static pthread_mutex_t CpuGrabMutex;

unsigned int size_tex_data;
unsigned int num_texels;
unsigned int num_values;

//----------------------------------------------------------------------------

///
///	Output video messages.
///
///	Reduce output.
///
///	@param level	message level (Error, Warning, Info, Debug, ...)
///	@param format	printf format string (NULL to flush messages)
///	@param ...	printf arguments
///
///	@returns true, if message shown
///
int CpuMessage(int level, const char *format, ...)
{
    if (LogLevel > level || DebugLevel > level) {
	static const char *last_format;
	static char buf[256];
	va_list ap;

	va_start(ap, format);
	if (format != last_format) {	// don't repeat same message
	    if (buf[0]) {		// print last repeated message
		syslog(LOG_ERR, "%s", buf);
		buf[0] = '\0';
	    }

	    if (format) {
		last_format = format;
		vsyslog(LOG_ERR, format, ap);
	    }
	    va_end(ap);
	    return 1;
	}
	if (format)
	    vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
    }
    return 0;
}

//	Surfaces -------------------------------------------------------------

void CpuCreateGlTexture(CpuDecoder * decoder, unsigned int size_x, unsigned int size_y)
{
    int n, i;

    if (GlxEnabled) {
        glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
        GlxCheck();
    }
#ifdef USE_EGL
    if (EglEnabled) {
        eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglThreadContext);
        EglCheck();;
    }
#endif
    // create texture planes
    for (n = 0; n < 2; n++) {       // number of planes
        glGenTextures(CODEC_SURFACES_MAX, decoder->gl_textures[n]);
        GlCheck();
    }
    Debug(3,"video/cpu: create %d Textures Format %s w %d h %d \n",
        decoder->SurfacesNeeded, decoder->PixFmt != AV_PIX_FMT_P010LE && decoder->PixFmt != AV_PIX_FMT_YUV420P10LE ? "YV12" : "P010", size_x, size_y);

    for (i = 0; i < decoder->SurfacesNeeded; i++) {
        for (n = 0; n < 2; n++) {   // number of planes

            glBindTexture(GL_TEXTURE_2D, decoder->gl_textures[i][n]);
            GlCheck();
            // set basic parameters
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            if (decoder->PixFmt != AV_PIX_FMT_P010LE && decoder->PixFmt != AV_PIX_FMT_YUV420P10LE)
                glTexImage2D(GL_TEXTURE_2D, 0, n == 0 ? GL_R8 : GL_RG8, n == 0 ? size_x : size_x/2, n == 0 ? size_y : size_y/2, 0,
                     n == 0 ? GL_RED : GL_RG, GL_UNSIGNED_BYTE, NULL);
            else
                glTexImage2D(GL_TEXTURE_2D, 0, n == 0 ? GL_R16 : GL_RG16, n == 0 ? size_x : size_x/2, n == 0 ? size_y : size_y/2, 0,
                    n == 0 ? GL_RED : GL_RG, GL_UNSIGNED_SHORT, NULL);

            GlCheck();
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    GlCheck();
}

///
///	Create surfaces for CPU decoder.
///
///	@param decoder	CPU hw decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void CpuCreateSurfaces(CpuDecoder * decoder, int width, int height)
{
    int i;

#ifdef DEBUG
    if (!decoder->SurfacesNeeded) {
	Error(_("video/cpu: surface needed not set\n"));
	decoder->SurfacesNeeded = VIDEO_SURFACES_MAX * 2;
    }
#endif
    Debug(3, "video/cpu: %s: %dx%d * %d\n", __FUNCTION__, width, height,
	decoder->SurfacesNeeded);

    // allocate only the number of needed surfaces
    decoder->SurfaceFreeN = decoder->SurfacesNeeded;

    CpuCreateGlTexture(decoder, width, height);

    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	    decoder->SurfacesFree[i] = i;
	}
	Debug(4, "video/cpu: created video surface %dx%d with id 0x%08x\n",
	    width, height, decoder->SurfacesFree[i]);
}

///
///	Destroy surfaces of CPU decoder.
///
///	@param decoder	CPU hw decoder
///
static void CpuDestroySurfaces(CpuDecoder * decoder)
{
    int i;

    if(GlxEnabled) {
        if(GlxThreadContext){
            glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
            GlxCheck();
        }
    }
#ifdef USE_EGL
    if (EglEnabled) {
        if(EglThreadContext){
            eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglThreadContext);
            EglCheck();
        }
    }
#endif
    for (i = 0; i < 2; i++) {       // number of planes
        glDeleteTextures(CODEC_SURFACES_MAX, decoder->gl_textures[i]);
        GlCheck();
    }
    if (decoder == CpuDecoders[0]) {   // only when last decoder closes
        Debug(3,"Last decoder closes\n");
        if (gl_prog)
            glDeleteProgram(gl_prog);
        gl_prog = 0;
    }

    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	Debug(4, "video/cpu: destroy video surface with id 0x%08x\n",
	    decoder->SurfacesFree[i]);
	decoder->SurfacesFree[i] = -1;
    }
    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	Debug(4, "video/cpu: destroy video surface with id 0x%08x\n",
	    decoder->SurfacesUsed[i]);
	decoder->SurfacesUsed[i] = -1;
    }
    decoder->SurfaceFreeN = 0;
    decoder->SurfaceUsedN = 0;
}

///
///	Get a free surface.
///
///	@param decoder	CPU hw decoder
///
///	@returns the oldest free surface
///
static unsigned CpuGetSurface0(CpuDecoder * decoder)
{
    int surface;
    int i;

    if (!decoder->SurfaceFreeN) {
	Error(_("video/cpu: out of surfaces\n"));
	return -1;
    }
    // use oldest surface
    surface = decoder->SurfacesFree[0];

    decoder->SurfaceFreeN--;
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	decoder->SurfacesFree[i] = decoder->SurfacesFree[i + 1];
    }
    decoder->SurfacesFree[i] = -1;

    // save as used
    decoder->SurfacesUsed[decoder->SurfaceUsedN++] = surface;

    return surface;
}

///
///	Release a surface.
///
///	@param decoder	CPU hw decoder
///	@param surface	surface no longer used
///
static void CpuReleaseSurface(CpuDecoder * decoder, int surface)
{
    int i;

    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	if (decoder->SurfacesUsed[i] == surface) {
	    // no problem, with last used
	    decoder->SurfacesUsed[i] =
		decoder->SurfacesUsed[--decoder->SurfaceUsedN];
	    decoder->SurfacesFree[decoder->SurfaceFreeN++] = surface;
	    return;
	}
    }
    Error(_("video/cpu: release surface %#08x, which is not in use\n"),
	surface);
}

///
///	Debug CPU decoder frames drop...
///
///	@param decoder	CPU hw decoder
///
static void CpuPrintFrames(const CpuDecoder * decoder)
{
    Debug(3, "video/cpu: %d missed, %d duped, %d dropped frames of %d,%d\n",
	decoder->FramesMissed, decoder->FramesDuped, decoder->FramesDropped,
	decoder->FrameCounter, decoder->FramesDisplayed);
#ifndef DEBUG
    (void)decoder;
#endif
}

static void CpuMixerSetup(CpuDecoder * decoder)
{
    if (decoder->video_ctx) {
#ifdef USE_AVFILTER
        VideoDecoder *ist = decoder->video_ctx->opaque;

        if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceWeave) {
            Debug(3, "video/cpudec: set weave");
        } else if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceBob) {
            Debug(3, "video/cpudec: set yadif");
            if (decoder->video_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO)
                CodecVideoInitFilter(ist, "yadif=1:-1:0");
            else
                CodecVideoInitFilter(ist, "yadif=1:-1:1");
        } else if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceTemporal) {
            Debug(3, "video/cpudec: set bwdif");
            if (decoder->video_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO)
                CodecVideoInitFilter(ist, "bwdif=1:-1:0");
            else
                CodecVideoInitFilter(ist, "bwdif=1:-1:1");
        }
#endif
    }
}

///
///	Allocate new CPU decoder.
///
///	@param stream	video stream
///
///	@returns a new prepared cuvid hardware decoder.
///
static CpuDecoder *CpuNewHwDecoder(VideoStream * stream)
{
    CpuDecoder *decoder;
    int i;

    Debug(3, "video/cpu: %s\n", __FUNCTION__);
    if ((unsigned)CpuDecoderN >=
	sizeof(CpuDecoders) / sizeof(*CpuDecoders)) {
    	Error(_("video/cpu: out of decoders\n"));
	return NULL;
    }

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
	Error(_("video/cpu: out of memory\n"));
	return NULL;
    }
    decoder->Window = VideoWindow;
    decoder->VideoWidth = VideoWindowWidth;
    decoder->VideoHeight = VideoWindowHeight;

    for (i = 0; i < CODEC_SURFACES_MAX; ++i) {
	decoder->SurfacesUsed[i] = -1;
	decoder->SurfacesFree[i] = -1;
    }

    //
    // setup video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < (VIDEO_SURFACES_MAX * 2); ++i) {
	decoder->SurfacesRb[i] = -1;
    }

#ifdef DEBUG
    if ((VIDEO_SURFACES_MAX) < 1 + 1 + 1 + 1) {
	Error(_
	    ("video/cpu: need 1 future, 1 current, 1 back and 1 work surface\n"));
    }
#endif

    // Procamp operation parameterization data
//    decoder->Procamp.struct_version = VDP_PROCAMP_VERSION;
//    decoder->Procamp.brightness = 0.0;
//    decoder->Procamp.contrast = 1.0;
//    decoder->Procamp.saturation = 1.0;
//    decoder->Procamp.hue = 0.0;		// default values

    decoder->OutputWidth = VideoWindowWidth;
    decoder->OutputHeight = VideoWindowHeight;

    decoder->PixFmt = AV_PIX_FMT_NONE;

#ifdef USE_AUTOCROP
    //decoder->AutoCropBuffer = NULL;	// done by calloc
    //decoder->AutoCropBufferSize = 0;
#endif

    decoder->Stream = stream;
    if (!CpuDecoderN) {		// FIXME: hack sync on audio
	decoder->SyncOnAudio = 1;
    }
    decoder->Closing = -300 - 1;

    decoder->PTS = AV_NOPTS_VALUE;

    CpuDecoders[CpuDecoderN++] = decoder;

    decoder->SurfaceUsedN = 1; // for correct cleanup after resume

    return decoder;
}

///
///	Cleanup CPU.
///
///	@param decoder	CPU hw decoder
///
static void CpuCleanup(CpuDecoder * decoder)
{
    int i;
    Debug(3, "video/cpu: %s\n", __FUNCTION__);
    if (decoder->SurfaceFreeN || decoder->SurfaceUsedN) {
	CpuDestroySurfaces(decoder);
    }
    //
    // reset video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX * 2; ++i) {
	decoder->SurfacesRb[i] = -1;
    }

    decoder->SurfaceRead = 0;
    decoder->SurfaceWrite = 0;
    decoder->SurfaceField = 0;
    decoder->SyncCounter = 0;
    decoder->Interlaced = 0;
    decoder->FrameCounter = 0;
    decoder->FramesDisplayed = 0;
    decoder->StartCounter = 0;
    decoder->Closing = 0;
    decoder->PTS = AV_NOPTS_VALUE;
    VideoDeltaPTS = 0;
}

///
///	Destroy a CPU decoder.
///
///	@param decoder	CPU hw decoder
///
static void CpuDelHwDecoder(CpuDecoder * decoder)
{
    int i;
    Debug(3, "video/cpu: %s\n", __FUNCTION__);
    for (i = 0; i < CpuDecoderN; ++i) {
	if (CpuDecoders[i] == decoder) {
	    CpuDecoders[i] = NULL;
	    // copy last slot into empty slot
	    if (i < --CpuDecoderN) {
		CpuDecoders[i] = CpuDecoders[CpuDecoderN];
	    }

	    CpuCleanup(decoder);
	    CpuPrintFrames(decoder);

#ifdef USE_AUTOCROP
	    free(decoder->AutoCropBuffer);
#endif
	    free(decoder);

	    return;
	}
    }
    Error(_("video/cpu: decoder not in decoder list.\n"));
}

///
///	CPU setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns true if CPU could be initialized, false otherwise.
///
static int CpuGlxInit(const char *display_name)
{
    Info(_("video/cpu: Start CPU\n"));

    GlxEnabled = 1;

    GlxInit();
    if (GlxEnabled) {
        GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight, GlxSharedContext);
    }
    if (!GlxEnabled) {
        Error(_("video/cpu: GLX error\n"));
        return 0;
    }

    glGenBuffers(1,&vao_buffer);
    GlxCheck();

    pthread_mutex_init(&CpuGrabMutex, NULL);

    Info(_("video/cpu: Start CPU ok\n"));

    (void)display_name;
    return 1;
}

#ifdef USE_EGL
static int CpuEglInit(const char *display_name)
{
    Info(_("video/cpu: Start CPU\n"));

    EglEnabled = 1;

    EglInit();
    if (EglEnabled) {
	EglSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight,
	    EglSharedContext);
    }
    if (!EglEnabled) {
        Error(_("video/cpu: EGL error\n"));
        return 0;
    }
    pthread_mutex_init(&CpuGrabMutex, NULL);

    Info(_("video/cpu: Start CPU ok\n"));

    (void)display_name;
    return 1;
}
#endif


///
///	CPU cleanup.
///
static void CpuExit(void)
{
    int i;
    Debug(3, "video/cpu: %s\n", __FUNCTION__);

    for (i = 0; i < CpuDecoderN; ++i) {
	if (CpuDecoders[i]) {
	    CpuDelHwDecoder(CpuDecoders[i]);
	    CpuDecoders[i] = NULL;
	}
    }
    CpuDecoderN = 0;
    pthread_mutex_destroy(&CpuGrabMutex);

    glDeleteBuffers(1, &vao_buffer);
    vao_buffer = 0;
}

///
///	Update output for new size or aspect ratio.
///
///	@param decoder	CPU hw decoder
///
static void CpuUpdateOutput(CpuDecoder * decoder)
{
    Debug(3, "video/cpu: %s\n", __FUNCTION__);

    VideoUpdateOutput(decoder->InputAspect, decoder->InputWidth,
	decoder->InputHeight, decoder->Resolution, decoder->VideoX,
	decoder->VideoY, decoder->VideoWidth, decoder->VideoHeight,
	&decoder->OutputX, &decoder->OutputY, &decoder->OutputWidth,
	&decoder->OutputHeight, &decoder->CropX, &decoder->CropY,
	&decoder->CropWidth, &decoder->CropHeight);
#ifdef USE_AUTOCROP
    decoder->AutoCrop->State = 0;
    decoder->AutoCrop->Count = AutoCropDelay;
#endif
}

///
///	Configure CPU for new video format.
///
///	@param decoder	CPU hw decoder
///
static void CpuSetupOutput(CpuDecoder * decoder)
{
    // FIXME: need only to create and destroy surfaces for size changes
    //		or when number of needed surfaces changed!
    decoder->Resolution =
	VideoResolutionGroup(decoder->InputWidth, decoder->InputHeight,
	decoder->Interlaced);
    VideoResolution = decoder->Resolution;

    CpuMixerSetup(decoder);

    CpuCreateSurfaces(decoder, decoder->InputWidth, decoder->InputHeight);

    CpuUpdateOutput(decoder);		// update aspect/scaling
}

///
///	Get a free surface.  Called from ffmpeg.
///
///	@param decoder		CPU hw decoder
///	@param video_ctx	ffmpeg video codec context
///
///	@returns the oldest free surface
///
static unsigned CpuGetSurface(CpuDecoder * decoder,
    const AVCodecContext * video_ctx)
{

#ifdef FFMPEG_BUG1_WORKAROUND
    // get_format not called with valid informations.
    if (video_ctx->width != decoder->InputWidth
	|| video_ctx->height != decoder->InputHeight) {

	CpuCleanup(decoder);

	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;
	CpuSetupOutput(decoder);
    }
#else
    (void)video_ctx;
#endif
    return CpuGetSurface0(decoder);
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param fmt	is the list of formats which are supported by the codec,
///			it is terminated by -1 as 0 is a valid format, the
///			formats are ordered by quality.
///
static enum AVPixelFormat Cpu_get_format(CpuDecoder * decoder,
    AVCodecContext * video_ctx, const enum AVPixelFormat *fmt)
{
    VideoDecoder *ist = video_ctx->opaque;

    Debug(3,"get format  %dx%d\n",video_ctx->width,video_ctx->height);

    ist->active_hwaccel_id = HWACCEL_NONE;
    ist->hwaccel_pix_fmt   = AV_PIX_FMT_NONE;
    ist->hwaccel_get_buffer = NULL;
    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX * 2 + 2;
    decoder->PixFmt = AV_PIX_FMT_NONE;
    video_ctx->thread_count = 0;
    decoder->InputWidth = 0;
    decoder->InputHeight = 0;
    video_ctx->hwaccel_context = NULL;
    video_ctx->draw_horiz_band = NULL;

    ist->GetFormatDone = 1;
    return avcodec_default_get_format(video_ctx, fmt);
}

#ifdef USE_GRAB
///
///	Grab output surface already locked.
//	Grab openGL video + osd (screenshot)
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *CpuGrabOutputSurfaceLocked(int *ret_size, int *ret_width, int *ret_height)
{
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint8_t *base;
    size_t i, j, k, cur_gl, cur_rgb;
    GLubyte *pixels;
    double scalew, scaleh;

    typedef struct {
        uint32_t x0;
        uint32_t y0;
        uint32_t x1;
        uint32_t y1;
    } CuRect;
    CuRect source_rect;

    CpuDecoder *decoder;

    decoder = CpuDecoders[0];
    if (decoder == NULL)   // no video aktiv
	return NULL;

    //get real surface size
    width = VideoWindowWidth;
    height = VideoWindowHeight;


    Debug(3, "video/cpu: grab %dx%d\n", width, height);

    source_rect.x0 = 0;
    source_rect.y0 = 0;
    source_rect.x1 = width;
    source_rect.y1 = height;

    if (ret_width && ret_height) {
	if (*ret_width <= -64) {	// this is an Atmo grab service request
	    int overscan;

	    // calculate aspect correct size of analyze image
	    width = *ret_width * -1;
	    height = (width * source_rect.y1) / source_rect.x1;

	    // calculate size of grab (sub) window
	    overscan = *ret_height;

	    if (overscan > 0 && overscan <= 200) {
		source_rect.x0 = source_rect.x1 * overscan / 1000;
		source_rect.x1 -= source_rect.x0;
		source_rect.y0 = source_rect.y1 * overscan / 1000;
		source_rect.y1 -= source_rect.y0;
	    }
	} else {
	    if (*ret_width > 0 && (unsigned)*ret_width < width) {
		width = *ret_width;
	    }
	    if (*ret_height > 0 && (unsigned)*ret_height < height) {
		height = *ret_height;
	    }
	}

	Debug(3, "video/cpu: grab source rect %d,%d:%d,%d dest dim %dx%d\n",
	source_rect.x0, source_rect.y0, source_rect.x1, source_rect.y1, width, height);

	size = width * height * 4;;

	base = malloc(size*sizeof(uint8_t));
	if (!base) {
	    Error(_("video/cpu: grab out of memory\n"));
	    return NULL;
	}

	pixels = malloc(VideoWindowWidth * VideoWindowHeight * 4 * sizeof(GLubyte));
	if (!pixels) {
	    Error(_("video/cpu: grab out of memory\n"));
	    free(base);
	    return NULL;
	}
	pthread_mutex_lock(&VideoLockMutex);
        if (GlxEnabled) {
	    glXMakeCurrent(XlibDisplay, VideoWindow, GlxSharedContext);
	    GlxCheck();
        }
#ifdef USE_EGL
        if (EglEnabled) {
            eglMakeCurrent(EglDisplay, EglSurface, EglSurface, EglSharedContext);
            EglCheck();
        }
#endif
	/* Get BGRA to align to 32 bits instead of just 24 for RGB */
	glReadPixels(source_rect.x0, source_rect.y0, VideoWindowWidth, VideoWindowHeight, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        GlCheck();

        if (GlxEnabled) {
	    glXMakeCurrent(XlibDisplay, None, NULL);
	    GlxCheck();
        }
#ifdef USE_EGL
        if (EglEnabled) {
            eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            EglCheck();
        }
#endif
	pthread_mutex_unlock(&VideoLockMutex);
	scalew = (double)VideoWindowWidth / width;
	scaleh = (double)VideoWindowHeight / height;

	// convert 32 -> 24 and simple scale
	for (i = 0; i < height; i++) {
	    for (j = 0; j < width; j++) {
	        cur_gl  = 4 * (VideoWindowWidth * (int)(scaleh * (height - i - 1)) + (int)(scalew * j));
	        cur_rgb = 4 * (width * i + j);
	            for (k = 0; k < 4; k++)
	                (base)[cur_rgb + k] = (pixels)[cur_gl + k];
	    }
	}

	free(pixels);

	Debug(3,"got grab data\n");

	*ret_size = size;
	*ret_width = width;
	*ret_height = height;
	return base;
    }

    return NULL;
}

///
///	Grab output surface.
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *CpuGrabOutputSurface(int *ret_size, int *ret_width,  int *ret_height)
{
    uint8_t *img;

    if (!gl_prog) {
        return NULL;			// cuvid video module not yet initialized
    }

    pthread_mutex_lock(&CpuGrabMutex);
//	pthread_mutex_lock(&VideoLockMutex);
    img = CpuGrabOutputSurfaceLocked(ret_size, ret_width, ret_height);
//	pthread_mutex_unlock(&VideoLockMutex);
    pthread_mutex_unlock(&CpuGrabMutex);
    return img;
}

#endif

#ifdef USE_AUTOCROP

///
///	CPU auto-crop support.
///
///	@param decoder	CPU hw decoder
///
static void CpuAutoCrop(CpuDecoder * decoder)
{
    int surface;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[3];
    uint32_t pitches[3];
    int crop14;
    int crop16;
    int next_state;

    surface = decoder->SurfacesRb[(decoder->SurfaceRead + 1) %  (VIDEO_SURFACES_MAX * 2)];

    width = decoder->InputWidth;
    height = decoder->InputHeight;

    size = width * height + ((width + 1) / 2) * ((height + 1) / 2)
	+ ((width + 1) / 2) * ((height + 1) / 2);
    // cache buffer for reuse
    base = decoder->AutoCropBuffer;
    if (size > decoder->AutoCropBufferSize) {
	free(base);
	decoder->AutoCropBuffer = malloc(size);
	base = decoder->AutoCropBuffer;
	decoder->AutoCropBufferSize = size;
    }
    if (!base) {
	Error(_("video/cpu: out of memory\n"));
	return;
    }
    pitches[0] = width;
    pitches[1] = width / 2;
    pitches[2] = width / 2;
    data[0] = base;
    data[1] = base + width * height;
    data[2] = base + width * height + width * height / 4;

    //we need Y in data[0] only
    glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][0]);
    glGetTexImage(GL_TEXTURE_2D,0,GL_RED, GL_UNSIGNED_BYTE,base);
    //glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][1]);
    //glGetTexImage(GL_TEXTURE_2D,0,GL_RG, GL_UNSIGNED_BYTE,base + width * height);

    AutoCropDetect(decoder->AutoCrop, width, height, data, pitches);

    glBindTexture(GL_TEXTURE_2D, 0);

    // ignore black frames
    if (decoder->AutoCrop->Y1 >= decoder->AutoCrop->Y2) {
	return;
    }

    crop14 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 14);
    crop14 = (decoder->InputHeight - crop14) / 2;
    crop16 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 16);
    crop16 = (decoder->InputHeight - crop16) / 2;

    if (decoder->AutoCrop->Y1 >= crop16 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop16 - AutoCropTolerance) {
	next_state = 16;
    } else if (decoder->AutoCrop->Y1 >= crop14 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop14 - AutoCropTolerance) {
	next_state = 14;
    } else {
	next_state = 0;
    }

    if (decoder->AutoCrop->State == next_state) {
	return;
    }

    Debug(3, "video: crop aspect %d:%d %d/%d %d%+d\n",
	decoder->InputAspect.num, decoder->InputAspect.den, crop14, crop16,
	decoder->AutoCrop->Y1, decoder->InputHeight - decoder->AutoCrop->Y2);

    Debug(3, "video: crop aspect %d -> %d\n", decoder->AutoCrop->State,	next_state);

    switch (decoder->AutoCrop->State) {
	case 16:
	case 14:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay / 2) {
		return;
	    }
	    break;
	case 0:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay) {
		return;
	    }
	    break;
    }

    decoder->AutoCrop->State = next_state;
    if (next_state) {
		decoder->CropX = VideoCutLeftRight[decoder->Resolution];
		decoder->CropY =
			(next_state ==
			16 ? crop16 : crop14) + VideoCutTopBottom[decoder->Resolution];
		decoder->CropWidth = decoder->InputWidth - decoder->CropX * 2;
		decoder->CropHeight = decoder->InputHeight - decoder->CropY * 2;

		// FIXME: this overwrites user choosen output position
		// FIXME: resize kills the auto crop values
		// FIXME: support other 4:3 zoom modes
		decoder->OutputX = decoder->VideoX;
		decoder->OutputY = decoder->VideoY;
		decoder->OutputWidth = (decoder->VideoHeight * next_state) / 9;
		decoder->OutputHeight = (decoder->VideoWidth * 9) / next_state;
		if (decoder->OutputWidth > decoder->VideoWidth) {
			decoder->OutputWidth = decoder->VideoWidth;
			decoder->OutputY +=
			(decoder->VideoHeight - decoder->OutputHeight) / 2;
		} else if (decoder->OutputHeight > decoder->VideoHeight) {
			decoder->OutputHeight = decoder->VideoHeight;
			decoder->OutputX +=
			(decoder->VideoWidth - decoder->OutputWidth) / 2;
		}
		Debug(3, "video: aspect output %dx%d %dx%d%+d%+d\n",
			decoder->InputWidth, decoder->InputHeight, decoder->OutputWidth,
			decoder->OutputHeight, decoder->OutputX, decoder->OutputY);
    } else {
		// sets AutoCrop->Count
		CpuUpdateOutput(decoder);
    }
    decoder->AutoCrop->Count = 0;
}

///
///	CPU check if auto-crop todo.
///
///	@param decoder	CPU hw decoder
///
///	@note a copy of VaapiCheckAutoCrop
///	@note auto-crop only supported with normal 4:3 display mode
///
static void CpuCheckAutoCrop(CpuDecoder * decoder)
{
    // reduce load, check only n frames
    if (Video4to3ZoomMode == VideoNormal && AutoCropInterval
	&& !(decoder->FrameCounter % AutoCropInterval)) {
	AVRational input_aspect_ratio;
	AVRational tmp_ratio;
	av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den,
	    decoder->InputWidth * decoder->InputAspect.num,
	    decoder->InputHeight * decoder->InputAspect.den, 1024 * 1024);

	tmp_ratio.num = 4;
	tmp_ratio.den = 3;
	// 4:3 with 16:9/14:9 inside
	if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	    CpuAutoCrop(decoder);
	} else {
	// 15:11 with 16:9/14:9 inside
	    tmp_ratio.num = 15;
	    tmp_ratio.den = 11;
	    if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	        CpuAutoCrop(decoder);
	    } else {
	        decoder->AutoCrop->Count = 0;
	        decoder->AutoCrop->State = 0;
	    }
	}
    }
}

///
///	CPU reset auto-crop.
///
static void CpuResetAutoCrop(void)
{
    int i;

    for (i = 0; i < CpuDecoderN; ++i) {
	CpuDecoders[i]->AutoCrop->State = 0;
	CpuDecoders[i]->AutoCrop->Count = 0;
    }
}

#endif

///
///	Queue output surface.
///
///	@param decoder	CPU hw decoder
///	@param surface	output surface
///	@param softdec	software decoder
///
///	@note we can't mix software and hardware decoder surfaces
///
static void CpuQueueSurface(CpuDecoder * decoder, int surface,
    int softdec)
{
    int old;

    ++decoder->FrameCounter;

    if (1) {				// can't wait for output queue empty
	if (atomic_read(&decoder->SurfacesFilled) >= (VIDEO_SURFACES_MAX * 2)) {
	    Warning(_
		("video/cpu: output buffer full, dropping frame (%d/%d)\n"),
		++decoder->FramesDropped, decoder->FrameCounter);
	    if (!(decoder->FramesDisplayed % 300)) {
		CpuPrintFrames(decoder);
	    }
	    // software surfaces only
	    if (softdec) {
		CpuReleaseSurface(decoder, surface);
	    }
	    return;
	}
#if 0
    } else {				// wait for output queue empty
	while (atomic_read(&decoder->SurfacesFilled) >= (VIDEO_SURFACES_MAX * 2)) {
	    VideoDisplayHandler();
	}
#endif
    }

    //
    //	    Check and release, old surface
    //
    if ((old = decoder->SurfacesRb[decoder->SurfaceWrite])
	!= -1) {

	// now we can release the surface, software surfaces only
	if (softdec) {
	    CpuReleaseSurface(decoder, old);
	}
    }

    Debug(4, "video/cpu: yy video surface %#08x@%d ready\n", surface,
	decoder->SurfaceWrite);

    decoder->SurfacesRb[decoder->SurfaceWrite] = surface;
    decoder->SurfaceWrite = (decoder->SurfaceWrite + 1)
	% (VIDEO_SURFACES_MAX * 2);
    atomic_inc(&decoder->SurfacesFilled);
}

///
///	Render a ffmpeg frame.
///
///	@param decoder		CPU hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void CpuRenderFrame(CpuDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    int surface;
    VideoDecoder        *ist = video_ctx->opaque;
    int interlaced;
    enum AVColorSpace color;

    AVRational aspect_ratio;

    // FIXME: some tv-stations toggle interlace on/off
    // frame->interlaced_frame isn't always correct set
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
    interlaced = frame->interlaced_frame;
#else
    interlaced = frame->flags & AV_FRAME_FLAG_INTERLACED;
#endif
    //25, 30 - interlaced; 50, 60 - progressive; hevc - progressive allways
    if (video_ctx->framerate.num > 0) {
	if (video_ctx->framerate.num / video_ctx->framerate.den > 30) interlaced = 0;
	else interlaced = 1;
    }

    if (video_ctx->codec_id == AV_CODEC_ID_HEVC) interlaced = 0;

    // FIXME: should be done by init video_ctx->field_order
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != frame->top_field_first) {

	Debug(4, "video/cpu: interlaced %d top-field-first %d num %d den %d\n", frame->interlaced_frame,
	    frame->top_field_first, video_ctx->framerate.num, video_ctx->framerate.den);

	decoder->TopFieldFirst = frame->top_field_first;
#else
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)) {

	Debug(4, "video/cpu: interlaced %d top-field-first %d num %d den %d\n", frame->flags & AV_FRAME_FLAG_INTERLACED,
	    frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST, video_ctx->framerate.num, video_ctx->framerate.den);

	decoder->TopFieldFirst = frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST;
#endif
	decoder->SurfaceField = !decoder->Interlaced;
	decoder->Interlaced = interlaced;
    }
    //aspect correction
    aspect_ratio = frame->sample_aspect_ratio;
    if ((video_ctx->width >= 352  && video_ctx->width <= 720 && video_ctx->height == 288) ||
	(video_ctx->width > 1300 && video_ctx->width <= 1920 && video_ctx->height == 540)) {
	aspect_ratio.den *= 2;
    }
    // update aspect ratio changes
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, aspect_ratio)) {
	Debug(3, "video/cpu: aspect ratio changed\n");
	decoder->InputAspect = aspect_ratio;
	CpuUpdateOutput(decoder);
    }

    color = frame->colorspace;
    if (color == AVCOL_SPC_UNSPECIFIED)   // if unknown
        color = AVCOL_SPC_BT709;

    //
    // render
    //
    //
    // Check image, format, size
    //

    if (ist->hwaccel_pix_fmt != video_ctx->pix_fmt
        || video_ctx->width != decoder->InputWidth
        || video_ctx->height != decoder->InputHeight) {

        decoder->PixFmt = video_ctx->pix_fmt;
        ist->active_hwaccel_id = HWACCEL_NONE;
        ist->hwaccel_pix_fmt = video_ctx->pix_fmt;

        decoder->InputWidth = video_ctx->width;
        decoder->InputHeight = video_ctx->height;
        decoder->video_ctx = (AVCodecContext *)video_ctx;

        CpuCleanup(decoder);
        decoder->SurfacesNeeded = VIDEO_SURFACES_MAX * 2 + 1;
        CpuSetupOutput(decoder);
    }
    //
    // Copy data from frame to image
    //
    switch (video_ctx->pix_fmt) {
//        case AV_PIX_FMT_CUDA:
//        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_YUV420P:      // for softdecode
//        case AV_PIX_FMT_YUVJ420P:     // some streams produce this
        case AV_PIX_FMT_YUV420P10LE:  // for softdecode of HEVC 10 Bit
        break;
        case AV_PIX_FMT_YUV422P:
//        case AV_PIX_FMT_YUV444P:
        break;
        default:
            Error(_("video/cpu: pixel format %d not supported\n"),
            video_ctx->pix_fmt);
            // FIXME: no fatals!
    }
    decoder->ColorSpace = color;     // save colorspace
    decoder->trc = frame->color_trc;
    decoder->color_primaries = frame->color_primaries;

    surface = CpuGetSurface0(decoder);

    if (surface == -1)     // no free surfaces
        return;
    {
        uint8_t *outY = NULL;
        uint8_t *outUV = NULL;

        if(GlxEnabled) {
            glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
            GlxCheck();
        }
#ifdef USE_EGL
        if (EglEnabled) {
            eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglThreadContext);
            EglCheck();
        }
#endif
        if (decoder->PixFmt != AV_PIX_FMT_YUV420P10LE) { //8bit
            //YV12 -> NV12
            outUV = (uint8_t*) malloc(frame->linesize[1] * decoder->InputHeight * sizeof(uint8_t));

            if (!outUV) {
                Error(_("video/cpu: out of memory\n"));
                return;
            }
            //TODO use shader or direct yuv render?
            for (int i = 0; i < (frame->linesize[1] * decoder->InputHeight / 2); i++) {
                memcpy(outUV + i * 2, frame->data[1] + i, 1); // For NV12, U first
                memcpy(outUV + i * 2 + 1, frame->data[2] + i, 1); // For NV12, V second
            }

            //Y
            glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][0]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[0]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decoder->InputWidth,
                decoder->InputHeight, GL_RED, GL_UNSIGNED_BYTE, frame->data[0]);
            GlCheck();
            //UV
            glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][1]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[1]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decoder->InputWidth/2,
                decoder->InputHeight/2, GL_RG, GL_UNSIGNED_BYTE, outUV);
            GlCheck();
            glBindTexture(GL_TEXTURE_2D, 0);

        } else { //10bit
            //yuv420ple -> p010le + 10bit -> 8bit
            outY = (uint8_t*) malloc(frame->linesize[0] * decoder->InputHeight / 2 * sizeof(uint8_t));

            if (!outY) {
                Error(_("video/cpu: out of memory\n"));
                return;
            }

            outUV = (uint8_t*) malloc(frame->linesize[1] * decoder->InputHeight / 2 * sizeof(uint8_t));

            if (!outUV) {
                Error(_("video/cpu: out of memory\n"));
                return;
            }

            //TODO use shader or direct yuv render?
            for (int i = 0; i < (frame->linesize[0] / 2 * decoder->InputHeight); i++) {
                outY[i] = (*((uint16_t*)frame->data[0] + i) + 2) >> 2; // Y
            }

            for (int i = 0; i < (frame->linesize[1] * decoder->InputHeight / 4); i++) {
                outUV[i * 2] = (*((uint16_t*)frame->data[1] + i) + 2) >> 2; // For p010le, U first
                outUV[i * 2 + 1] = (*((uint16_t*)frame->data[2] + i) + 2) >> 2; // For p010le, V second
            }

            //Y
            glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][0]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[0]/2);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decoder->InputWidth,
                decoder->InputHeight,  GL_RED, GL_UNSIGNED_BYTE, outY);
            GlCheck();
            //UV
            glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[surface][1]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[1]/2);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decoder->InputWidth/2,
                decoder->InputHeight/2, GL_RG, GL_UNSIGNED_BYTE, outUV);
            GlCheck();
            glBindTexture(GL_TEXTURE_2D, 0);

        }
        Debug(4, "video/cpu: sw render hw surface %#08x\n", surface);

        CpuQueueSurface(decoder, surface, 1);
        if (outY) free(outY);
        if (outUV) free(outUV);
    }
    if (decoder->Interlaced) {
	++decoder->FrameCounter;
    }
}

///
///	Get hwaccel context for ffmpeg.
///
///	@param decoder	CPU hw decoder
///
static void *CpuGetHwAccelContext(CpuDecoder * decoder)
{
    (void)decoder;

    return NULL;
}

static void CpuMixVideo(CpuDecoder * decoder, int level)
{
    int current;
    int y;
    float xcropf, ycropf;
    GLint texLoc;

#ifdef USE_AUTOCROP
    // FIXME: can move to render frame
    CpuCheckAutoCrop(decoder);
#endif

    xcropf = (float) decoder->CropX / (float) decoder->InputWidth;
    ycropf = (float) decoder->CropY / (float) decoder->InputHeight;

    current = decoder->SurfacesRb[decoder->SurfaceRead];

    if (!decoder->gl_textures[current][0] || !decoder->gl_textures[current][1]) return;
    // Render Progressive frame and simple interlaced
    y = VideoWindowHeight - decoder->OutputY - decoder->OutputHeight;
    if (y < 0)
        y = 0;
    glViewport(decoder->OutputX, y, decoder->OutputWidth, decoder->OutputHeight);

    if (gl_prog == 0)
        gl_prog = sc_generate(gl_prog, decoder->ColorSpace);    // generate shader programm
    if (!gl_prog) return;

    glUseProgram(gl_prog);
    texLoc = glGetUniformLocation(gl_prog, "texture0");
    glUniform1i(texLoc, 0);
    texLoc = glGetUniformLocation(gl_prog, "texture1");
    glUniform1i(texLoc, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[current][0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D,decoder->gl_textures[current][1]);

    render_pass_quad(0, xcropf, ycropf);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);

    Debug(4,"video/cpu: yy video surface %d displayed\n", current);
    (void)level;
}

///
///	Create and display a black empty surface.
///
///	@param decoder	CPU hw decoder
///
///	@FIXME: render only video area, not fullscreen!
///	decoder->Output.. isn't correct setup for radio stations
///
static void CpuBlackSurface(CpuDecoder * decoder)
{
    glClear(GL_COLOR_BUFFER_BIT);
    (void)decoder;
    return;
}

///
///	Advance displayed frame of decoder.
///
///	@param decoder	CPU hw decoder
///
static void CpuAdvanceDecoderFrame(CpuDecoder * decoder)
{
    // next surface, if complete frame is displayed (1 -> 0)
    if (decoder->SurfaceField) {
	int filled;

	// FIXME: this should check the caller
	// check decoder, if new surface is available
	// need 2 frames for progressive
	// need 4 frames for interlaced
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled < 1 + 2 * decoder->Interlaced) {
	    // keep use of last surface
	    ++decoder->FramesDuped;
	    // FIXME: don't warn after stream start, don't warn during pause
	    Debug(4,"video: display buffer empty, duping frame (%d/%d) %d\n",
		decoder->FramesDuped, decoder->FrameCounter,
		VideoGetBuffers(decoder->Stream));
	    return;
	}
	decoder->SurfaceRead = (decoder->SurfaceRead + 1) % (VIDEO_SURFACES_MAX * 2);
	atomic_dec(&decoder->SurfacesFilled);
	decoder->SurfaceField = !decoder->Interlaced;
	return;
    }
    // next field
    decoder->SurfaceField = 1;
    (void)decoder;
}

///
///	Display a video frame.
///
static void CpuDisplayFrame(void)
{
    uint64_t first_time = 0;
    static uint64_t last_time;
    static int timeSS;
    int i;

    if (VideoSurfaceModesChanged) {	// handle changed modes
	VideoSurfaceModesChanged = 0;
	for (i = 0; i < CpuDecoderN; ++i) {
		CpuMixerSetup(CpuDecoders[i]);
	}
    }

    if (GlxEnabled) {
	glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext);
	glClear(GL_COLOR_BUFFER_BIT);
    }
#ifdef USE_EGL
    if (EglEnabled) {
	eglMakeCurrent(EglDisplay, EglSurface, EglSurface, EglThreadContext);
	glClear(GL_COLOR_BUFFER_BIT);
    }
#endif
    // check if surface was displayed for more than 1 frame
	first_time = GetUsTicks();
    // FIXME: 21 only correct for 50Hz
    if (last_time && first_time > last_time + 21 * 1000 * 1000) {
	// FIXME: ignore still-frame, trick-speed
	Debug(3, "video/cpu: %" PRId64 " display time %" PRId64 "\n",
	    first_time / 1000, (first_time - last_time) / 1000);
	// FIXME: can be more than 1 frame long shown
	for (i = 0; i < CpuDecoderN; ++i) {
	    CpuDecoders[i]->FramesMissed++;
	    CpuMessage(2, _("video/cpu: missed frame (%d/%d)\n"),
		CpuDecoders[i]->FramesMissed,
		CpuDecoders[i]->FrameCounter);
	}
    }

    last_time = first_time;
#ifdef USE_SCREENSAVER
    //get up screensaver every 20 sec
    if (DisableScreensaver) {
	if (first_time / 1000000 - timeSS > 20) {
	    xcb_force_screen_saver(Connection,XCB_SCREEN_SAVER_RESET);
	    timeSS = first_time / 1000000;
	}
    }
#endif
    //
    //	Render videos into output
    //
    for (i = 0; i < CpuDecoderN; ++i) {
	int filled;
	CpuDecoder *decoder;

	decoder = CpuDecoders[i];
	decoder->FramesDisplayed++;
	decoder->StartCounter++;

	filled = atomic_read(&decoder->SurfacesFilled);
	// need 1 frame for progressive, 3 frames for interlaced
	if (filled < 1 + 2 * decoder->Interlaced && decoder->Closing) {
	    // FIXME: rewrite MixVideo to support less surfaces
	    if ((VideoShowBlackPicture && !decoder->TrickSpeed)
		|| (VideoShowBlackPicture && decoder->Closing < -300)) {
		CpuBlackSurface(decoder);
		CpuMessage(4, "video/cpu: black surface displayed\n");
#ifdef USE_SCREENSAVER
		if (EnableDPMSatBlackScreen && DPMSDisabled) {
		    CpuMessage(4, "Black surface, DPMS enabled\n");
		    X11DPMSReenable(Connection);
		    X11SuspendScreenSaver(Connection, 1);
		}
#endif
	    }
	    if (VideoShowBlackPicture)
		continue;
#ifdef USE_SCREENSAVER
	} else if (!DPMSDisabled) {	// always disable
	    CpuMessage(4, "DPMS disabled\n");
	    X11DPMSDisable(Connection);
	    X11SuspendScreenSaver(Connection, 0);
#endif
	}

	CpuMixVideo(decoder, i);
    }

    //
    //	add osd to surface
    //
    if(GlxEnabled) {
        if (OsdShown) {
            glViewport(0, 0, VideoWindowWidth, VideoWindowHeight);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, VideoWindowWidth, VideoWindowHeight, 0.0, -1.0, 1.0);
            GlxCheck();
#ifdef USE_OPENGLOSD
            if(!DisableOglOsd && OsdGlTexture) {
                GlxRenderTexture(OsdGlTexture, 0,0, VideoWindowWidth, VideoWindowHeight);
            } else
#endif
                GlxRenderTexture(OsdGlTextures[OsdIndex], 0,0, VideoWindowWidth, VideoWindowHeight);
        }

        glXSwapBuffers(XlibDisplay, VideoWindow);
        glXMakeCurrent(XlibDisplay, None, NULL);
    }
#ifdef USE_EGL
    if (EglEnabled) {
	if (OsdShown) {
            glViewport(0, 0, VideoWindowWidth, VideoWindowHeight);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, VideoWindowWidth, VideoWindowHeight, 0.0, -1.0, 1.0);
            EglCheck();
#ifdef USE_OPENGLOSD
            if(!DisableOglOsd && OsdGlTexture) {
                EglRenderTexture(OsdGlTexture, 0,0, VideoWindowWidth, VideoWindowHeight);
            } else
#endif
	    EglRenderTexture(OsdGlTextures[OsdIndex], 0, 0, VideoWindowWidth, VideoWindowHeight);
	}

	eglSwapBuffers(EglDisplay, EglSurface);
	eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	EglCheck();
    }
#endif
    // FIXME: CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC, &CpuFrameTime);
    for (i = 0; i < CpuDecoderN; ++i) {
	// remember time of last shown surface
	CpuDecoders[i]->FrameTime = CpuFrameTime;
    }

    xcb_flush(Connection);
}


///
///	Set CPU decoder video clock.
///
///	@param decoder	CPU hardware decoder
///	@param pts	audio presentation timestamp
///
void CpuSetClock(CpuDecoder * decoder, int64_t pts)
{
    decoder->PTS = pts;
}

///
///	Get CPU decoder video clock.
///
///	@param decoder	CPU hw decoder
///
///	FIXME: 20 wrong for 60hz dvb streams
///
static int64_t CpuGetClock(const CpuDecoder * decoder)
{
    // pts is the timestamp of the latest decoded frame
    if (decoder->PTS == (int64_t) AV_NOPTS_VALUE) {
	return AV_NOPTS_VALUE;
    }
    // subtract buffered decoded frames
    if (decoder->Interlaced) {
	/*
	   Info("video: %s =pts field%d #%d\n",
	   Timestamp2String(decoder->PTS),
	   decoder->SurfaceField,
	   atomic_read(&decoder->SurfacesFilled));
	 */
	// 1 field is future, 2 fields are past, + 2 in driver queue
	return decoder->PTS -
	    20 * 90 * (2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField - 2 + 2);
    }
    // + 2 in driver queue
    return decoder->PTS - 20 * 90 * (atomic_read(&decoder->SurfacesFilled) + 2);
}

///
///	Set CPU decoder closing stream flag.
///
///	@param decoder	CPU decoder
///
static void CpuSetClosing(CpuDecoder * decoder)
{
    decoder->Closing = 1;
}

///
///	Reset start of frame counter.
///
///	@param decoder	CPU decoder
///
static void CpuResetStart(CpuDecoder * decoder)
{
    decoder->StartCounter = 0;
}

///
///	Set trick play speed.
///
///	@param decoder	CPU decoder
///	@param speed	trick speed (0 = normal)
///
static void CpuSetTrickSpeed(CpuDecoder * decoder, int speed)
{
    decoder->TrickSpeed = speed;
    decoder->TrickCounter = speed;
    if (speed) {
	decoder->Closing = 0;
    }
}

///
///	Get CPU decoder statistics.
///
///	@param decoder		CPU decoder
///	@param[out] missed	missed frames
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void CpuGetStats(CpuDecoder * decoder, int *missed, int *duped,
    int *dropped, int *counter, int *dec)
{
    *missed = decoder->FramesMissed;
    *duped = decoder->FramesDuped;
    *dropped = decoder->FramesDropped;
    *counter = decoder->FrameCounter;
    *dec = HWACCEL_NONE;
}

///
///	Sync decoder output to audio.
///
///	trick-speed	show frame <n> times
///	still-picture	show frame until new frame arrives
///	60hz-mode	repeat every 5th picture
///	video>audio	slow down video by duplicating frames
///	video<audio	speed up video by skipping frames
///	soft-start	show every second frame
///
///	@param decoder	CPU hw decoder
///
static void CpuSyncDecoder(CpuDecoder * decoder)
{
    int err;
    int64_t audio_clock;
    int64_t video_clock;

    err = 0;
    video_clock = CpuGetClock(decoder);
    audio_clock = AudioGetClock();

    if (!decoder->SyncOnAudio) {
	audio_clock = AV_NOPTS_VALUE;
	// FIXME: 60Hz Mode
	goto skip_sync;
    }

    EnoughVideo = (VideoGetBuffers(decoder->Stream) >= (VideoResolution == VideoResolution576i ? VideoStartThreshold_SD * !!VideoSoftStartSync : 0));
    if (!EnoughVideo && EnoughAudio && !AudioRunning && decoder->StartCounter > 200) {
	Debug (3,"video: force enough video\n");
	EnoughVideo = 1;
    }
    if (EnoughVideo && EnoughAudio && !AudioRunning && audio_clock != (int64_t) AV_NOPTS_VALUE) {
        Debug(3, "video: start audio after waiting for enough video: SurfacesFilled: %d, PacketsFilled: %d\n", atomic_read(&decoder->SurfacesFilled), VideoGetBuffers(decoder->Stream));
        AudioStarted = 1;
        AudioRunning = 1;
        pthread_cond_signal(&AudioStartCond);
    }

    // 60Hz: repeat every 5th field
    if (Video60HzMode && !(decoder->FramesDisplayed % 6)) {
	if (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock == (int64_t) AV_NOPTS_VALUE) {
	    goto out;
	}
	// both clocks are known
	if (audio_clock + VideoAudioDelay <= video_clock + 25 * 90) {
	    goto out;
	}
	// out of sync: audio before video
	if (!decoder->TrickSpeed) {
	    goto skip_sync;
	}
    }
    // TrickSpeed
    if (decoder->TrickSpeed) {
	if (decoder->TrickCounter--) {
	    goto out;
	}
	decoder->TrickCounter = decoder->TrickSpeed;
	goto skip_sync;
    }
    // at start of new video stream, soft or hard sync video to audio
    if (!IsReplay() && VideoSoftStartSync < 2 && decoder->StartCounter < VideoSoftStartFrames
	&& video_clock != (int64_t) AV_NOPTS_VALUE
	&& (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock > audio_clock + VideoAudioDelay + 120 * 90)) {
	err =
	    CpuMessage(3, "video: initial slow down video, frame %d\n",
	    decoder->StartCounter);
	goto out;
    }

    if (decoder->SyncCounter && decoder->SyncCounter-- && VideoSoftStartSync > 1) {
	goto skip_sync;
    }

    if (audio_clock != (int64_t) AV_NOPTS_VALUE
	&& video_clock != (int64_t) AV_NOPTS_VALUE) {
	// both clocks are known
	int diff;
	int lower_limit;
	if (IsReplay()) {
	    if((video_clock > audio_clock + VideoAudioDelay + 8000 * 90) && decoder->StartCounter < VideoSoftStartFrames) { // 8s
		Debug(3,"flush audio\n");
		AudioFlushBuffers();
		goto out;
	    }
	    if((video_clock < audio_clock + VideoAudioDelay - 120 * 90) && VideoSoftStartSync < 2) {
		if (!(decoder->SurfaceField && atomic_read(&decoder->SurfacesFilled) < 1 + 2 * decoder->Interlaced)) {
		    Debug(3,"drop video\n");
		    CpuAdvanceDecoderFrame(decoder);
		}
		goto skip_sync;
	    }
	}
	diff = video_clock - audio_clock - VideoAudioDelay;
	lower_limit = !IsReplay() ? -25 - (VideoResolution == VideoResolution576i ? 40 : 0) : 32;
	//diff = (decoder->LastAVDiff + diff) / 2;
	decoder->LastAVDiff = diff;
#ifdef DEBUG
	if (!decoder->SyncCounter && decoder->StartCounter < 1000)
	    Debug(3, "video/cpu: diff %d %d lim %d fill %d\n", diff, diff/90, lower_limit, atomic_read(&decoder->SurfacesFilled));
#endif
	if (abs(diff) > 8000 * 90) {	// more than 8s
	    err = CpuMessage(3, "video: audio/video difference too big\n");
	}
	if (diff > 100 * 90) {
	    // FIXME: this quicker sync step, did not work with new code!
	    err = CpuMessage(3, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    if (VideoSoftStartSync > 1) {
		decoder->SyncCounter = diff > 100 * 90 ? diff % 2 : 1; //softsync :)
	    }
	    goto out;
	} else if (diff > 55 * 90) {
	    err = CpuMessage(3, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    decoder->SyncCounter = 1;
	    goto out;
	} else if (diff < lower_limit * 90 && atomic_read(&decoder->SurfacesFilled) > 1 + decoder->Interlaced) { // double advance possible?
	    err = CpuMessage(3, "video: speed up video, droping frame\n");
	    ++decoder->FramesDropped;
	    CpuAdvanceDecoderFrame(decoder);
	    decoder->SyncCounter = 2;
	} else if (diff < lower_limit * 90 && atomic_read(&decoder->SurfacesFilled) < 1 + 2 * decoder->Interlaced && !IsReplay()) {
	    err = CpuMessage(3, "video: speed up audio, delay audio\n");
	    AudioDelayms(-diff / 90 + 55);
	}
#if defined(DEBUG) || defined(AV_INFO)
	if (!decoder->SyncCounter && decoder->StartCounter < 1000) {
#ifdef DEBUG
	    Debug(3, "video/cpu: synced after %d frames %dms\n",
		decoder->StartCounter, GetMsTicks() - VideoSwitch);
#else
	    Info("video/cpu: synced after %d frames\n",
		decoder->StartCounter);
#endif
	    decoder->StartCounter += 1000;
	}
#endif
    }

  skip_sync:
    // is it not possible, to advance the surface and/or the field?
    if (decoder->SurfaceField && atomic_read(&decoder->SurfacesFilled) < 1 + 2 * decoder->Interlaced + 1 * !IsReplay()) {
        ++decoder->FramesDuped;
        // FIXME: don't warn after stream start, don't warn during pause
        err =
            CpuMessage(3,
            _("video: decoder buffer empty, "
            "duping frame (%d/%d) %d v-buf closing %d\n"), decoder->FramesDuped,
            decoder->FrameCounter, VideoGetBuffers(decoder->Stream), decoder->Closing);
        // some time no new picture or black video configured
        if (decoder->Closing < -300 || (VideoShowBlackPicture
            && decoder->Closing)) {
            // clear ring buffer to trigger black picture
            atomic_set(&decoder->SurfacesFilled, 0);
	}
	goto out;
    }

    CpuAdvanceDecoderFrame(decoder);
  out:
#if defined(DEBUG) || defined(AV_INFO)
    // debug audio/video sync
    if (err || !(decoder->FramesDisplayed % AV_INFO_TIME)) {
	if (!err) {
	    CpuMessage(0, NULL);
	}
	Info("video: %s%+5" PRId64 " %4" PRId64 " %3d/\\ms %3d%+d%+d v-buf\n",
	    Timestamp2String(video_clock),
	    abs((int)(video_clock - audio_clock) / 90) <
	    8888 ? ((video_clock - audio_clock) / 90) : 8888,
	    AudioGetDelay() / 90, (int)VideoDeltaPTS / 90,
	    VideoGetBuffers(decoder->Stream),
	    decoder->Interlaced ? 2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField : atomic_read(&decoder->SurfacesFilled),
	    CpuSurfaceQueued);
	if (!(decoder->FramesDisplayed % (5 * 60 * 60))) {
	    CpuPrintFrames(decoder);
	}
    }
#endif
    return;				// fix gcc bug!
}

///
///	Sync a video frame.
///
static void CpuSyncFrame(void)
{
    int i;

    //
    //	Sync video decoder to audio
    //
    for (i = 0; i < CpuDecoderN; ++i) {
	CpuSyncDecoder(CpuDecoders[i]);
    }
}

///
///	Sync and display surface.
///
static void CpuSyncDisplayFrame(void)
{
    CpuDisplayFrame();
    CpuSyncFrame();
}

///
///	Sync and render a ffmpeg frame
///
///	@param decoder		CPU hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void CpuSyncRenderFrame(CpuDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    // FIXME: temp debug
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,61,100)
    if (0 && frame->pkt_pts != (int64_t) AV_NOPTS_VALUE) {
	Debug(3, "video: render frame pts %s\n",
	    Timestamp2String(frame->pkt_pts));
#else
    if (0 && frame->pts != (int64_t) AV_NOPTS_VALUE) {
	Debug(3, "video: render frame pts %s\n",
	    Timestamp2String(frame->pts));
#endif
    }
#ifdef DEBUG
    if (!atomic_read(&decoder->SurfacesFilled)) {
	Debug(3, "video: new stream frame %dms\n", GetMsTicks() - VideoSwitch);
    }
#endif

#if 1
#ifndef USE_PIP
#error	"-DUSE_PIP or #define USE_PIP is needed,"
#endif
    // if video output buffer is full, wait and display surface.
    // loop for interlace
    if (atomic_read(&decoder->SurfacesFilled) >= (VIDEO_SURFACES_MAX * 2)) {
	Info("video/cpu: this code part shouldn't be used\n");
	return;
    }
#else
    // FIXME: disabled for remove
    // FIXME: wrong for multiple streams
    // FIXME: this part code should be no longer be needed with new mpeg fix
    while (atomic_read(&decoder->SurfacesFilled) >= (VIDEO_SURFACES_MAX * 2)) {
	struct timespec abstime;

	pthread_mutex_unlock(&VideoLockMutex);

	abstime = decoder->FrameTime;
	abstime.tv_nsec += 14 * 1000 * 1000;
	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
	    // avoid overflow
	    abstime.tv_sec++;
	    abstime.tv_nsec -= 1000 * 1000 * 1000;
	}

	VideoPollEvent();

	// fix dead-lock with VdpauExit
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_mutex_lock(&VideoLockMutex);
	// give osd some time slot
	while (pthread_cond_timedwait(&VideoWakeupCond, &VideoLockMutex,
		&abstime) != ETIMEDOUT) {
	    // SIGUSR1
	    Debug(3, "video/cpu: pthread_cond_timedwait error\n");
	}
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	CpuSyncDisplayFrame();
    }
#endif

    if (!decoder->Closing) {
	VideoSetPts(&decoder->PTS, decoder->Interlaced, video_ctx, frame);
    }
    CpuRenderFrame(decoder, video_ctx, frame);
}

///
///	Set CPU background color.
///
///	@param rgba	32 bit RGBA color.
///
static void CpuSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
}

///
///	Set video output position.
///
///	@param decoder	CPU hw decoder
///	@param x	video output x coordinate inside the window
///	@param y	video output y coordinate inside the window
///	@param width	video output width
///	@param height	video output height
///
///	@note FIXME: need to know which stream.
///
static void CpuSetOutputPosition(CpuDecoder * decoder, int x, int y,
    int width, int height)
{
    Debug(3, "video/cpu: output %dx%d%+d%+d\n", width, height, x, y);

    decoder->VideoX = x;
    decoder->VideoY = y;
    decoder->VideoWidth = width;
    decoder->VideoHeight = height;

    // next video pictures are automatic rendered to correct position
}

///
///	Set CPU video mode.
///
static void CpuSetVideoMode(void)
{
    int i;

    for (i = 0; i < CpuDecoderN; ++i) {
	// reset video window, upper level needs to fix the positions
	CpuDecoders[i]->VideoX = 0;
	CpuDecoders[i]->VideoY = 0;
	CpuDecoders[i]->VideoWidth = VideoWindowWidth;
	CpuDecoders[i]->VideoHeight = VideoWindowHeight;
	// for window mode need restore scale
	if (CpuDecoders[i]->ScaleX || CpuDecoders[i]->ScaleY
	 || CpuDecoders[i]->ScaleWidth || CpuDecoders[i]->ScaleHeight) {
	    // convert OSD coordinates to window coordinates
	    int x = (CpuDecoders[i]->ScaleX * VideoWindowWidth) / OsdWidth;
	    int width = (CpuDecoders[i]->ScaleWidth * VideoWindowWidth) / OsdWidth;
	    int y = (CpuDecoders[i]->ScaleY * VideoWindowHeight) / OsdHeight;
	    int height = (CpuDecoders[i]->ScaleHeight * VideoWindowHeight) / OsdHeight;
	    CpuSetOutputPosition(CpuDecoders[i], x, y, width, height);
	}
	CpuUpdateOutput(CpuDecoders[i]);
    }
}

#ifdef USE_VIDEO_THREAD2

#else

#ifdef USE_VIDEO_THREAD

///
///	Handle a CPU display.
///
static void CpuDisplayHandlerThread(void)
{
    int i;
    int err;
    int allfull;
    int decoded;
    struct timespec nowtime;
    CpuDecoder *decoder;

    allfull = 1;
    decoded = 0;
    pthread_mutex_lock(&VideoLockMutex);
    for (i = 0; i < CpuDecoderN; ++i) {
	int filled;

	decoder = CpuDecoders[i];

	//
	// fill frame output ring buffer
	//
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled <= 1 + 2 * decoder->Interlaced) {
	    // FIXME: hot polling
	    // fetch+decode or reopen
	    allfull = 0;
	    err = VideoDecodeInput(decoder->Stream);
	} else {
	    err = VideoPollInput(decoder->Stream);
	}
	// decoder can be invalid here
	if (err) {
	    // nothing buffered?
	    if (err == -1 && decoder->Closing) {
		decoder->Closing--;
		if (!decoder->Closing) {
		    Debug(3, "video/cpu: closing eof\n");
		    decoder->Closing = -1;
		}
	    }
	    continue;
	}
	decoded = 1;
    }
    pthread_mutex_unlock(&VideoLockMutex);

    if (!decoded) {			// nothing decoded, sleep
	// FIXME: sleep on wakeup
	usleep(1 * 1000);
    }
    // all decoder buffers are full
    // and display is not preempted
    // speed up filling display queue, wait on display queue empty
    if (!allfull) {
	clock_gettime(CLOCK_MONOTONIC, &nowtime);
	// time for one frame over?
	if ((nowtime.tv_sec - CpuFrameTime.tv_sec) * 1000 * 1000 * 1000 +
	    (nowtime.tv_nsec - CpuFrameTime.tv_nsec) < 15 * 1000 * 1000) {
	    return;
	}
    }

    pthread_mutex_lock(&VideoLockMutex);
    CpuSyncDisplayFrame();
    pthread_mutex_unlock(&VideoLockMutex);
}

#else

#define CpuDisplayHandlerThread	NULL

#endif

#endif

//----------------------------------------------------------------------------
//	CPU OSD USE GLX
//----------------------------------------------------------------------------
#ifdef USE_OPENGLOSD
void *GetCpuGlxOsdOutputTexture(GLuint texture) {
    OsdGlTexture = texture;
    return 0;
}

int CpuInitGlx(void) {
    int a = 0;

    if (!GlxEnabled) {
        Debug(3,"video/osd: can't create glx context\n");
        return 0;
    }
    //after run an external player from time to time vdr not set playmode 1
    //then try start GLX forced.
    //is it a vdr bug or external player plugin???
    while (!GlxContext || !GlxThreadContext){
        usleep(1000);
        a++;
        if (a > 10 && a < 1000) {
            Debug(3,"Try start GLX forced\n");
            VideoDisplayWakeup();
            a = 1000;
        }
        if (a > 1010) return 0;
    }
    Debug(3,"Create OSD GLX context\n");
    glXMakeCurrent(XlibDisplay, None, NULL);
    glXMakeCurrent(XlibDisplay, VideoWindow, GlxContext);
    return 1;
}
#endif

///
///	CPU module.
///
static const VideoModule CpuGlxModule = {
    .Name = "cpu-glx",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) CpuNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))CpuDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))CpuGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))CpuReleaseSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Cpu_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))CpuSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	CpuGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))CpuSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))CpuGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))CpuSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))CpuResetStart,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))CpuSetTrickSpeed,
#ifdef USE_GRAB
    .GrabOutput = CpuGrabOutputSurface,
#endif
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, int *))CpuGetStats,
    .SetBackground = CpuSetBackground,
    .SetVideoMode = CpuSetVideoMode,
#ifdef USE_AUTOCROP
    .ResetAutoCrop = CpuResetAutoCrop,
#endif
    .DisplayHandlerThread = CpuDisplayHandlerThread,
    .OsdClear = GlxOsdClear,
    .OsdDrawARGB = GlxOsdDrawARGB,
    .OsdInit = GlxOsdInit,
    .OsdExit = GlxOsdExit,
    .MaxPixmapSize = GlxMaxPixmapSize,
    .Init = CpuGlxInit,
    .Exit = CpuExit,
};

//----------------------------------------------------------------------------
//	CPU OSD USE EGL
//----------------------------------------------------------------------------
#ifdef USE_EGL

#ifdef USE_OPENGLOSD
void *GetCpuEglOsdOutputTexture(GLuint texture) {
    OsdGlTexture = texture;
    return 0;
}

int CpuInitEgl(void) {
    int a = 0;

    if (!EglEnabled) {
        Debug(3,"video/osd: can't create egl context\n");
        return 0;
    }
    //after run an external player from time to time vdr not set playmode 1
    //then try start EGL forced.
    //is it a vdr bug or external player plugin???
    while (!EglContext || !EglThreadContext){
        usleep(1000);
        a++;
        if (a > 10 && a < 1000) {
            Debug(3,"Try start EGL forced\n");
            VideoDisplayWakeup();
            a = 1000;
        }
        if (a > 1010) return 0;
    }
    Debug(3,"Create OSD EGL context\n");
    eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EglContext);
    return 1;
}
#endif

///
///	CPU EGL module.
///
static const VideoModule CpuEglModule = {
    .Name = "cpu-egl",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) CpuNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))CpuDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))CpuGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))CpuReleaseSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Cpu_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))CpuSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	CpuGetHwAccelContext,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))CpuSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))CpuGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))CpuSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))CpuResetStart,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))CpuSetTrickSpeed,
#ifdef USE_GRAB
    .GrabOutput = CpuGrabOutputSurface,
#endif
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, int *))CpuGetStats,
    .SetBackground = CpuSetBackground,
    .SetVideoMode = CpuSetVideoMode,
#ifdef USE_AUTOCROP
    .ResetAutoCrop = CpuResetAutoCrop,
#endif
    .DisplayHandlerThread = CpuDisplayHandlerThread,
    .OsdClear = EglOsdClear,
    .OsdDrawARGB = EglOsdDrawARGB,
    .OsdInit = EglOsdInit,
    .OsdExit = EglOsdExit,
    .MaxPixmapSize = EglMaxPixmapSize,
    .Init = CpuEglInit,
    .Exit = CpuExit,
};
#endif

#endif



//----------------------------------------------------------------------------
//	NOOP
//----------------------------------------------------------------------------

///
///	Allocate new noop decoder.
///
///	@param stream	video stream
///
///	@returns always NULL.
///
static VideoHwDecoder *NoopNewHwDecoder(
    __attribute__ ((unused)) VideoStream * stream)
{
    return NULL;
}

///
///	Release a surface.
///
///	Can be called while exit.
///
///	@param decoder	noop hw decoder
///	@param surface	surface no longer used
///
static void NoopReleaseSurface(
    __attribute__ ((unused)) VideoHwDecoder * decoder, __attribute__ ((unused))
    unsigned surface)
{
}

///
///	Set noop decoder video clock.
///
///	Can be called while exiting.
///
///	@param decoder	noop hardware decoder
///	@param pts	audio presentation timestamp
///
void NoopSetClock(__attribute__ ((unused)) VideoHwDecoder * decoder,
    __attribute__ ((unused)) int64_t pts)
{
}

///
///	Set noop decoder closing stream flag.
///
///	Can be called while exiting.
///
///	@param decoder	noop hw decoder
///
static void NoopSetClosing(__attribute__ ((unused)) VideoHwDecoder * decoder)
{
}

///
///	Reset start of frame counter.
///
///	Can be called while exiting.
///
///	@param decoder	noop decoder
///
static void NoopResetStart(__attribute__ ((unused)) VideoHwDecoder * decoder)
{
}

///
///	Set noop background color.
///
///	@param rgba	32 bit RGBA color.
///
static void NoopSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
}

///
///	Noop initialize OSD.
///
///	@param width	osd width
///	@param height	osd height
///
static void NoopOsdInit( __attribute__ ((unused))
    int width, __attribute__ ((unused))
    int height)
{
}

///
///	Draw OSD ARGB image.
///
///	@param xi	x-coordinate in argb image
///	@param yi	y-coordinate in argb image
///	@paran height	height in pixel in argb image
///	@paran width	width in pixel in argb image
///	@param pitch	pitch of argb image
///	@param argb	32bit ARGB image data
///	@param x	x-coordinate on screen of argb image
///	@param y	y-coordinate on screen of argb image
///
///	@note looked by caller
///
static void NoopOsdDrawARGB( __attribute__ ((unused))
    int xi, __attribute__ ((unused))
    int yi, __attribute__ ((unused))
    int width, __attribute__ ((unused))
    int height, __attribute__ ((unused))
    int pitch, __attribute__ ((unused))
    const uint8_t * argb, __attribute__ ((unused))
    int x, __attribute__ ((unused))
    int y)
{
}

static int NoopMaxPixmapSize(void)
{
    return 0;
}

///
///	Noop setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns always true.
///
static int NoopInit(const char *display_name)
{
    Info("video/noop: noop driver running on display '%s'\n", display_name);
    return 1;
}

static uint8_t *NoopGrabOutputSurface(int *ret_size, int *ret_width,  int *ret_height)
{
    *ret_size = 0;
    *ret_width = 0;
    *ret_height = 0;
    return NULL;
}

#ifdef USE_VIDEO_THREAD

///
///	Handle a noop display.
///
static void NoopDisplayHandlerThread(void)
{
    // avoid 100% cpu use
    usleep(20 * 1000);
#if 0
    // this can't be canceled
    if (XlibDisplay) {
	XEvent event;

	XPeekEvent(XlibDisplay, &event);
    }
#endif
}

#else

#define NoopDisplayHandlerThread	NULL

#endif

///
///	Noop void function.
///
static void NoopVoid(void)
{
}

///
///	Noop video module.
///
static const VideoModule NoopModule = {
    .Name = "noop",
    .Enabled = 1,
    .NewHwDecoder = NoopNewHwDecoder,
#if 0
    // can't be called:
    .DelHwDecoder = NoopDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))NoopGetSurface,
#endif
    .ReleaseSurface = NoopReleaseSurface,
#if 0
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Noop_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))NoopSyncRenderFrame,
    .GetHwAccelContext = (void *(*const)(VideoHwDecoder *))
	DummyGetHwAccelContext,
#endif
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))NoopSetClock,
#if 0
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))NoopGetClock,
#endif
    .SetClosing = (void (*const) (const VideoHwDecoder *))NoopSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))NoopResetStart,
    .GrabOutput = NoopGrabOutputSurface,
#if 0
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))NoopSetTrickSpeed,
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *, int *))NoopGetStats,
#endif
    .SetBackground = NoopSetBackground,
    .SetVideoMode = NoopVoid,
    .ResetAutoCrop = NoopVoid,
    .DisplayHandlerThread = NoopDisplayHandlerThread,
    .OsdClear = NoopVoid,
    .OsdDrawARGB = NoopOsdDrawARGB,
    .OsdInit = NoopOsdInit,
    .OsdExit = NoopVoid,
    .MaxPixmapSize = NoopMaxPixmapSize,
    .Init = NoopInit,
    .Exit = NoopVoid,
};

//----------------------------------------------------------------------------
//	OSD
//----------------------------------------------------------------------------

///
///	Clear the OSD.
///
///	@todo I use glTexImage2D to clear the texture, are there faster and
///	better ways to clear a texture?
///
void VideoOsdClear(void)
{
    VideoThreadLock();
    VideoUsedModule->OsdClear();

    OsdDirtyX = OsdWidth;		// reset dirty area
    OsdDirtyY = OsdHeight;
    OsdDirtyWidth = 0;
    OsdDirtyHeight = 0;
    OsdShown = 0;

    VideoThreadUnlock();
}

///
///	Draw an OSD ARGB image.
///
///	@param xi	x-coordinate in argb image
///	@param yi	y-coordinate in argb image
///	@paran height	height in pixel in argb image
///	@paran width	width in pixel in argb image
///	@param pitch	pitch of argb image
///	@param argb	32bit ARGB image data
///	@param x	x-coordinate on screen of argb image
///	@param y	y-coordinate on screen of argb image
///
void VideoOsdDrawARGB(int xi, int yi, int width, int height, int pitch,
    const uint8_t * argb, int x, int y)
{
    VideoThreadLock();
    // update dirty area
    if (x < OsdDirtyX) {
	if (OsdDirtyWidth) {
	    OsdDirtyWidth += OsdDirtyX - x;
	}
	OsdDirtyX = x;
    }
    if (y < OsdDirtyY) {
	if (OsdDirtyHeight) {
	    OsdDirtyHeight += OsdDirtyY - y;
	}
	OsdDirtyY = y;
    }
    if (x + width > OsdDirtyX + OsdDirtyWidth) {
	OsdDirtyWidth = x + width - OsdDirtyX;
    }
    if (y + height > OsdDirtyY + OsdDirtyHeight) {
	OsdDirtyHeight = y + height - OsdDirtyY;
    }
    Debug(4, "video: osd dirty %dx%d%+d%+d -> %dx%d%+d%+d\n", width, height, x,
	y, OsdDirtyWidth, OsdDirtyHeight, OsdDirtyX, OsdDirtyY);

    VideoUsedModule->OsdDrawARGB(xi, yi, width, height, pitch, argb, x, y);
    OsdShown = 1;

    VideoThreadUnlock();
}

void ActivateOsd(void) {
    OsdShown = 1;
}

///
///	Get OSD size.
///
///	@param[out] width	OSD width
///	@param[out] height	OSD height
///
void VideoGetOsdSize(int *width, int *height)
{
    *width = 1920;
    *height = 1080;			// unknown default
    if (OsdWidth && OsdHeight) {
	*width = OsdWidth;
	*height = OsdHeight;
    }
}

///	Set OSD Size.
///
///	@param width	OSD width
///	@param height	OSD height
///
void VideoSetOsdSize(int width, int height)
{
    if (OsdConfigWidth != width || OsdConfigHeight != height) {
	VideoOsdExit();
	OsdConfigWidth = width;
	OsdConfigHeight = height;
	VideoOsdInit();
    }
}

///
///	Set the 3d OSD mode.
///
///	@param mode	OSD mode (0=off, 1=SBS, 2=Top Bottom)
///
void VideoSetOsd3DMode(int mode)
{
    Osd3DMode = mode;
}

///
///	Setup osd.
///
///	FIXME: looking for BGRA, but this fourcc isn't supported by the
///	drawing functions yet.
///
void VideoOsdInit(void)
{
    if (OsdConfigWidth && OsdConfigHeight) {
	OsdWidth = OsdConfigWidth;
	OsdHeight = OsdConfigHeight;
    } else {
	OsdWidth = VideoScreenWidth;
	OsdHeight = VideoScreenHeight;
    }

    VideoThreadLock();
    VideoUsedModule->OsdInit(OsdWidth, OsdHeight);
    VideoThreadUnlock();
    VideoOsdClear();
}

///
///	Cleanup OSD.
///
void VideoOsdExit(void)
{
    VideoOsdClear();
    VideoThreadLock();
    VideoUsedModule->OsdExit();
    VideoThreadUnlock();
    OsdDirtyWidth = 0;
    OsdDirtyHeight = 0;
}

#ifdef USE_OPENGLOSD
///
///	Need Restart OpenGL OSD
///
int VideoOsdNeedRestart(void)
{
    return OsdNeedRestart;
}
#endif

int VideoMaxPixmapSize (void)
{
	return VideoUsedModule->MaxPixmapSize();
}

//----------------------------------------------------------------------------
//	Events
//----------------------------------------------------------------------------

/// C callback feed key press
extern void FeedKeyPress(const char *, const char *, int, int, const char *);

extern uint8_t *GrabExtService(int *, int *, int *);

///
///	Handle XLib I/O Errors.
///
///	@param display	display with i/o error
///
static int VideoIOErrorHandler( __attribute__ ((unused)) Display * display)
{

    Error(_("video: fatal i/o error\n"));
    // should be called from VideoThread
    if (VideoThread && VideoThread == pthread_self()) {
	Debug(3, "video: called from video thread\n");
	VideoUsedModule = &NoopModule;
	XlibDisplay = NULL;
	VideoWindow = XCB_NONE;
#ifdef USE_VIDEO_THREAD
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_cond_destroy(&VideoWakeupCond);
	pthread_mutex_destroy(&VideoLockMutex);
	pthread_mutex_destroy(&VideoMutex);
	VideoThread = 0;
	pthread_exit("video thread exit");
#endif
    }
    do {
	sleep(1000);
    } while (1);			// let other threads running

    return -1;
}

///
///	Handle X11 events.
///
///	@todo	Signal WmDeleteMessage to application.
///
static void VideoEvent(void)
{
    XEvent event;
    KeySym keysym;
    const char *keynam;
    char buf[64];
    char letter[64];
    int letter_len;
    uint32_t values[1];
    static Time clicktime;

    VideoThreadLock();
    XNextEvent(XlibDisplay, &event);
    VideoThreadUnlock();
    switch (event.type) {
	case ClientMessage:
	    Debug(3, "video/event: ClientMessage\n");
	    if (event.xclient.data.l[0] == (long)WmDeleteWindowAtom) {
		Debug(3, "video/event: wm-delete-message\n");
		Shutdown();
	    }
	    break;

	case MapNotify:
	    Debug(3, "video/event: MapNotify\n");
	    // wm workaround
	    VideoThreadLock();
	    xcb_change_window_attributes(Connection, VideoWindow,
		XCB_CW_CURSOR, &VideoBlankCursor);
	    VideoThreadUnlock();
	    VideoBlankTick = 0;
	    break;
	case Expose:
	    //Debug(3, "video/event: Expose\n");
	    break;
	case ReparentNotify:
	    Debug(3, "video/event: ReparentNotify\n");
	    break;
	case ConfigureNotify:
	    {
	    //Debug(3, "video/event: ConfigureNotify\n");
	    //get window position and save it
	    //x11 creates window with title and child window with video
	    xcb_window_t parent = 0;
	    int x = 0, y = 0;
	    xcb_query_tree_cookie_t  cookie;
	    xcb_query_tree_reply_t *reply = NULL;
	    xcb_get_geometry_cookie_t geocookie;
	    xcb_get_geometry_reply_t *georeply = NULL;
	    //get video window position
	    geocookie = xcb_get_geometry(Connection, VideoWindow);
	    georeply = xcb_get_geometry_reply(Connection, geocookie, NULL);
	    if (georeply) {
	        x = georeply->x;
	        y = 8;//georeply->y; //height of window title "softhddevice"
	        free(georeply);
	    }
	    //get parent window
	    cookie = xcb_query_tree(Connection, VideoWindow);
	    reply = xcb_query_tree_reply(Connection, cookie, 0);
	    if (reply) {
	        parent = reply->parent;
	        free(reply);
	    }
	    //get parent window position
	    if (parent) {
	        geocookie = xcb_get_geometry(Connection, parent);
	        georeply = xcb_get_geometry_reply(Connection, geocookie, NULL);
	        if (georeply) {
	            x += georeply->x;
	            y += georeply->y;
	            free(georeply);
	        }
	    }
	    VideoSetVideoMode(x, y, event.xconfigure.width, event.xconfigure.height);
	    break;
	    }
	case ButtonPress:
	    if (event.xbutton.button == 1) {
		Time difftime = event.xbutton.time - clicktime;
		if (difftime < 500)
		    VideoSetFullscreen(-1);
		clicktime = event.xbutton.time;
	    }
	    else if (event.xbutton.button == 2) {
		FeedKeyPress("XKeySym", "Ok", 0, 0, NULL);
	    }
	    else if (event.xbutton.button == 3) {
		FeedKeyPress("XKeySym", "Menu", 0, 0, NULL);
	    }
	    if (event.xbutton.button == 4) {
		FeedKeyPress("XKeySym", "Volume+", 0, 0, NULL);
	    }
	    if (event.xbutton.button == 5) {
		FeedKeyPress("XKeySym", "Volume-", 0, 0, NULL);
	    }
	    break;
	case ButtonRelease:
	    break;
	case KeyPress:
	    VideoThreadLock();
	    letter_len =
		XLookupString(&event.xkey, letter, sizeof(letter) - 1, &keysym,
		NULL);
	    VideoThreadUnlock();
	    if (letter_len < 0) {
		letter_len = 0;
	    }
	    letter[letter_len] = '\0';
	    if (keysym == NoSymbol) {
		Warning(_("video/event: No symbol for %d\n"),
		    event.xkey.keycode);
		break;
	    }
	    VideoThreadLock();
	    keynam = XKeysymToString(keysym);
	    VideoThreadUnlock();
	    // check for key modifiers (Alt/Ctrl)
	    if (event.xkey.state & (Mod1Mask | ControlMask)) {
		if (event.xkey.state & Mod1Mask) {
		    strcpy(buf, "Alt+");
		} else {
		    buf[0] = '\0';
		}
		if (event.xkey.state & ControlMask) {
		    strcat(buf, "Ctrl+");
		}
		strncat(buf, keynam, sizeof(buf) - 10);
		keynam = buf;
	    }
	    FeedKeyPress("XKeySym", keynam, 0, 0, letter);
	    break;
	case KeyRelease:
	    break;
	case MotionNotify:
	    values[0] = XCB_NONE;
	    VideoThreadLock();
	    xcb_change_window_attributes(Connection, VideoWindow,
		XCB_CW_CURSOR, values);
	    VideoThreadUnlock();
	    VideoBlankTick = GetMsTicks();
	    break;
	default:
#if 0
	    if (XShmGetEventBase(XlibDisplay) + ShmCompletion == event.type) {
		// printf("ShmCompletion\n");
	    }
#endif
	    Debug(3, "Unsupported event type %d\n", event.type);
	    break;
    }
}

///
///	Poll all x11 events.
///
void VideoPollEvent(void)
{
    // hide cursor, after xx ms
    if (VideoBlankTick && VideoWindow != XCB_NONE
	&& VideoBlankTick + 200 < GetMsTicks()) {
	VideoThreadLock();
	xcb_change_window_attributes(Connection, VideoWindow, XCB_CW_CURSOR,
	    &VideoBlankCursor);
	VideoThreadUnlock();
	VideoBlankTick = 0;
    }
    while (XlibDisplay) {
	VideoThreadLock();
	if (!XPending(XlibDisplay)) {
	    VideoThreadUnlock();
	    break;
	}
	VideoThreadUnlock();
	VideoEvent();
    }
}

//----------------------------------------------------------------------------
//	Thread
//----------------------------------------------------------------------------

#ifdef USE_VIDEO_THREAD

///
///	Lock video thread.
///
static void VideoThreadLock(void)
{
    if (VideoThread) {
	if (pthread_mutex_lock(&VideoLockMutex)) {
	    Error(_("video: can't lock thread\n"));
	}
    }
}

///
///	Unlock video thread.
///
static void VideoThreadUnlock(void)
{
    if (VideoThread) {
	if (pthread_mutex_unlock(&VideoLockMutex)) {
	    Error(_("video: can't unlock thread\n"));
	}
    }
}

///
///	Video render thread.
///
static void *VideoDisplayHandlerThread(void *dummy)
{
    Debug(3, "video: display thread started\n");

#ifdef USE_GLX
    if (GlxEnabled) {
	Debug(3, "video/glx: thread context %p <-> %p\n",
	    glXGetCurrentContext(), GlxThreadContext);
	Debug(3, "video/glx: context %p <-> %p\n", glXGetCurrentContext(),
	    GlxContext);

	GlxThreadContext =
	    glXCreateContext(XlibDisplay, GlxVisualInfo, GlxSharedContext,
	    GL_TRUE);
	if (!GlxThreadContext) {
	    Error(_("video/glx: can't create glx context\n"));
	    return NULL;
	}
	// set glx context
	GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight,
	    GlxThreadContext);
    }
#endif
#ifdef USE_EGL
    if (EglEnabled) {
	eglBindAPI(EGL_OPENGL_API);
	Debug(3, "video/egl: thread context %p <-> %p\n",
	    eglGetCurrentContext(), EglThreadContext);
	Debug(3, "video/egl: context %p <-> %p\n", eglGetCurrentContext(),
	    EglContext);

	EglThreadContext =
	    eglCreateContext(EglDisplay, EglConfig, EglSharedContext, EglContextAttr);
	if (!EglThreadContext) {
	    Error(_("video/egl: can't create egl thread context\n"));
	    return NULL;
	}
	// set egl context
	EglSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight,
	    EglThreadContext);
    }
#endif

    for (;;) {
	// fix dead-lock with VdpauExit
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	VideoPollEvent();
	VideoUsedModule->DisplayHandlerThread();
    }
    return dummy;
}

///
///	Initialize video threads.
///
static void VideoThreadInit(void)
{
#ifdef USE_GLX
    if (GlxEnabled)
        glXMakeCurrent(XlibDisplay, None, NULL);
#endif
#ifdef USE_EGL
//    if (EglEnabled)
//        eglMakeCurrent(EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
#endif
    pthread_mutex_init(&VideoMutex, NULL);
    pthread_mutex_init(&VideoLockMutex, NULL);
    pthread_cond_init(&VideoWakeupCond, NULL);
    pthread_create(&VideoThread, NULL, VideoDisplayHandlerThread, NULL);
    pthread_setname_np(VideoThread, "softhddev video");
}

///
///	Exit and cleanup video threads.
///
static void VideoThreadExit(void)
{
    if (VideoThread) {
	void *retval;

	Debug(3, "video: video thread canceled\n");
	//VideoThreadLock();
	// FIXME: can't cancel locked
	if (pthread_cancel(VideoThread)) {
	    Error(_("video: can't queue cancel video display thread\n"));
	}
	//VideoThreadUnlock();
	if (pthread_join(VideoThread, &retval) || retval != PTHREAD_CANCELED) {
	    Error(_("video: can't cancel video display thread\n"));
	}
	VideoThread = 0;
	pthread_cond_destroy(&VideoWakeupCond);
	pthread_mutex_destroy(&VideoLockMutex);
	pthread_mutex_destroy(&VideoMutex);
    }
}

///
///	Video display wakeup.
///
///	New video arrived, wakeup video thread.
///
void VideoDisplayWakeup(void)
{
    if (!XlibDisplay) {			// not yet started
	return;
    }

    if (!VideoThread) {			// start video thread, if needed
	VideoThreadInit();
    }
}

#endif

//----------------------------------------------------------------------------
//	Video API
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

///
///	Table of all video modules.
///
static const VideoModule *VideoModules[] = {
#ifdef USE_VDPAU
    &VdpauModule,
#ifdef USE_GLX
    &VdpauGlxModule,
#endif
#endif
#ifdef USE_VAAPI
    &VaapiModule,
#if defined USE_GLX && !defined VAAPI_NO_GLX
    &VaapiGlxModule,
#endif
#ifdef USE_EGL
    &VaapiEglModule,
#endif
#endif
#ifdef USE_CUVID
    &CuvidModule,
    &NVdecModule,
#ifdef USE_EGL
    &CuvidEglModule,
    &NVdecEglModule,
#endif
#endif
#ifdef USE_GLX
    &CpuGlxModule,
#endif
#ifdef USE_EGL
    &CpuEglModule,
#endif
    &NoopModule
};

///
///	Video hardware decoder
///
struct _video_hw_decoder_
{
    union
    {
#ifdef USE_VAAPI
	VaapiDecoder Vaapi;		///< VA-API decoder structure
#endif
#ifdef USE_VDPAU
	VdpauDecoder Vdpau;		///< vdpau decoder structure
#endif
#ifdef USE_CUVID
	CuvidDecoder Cuvid;		///< cuvid decoder structure
	NVdecDecoder NVdec;		///< nvdec decoder structure
#endif
#if defined USE_GLX || defined USE_EGL
	CpuDecoder Cpu;			///< cpu software decoder structure
#endif
    };
};

///
///	Allocate new video hw decoder.
///
///	@param stream	video stream
///
///	@returns a new initialized video hardware decoder.
///
VideoHwDecoder *VideoNewHwDecoder(VideoStream * stream)
{
    VideoHwDecoder *hw;

    VideoThreadLock();
    hw = VideoUsedModule->NewHwDecoder(stream);
    VideoThreadUnlock();

    return hw;
}

///
///	Destroy a video hw decoder.
///
///	@param hw_decoder	video hardware decoder
///
void VideoDelHwDecoder(VideoHwDecoder * hw_decoder)
{
    if (hw_decoder) {
#ifdef DEBUG
	if (!pthread_equal(pthread_self(), VideoThread)) {
	    Debug(3, "video: should only be called from inside the thread\n");
	}
#endif
	// only called from inside the thread
	//VideoThreadLock();
	VideoUsedModule->DelHwDecoder(hw_decoder);
	//VideoThreadUnlock();
    }
}

///
///	Get a free hardware decoder surface.
///
///	@param hw_decoder	video hardware decoder
///	@param video_ctx	ffmpeg video codec context
///
///	@returns the oldest free surface or invalid surface
///
unsigned VideoGetSurface(VideoHwDecoder * hw_decoder,
    const AVCodecContext * video_ctx)
{
    return VideoUsedModule->GetSurface(hw_decoder, video_ctx);
}

///
///	Release a hardware decoder surface.
///
///	@param hw_decoder	video hardware decoder
///	@param surface		surface no longer used
///
void VideoReleaseSurface(VideoHwDecoder * hw_decoder, unsigned surface)
{
    // FIXME: must be guarded against calls, after VideoExit
    VideoUsedModule->ReleaseSurface(hw_decoder, surface);
}

void VideoUnregisterSurface(VideoHwDecoder * hw_decoder)
{
    VideoUsedModule->UnregisterSurface(hw_decoder);
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param hw_decoder	video hardware decoder
///	@param video_ctx	ffmpeg video codec context
///	@param fmt		is the list of formats which are supported by
///				the codec, it is terminated by -1 as 0 is a
///				valid format, the formats are ordered by
///				quality.
///
enum AVPixelFormat Video_get_format(VideoHwDecoder * hw_decoder,
    AVCodecContext * video_ctx, const enum AVPixelFormat *fmt)
{
#ifdef DEBUG
    int ms_delay = 0;

    // FIXME: use frame time
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56,13,100)
    ms_delay = (1000 * video_ctx->time_base.num * video_ctx->ticks_per_frame)
	/ video_ctx->time_base.den;
#else
    if (video_ctx->framerate.den) {
        ms_delay = (1000 * video_ctx->framerate.num)
	    / video_ctx->framerate.den;
    }
#endif
    Debug(3, "video: ready %s %2dms/frame %dms\n",
	Timestamp2String(VideoGetClock(hw_decoder)), ms_delay,
	GetMsTicks() - VideoSwitch);
#endif

    return VideoUsedModule->get_format(hw_decoder, video_ctx, fmt);
}

///
///	Display a ffmpeg frame
///
///	@param hw_decoder	video hardware decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
void VideoRenderFrame(VideoHwDecoder * hw_decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
#if 0
    fprintf(stderr, "video: render frame pts %s closing %d\n",
	Timestamp2String(frame->pkt_pts), hw_decoder->Vdpau.Closing);
#endif
    if (frame->repeat_pict && !VideoIgnoreRepeatPict) {
	Warning(_("video: repeated pict %d found, but not handled\n"),
	    frame->repeat_pict);
    }
    VideoUsedModule->RenderFrame(hw_decoder, video_ctx, frame);
}

///
///	Get hwaccel context for ffmpeg.
///
///	FIXME: new ffmpeg supports vdpau hw context
///
///	@param hw_decoder	video hardware decoder (must be VA-API)
///
void *VideoGetHwAccelContext(VideoHwDecoder * hw_decoder)
{
    return VideoUsedModule->GetHwAccelContext(hw_decoder);
}

#ifdef USE_VDPAU
#if LIBAVUTIL_VERSION_MAJOR < 56
///
///	Draw ffmpeg vdpau render state.
///
///	@param hw_decoder	video hardware decoder
///	@param vrs		vdpau render state
///
void VideoDrawRenderState(VideoHwDecoder * hw_decoder,
    struct vdpau_render_state *vrs)
{
    if (VideoUsedModule == &VdpauModule) {
	VdpStatus status;
	uint32_t start;
	uint32_t end;
	VdpauDecoder *decoder;

	if (VdpauPreemption) {		// display preempted
	    return;
	}

	decoder = &hw_decoder->Vdpau;
	if (decoder->VideoDecoder == VDP_INVALID_HANDLE) {
	    // must be hardware decoder!
	    Debug(3, "video/vdpau: recover preemption\n");
	    status =
		VdpauDecoderCreate(VdpauDevice, decoder->Profile,
		decoder->InputWidth, decoder->InputHeight,
		decoder->SurfacesNeeded - VIDEO_SURFACES_MAX - 1,
		&decoder->VideoDecoder);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't create decoder: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    VdpauSetupOutput(decoder);
	    return;
	}

	Debug(4, "video/vdpau: decoder render to %#010x\n", vrs->surface);
	start = GetMsTicks();
	status =
	    VdpauDecoderRender(decoder->VideoDecoder, vrs->surface,
	    (VdpPictureInfo const *)&vrs->info, vrs->bitstream_buffers_used,
	    vrs->bitstream_buffers);
	end = GetMsTicks();
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: decoder rendering failed: %s\n"),
		VdpauGetErrorString(status));
	}
	if (end - start > 35) {
	    // report this
	    Info(_("video/vdpau: %s: decoder render too slow %ums\n"),
		Timestamp2String(decoder->PTS), end - start);
	}
	return;
    }
    Error(_("video/vdpau: draw render state, without vdpau enabled\n"));
}
#endif
#endif

///
///	Set video clock.
///
///	@param hw_decoder	video hardware decoder
///	@param pts		audio presentation timestamp
///
void VideoSetClock(VideoHwDecoder * hw_decoder, int64_t pts)
{
    Debug(3, "video: set clock %s\n", Timestamp2String(pts));
    if (hw_decoder) {
	VideoUsedModule->SetClock(hw_decoder, pts);
    }
}

///
///	Get video clock.
///
///	@param hw_decoder	video hardware decoder
///
///	@note this isn't monoton, decoding reorders frames, setter keeps it
///	monotonic
///
int64_t VideoGetClock(const VideoHwDecoder * hw_decoder)
{
    if (hw_decoder) {
	return VideoUsedModule->GetClock(hw_decoder);
    }
    return AV_NOPTS_VALUE;
}

///
///	Set closing stream flag.
///
///	@param hw_decoder	video hardware decoder
///
void VideoSetClosing(VideoHwDecoder * hw_decoder)
{
    Debug(3, "video: set closing\n");
    VideoUsedModule->SetClosing(hw_decoder);
    // clear clock to avoid further sync
    VideoSetClock(hw_decoder, AV_NOPTS_VALUE);
}

///
///	Reset start of frame counter.
///
///	@param hw_decoder	video hardware decoder
///
void VideoResetStart(VideoHwDecoder * hw_decoder)
{
    Debug(3, "video: reset start\n");
    VideoUsedModule->ResetStart(hw_decoder);
    // clear clock to trigger new video stream
    VideoSetClock(hw_decoder, AV_NOPTS_VALUE);
}

///
///	Set trick play speed.
///
///	@param hw_decoder	video hardware decoder
///	@param speed		trick speed (0 = normal)
///
void VideoSetTrickSpeed(VideoHwDecoder * hw_decoder, int speed)
{
    Debug(3, "video: set trick-speed %d\n", speed);
    VideoUsedModule->SetTrickSpeed(hw_decoder, speed);
}

///
///	Grab full screen image.
///
///	@param size[out]	size of allocated image
///	@param width[in,out]	width of image
///	@param height[in,out]	height of image
///
uint8_t *VideoGrab(int *size, int *width, int *height, int write_header)
{
    Debug(3, "video: grab\n");

#ifdef USE_GRAB
    if (VideoUsedModule->GrabOutput) {
	uint8_t *data;
	uint8_t *rgb;
	char buf[64];
	int i;
	int n;
	int scale_width;
	int scale_height;
	int x;
	int y;
	double src_x;
	double src_y;
	double scale_x;
	double scale_y;

	scale_width = *width;
	scale_height = *height;
	n = 0;
	if (VideoUsedModule != &NoopModule)
	    data = VideoUsedModule->GrabOutput(size, width, height);
	// try external service
	else
	    data = GrabExtService(size, width, height);

	if (data == NULL)
	    return NULL;

	if (scale_width <= 0) {
	    scale_width = *width;
	}
	if (scale_height <= 0) {
	    scale_height = *height;
	}
	// hardware didn't scale for us, use simple software scaler
	if (scale_width != *width && scale_height != *height) {
	    if (write_header) {
		n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n",
		    scale_width, scale_height);
	    }
	    rgb = malloc(scale_width * scale_height * 3 + n);
	    if (!rgb) {
		Error(_("video: out of memory\n"));
		free(data);
		return NULL;
	    }
	    *size = scale_width * scale_height * 3 + n;
	    memcpy(rgb, buf, n);	// header

	    scale_x = (double)*width / scale_width;
	    scale_y = (double)*height / scale_height;

	    src_y = 0.0;
	    for (y = 0; y < scale_height; y++) {
		int o;

		src_x = 0.0;
		o = (int)src_y **width;

		for (x = 0; x < scale_width; x++) {
		    i = 4 * (o + (int)src_x);

		    rgb[n + (x + y * scale_width) * 3 + 0] = data[i + 2];
		    rgb[n + (x + y * scale_width) * 3 + 1] = data[i + 1];
		    rgb[n + (x + y * scale_width) * 3 + 2] = data[i + 0];

		    src_x += scale_x;
		}

		src_y += scale_y;
	    }

	    *width = scale_width;
	    *height = scale_height;

	    // grabed image of correct size convert BGRA -> RGB
	} else {
	    if (write_header) {
		n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n", *width,
		    *height);
	    }
	    rgb = malloc(*width * *height * 3 + n);
	    if (!rgb) {
		Error(_("video: out of memory\n"));
		free(data);
		return NULL;
	    }
	    memcpy(rgb, buf, n);	// header

	    for (i = 0; i < *size / 4; ++i) {	// convert bgra -> rgb
		rgb[n + i * 3 + 0] = data[i * 4 + 2];
		rgb[n + i * 3 + 1] = data[i * 4 + 1];
		rgb[n + i * 3 + 2] = data[i * 4 + 0];
	    }

	    *size = *width * *height * 3 + n;
	}
	free(data);

	return rgb;
    } else
#endif
    {
	Warning(_("softhddev: grab unsupported\n"));
    }

    (void)size;
    (void)width;
    (void)height;
    (void)write_header;
    return NULL;
}

///
///	Grab image service.
///
///	@param size[out]	size of allocated image
///	@param width[in,out]	width of image
///	@param height[in,out]	height of image
///
uint8_t *VideoGrabService(int *size, int *width, int *height)
{
    Debug(3, "video: grab service\n");

#ifdef USE_GRAB
    if (VideoUsedModule->GrabOutput) {
	if (VideoUsedModule != &NoopModule)
	    return VideoUsedModule->GrabOutput(size, width, height);
	else
	    return GrabExtService(size, width, height);
    } else
#endif
    {
	Warning(_("softhddev: grab unsupported\n"));
    }

    (void)size;
    (void)width;
    (void)height;
    return NULL;
}

///
///	Get decoder statistics.
///
///	@param hw_decoder	video hardware decoder
///	@param[out] missed	missed frames
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void VideoGetStats(VideoHwDecoder * hw_decoder, int *missed, int *duped,
    int *dropped, int *counter, int *dec)
{
    VideoUsedModule->GetStats(hw_decoder, missed, duped, dropped, counter, dec);
}

///
///	Get decoder video stream size.
///
///	@param hw_decoder	video hardware decoder
///	@param[out] width	video stream width
///	@param[out] height	video stream height
///	@param[out] aspect_num	video stream aspect numerator
///	@param[out] aspect_den	video stream aspect denominator
///
void VideoGetVideoSize(VideoHwDecoder * hw_decoder, int *width, int *height,
    int *aspect_num, int *aspect_den)
{
    *width = 1920;
    *height = 1080;
    *aspect_num = 16;
    *aspect_den = 9;
    // FIXME: test to check if working, than make module function
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	*width = hw_decoder->Vdpau.InputWidth;
	*height = hw_decoder->Vdpau.InputHeight;
	av_reduce(aspect_num, aspect_den,
	    hw_decoder->Vdpau.InputWidth * hw_decoder->Vdpau.InputAspect.num,
	    hw_decoder->Vdpau.InputHeight * hw_decoder->Vdpau.InputAspect.den,
	    1024 * 1024);
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	*width = hw_decoder->Vaapi.InputWidth;
	*height = hw_decoder->Vaapi.InputHeight;
	av_reduce(aspect_num, aspect_den,
	    hw_decoder->Vaapi.InputWidth * hw_decoder->Vaapi.InputAspect.num,
	    hw_decoder->Vaapi.InputHeight * hw_decoder->Vaapi.InputAspect.den,
	    1024 * 1024);
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
	*width = hw_decoder->Cuvid.InputWidth;
	*height = hw_decoder->Cuvid.InputHeight;
	av_reduce(aspect_num, aspect_den,
	    hw_decoder->Cuvid.InputWidth * hw_decoder->Cuvid.InputAspect.num,
	    hw_decoder->Cuvid.InputHeight * hw_decoder->Cuvid.InputAspect.den,
	    1024 * 1024);
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
	*width = hw_decoder->NVdec.InputWidth;
	*height = hw_decoder->NVdec.InputHeight;
	av_reduce(aspect_num, aspect_den,
	    hw_decoder->NVdec.InputWidth * hw_decoder->NVdec.InputAspect.num,
	    hw_decoder->NVdec.InputHeight * hw_decoder->NVdec.InputAspect.den,
	    1024 * 1024);
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
	*width = hw_decoder->Cpu.InputWidth;
	*height = hw_decoder->Cpu.InputHeight;
	av_reduce(aspect_num, aspect_den,
	    hw_decoder->Cpu.InputWidth * hw_decoder->Cpu.InputAspect.num,
	    hw_decoder->Cpu.InputHeight * hw_decoder->Cpu.InputAspect.den,
	    1024 * 1024);
    }
#endif
}

#ifdef USE_SCREENSAVER

//----------------------------------------------------------------------------
//	DPMS / Screensaver
//----------------------------------------------------------------------------

///
///	Suspend X11 screen saver.
///
///	@param connection	X11 connection to enable/disable screensaver
///	@param suspend		True suspend screensaver,
///				false enable screensaver
///
static void X11SuspendScreenSaver(xcb_connection_t * connection, int suspend)
{
    const xcb_query_extension_reply_t *query_extension_reply;

    query_extension_reply =
	xcb_get_extension_data(connection, &xcb_screensaver_id);
    if (query_extension_reply && query_extension_reply->present) {
	xcb_screensaver_query_version_cookie_t cookie;
	xcb_screensaver_query_version_reply_t *reply;

	Debug(3, "video: screen saver extension present\n");

	cookie =
	    xcb_screensaver_query_version_unchecked(connection,
	    XCB_SCREENSAVER_MAJOR_VERSION, XCB_SCREENSAVER_MINOR_VERSION);
	reply = xcb_screensaver_query_version_reply(connection, cookie, NULL);
	if (reply
	    && (reply->server_major_version >= XCB_SCREENSAVER_MAJOR_VERSION)
	    && (reply->server_minor_version >= XCB_SCREENSAVER_MINOR_VERSION)
	    ) {
	    xcb_screensaver_suspend(connection, suspend);
	}
	free(reply);
    }
}

///
///	DPMS (Display Power Management Signaling) extension available.
///
///	@param connection	X11 connection to check for DPMS
///
static int X11HaveDPMS(xcb_connection_t * connection)
{
    static int have_dpms = -1;
    const xcb_query_extension_reply_t *query_extension_reply;

    if (have_dpms != -1) {		// already checked
	return have_dpms;
    }

    have_dpms = 0;
    query_extension_reply = xcb_get_extension_data(connection, &xcb_dpms_id);
    if (query_extension_reply && query_extension_reply->present) {
	xcb_dpms_get_version_cookie_t cookie;
	xcb_dpms_get_version_reply_t *reply;
	int major;
	int minor;

	Debug(3, "video: dpms extension present\n");

	cookie =
	    xcb_dpms_get_version_unchecked(connection, XCB_DPMS_MAJOR_VERSION,
	    XCB_DPMS_MINOR_VERSION);
	reply = xcb_dpms_get_version_reply(connection, cookie, NULL);
	// use locals to avoid gcc warning
	major = XCB_DPMS_MAJOR_VERSION;
	minor = XCB_DPMS_MINOR_VERSION;
	if (reply && (reply->server_major_version >= major)
	    && (reply->server_minor_version >= minor)
	    ) {
	    have_dpms = 1;
	}
	free(reply);
    }
    return have_dpms;
}

///
///	Disable DPMS (Display Power Management Signaling)
///
///	@param connection	X11 connection to disable DPMS
///
static void X11DPMSDisable(xcb_connection_t * connection)
{
    if (X11HaveDPMS(connection)) {
	xcb_dpms_info_cookie_t cookie;
	xcb_dpms_info_reply_t *reply;

	cookie = xcb_dpms_info_unchecked(connection);
	reply = xcb_dpms_info_reply(connection, cookie, NULL);
	if (reply) {
	    if (reply->state) {
		Debug(3, "video: dpms was enabled\n");
		xcb_dpms_disable(connection);	// monitor powersave off
	    }
	    free(reply);
	}
	DPMSDisabled = 1;
    }
}

///
///	Reenable DPMS (Display Power Management Signaling)
///
///	@param connection	X11 connection to enable DPMS
///
static void X11DPMSReenable(xcb_connection_t * connection)
{
    if (DPMSDisabled && X11HaveDPMS(connection)) {
	xcb_dpms_enable(connection);	// monitor powersave on
	xcb_dpms_force_level(connection, XCB_DPMS_DPMS_MODE_ON);
	DPMSDisabled = 0;
    }
}

#else

    /// dummy function: Suspend X11 screen saver.
#define X11SuspendScreenSaver(connection, suspend)
    /// dummy function: Disable X11 DPMS.
#define X11DPMSDisable(connection)
    /// dummy function: Reenable X11 DPMS.
#define X11DPMSReenable(connection)

#endif

//----------------------------------------------------------------------------
//	Setup
//----------------------------------------------------------------------------

///
///	Create main window.
///
///	@param parent	parent of new window
///	@param visual	visual of parent
///	@param depth	depth of parent
///
static void VideoCreateWindow(xcb_window_t parent, xcb_visualid_t visual,
    uint8_t depth)
{
    uint32_t values[4];
    xcb_intern_atom_reply_t *reply;
    xcb_pixmap_t pixmap;
    xcb_cursor_t cursor;
    xcb_randr_get_screen_resources_current_reply_t *randr_reply;
    xcb_timestamp_t timestamp;
    xcb_randr_output_t *randr_outputs;
    int len;

    const uint32_t coords[2] = { VideoWindowX, VideoWindowY };

    Debug(3, "video: visual %#0x depth %d\n", visual, depth);

    // Color map
    VideoColormap = xcb_generate_id(Connection);
    xcb_create_colormap(Connection, XCB_COLORMAP_ALLOC_NONE, VideoColormap,
	parent, visual);

    values[0] = 0;
    values[1] = 0;
    values[2] =
	XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
	XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
	XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_EXPOSURE |
	XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    values[3] = VideoColormap;
    VideoWindow = xcb_generate_id(Connection);
    xcb_create_window(Connection, depth, VideoWindow, parent, 0,
	0, VideoWindowWidth, VideoWindowHeight, 0,
	XCB_WINDOW_CLASS_INPUT_OUTPUT, visual,
	XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK |
	XCB_CW_COLORMAP, values);

    // define only available with xcb-utils-0.3.8
#ifdef XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS
    // FIXME: utf _NET_WM_NAME
    xcb_icccm_set_wm_name(Connection, VideoWindow, XCB_ATOM_STRING, 8,
	sizeof("softhddevice") - 1, "softhddevice");
    xcb_icccm_set_wm_icon_name(Connection, VideoWindow, XCB_ATOM_STRING, 8,
	sizeof("softhddevice") - 1, "softhddevice");
#endif
    // define only available with xcb-utils-0.3.6
#ifdef XCB_NUM_WM_HINTS_ELEMENTS
    // FIXME: utf _NET_WM_NAME
    xcb_set_wm_name(Connection, VideoWindow, XCB_ATOM_STRING,
	sizeof("softhddevice") - 1, "softhddevice");
    xcb_set_wm_icon_name(Connection, VideoWindow, XCB_ATOM_STRING,
	sizeof("softhddevice") - 1, "softhddevice");
#endif

#if 1
    // register interest in the delete window message
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0,
		    sizeof("WM_DELETE_WINDOW") - 1, "WM_DELETE_WINDOW"),
		NULL))) {
	WmDeleteWindowAtom = reply->atom;
	free(reply);
	if ((reply =
		xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection,
			0, sizeof("WM_PROTOCOLS") - 1, "WM_PROTOCOLS"),
		    NULL))) {
#ifdef XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS
	    xcb_icccm_set_wm_protocols(Connection, VideoWindow, reply->atom, 1,
		&WmDeleteWindowAtom);
#endif
#ifdef XCB_NUM_WM_HINTS_ELEMENTS
	    xcb_set_wm_protocols(Connection, reply->atom, VideoWindow, 1,
		&WmDeleteWindowAtom);
#endif
	    free(reply);
	}
    }
#endif
    //
    //	prepare fullscreen.
    //
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0,
		    sizeof("_NET_WM_STATE") - 1, "_NET_WM_STATE"), NULL))) {
	NetWmState = reply->atom;
	free(reply);
    }
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0,
		    sizeof("_NET_WM_STATE_FULLSCREEN") - 1,
		    "_NET_WM_STATE_FULLSCREEN"), NULL))) {
	NetWmStateFullscreen = reply->atom;
	free(reply);
    }

    xcb_map_window(Connection, VideoWindow);

    // For some WMs the X/Y coordinates are not taken into account when passed to xcb_create_window.
    // As a workaround we must manually set the coordinates after mapping the window.
    xcb_configure_window (Connection, VideoWindow, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coords);

    //
    //	hide cursor
    //
    pixmap = xcb_generate_id(Connection);
    xcb_create_pixmap(Connection, 1, pixmap, parent, 1, 1);
    cursor = xcb_generate_id(Connection);
    xcb_create_cursor(Connection, cursor, pixmap, pixmap, 0, 0, 0, 0, 0, 0, 1,
	1);

    values[0] = cursor;
    xcb_change_window_attributes(Connection, VideoWindow, XCB_CW_CURSOR,
	values);
    VideoCursorPixmap = pixmap;
    VideoBlankCursor = cursor;
    VideoBlankTick = 0;

    //get screen size for multimonitor systems
    randr_reply = xcb_randr_get_screen_resources_current_reply(
        Connection, xcb_randr_get_screen_resources_current(Connection, VideoWindow), NULL);

    timestamp = randr_reply->config_timestamp;
    len = xcb_randr_get_screen_resources_current_outputs_length(randr_reply);
    randr_outputs = xcb_randr_get_screen_resources_current_outputs(randr_reply);

    for (int i = 0; i < len; i++) {
        xcb_randr_get_crtc_info_reply_t *crtc;
        xcb_randr_get_output_info_reply_t *output = xcb_randr_get_output_info_reply(
            Connection, xcb_randr_get_output_info(Connection, randr_outputs[i], timestamp), NULL);
        if (output == NULL)
            continue;

        if (output->crtc == XCB_NONE || output->connection == XCB_RANDR_CONNECTION_DISCONNECTED)
            continue;

        crtc = xcb_randr_get_crtc_info_reply(Connection,
            xcb_randr_get_crtc_info(Connection, output->crtc, timestamp), NULL);
        Debug(3, "video: crtc = %d | x = %d | y = %d | w = %d | h = %d\n", i, crtc->x, crtc->y, crtc->width, crtc->height);

        //set video screen size to crtc size where video window was created
        if (VideoWindowX > crtc->x && VideoWindowX < crtc->x + crtc->width) {
            VideoScreenWidth = crtc->width;
            VideoScreenHeight = crtc->height;
        }
        free(crtc);
        free(output);
    }

    free(randr_reply);
}

///
///	Set video device.
///
///	Currently this only choose the driver.
///
void VideoSetDevice(const char *device)
{
    VideoDriverName = device;
}

///
///	Get video driver name.
///
///	@returns name of current video driver.
///
const char *VideoGetDriverName(void)
{
    if (VideoUsedModule) {
	return VideoUsedModule->Name;
    }
    return "";
}

///
///     Get used video driver.
///
int VideoIsDriverVdpau(void)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	return 1;
    }
#endif
   return 0;
}

int VideoIsDriverVaapi(void)
{
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	return 1;
    }
#endif
   return 0;
}

int VideoIsDriverCuvid(void)
{
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
	return 1;
    }
#endif
   return 0;
}

int VideoIsDriverNVdec(void)
{
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
	return 1;
    }
#endif
   return 0;
}

int VideoIsDriverCpu(void)
{
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
	return 1;
    }
#endif
   return 0;
}

///
///	Set video geometry.
///
///	@param geometry	 [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]
///
int VideoSetGeometry(const char *geometry)
{
    XParseGeometry(geometry, &VideoWindowX, &VideoWindowY, &VideoWindowWidth,
	&VideoWindowHeight);

    return 0;
}

///
///	Get video geometry.
///
///	@param geometry	 [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]
///
const char *VideoGetGeometry(void)
{
    snprintf(VideoGeometry, sizeof(VideoGeometry), "%dx%d%+d%+d", VideoWindowWidth, VideoWindowHeight, VideoWindowX, VideoWindowY);

    return VideoGeometry;
}

///
///	Set 60hz display mode.
///
///	Pull up 50 Hz video for 60 Hz display.
///
///	@param onoff	enable / disable the 60 Hz mode.
///
void VideoSet60HzMode(int onoff)
{
    Video60HzMode = onoff;
}

///
///	Set soft start audio/video sync.
///
///	@param onoff	enable / disable the soft start sync.
///
void VideoSetSoftStartSync(int onoff)
{
    VideoSoftStartSync = onoff;
}

///
///	Set show black picture during channel switch.
///
///	@param onoff	enable / disable black picture.
///
void VideoSetBlackPicture(int onoff)
{
    VideoShowBlackPicture = onoff;
}

#ifdef USE_VAAPI
///
///	Vaapi helper to set various video params (brightness, contrast etc.)
///
///	@param buf	Pointer to value to set
///	@param Index	which part of the buffer to touch
///	@param value	new value to set
///	@return 	status whether successful
///
static VAStatus VaapiVideoSetColorbalance(VABufferID* buf, int Index, float value)
{
   VAStatus va_status;
   VAProcFilterParameterBufferColorBalance *cbal_param;

   if (!buf || Index < 0)
	return VA_STATUS_ERROR_INVALID_PARAMETER;

   va_status = vaMapBuffer(VaDisplay, *buf, (void**)&cbal_param);
   if (va_status != VA_STATUS_SUCCESS)
       return va_status;

   /* Assuming here that the type is set before and does not need to be modified */
   cbal_param[Index].value = value;

   vaUnmapBuffer(VaDisplay, *buf);

   return va_status;
}
#endif

///
///	Set brightness adjustment.
///
///	@param brightness	between min and max.
///
void VideoSetBrightness(int brightness)
{
    // FIXME: test to check if working, than make module function
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	VdpauDecoders[0]->Procamp.brightness = VideoConfigClamp(&VdpauConfigBrightness, brightness) *
					       VdpauConfigBrightness.scale;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	VaapiVideoSetColorbalance(VaapiDecoders[0]->vpp_cbal_buf, VaapiDecoders[0]->vpp_brightness_idx,
				  VideoConfigClamp(&VaapiConfigBrightness, brightness) *
				  VaapiConfigBrightness.scale);
    }
#endif
#ifdef USE_CUVID
//    if (VideoUsedModule == &CuvidModule) {
//	CuvidDecoders[0]->VOparams.brightness = VideoConfigClamp(&CuvidConfigBrightness, brightness) *
//					       CuvidConfigBrightness.scale;
//    }
#endif
//    if (VideoUsedModule == &CpuGlxModule) {
//	CpuDecoders[0]->VOparams.brightness = VideoConfigClamp(&CpuConfigBrightness, brightness) *
//					       CpuConfigBrightness.scale;
//    }
}

///
///     Get brightness configurations.
///
int VideoGetBrightnessConfig(int *minvalue, int *defvalue, int *maxvalue)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	*minvalue = VdpauConfigBrightness.min_value;
	*defvalue = VdpauConfigBrightness.def_value;
	*maxvalue = VdpauConfigBrightness.max_value;
	return VdpauConfigBrightness.active;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	*minvalue = VaapiConfigBrightness.min_value;
	*defvalue = VaapiConfigBrightness.def_value;
	*maxvalue = VaapiConfigBrightness.max_value;
	return VaapiConfigBrightness.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
	*minvalue = CuvidConfigBrightness.min_value;
	*defvalue = CuvidConfigBrightness.def_value;
	*maxvalue = CuvidConfigBrightness.max_value;
	return CuvidConfigBrightness.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
	*minvalue = NVdecConfigBrightness.min_value;
	*defvalue = NVdecConfigBrightness.def_value;
	*maxvalue = NVdecConfigBrightness.max_value;
	return NVdecConfigBrightness.active;
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
	*minvalue = CpuConfigBrightness.min_value;
	*defvalue = CpuConfigBrightness.def_value;
	*maxvalue = CpuConfigBrightness.max_value;
	return CpuConfigBrightness.active;
    }
#endif
    return 0;
}

///
///	Set contrast adjustment.
///
///	@param contrast		between min and max.
///
void VideoSetContrast(int contrast)
{
    // FIXME: test to check if working, than make module function
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	VdpauDecoders[0]->Procamp.contrast = VideoConfigClamp(&VdpauConfigContrast, contrast) *
					     VdpauConfigContrast.scale;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	VaapiVideoSetColorbalance(VaapiDecoders[0]->vpp_cbal_buf, VaapiDecoders[0]->vpp_contrast_idx,
				  VideoConfigClamp(&VaapiConfigContrast, contrast) *
				  VaapiConfigContrast.scale);
    }
#endif
#ifdef USE_CUVID
//    if (VideoUsedModule == &CuvidModule) {
//	CuvidDecoders[0]->Procamp.contrast = VideoConfigClamp(&CuvidConfigContrast, contrast) *
//					     CuvidConfigContrast.scale;
//    }
#endif
//    if (VideoUsedModule == &CpuGlxModule) {
//	CpuDecoders[0]->Procamp.contrast = VideoConfigClamp(&CpuConfigContrast, contrast) *
//					     CpuConfigContrast.scale;
//    }
}

///
///     Get contrast configurations.
///
int VideoGetContrastConfig(int *minvalue, int *defvalue, int *maxvalue)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	*minvalue = VdpauConfigContrast.min_value;
	*defvalue = VdpauConfigContrast.def_value;
	*maxvalue = VdpauConfigContrast.max_value;
	return VdpauConfigContrast.active;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	*minvalue = VaapiConfigContrast.min_value;
	*defvalue = VaapiConfigContrast.def_value;
	*maxvalue = VaapiConfigContrast.max_value;
	return VaapiConfigContrast.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
	*minvalue = CuvidConfigContrast.min_value;
	*defvalue = CuvidConfigContrast.def_value;
	*maxvalue = CuvidConfigContrast.max_value;
	return CuvidConfigContrast.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
	*minvalue = NVdecConfigContrast.min_value;
	*defvalue = NVdecConfigContrast.def_value;
	*maxvalue = NVdecConfigContrast.max_value;
	return NVdecConfigContrast.active;
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
	*minvalue = CpuConfigContrast.min_value;
	*defvalue = CpuConfigContrast.def_value;
	*maxvalue = CpuConfigContrast.max_value;
	return CpuConfigContrast.active;
    }
#endif
    return 0;
}

///
///	Set saturation adjustment.
///
///	@param saturation	between min and max.
///
void VideoSetSaturation(int saturation)
{
    // FIXME: test to check if working, than make module function
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	VdpauDecoders[0]->Procamp.saturation = VideoConfigClamp(&VdpauConfigSaturation, saturation) *
					       VdpauConfigSaturation.scale;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	VaapiVideoSetColorbalance(VaapiDecoders[0]->vpp_cbal_buf, VaapiDecoders[0]->vpp_saturation_idx,
				  VideoConfigClamp(&VaapiConfigSaturation, saturation) *
				  VaapiConfigSaturation.scale);
    }
#endif
#ifdef USE_CUVID
//    if (VideoUsedModule == &CuvidModule) {
//	CuvidDecoders[0]->Procamp.saturation = VideoConfigClamp(&CuvidConfigSaturation, saturation) *
//					       CuvidConfigSaturation.scale;
//    }
#endif
//    if (VideoUsedModule == &CpuGlxModule) {
//	CpuDecoders[0]->Procamp.saturation = VideoConfigClamp(&CpuConfigSaturation, saturation) *
//					       CpuConfigSaturation.scale;
//    }
}

///
///     Get saturation configurations.
///
int VideoGetSaturationConfig(int *minvalue, int *defvalue, int *maxvalue)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	*minvalue = VdpauConfigSaturation.min_value;
	*defvalue = VdpauConfigSaturation.def_value;
	*maxvalue = VdpauConfigSaturation.max_value;
	return VdpauConfigSaturation.active;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	*minvalue = VaapiConfigSaturation.min_value;
	*defvalue = VaapiConfigSaturation.def_value;
	*maxvalue = VaapiConfigSaturation.max_value;
	return VaapiConfigSaturation.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
	*minvalue = CuvidConfigSaturation.min_value;
	*defvalue = CuvidConfigSaturation.def_value;
	*maxvalue = CuvidConfigSaturation.max_value;
	return CuvidConfigSaturation.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
	*minvalue = NVdecConfigSaturation.min_value;
	*defvalue = NVdecConfigSaturation.def_value;
	*maxvalue = NVdecConfigSaturation.max_value;
	return NVdecConfigSaturation.active;
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
	*minvalue = CpuConfigSaturation.min_value;
	*defvalue = CpuConfigSaturation.def_value;
	*maxvalue = CpuConfigSaturation.max_value;
	return CpuConfigSaturation.active;
    }
#endif
    return 0;
}

///
///	Set hue adjustment.
///
///	@param hue	between min and max.
///
void VideoSetHue(int hue)
{
    // FIXME: test to check if working, than make module function
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	VdpauDecoders[0]->Procamp.hue = VideoConfigClamp(&VdpauConfigHue, hue) *
					VdpauConfigHue.scale;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	VaapiVideoSetColorbalance(VaapiDecoders[0]->vpp_cbal_buf, VaapiDecoders[0]->vpp_hue_idx,
				  VideoConfigClamp(&VaapiConfigHue, hue) * VaapiConfigHue.scale);
    }
#endif
#ifdef USE_CUVID
//    if (VideoUsedModule == &CuvidModule) {
//	CuvidDecoders[0]->Procamp.hue = VideoConfigClamp(&CuvidConfigHue, hue) *
//					CuvidConfigHue.scale;
//    }
#endif
//    if (VideoUsedModule == &CpuGlxModule) {
//	CpuDecoders[0]->Procamp.hue = VideoConfigClamp(&CpuConfigHue, hue) *
//					CpuConfigHue.scale;
//    }
}

///
///     Get hue configurations.
///
int VideoGetHueConfig(int *minvalue, int *defvalue, int *maxvalue)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	*minvalue = VdpauConfigHue.min_value;
	*defvalue = VdpauConfigHue.def_value;
	*maxvalue = VdpauConfigHue.max_value;
	return VdpauConfigHue.active;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	*minvalue = VaapiConfigHue.min_value;
	*defvalue = VaapiConfigHue.def_value;
	*maxvalue = VaapiConfigHue.max_value;
	return VaapiConfigHue.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
	*minvalue = CuvidConfigHue.min_value;
	*defvalue = CuvidConfigHue.def_value;
	*maxvalue = CuvidConfigHue.max_value;
	return CuvidConfigHue.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
	*minvalue = NVdecConfigHue.min_value;
	*defvalue = NVdecConfigHue.def_value;
	*maxvalue = NVdecConfigHue.max_value;
	return NVdecConfigHue.active;
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
	*minvalue = CpuConfigHue.min_value;
	*defvalue = CpuConfigHue.def_value;
	*maxvalue = CpuConfigHue.max_value;
	return CpuConfigHue.active;
    }
#endif
    return 0;
}

///
///     Set skin tone enhancement.
///
///     @param stde    between min and max.
///
void VideoSetSkinToneEnhancement(int stde)
{
    // FIXME: test to check if working, than make module function
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	VideoSkinToneEnhancement = VideoConfigClamp(&VdpauConfigStde, stde);
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	VideoSkinToneEnhancement = VideoConfigClamp(&VaapiConfigStde, stde);
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
	VideoSkinToneEnhancement = VideoConfigClamp(&CuvidConfigStde, stde);
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
	VideoSkinToneEnhancement = VideoConfigClamp(&NVdecConfigStde, stde);
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
	VideoSkinToneEnhancement = VideoConfigClamp(&CpuConfigStde, stde);
    }
#endif
    VideoSurfaceModesChanged = 1;
}

///
///     Get skin tone enhancement configurations.
///
int VideoGetSkinToneEnhancementConfig(int *minvalue, int *defvalue, int *maxvalue)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
        *minvalue = VdpauConfigStde.min_value;
        *defvalue = VdpauConfigStde.def_value;
        *maxvalue = VdpauConfigStde.max_value;
        return VdpauConfigStde.active;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
        *minvalue = VaapiConfigStde.min_value;
        *defvalue = VaapiConfigStde.def_value;
        *maxvalue = VaapiConfigStde.max_value;
        return VaapiConfigStde.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
        *minvalue = CuvidConfigStde.min_value;
        *defvalue = CuvidConfigStde.def_value;
        *maxvalue = CuvidConfigStde.max_value;
        return CuvidConfigStde.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
        *minvalue = NVdecConfigStde.min_value;
        *defvalue = NVdecConfigStde.def_value;
        *maxvalue = NVdecConfigStde.max_value;
        return NVdecConfigStde.active;
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
        *minvalue = CpuConfigStde.min_value;
        *defvalue = CpuConfigStde.def_value;
        *maxvalue = CpuConfigStde.max_value;
        return CpuConfigStde.active;
    }
#endif
    return 0;
}

///
///	Set video output position.
///
///	@param hw_decoder	video hardware decoder
///	@param x		video output x coordinate OSD relative
///	@param y		video output y coordinate OSD relative
///	@param width		video output width
///	@param height		video output height
///
void VideoSetOutputPosition(VideoHwDecoder * hw_decoder, int x, int y,
    int width, int height)
{
    int sx = x, sy = y, swidth = width, sheight = height;

    if (!OsdWidth || !OsdHeight) {
	return;
    }
    if (!width || !height) {
	// restore full size
	width = VideoWindowWidth;
	height = VideoWindowHeight;
    } else {
	// convert OSD coordinates to window coordinates
	x = (x * VideoWindowWidth) / OsdWidth;
	width = (width * VideoWindowWidth) / OsdWidth;
	y = (y * VideoWindowHeight) / OsdHeight;
	height = (height * VideoWindowHeight) / OsdHeight;
    }

    // FIXME: add function to module class
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	// check values to be able to avoid
	// interfering with the video thread if possible

	if (hw_decoder->Vdpau.InputHeight == 1088) height += 8 * hw_decoder->Vdpau.VideoHeight / 1080; //fix 1080i/1088i
	if (x == hw_decoder->Vdpau.VideoX && y == hw_decoder->Vdpau.VideoY
	    && width == hw_decoder->Vdpau.VideoWidth
	    && height == hw_decoder->Vdpau.VideoHeight) {
	    // not necessary...
	    return;
	}
	VideoThreadLock();
	//store scale for window mode
	hw_decoder->Vdpau.ScaleX = sx;
	hw_decoder->Vdpau.ScaleY = sy;
	hw_decoder->Vdpau.ScaleWidth = swidth;
	hw_decoder->Vdpau.ScaleHeight = sheight;

	VdpauSetOutputPosition(&hw_decoder->Vdpau, x, y, width, height);
	VdpauUpdateOutput(&hw_decoder->Vdpau);
	VideoThreadUnlock();
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
        // check values to be able to avoid
        // interfering with the video thread if possible

        if(!((char*)hw_decoder->Vaapi.Stream)[0] && (!hw_decoder->Vaapi.InputWidth || !hw_decoder->Vaapi.InputHeight)) return; //no scale if no video and not pip
        if (x == hw_decoder->Vaapi.VideoX && y == hw_decoder->Vaapi.VideoY
            && width == hw_decoder->Vaapi.VideoWidth
            && height == hw_decoder->Vaapi.VideoHeight) {
            // not necessary...
            return;
        }
        VideoThreadLock();
	//store scale for window mode
	hw_decoder->Vaapi.ScaleX = sx;
	hw_decoder->Vaapi.ScaleY = sy;
	hw_decoder->Vaapi.ScaleWidth = swidth;
	hw_decoder->Vaapi.ScaleHeight = sheight;

        VaapiSetOutputPosition(&hw_decoder->Vaapi, x, y, width, height);
        VaapiUpdateOutput(&hw_decoder->Vaapi);
        VideoThreadUnlock();
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
	// check values to be able to avoid
	// interfering with the video thread if possible

	if (x == hw_decoder->Cuvid.VideoX && y == hw_decoder->Cuvid.VideoY
	    && width == hw_decoder->Cuvid.VideoWidth
	    && height == hw_decoder->Cuvid.VideoHeight) {
	    // not necessary...
	    return;
	}
	VideoThreadLock();
	//store scale for window mode
	hw_decoder->Cuvid.ScaleX = sx;
	hw_decoder->Cuvid.ScaleY = sy;
	hw_decoder->Cuvid.ScaleWidth = swidth;
	hw_decoder->Cuvid.ScaleHeight = sheight;

	CuvidSetOutputPosition(&hw_decoder->Cuvid, x, y, width, height);
	CuvidUpdateOutput(&hw_decoder->Cuvid);
	VideoThreadUnlock();
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
	// check values to be able to avoid
	// interfering with the video thread if possible

	if (x == hw_decoder->NVdec.VideoX && y == hw_decoder->NVdec.VideoY
	    && width == hw_decoder->NVdec.VideoWidth
	    && height == hw_decoder->NVdec.VideoHeight) {
	    // not necessary...
	    return;
	}
	VideoThreadLock();
	//store scale for window mode
	hw_decoder->NVdec.ScaleX = sx;
	hw_decoder->NVdec.ScaleY = sy;
	hw_decoder->NVdec.ScaleWidth = swidth;
	hw_decoder->NVdec.ScaleHeight = sheight;

	NVdecSetOutputPosition(&hw_decoder->NVdec, x, y, width, height);
	NVdecUpdateOutput(&hw_decoder->NVdec);
	VideoThreadUnlock();
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
	// check values to be able to avoid
	// interfering with the video thread if possible

	if (x == hw_decoder->Cpu.VideoX && y == hw_decoder->Cpu.VideoY
	    && width == hw_decoder->Cpu.VideoWidth
	    && height == hw_decoder->Cpu.VideoHeight) {
	    // not necessary...
	    return;
	}
	VideoThreadLock();
	//store scale for window mode
	hw_decoder->Cpu.ScaleX = sx;
	hw_decoder->Cpu.ScaleY = sy;
	hw_decoder->Cpu.ScaleWidth = swidth;
	hw_decoder->Cpu.ScaleHeight = sheight;

	CpuSetOutputPosition(&hw_decoder->Cpu, x, y, width, height);
	CpuUpdateOutput(&hw_decoder->Cpu);
	VideoThreadUnlock();
    }
#endif
    (void)hw_decoder;
}

///
///	Set video window position.
///
///	@param x	window x coordinate
///	@param y	window y coordinate
///	@param width	window width
///	@param height	window height
///
///	@note no need to lock, only called from inside the video thread
///
void VideoSetVideoMode( __attribute__ ((unused))
    int x, __attribute__ ((unused))
    int y, int width, int height)
{
    Debug(4, "video: %s %dx%d%+d%+d\n", __FUNCTION__, width, height, x, y);

    if ((unsigned)width == VideoWindowWidth
	&& (unsigned)height == VideoWindowHeight
	&& x == VideoWindowX && y == VideoWindowY) {
	return;				// same size nothing todo
    }

    VideoThreadLock();
    VideoWindowX = x;
    VideoWindowY = y;
    VideoWindowWidth = width;
    VideoWindowHeight = height;
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();
}

///
///	Set 4:3 video display format.
///
///	@param format	video format (stretch, normal, center cut-out)
///
void VideoSet4to3DisplayFormat(int format)
{
    // convert api to internal format
    switch (format) {
	case -1:			// rotate settings
	    format = (Video4to3ZoomMode + 1) % (VideoCenterCutOut + 1);
	    break;
	case 0:			// pan&scan (we have no pan&scan)
	    format = VideoStretch;
	    break;
	case 1:			// letter box
	    format = VideoNormal;
	    break;
	case 2:			// center cut-out
	    format = VideoCenterCutOut;
	    break;
    }

    if ((unsigned)format == Video4to3ZoomMode) {
	return;				// no change, no need to lock
    }

    VideoOsdExit();
    // FIXME: must tell VDR that the OsdSize has been changed!

    VideoThreadLock();
    Video4to3ZoomMode = format;
    // FIXME: need only VideoUsedModule->UpdateOutput();
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();

    VideoOsdInit();
}

///
///	Set other video display format.
///
///	@param format	video format (stretch, normal, center cut-out)
///
void VideoSetOtherDisplayFormat(int format)
{
    // convert api to internal format
    switch (format) {
	case -1:			// rotate settings
	    format = (VideoOtherZoomMode + 1) % (VideoCenterCutOut + 1);
	    break;
	case 0:			// pan&scan (we have no pan&scan)
	    format = VideoStretch;
	    break;
	case 1:			// letter box
	    format = VideoNormal;
	    break;
	case 2:			// center cut-out
	    format = VideoCenterCutOut;
	    break;
    }

    if ((unsigned)format == VideoOtherZoomMode) {
	return;				// no change, no need to lock
    }

    VideoOsdExit();
    // FIXME: must tell VDR that the OsdSize has been changed!

    VideoThreadLock();
    VideoOtherZoomMode = format;
    // FIXME: need only VideoUsedModule->UpdateOutput();
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();

    VideoOsdInit();
}

///
///	Send fullscreen message to window.
///
///	@param onoff	-1 toggle, true turn on, false turn off
///
void VideoSetFullscreen(int onoff)
{
    if (XlibDisplay) {			// needs running connection
	xcb_client_message_event_t event;

	memset(&event, 0, sizeof(event));
	event.response_type = XCB_CLIENT_MESSAGE;
	event.format = 32;
	event.window = VideoWindow;
	event.type = NetWmState;
	if (onoff < 0) {
	    event.data.data32[0] = XCB_EWMH_WM_STATE_TOGGLE;
	} else if (onoff) {
	    event.data.data32[0] = XCB_EWMH_WM_STATE_ADD;
	} else {
	    event.data.data32[0] = XCB_EWMH_WM_STATE_REMOVE;
	}
	event.data.data32[1] = NetWmStateFullscreen;

	xcb_send_event(Connection, XCB_SEND_EVENT_DEST_POINTER_WINDOW,
	    DefaultRootWindow(XlibDisplay),
	    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
	    XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (void *)&event);
	Debug(3, "video/x11: send fullscreen message %x %x\n",
	    event.data.data32[0], event.data.data32[1]);
    }
}

///
///     Get scaling modes.
///
#ifdef USE_VDPAU
static const char *vdpau_scaling[] = {
    "Normal",      ///< VideoScalingNormal
    "Fast",        ///< VideoScalingFast
    "HighQuality", ///< VideoScalingHQ
    "Anamorphic"   ///< VideoScalingAnamorphic
};

static const char *vdpau_scaling_short[] = {
    "N",           ///< VideoScalingNormal
    "F",           ///< VideoScalingFast
    "HQ",          ///< VideoScalingHQ
    "A"            ///< VideoScalingAnamorphic
};
#endif

#ifdef USE_VAAPI
static const char *vaapi_scaling[] = {
    "Normal",      ///< VideoScalingNormal
    "Fast",        ///< VideoScalingFast
    "HighQuality"  ///< VideoScalingHQ
};

static const char *vaapi_scaling_short[] = {
    "N",           ///< VideoScalingNormal
    "F",           ///< VideoScalingFast
    "HQ"           ///< VideoScalingHQ
};
#endif

#ifdef USE_CUVID
static const char *cuvid_scaling[] = {
    "Normal",      ///< VideoScalingNormal
    "Fast",        ///< VideoScalingFast
    "HighQuality", ///< VideoScalingHQ
    "Anamorphic"   ///< VideoScalingAnamorphic
};

static const char *cuvid_scaling_short[] = {
    "N",           ///< VideoScalingNormal
    "F",           ///< VideoScalingFast
    "HQ",          ///< VideoScalingHQ
    "A"            ///< VideoScalingAnamorphic
};
#endif

#ifdef USE_CUVID
static const char *nvdec_scaling[] = {
    "Normal",      ///< VideoScalingNormal
    "Fast",        ///< VideoScalingFast
    "HighQuality", ///< VideoScalingHQ
    "Anamorphic"   ///< VideoScalingAnamorphic
};

static const char *nvdec_scaling_short[] = {
    "N",           ///< VideoScalingNormal
    "F",           ///< VideoScalingFast
    "HQ",          ///< VideoScalingHQ
    "A"            ///< VideoScalingAnamorphic
};
#endif

static const char *cpu_scaling[] = {
    "Normal",      ///< VideoScalingNormal
//    "Fast",        ///< VideoScalingFast
//    "HighQuality", ///< VideoScalingHQ
//    "Anamorphic"   ///< VideoScalingAnamorphic
};

static const char *cpu_scaling_short[] = {
    "N",           ///< VideoScalingNormal
//    "F",           ///< VideoScalingFast
//    "HQ",          ///< VideoScalingHQ
//    "A"            ///< VideoScalingAnamorphic
};


int VideoGetScalingModes(const char* **long_table, const char* **short_table)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	*long_table = vdpau_scaling;
	*short_table = vdpau_scaling_short;
	return ARRAY_ELEMS(vdpau_scaling);
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	*long_table = vaapi_scaling;
	*short_table = vaapi_scaling_short;
	return ARRAY_ELEMS(vaapi_scaling);
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
	*long_table = cuvid_scaling;
	*short_table = cuvid_scaling_short;
	return ARRAY_ELEMS(cuvid_scaling);
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
	*long_table = nvdec_scaling;
	*short_table = nvdec_scaling_short;
	return ARRAY_ELEMS(nvdec_scaling);
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
	*long_table = cpu_scaling;
	*short_table = cpu_scaling_short;
	return ARRAY_ELEMS(cpu_scaling);
    }
#endif
    return 0;
}

///
///     Get deinterlace modes.
///
#ifdef USE_VDPAU
static const char *vdpau_deinterlace[] = {
    "Bob",                ///< VideoDeinterlaceBob
    "Weave/None",         ///< VideoDeinterlaceWeave
    "Temporal",           ///< VideoDeinterlaceTemporal
    "TemporalSpatial",    ///< VideoDeinterlaceTemporalSpatial
    "Software Bob",       ///< VideoDeinterlaceSoftBob
    "Software Spatial"    ///< VideoDeinterlaceSoftSpatial
};

static const char *vdpau_deinterlace_short[] = {
    "B",                  ///< VideoDeinterlaceBob
    "W",                  ///< VideoDeinterlaceWeave
    "T",                  ///< VideoDeinterlaceTemporal
    "T+S",                ///< VideoDeinterlaceTemporalSpatial
    "S+B",                ///< VideoDeinterlaceSoftBob
    "S+S"                 ///< VideoDeinterlaceSoftSpatial
};
#endif

#ifdef USE_VAAPI
static const char *vaapi_deinterlace[] = {
    "Bob",                ///< VideoDeinterlaceBob
    "Weave/None",         ///< VideoDeinterlaceWeave
    "MotionAdaptive",     ///< VideoDeinterlaceTemporal
    "MotionCompensated"   ///< VideoDeinterlaceTemporalSpatial
};

static const char *vaapi_deinterlace_short[] = {
    "B",                  ///< VideoDeinterlaceBob
    "W",                  ///< VideoDeinterlaceWeave
    "MADI",               ///< VideoDeinterlaceTemporal
    "MCDI"                ///< VideoDeinterlaceTemporalSpatial
};
#endif

#ifdef USE_CUVID
static const char *cuvid_deinterlace[] = {
    "Bob",                ///< VideoDeinterlaceBob
    "Weave/None",         ///< VideoDeinterlaceWeave
    "Adaptive"            ///< VideoDeinterlaceTemporal
};

static const char *cuvid_deinterlace_short[] = {
    "B",                  ///< VideoDeinterlaceBob
    "W",                  ///< VideoDeinterlaceWeave
    "A"                   ///< VideoDeinterlaceTemporal
};
#endif

#ifdef USE_CUVID
static const char *nvdec_deinterlace[] = {
    "YADIF",              ///< VideoDeinterlaceBob
    "Weave/None",         ///< VideoDeinterlaceWeave
    "BWDIF"               ///< VideoDeinterlaceTemporal
};

static const char *nvdec_deinterlace_short[] = {
    "Y",                  ///< VideoDeinterlaceBob
    "W",                  ///< VideoDeinterlaceWeave
    "B"                   ///< VideoDeinterlaceTemporal
};
#endif

static const char *cpu_deinterlace[] = {
    "YADIF",              ///< VideoDeinterlaceBob
    "Weave/None",         ///< VideoDeinterlaceWeave
    "BWDIF"               ///< VideoDeinterlaceTemporal
};

static const char *cpu_deinterlace_short[] = {
    "Y",                  ///< VideoDeinterlaceBob
    "W",                  ///< VideoDeinterlaceWeave
    "B"                   ///< VideoDeinterlaceTemporal
};

int VideoGetDeinterlaceModes(const char* **long_table, const char* **short_table)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	*long_table = vdpau_deinterlace;
	*short_table = vdpau_deinterlace_short;
	return ARRAY_ELEMS(vdpau_deinterlace);
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	unsigned int len = VaapiDecoders[0]->MaxSupportedDeinterlacer;
	*long_table = vaapi_deinterlace;
	*short_table = vaapi_deinterlace_short;
	if (len > ARRAY_ELEMS(vaapi_deinterlace))
	   len = ARRAY_ELEMS(vaapi_deinterlace);
	return len;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
	*long_table = cuvid_deinterlace;
	*short_table = cuvid_deinterlace_short;
	return ARRAY_ELEMS(cuvid_deinterlace);
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
	*long_table = nvdec_deinterlace;
	*short_table = nvdec_deinterlace_short;
	return ARRAY_ELEMS(nvdec_deinterlace);
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
	*long_table = cpu_deinterlace;
	*short_table = cpu_deinterlace_short;
	return ARRAY_ELEMS(cpu_deinterlace);
    }
#endif
    return 0;
}

///
///	Set deinterlace mode.
///
void VideoSetDeinterlace(int mode[VideoResolutionMax])
{
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
     ) {
	int i;
	for (i = 0; i < VideoResolutionMax; ++i) {
            if (mode[i] > (int)VaapiDecoders[0]->MaxSupportedDeinterlacer)
		mode[i] = VaapiDecoders[0]->MaxSupportedDeinterlacer;
	}
    }
#endif
    VideoDeinterlace[0] = mode[0];
    VideoDeinterlace[1] = mode[1];
    VideoDeinterlace[2] = mode[2];
    VideoDeinterlace[3] = mode[3];
    VideoDeinterlace[4] = mode[4];
    VideoSurfaceModesChanged = 1;
}

///
///	Set skip chroma deinterlace on/off.
///
void VideoSetSkipChromaDeinterlace(int onoff[VideoResolutionMax])
{
    VideoSkipChromaDeinterlace[0] = onoff[0];
    VideoSkipChromaDeinterlace[1] = onoff[1];
    VideoSkipChromaDeinterlace[2] = onoff[2];
    VideoSkipChromaDeinterlace[3] = onoff[3];
    VideoSkipChromaDeinterlace[4] = onoff[4];
    VideoSurfaceModesChanged = 1;
}

///
///	Set inverse telecine on/off.
///
void VideoSetInverseTelecine(int onoff[VideoResolutionMax])
{
    VideoInverseTelecine[0] = onoff[0];
    VideoInverseTelecine[1] = onoff[1];
    VideoInverseTelecine[2] = onoff[2];
    VideoInverseTelecine[3] = onoff[3];
    VideoInverseTelecine[4] = onoff[4];
    VideoSurfaceModesChanged = 1;
}

///
///	Set denoise level.
///
void VideoSetDenoise(int level[VideoResolutionMax])
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	int i;
	for (i = 0; i < VideoResolutionMax; ++i) {
	    level[i] = VideoConfigClamp(&VdpauConfigDenoise, level[i]);
	}
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	int i;
	for (i = 0; i < VideoResolutionMax; ++i) {
	    level[i] = VideoConfigClamp(&VaapiConfigDenoise, level[i]);
	}
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
	int i;
	for (i = 0; i < VideoResolutionMax; ++i) {
	    level[i] = VideoConfigClamp(&CuvidConfigDenoise, level[i]);
	}
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
	int i;
	for (i = 0; i < VideoResolutionMax; ++i) {
	    level[i] = VideoConfigClamp(&NVdecConfigDenoise, level[i]);
	}
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
	int i;
	for (i = 0; i < VideoResolutionMax; ++i) {
	    level[i] = VideoConfigClamp(&CpuConfigDenoise, level[i]);
	}
    }
#endif
    VideoDenoise[0] = level[0];
    VideoDenoise[1] = level[1];
    VideoDenoise[2] = level[2];
    VideoDenoise[3] = level[3];
    VideoDenoise[4] = level[4];
    VideoSurfaceModesChanged = 1;
}

///
///     Get denoise configurations.
///
int VideoGetDenoiseConfig(int *minvalue, int *defvalue, int *maxvalue)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
        *minvalue = VdpauConfigDenoise.min_value;
        *defvalue = VdpauConfigDenoise.def_value;
        *maxvalue = VdpauConfigDenoise.max_value;
        return VdpauConfigDenoise.active;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
        *minvalue = VaapiConfigDenoise.min_value;
        *defvalue = VaapiConfigDenoise.def_value;
        *maxvalue = VaapiConfigDenoise.max_value;
        return VaapiConfigDenoise.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
        *minvalue = CuvidConfigDenoise.min_value;
        *defvalue = CuvidConfigDenoise.def_value;
        *maxvalue = CuvidConfigDenoise.max_value;
        return CuvidConfigDenoise.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
        *minvalue = NVdecConfigDenoise.min_value;
        *defvalue = NVdecConfigDenoise.def_value;
        *maxvalue = NVdecConfigDenoise.max_value;
        return NVdecConfigDenoise.active;
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
        *minvalue = CpuConfigDenoise.min_value;
        *defvalue = CpuConfigDenoise.def_value;
        *maxvalue = CpuConfigDenoise.max_value;
        return CpuConfigDenoise.active;
    }
#endif
    return 0;
}

///
///	Set sharpness level.
///
void VideoSetSharpen(int level[VideoResolutionMax])
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
	int i;
	for (i = 0; i < VideoResolutionMax; ++i) {
	    level[i] = VideoConfigClamp(&VdpauConfigSharpen, level[i]);
	}
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
	int i;
	for (i = 0; i < VideoResolutionMax; ++i) {
	    level[i] = VideoConfigClamp(&VaapiConfigSharpen, level[i]);
	}
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
	int i;
	for (i = 0; i < VideoResolutionMax; ++i) {
	    level[i] = VideoConfigClamp(&CuvidConfigSharpen, level[i]);
	}
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
	int i;
	for (i = 0; i < VideoResolutionMax; ++i) {
	    level[i] = VideoConfigClamp(&NVdecConfigSharpen, level[i]);
	}
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
	int i;
	for (i = 0; i < VideoResolutionMax; ++i) {
	    level[i] = VideoConfigClamp(&CpuConfigSharpen, level[i]);
	}
    }
#endif
    VideoSharpen[0] = level[0];
    VideoSharpen[1] = level[1];
    VideoSharpen[2] = level[2];
    VideoSharpen[3] = level[3];
    VideoSharpen[4] = level[4];
    VideoSurfaceModesChanged = 1;
}

///
///     Get sharpness configurations.
///
int VideoGetSharpenConfig(int *minvalue, int *defvalue, int *maxvalue)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule
#ifdef USE_GLX
    || VideoUsedModule == &VdpauGlxModule
#endif
    ) {
        *minvalue = VdpauConfigSharpen.min_value;
        *defvalue = VdpauConfigSharpen.def_value;
        *maxvalue = VdpauConfigSharpen.max_value;
        return VdpauConfigSharpen.active;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule
#ifdef USE_GLX
    || VideoUsedModule == &VaapiGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &VaapiEglModule
#endif
    ) {
        *minvalue = VaapiConfigSharpen.min_value;
        *defvalue = VaapiConfigSharpen.def_value;
        *maxvalue = VaapiConfigSharpen.max_value;
        return VaapiConfigSharpen.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &CuvidModule
#ifdef USE_EGL
    || VideoUsedModule == &CuvidEglModule
#endif
    ) {
        *minvalue = CuvidConfigSharpen.min_value;
        *defvalue = CuvidConfigSharpen.def_value;
        *maxvalue = CuvidConfigSharpen.max_value;
        return CuvidConfigSharpen.active;
    }
#endif
#ifdef USE_CUVID
    if (VideoUsedModule == &NVdecModule
#ifdef USE_EGL
    || VideoUsedModule == &NVdecEglModule
#endif
    ) {
        *minvalue = NVdecConfigSharpen.min_value;
        *defvalue = NVdecConfigSharpen.def_value;
        *maxvalue = NVdecConfigSharpen.max_value;
        return NVdecConfigSharpen.active;
    }
#endif
#ifdef USE_GLX
    if (VideoUsedModule == &CpuGlxModule
#endif
#ifdef USE_EGL
    || VideoUsedModule == &CpuEglModule
#endif
#if defined USE_GLX || defined USE_EGL
    ) {
        *minvalue = CpuConfigSharpen.min_value;
        *defvalue = CpuConfigSharpen.def_value;
        *maxvalue = CpuConfigSharpen.max_value;
        return CpuConfigSharpen.active;
    }
#endif
    return 0;
}

///
///	Set scaling mode.
///
///	@param mode	table with VideoResolutionMax values
///
void VideoSetScaling(int mode[VideoResolutionMax])
{
    VideoScaling[0] = mode[0];
    VideoScaling[1] = mode[1];
    VideoScaling[2] = mode[2];
    VideoScaling[3] = mode[3];
    VideoScaling[4] = mode[4];
    VideoSurfaceModesChanged = 1;
}

///
///	Set cut top and bottom.
///
///	@param pixels table with VideoResolutionMax values
///
void VideoSetCutTopBottom(int pixels[VideoResolutionMax])
{
    VideoCutTopBottom[0] = pixels[0];
    VideoCutTopBottom[1] = pixels[1];
    VideoCutTopBottom[2] = pixels[2];
    VideoCutTopBottom[3] = pixels[3];
    VideoCutTopBottom[4] = pixels[4];
    // FIXME: update output
}

///
///	Set cut left and right.
///
///	@param pixels	table with VideoResolutionMax values
///
void VideoSetCutLeftRight(int pixels[VideoResolutionMax])
{
    VideoCutLeftRight[0] = pixels[0];
    VideoCutLeftRight[1] = pixels[1];
    VideoCutLeftRight[2] = pixels[2];
    VideoCutLeftRight[3] = pixels[3];
    VideoCutLeftRight[4] = pixels[4];
    // FIXME: update output
}

///
///	Set first field ordering.
///
///	@param first	table with VideoResolutionMax values
///
void VideoSetFirstField(int first[VideoResolutionMax])
{
    VideoFirstField[0] = first[0];
    VideoFirstField[1] = first[1];
    VideoFirstField[2] = first[2];
    VideoFirstField[3] = first[3];
}
///
///	Set second field ordering.
///
///	@param second	table with VideoResolutionMax values
///
void VideoSetSecondField(int second[VideoResolutionMax])
{
    VideoSecondField[0] = second[0];
    VideoSecondField[1] = second[1];
    VideoSecondField[2] = second[2];
    VideoSecondField[3] = second[3];
}

///
///	Set studio levels.
///
///	@param onoff	flag on/off
///
void VideoSetStudioLevels(int onoff)
{
    VideoStudioLevels = onoff;
}

///
///	Set background color.
///
///	@param rgba	32 bit RGBA color.
///
void VideoSetBackground(uint32_t rgba)
{
    VideoBackground = rgba;		// saved for later start
    VideoUsedModule->SetBackground(rgba);
}

///
///	Set audio delay.
///
///	@param ms	delay in ms
///
void VideoSetAudioDelay(int ms)
{
    VideoAudioDelay = ms * 90;
}

///
///	Set auto-crop parameters.
///
void VideoSetAutoCrop(int interval, int delay, int tolerance)
{
#ifdef USE_AUTOCROP
    AutoCropInterval = interval;
    AutoCropDelay = delay;
    AutoCropTolerance = tolerance;

    VideoThreadLock();
    VideoUsedModule->ResetAutoCrop();
    VideoThreadUnlock();
#else
    (void)interval;
    (void)delay;
    (void)tolerance;
#endif
}

///
///	Set EnableDPMSatBlackScreen
///
///	Currently this only choose the driver.
///
void SetDPMSatBlackScreen(int enable)
{
#ifdef USE_SCREENSAVER
    EnableDPMSatBlackScreen = enable;
#endif
}

///
///	Set Disable ScreenSaver
///
///	Currently this only choose the driver.
///
void SetDisableScreenSaver(int disable)
{
#ifdef USE_SCREENSAVER
    DisableScreensaver = disable;
#endif
}

///
///	Raise video window.
///
int VideoRaiseWindow(void)
{
    static const uint32_t values[] = { XCB_STACK_MODE_ABOVE };

    xcb_configure_window(Connection, VideoWindow, XCB_CONFIG_WINDOW_STACK_MODE,
	values);

    return 1;
}

///
///	Initialize video output module.
///
///	@param display_name	X11 display name
///
void VideoInit(const char *display_name)
{
    int screen_nr;
    int i;
    xcb_screen_iterator_t screen_iter;
    xcb_screen_t const *screen;

    if (XlibDisplay) {			// allow multiple calls
	Debug(3, "video: x11 already setup\n");
	return;
    }
#if defined USE_GLX || defined USE_EGL
    if (!XInitThreads()) {
	Error(_("video: Can't initialize X11 thread support on '%s'\n"),
	    display_name);
    }
#endif

    // Open the connection to the X server.
    // use the DISPLAY environment variable as the default display name
    if (!display_name && !(display_name = getenv("DISPLAY"))) {
	// if no environment variable, use :0.0 as default display name
	display_name = ":0.0";
    }
    if (!(XlibDisplay = XOpenDisplay(display_name))) {
	Error(_("video: Can't connect to X11 server on '%s'\n"), display_name);
	// FIXME: we need to retry connection
	return;
    }
    // Register error handler
    XSetIOErrorHandler(VideoIOErrorHandler);

    // Convert XLIB display to XCB connection
    if (!(Connection = XGetXCBConnection(XlibDisplay))) {
	Error(_("video: Can't convert XLIB display to XCB connection\n"));
	VideoExit();
	return;
    }
    // prefetch extensions
    //xcb_prefetch_extension_data(Connection, &xcb_big_requests_id);
#ifdef xcb_USE_GLX
    xcb_prefetch_extension_data(Connection, &xcb_glx_id);
#endif
    //xcb_prefetch_extension_data(Connection, &xcb_randr_id);
#ifdef USE_SCREENSAVER
    xcb_prefetch_extension_data(Connection, &xcb_screensaver_id);
    xcb_prefetch_extension_data(Connection, &xcb_dpms_id);
#endif
    //xcb_prefetch_extension_data(Connection, &xcb_shm_id);
    //xcb_prefetch_extension_data(Connection, &xcb_xv_id);

    // Get the requested screen number
    screen_nr = DefaultScreen(XlibDisplay);
    screen_iter = xcb_setup_roots_iterator(xcb_get_setup(Connection));
    for (i = 0; i < screen_nr; ++i) {
	xcb_screen_next(&screen_iter);
    }
    screen = screen_iter.data;
    VideoScreen = screen;

    VideoScreenWidth = screen->width_in_pixels;
    VideoScreenHeight = screen->height_in_pixels;

    //
    //	Default window size
    //
    if (!VideoWindowHeight) {
	if (VideoWindowWidth) {
	    VideoWindowHeight = (VideoWindowWidth * 9) / 16;
	} else {			// default to fullscreen
	    VideoWindowHeight = screen->height_in_pixels;
	    VideoWindowWidth = screen->width_in_pixels;
	}
    }
    if (!VideoWindowWidth) {
	VideoWindowWidth = (VideoWindowHeight * 16) / 9;
    }
    //
    //	prepare opengl
    //
#ifdef USE_GLX
    // FIXME: module selected below
    if (0) {

	GlxInit();
	// FIXME: use root window?
	VideoCreateWindow(screen->root, GlxVisualInfo->visualid,
	    GlxVisualInfo->depth);
	GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight,
	    GlxThreadContext);
    } else
#endif

	//
	// Create output window
	//
    if (1) {				// FIXME: use window mode

	VideoCreateWindow(screen->root, screen->root_visual,
	    screen->root_depth);

    } else {
	// FIXME: support embedded mode
	VideoWindow = screen->root;

	// FIXME: VideoWindowHeight VideoWindowWidth
    }

    Debug(3, "video: window prepared\n");

    //
    //	prepare hardware decoder VA-API/VDPAU/CUVID/NVDEC
    //
    for (i = 0; i < (int)(sizeof(VideoModules) / sizeof(*VideoModules)); ++i) {
	// FIXME: support list of drivers and include display name
	// use user device or first working enabled device driver
	if ((VideoDriverName
		&& !strcasecmp(VideoDriverName, VideoModules[i]->Name))
	    || (!VideoDriverName && VideoModules[i]->Enabled)) {
	    if (VideoModules[i]->Init(display_name)) {
		VideoUsedModule = VideoModules[i];
		goto found;
	    }
	}
    }
    Error(_("video: '%s' output module isn't supported\n"), VideoDriverName);
    VideoUsedModule = &NoopModule;

  found:
    // FIXME: make it configurable from gui
    if (getenv("NO_MPEG_HW")) {
	VideoHardwareDecoder = HWmpeg2Off;
    }
    if (getenv("NO_HW")) {
	VideoHardwareDecoder = HWOff;
    }
    // disable x11 screensaver
    X11SuspendScreenSaver(Connection, 1);
    X11DPMSDisable(Connection);

    //xcb_prefetch_maximum_request_length(Connection);
    xcb_flush(Connection);

    // I would like to start threads here, but this produces:
    // [xcb] Unknown sequence number while processing queue
    // [xcb] Most likely this is a multi-threaded client and XInitThreads
    // has not been called
    //VideoPollEvent();
    //VideoThreadInit();
}

///
///	Cleanup video output module.
///
void VideoExit(void)
{
    if (!XlibDisplay) {			// no init or failed
	return;
    }
    //
    //	Reenable screensaver / DPMS.
    //
    X11DPMSReenable(Connection);
    X11SuspendScreenSaver(Connection, 0);

#ifdef USE_VIDEO_THREAD
    VideoThreadExit();
    // VDPAU cleanup hangs in XLockDisplay every 100 exits
    // XUnlockDisplay(XlibDisplay);
    // xcb_flush(Connection);
#endif
    VideoUsedModule->Exit();
    VideoUsedModule = &NoopModule;
#ifdef USE_GLX
    if (GlxEnabled) {
	GlxExit();
        GlxEnabled = 0;
    }
#endif
#ifdef USE_EGL
    if (EglEnabled) {
	EglExit();
        EglEnabled = 0;
    }
#endif

    //
    //	FIXME: cleanup.
    //
    //RandrExit();

    //
    //	X11/xcb cleanup
    //
    if (VideoWindow != XCB_NONE) {
	xcb_destroy_window(Connection, VideoWindow);
	VideoWindow = XCB_NONE;
    }
    if (VideoColormap != XCB_NONE) {
	xcb_free_colormap(Connection, VideoColormap);
	VideoColormap = XCB_NONE;
    }
    if (VideoBlankCursor != XCB_NONE) {
	xcb_free_cursor(Connection, VideoBlankCursor);
	VideoBlankCursor = XCB_NONE;
    }
    if (VideoCursorPixmap != XCB_NONE) {
	xcb_free_pixmap(Connection, VideoCursorPixmap);
	VideoCursorPixmap = XCB_NONE;
    }
    xcb_flush(Connection);
    if (XlibDisplay) {
	if (XCloseDisplay(XlibDisplay)) {
	    Error(_("video: error closing display\n"));
	}
	XlibDisplay = NULL;
	Connection = 0;
    }
}

#endif

#ifdef VIDEO_TEST

#include <getopt.h>

uint32_t VideoSwitch;			///< required

int64_t AudioGetDelay(void)		///< required
{
    return 0L;
}

int64_t AudioGetClock(void)		///< required
{
    return AV_NOPTS_VALUE;
}

void FeedKeyPress( __attribute__ ((unused))
    const char *x, __attribute__ ((unused))
    const char *y, __attribute__ ((unused))
    int a, __attribute__ ((unused))
    int b, __attribute__ ((unused))
    const char *s)
{
}

int VideoDecodeInput( __attribute__ ((unused)) VideoStream * stream)
{
    return -1;
}

///
///	Print version.
///
static void PrintVersion(void)
{
    printf("video_test: video tester Version " VERSION
#ifdef GIT_REV
	"(GIT-" GIT_REV ")"
#endif
	",\n\t(c) 2009 - 2014 by Johns\n"
	"\tLicense AGPLv3: GNU Affero General Public License version 3\n");
}

///
///	Print usage.
///
static void PrintUsage(void)
{
    printf("Usage: video_test [-?dhv]\n"
	"\t-d\tenable debug, more -d increase the verbosity\n"
	"\t-? -h\tdisplay this message\n" "\t-v\tdisplay version information\n"
	"Only idiots print usage on stderr!\n");
}

///
///	Main entry point.
///
///	@param argc	number of arguments
///	@param argv	arguments vector
///
///	@returns -1 on failures, 0 clean exit.
///
int main(int argc, char *const argv[])
{
    uint32_t start_tick;
    uint32_t tick;
    int n;
    VideoHwDecoder *video_hw_decoder;

    LogLevel = 0;

    //
    //	Parse command line arguments
    //
    for (;;) {
	switch (getopt(argc, argv, "hv?-c:dg:")) {
	    case 'd':			// enabled debug
		++LogLevel;
		continue;
	    case 'g':			// geometry
		if (VideoSetGeometry(optarg) < 0) {
		    fprintf(stderr,
			_
			("Bad formated geometry please use: [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]\n"));
		    return 0;
		}
		continue;

	    case EOF:
		break;
	    case 'v':			// print version
		PrintVersion();
		return 0;
	    case '?':
	    case 'h':			// help usage
		PrintVersion();
		PrintUsage();
		return 0;
	    case '-':
		PrintVersion();
		PrintUsage();
		fprintf(stderr, "\nWe need no long options\n");
		return -1;
	    case ':':
		PrintVersion();
		fprintf(stderr, "Missing argument for option '%c'\n", optopt);
		return -1;
	    default:
		PrintVersion();
		fprintf(stderr, "Unknown option '%c'\n", optopt);
		return -1;
	}
	break;
    }
    if (optind < argc) {
	PrintVersion();
	while (optind < argc) {
	    fprintf(stderr, "Unhandled argument '%s'\n", argv[optind++]);
	}
	return -1;
    }
    //
    //	  main loop
    //
    VideoInit(NULL);
    VideoOsdInit();
    video_hw_decoder = VideoNewHwDecoder(NULL);
    start_tick = GetMsTicks();
    n = 0;
    for (;;) {
#ifdef USE_VAAPI
	if (VideoVaapiEnabled) {
	    VaapiDisplayFrame();
	}
#endif
#ifdef USE_VDPAU
	if (VideoVdpauEnabled) {
	    VdpauDisplayFrame();
	}
#endif
#ifdef USE_CUVID
	if (VideoCuvidEnabled) {
	    CuvidDisplayFrame();
	}
#endif
#ifdef USE_CUVID
	if (VideoNVdecEnabled) {
	    NVdecDisplayFrame();
	}
#endif
	tick = GetMsTicks();
	n++;
	if (!(n % 100)) {
	    printf("%dms / frame\n", (tick - start_tick) / n);
	}
	usleep(2 * 1000);
    }
    VideoExit();

    return 0;
}

#endif
