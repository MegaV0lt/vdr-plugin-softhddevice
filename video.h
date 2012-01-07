///
///	@file video.h	@brief Video module header file
///
///	Copyright (c) 2009 - 2012 by Johns.  All Rights Reserved.
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

/// @addtogroup Video
/// @{

//----------------------------------------------------------------------------
//	Typedefs
//----------------------------------------------------------------------------

    /// Video hardware decoder typedef
typedef struct _video_hw_decoder_ VideoHwDecoder;

//----------------------------------------------------------------------------
//	Prototypes
//----------------------------------------------------------------------------

    /// Allocate new video hardware decoder.
extern VideoHwDecoder *VideoNewHwDecoder(void);

    /// Get and allocate a video hardware surface.
extern unsigned VideoGetSurface(VideoHwDecoder *);

    /// Release a video hardware surface
extern void VideoReleaseSurface(VideoHwDecoder *, unsigned);

#ifdef LIBAVCODEC_VERSION
    /// Render a ffmpeg frame.
extern void VideoRenderFrame(VideoHwDecoder *, AVCodecContext *, AVFrame *);

    /// Get ffmpeg vaapi context.
extern struct vaapi_context *VideoGetVaapiContext(VideoHwDecoder *);

    /// Callback to negotiate the PixelFormat.
extern enum PixelFormat Video_get_format(VideoHwDecoder *, AVCodecContext *,
    const enum PixelFormat *);

#ifdef AVCODEC_VDPAU_H
    /// Draw vdpau render state.
extern void VideoDrawRenderState(VideoHwDecoder *,
    struct vdpau_render_state *);
#endif
#endif

    /// Poll video events.
extern void VideoPollEvent(void);

    /// Wakeup display handler.
extern void VideoDisplayWakeup(void);

    /// Set video mode.
//extern void VideoSetVideoMode(int, int, int, int);

    /// Set video geometry.
extern int VideoSetGeometry(const char *);

    /// Set video output position.
extern void VideoSetOutputPosition(int, int, int, int);

    /// Set deinterlace.
extern void VideoSetDeinterlace(int);

    /// Set skip chroma deinterlace.
extern void VideoSetSkipChromaDeinterlace(int);

    /// Set scaling.
extern void VideoSetScaling(int);

    /// Set denoise.
extern void VideoSetDenoise(int);

    /// Set sharpen.
extern void VideoSetSharpen(int);

    /// Set audio delay.
extern void VideoSetAudioDelay(int);

    /// Clear OSD.
extern void VideoOsdClear(void);

    /// Draw an OSD ARGB image.
extern void VideoOsdDrawARGB(int, int, int, int, const uint8_t *);

extern int64_t VideoGetClock(void);	///< Get video clock.

extern void VideoOsdInit(void);		///< Setup osd.
extern void VideoOsdExit(void);		///< Cleanup osd.

extern void VideoInit(const char *);	///< Setup video module.
extern void VideoExit(void);		///< Cleanup and exit video module.

extern void VideoFlushInput(void);	///< Flush video input buffers.
extern int VideoDecode(void);		///< Decode video input buffers.

/// @}
