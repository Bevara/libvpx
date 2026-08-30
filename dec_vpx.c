/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / VP8/VP9 video decoder filter
 *  based on libvpx (https://chromium.googlesource.com/webm/libvpx)
 *
 *  Pairs with a demuxer (e.g. webmdmx) that provides framed VP8/VP9
 *  frames (one coded frame per packet, as stored in WebM SimpleBlocks)
 *  - this filter does not parse a container itself. Handles both
 *  codecs since libvpx exposes them through the same vpx_codec_ctx_t
 *  API, just a different vpx_codec_iface_t chosen from the input PID's
 *  codec ID.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>

#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;

	vpx_codec_ctx_t codec;
	Bool codec_ready;
	u32 codec_id;

	u32 width, height;
	Bool is_playing;
} GF_VPxDecCtx;

static GF_Err vpxdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *prop;
	vpx_codec_iface_t *iface;
	GF_VPxDecCtx *ctx = (GF_VPxDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		if (ctx->codec_ready)
		{
			vpx_codec_destroy(&ctx->codec);
			ctx->codec_ready = GF_FALSE;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_CODECID);
	if (!prop) return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	ctx->codec_id = prop->value.uint;

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}

	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_YUV));

	if (!ctx->codec_ready)
	{
		vpx_codec_err_t res;
		iface = (ctx->codec_id == GF_CODECID_VP8) ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx();
		res = vpx_codec_dec_init(&ctx->codec, iface, NULL, 0);
		if (res != VPX_CODEC_OK)
		{
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[VPxDec] Failed to init libvpx decoder: %s\n", vpx_codec_error(&ctx->codec)));
			return GF_IO_ERR;
		}
		ctx->codec_ready = GF_TRUE;
	}

	return GF_OK;
}

static Bool vpxdec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_VPxDecCtx *ctx = (GF_VPxDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static void vpxdec_send_frame(GF_VPxDecCtx *ctx, const vpx_image_t *img, u64 cts)
{
	GF_FilterPacket *dst_pck;
	u8 *output;
	u32 y, c;
	u32 w = img->d_w;
	u32 h = img->d_h;
	u32 cw = (w + img->x_chroma_shift) >> img->x_chroma_shift;
	u32 ch = (h + img->y_chroma_shift) >> img->y_chroma_shift;
	u32 y_size = w * h;
	u32 c_size = cw * ch;
	u32 out_size = y_size + 2 * c_size;

	if ((w != ctx->width) || (h != ctx->height))
	{
		ctx->width = w;
		ctx->height = h;
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(w));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(h));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(w));
	}

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_size, &output);
	if (!dst_pck) return;

	for (y = 0; y < h; y++)
	{
		memcpy(output + y * w, img->planes[VPX_PLANE_Y] + y * img->stride[VPX_PLANE_Y], w);
	}
	output += y_size;

	for (c = VPX_PLANE_U; c <= VPX_PLANE_V; c++)
	{
		for (y = 0; y < ch; y++)
		{
			memcpy(output + y * cw, img->planes[c] + y * img->stride[c], cw);
		}
		output += c_size;
	}

	gf_filter_pck_set_cts(dst_pck, cts);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);
}

static void vpxdec_flush_frames(GF_VPxDecCtx *ctx, u64 cts)
{
	vpx_codec_iter_t iter = NULL;
	vpx_image_t *img;
	while ((img = vpx_codec_get_frame(&ctx->codec, &iter)) != NULL)
	{
		vpxdec_send_frame(ctx, img, cts);
	}
}

static GF_Err vpxdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck;
	u8 *data;
	u32 size;
	u64 cts;
	vpx_codec_err_t res;
	GF_VPxDecCtx *ctx = (GF_VPxDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}
	cts = gf_filter_pck_get_cts(pck);

	res = vpx_codec_decode(&ctx->codec, data, size, NULL, 0);
	gf_filter_pid_drop_packet(ctx->ipid);

	if (res != VPX_CODEC_OK)
	{
		GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[VPxDec] Failed to decode frame: %s\n", vpx_codec_error(&ctx->codec)));
		return GF_OK;
	}

	vpxdec_flush_frames(ctx, cts);

	return GF_OK;
}

static void vpxdec_finalize(GF_Filter *filter)
{
	GF_VPxDecCtx *ctx = (GF_VPxDecCtx *)gf_filter_get_udta(filter);
	if (ctx->codec_ready)
	{
		vpx_codec_destroy(&ctx->codec);
		ctx->codec_ready = GF_FALSE;
	}
}

static const GF_FilterCapability VPxDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_VP8),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_VP9),
		CAP_BOOL(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister VPxDecoderRegister = {
	.name = "vpxdec",
	GF_FS_SET_DESCRIPTION("VP8/VP9 video decoder")
		GF_FS_SET_HELP("This filter decodes VP8/VP9 video elementary streams using libvpx.")
			.private_size = sizeof(GF_VPxDecCtx),
	SETCAPS(VPxDecCaps),
	.configure_pid = vpxdec_configure_pid,
	.process = vpxdec_process,
	.process_event = vpxdec_process_event,
	.finalize = vpxdec_finalize,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE dynCall_vpxdec_register(GF_FilterSession *session)
{
	return &VPxDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_vpxdec(void) {
    gf_filter_auto_register("vpxdec", dynCall_vpxdec_register);
}
