/*
 * - CrystalHD decoder module -
 *
 * Copyright(C) 2010,2011 Philip Langdale <ffmpeg.philipl@overt.org>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * - Principles of Operation -
 *
 * The CrystalHD decoder operates at the bitstream level - which is an even
 * higher level than the decoding hardware you typically see in modern GPUs.
 * This means it has a very simple interface, in principle. You feed demuxed
 * packets in one end and get decoded picture (fields/frames) out the other.
 *
 * Of course, nothing is ever that simple. Due, at the very least, to b-frame
 * dependencies in the supported formats, the hardware has a delay between
 * when a packet goes in, and when a picture comes out. Furthermore, this delay
 * is not just a function of time, but also one of the dependency on additional
 * frames being fed into the decoder to satisfy the b-frame dependencies.
 *
 * As such, a pipeline will build up that is roughly equivalent to the required
 * DPB for the file being played. If that was all it took, things would still
 * be simple - so, of course, it isn't.
 *
 * The hardware has a way of indicating that a picture is ready to be copied out,
 * but this is unreliable - and sometimes the attempt will still fail so, based
 * on testing, the code will wait until 3 pictures are ready before starting
 * to copy out - and this has the effect of extending the pipeline.
 *
 * Finally, while it is tempting to say that once the decoder starts outputting
 * frames, the software should never fail to return a frame from a decode(),
 * this is a hard assertion to make, because the stream may switch between
 * differently encoded content (number of b-frames, interlacing, etc) which
 * might require a longer pipeline than before. If that happened, you could
 * deadlock trying to retrieve a frame that can't be decoded without feeding
 * in additional packets.
 *
 * As such, the code will return in the event that a picture cannot be copied
 * out, leading to an increase in the length of the pipeline. This in turn,
 * means we have to be sensitive to the time it takes to decode a picture;
 * We do not want to give up just because the hardware needed a little more
 * time to prepare the picture! For this reason, there are delays included
 * in the decode() path that ensure that, under normal conditions, the hardware
 * will only fail to return a frame if it really needs additional packets to
 * complete the decoding.
 *
 * Finally, to be explicit, we do not want the pipeline to grow without bound
 * for two reasons: 1) The hardware can only buffer a finite number of packets,
 * and 2) The client application may not be able to cope with arbitrarily long
 * delays in the video path relative to the audio path. For example. MPlayer
 * can only handle a 20 picture delay (although this is arbitrary, and needs
 * to be extended to fully support the CrystalHD where the delay could be up
 * to 32 pictures - consider PAFF H.264 content with 16 b-frames).
 */

/*****************************************************************************
 * Includes
 ****************************************************************************/

#define _XOPEN_SOURCE 600
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <libcrystalhd/bc_dts_types.h>
#include <libcrystalhd/bc_dts_defs.h>
#include <libcrystalhd/libcrystalhd_if.h>

#include "avcodec.h"
#include "h264dec.h"
#include "internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"

#if HAVE_UNISTD_H
#include <unistd.h>
#endif

/** Timeout parameter passed to DtsProcOutput() in us */
#define OUTPUT_PROC_TIMEOUT 50
/** Step between fake timestamps passed to hardware in units of 100ns */
#define TIMESTAMP_UNIT 100000


/*****************************************************************************
 * Module private data
 ****************************************************************************/

typedef enum {
    RET_ERROR           = -1,
    RET_OK              = 0,
} CopyRet;

typedef struct OpaqueList {
    struct OpaqueList *next;
    uint64_t fake_timestamp;
    uint64_t reordered_opaque;
    uint8_t pic_type;
} OpaqueList;

typedef struct {
    AVClass *av_class;
    AVCodecContext *avctx;
    AVFrame *pic;
    HANDLE dev;

    uint8_t *orig_extradata;
    uint32_t orig_extradata_size;

    AVBSFContext *bsfc;
    AVCodecParserContext *parser;

    uint8_t is_70012;
    uint8_t *sps_pps_buf;
    uint32_t sps_pps_size;
    uint8_t is_nal;
    uint8_t need_second_field;

    uint64_t last_picture;

    OpaqueList *head;
    OpaqueList *tail;

    /* Options */
    uint32_t sWidth;
} CHDContext;

static const AVOption options[] = {
    { "crystalhd_downscale_width",
      "Turn on downscaling to the specified width",
      offsetof(CHDContext, sWidth),
      AV_OPT_TYPE_INT, {.i64 = 0}, 0, UINT32_MAX,
      AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM, },
    { NULL, },
};


/*****************************************************************************
 * Helper functions
 ****************************************************************************/

static inline BC_MEDIA_SUBTYPE id2subtype(CHDContext *priv, enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_MPEG4:
        return BC_MSUBTYPE_DIVX;
    case AV_CODEC_ID_MSMPEG4V3:
        return BC_MSUBTYPE_DIVX311;
    case AV_CODEC_ID_MPEG2VIDEO:
        return BC_MSUBTYPE_MPEG2VIDEO;
    case AV_CODEC_ID_VC1:
        return BC_MSUBTYPE_VC1;
    case AV_CODEC_ID_WMV3:
        return BC_MSUBTYPE_WMV3;
    case AV_CODEC_ID_H264:
        return priv->is_nal ? BC_MSUBTYPE_AVC1 : BC_MSUBTYPE_H264;
    default:
        return BC_MSUBTYPE_INVALID;
    }
}

static inline void print_frame_info(CHDContext *priv, BC_DTS_PROC_OUT *output)
{
    av_log(priv->avctx, AV_LOG_TRACE, "\tYBuffSz: %u\n", output->YbuffSz);
    av_log(priv->avctx, AV_LOG_TRACE, "\tYBuffDoneSz: %u\n",
           output->YBuffDoneSz);
    av_log(priv->avctx, AV_LOG_TRACE, "\tUVBuffDoneSz: %u\n",
           output->UVBuffDoneSz);
    av_log(priv->avctx, AV_LOG_TRACE, "\tTimestamp: %"PRIu64"\n",
           output->PicInfo.timeStamp);
    av_log(priv->avctx, AV_LOG_TRACE, "\tPicture Number: %u\n",
           output->PicInfo.picture_number);
    av_log(priv->avctx, AV_LOG_TRACE, "\tWidth: %u\n",
           output->PicInfo.width);
    av_log(priv->avctx, AV_LOG_TRACE, "\tHeight: %u\n",
           output->PicInfo.height);
    av_log(priv->avctx, AV_LOG_TRACE, "\tChroma: 0x%03x\n",
           output->PicInfo.chroma_format);
    av_log(priv->avctx, AV_LOG_TRACE, "\tPulldown: %u\n",
           output->PicInfo.pulldown);
    av_log(priv->avctx, AV_LOG_TRACE, "\tFlags: 0x%08x\n",
           output->PicInfo.flags);
    av_log(priv->avctx, AV_LOG_TRACE, "\tFrame Rate/Res: %u\n",
           output->PicInfo.frame_rate);
    av_log(priv->avctx, AV_LOG_TRACE, "\tAspect Ratio: %u\n",
           output->PicInfo.aspect_ratio);
    av_log(priv->avctx, AV_LOG_TRACE, "\tColor Primaries: %u\n",
           output->PicInfo.colour_primaries);
    av_log(priv->avctx, AV_LOG_TRACE, "\tMetaData: %u\n",
           output->PicInfo.picture_meta_payload);
    av_log(priv->avctx, AV_LOG_TRACE, "\tSession Number: %u\n",
           output->PicInfo.sess_num);
    av_log(priv->avctx, AV_LOG_TRACE, "\tycom: %u\n",
           output->PicInfo.ycom);
    av_log(priv->avctx, AV_LOG_TRACE, "\tCustom Aspect: %u\n",
           output->PicInfo.custom_aspect_ratio_width_height);
    av_log(priv->avctx, AV_LOG_TRACE, "\tFrames to Drop: %u\n",
           output->PicInfo.n_drop);
    av_log(priv->avctx, AV_LOG_TRACE, "\tH264 Valid Fields: 0x%08x\n",
           output->PicInfo.other.h264.valid);
}


/*****************************************************************************
 * OpaqueList functions
 ****************************************************************************/

static uint64_t opaque_list_push(CHDContext *priv, uint64_t reordered_opaque,
                                 uint8_t pic_type)
{
    OpaqueList *newNode = av_mallocz(sizeof (OpaqueList));
    if (!newNode) {
        av_log(priv->avctx, AV_LOG_ERROR,
               "Unable to allocate new node in OpaqueList.\n");
        return 0;
    }
    if (!priv->head) {
        newNode->fake_timestamp = TIMESTAMP_UNIT;
        priv->head              = newNode;
    } else {
        newNode->fake_timestamp = priv->tail->fake_timestamp + TIMESTAMP_UNIT;
        priv->tail->next        = newNode;
    }
    priv->tail = newNode;
    newNode->reordered_opaque = reordered_opaque;
    newNode->pic_type = pic_type;

    return newNode->fake_timestamp;
}

/*
 * The OpaqueList is built in decode order, while elements will be removed
 * in presentation order. If frames are reordered, this means we must be
 * able to remove elements that are not the first element.
 *
 * Returned node must be freed by caller.
 */
static OpaqueList *opaque_list_pop(CHDContext *priv, uint64_t fake_timestamp)
{
    OpaqueList *node = priv->head;

    if (!priv->head) {
        av_log(priv->avctx, AV_LOG_ERROR,
               "CrystalHD: Attempted to query non-existent timestamps.\n");
        return NULL;
    }

    /*
     * The first element is special-cased because we have to manipulate
     * the head pointer rather than the previous element in the list.
     */
    if (priv->head->fake_timestamp == fake_timestamp) {
        priv->head = node->next;

        if (!priv->head->next)
            priv->tail = priv->head;

        node->next = NULL;
        return node;
    }

    /*
     * The list is processed at arm's length so that we have the
     * previous element available to rewrite its next pointer.
     */
    while (node->next) {
        OpaqueList *current = node->next;
        if (current->fake_timestamp == fake_timestamp) {
            node->next = current->next;

            if (!node->next)
               priv->tail = node;

            current->next = NULL;
            return current;
        } else {
            node = current;
        }
    }

    av_log(priv->avctx, AV_LOG_VERBOSE,
           "CrystalHD: Couldn't match fake_timestamp.\n");
    return NULL;
}


/*****************************************************************************
 * Video decoder API function definitions
 ****************************************************************************/

static void flush(AVCodecContext *avctx)
{
    CHDContext *priv = avctx->priv_data;

    priv->last_picture      = -1;
    priv->need_second_field = 0;

    av_frame_unref (priv->pic);

    /* Flush mode 4 flushes all software and hardware buffers. */
    DtsFlushInput(priv->dev, 4);
}


static av_cold int uninit(AVCodecContext *avctx)
{
    CHDContext *priv = avctx->priv_data;
    HANDLE device;

    device = priv->dev;
    DtsStopDecoder(device);
    DtsCloseDecoder(device);
    DtsDeviceClose(device);

    /*
     * Restore original extradata, so that if the decoder is
     * reinitialised, the bitstream detection and filtering
     * will work as expected.
     */
    if (priv->orig_extradata) {
        av_free(avctx->extradata);
        avctx->extradata = priv->orig_extradata;
        avctx->extradata_size = priv->orig_extradata_size;
        priv->orig_extradata = NULL;
        priv->orig_extradata_size = 0;
    }

    av_parser_close(priv->parser);
    if (priv->bsfc) {
        av_bsf_free(&priv->bsfc);
    }

    av_freep(&priv->sps_pps_buf);

    av_frame_free (&priv->pic);

    if (priv->head) {
       OpaqueList *node = priv->head;
       while (node) {
          OpaqueList *next = node->next;
          av_free(node);
          node = next;
       }
    }

    return 0;
}


static av_cold int init_bsf(AVCodecContext *avctx, const char *bsf_name)
{
    CHDContext *priv = avctx->priv_data;
    const AVBitStreamFilter *bsf;
    int avret;
    void *extradata = NULL;
    size_t size = 0;

    bsf = av_bsf_get_by_name(bsf_name);
    if (!bsf) {
        av_log(avctx, AV_LOG_ERROR,
               "Cannot open the %s BSF!\n", bsf_name);
        return AVERROR_BSF_NOT_FOUND;
    }

    avret = av_bsf_alloc(bsf, &priv->bsfc);
    if (avret != 0) {
        return avret;
    }

    avret = avcodec_parameters_from_context(priv->bsfc->par_in, avctx);
    if (avret != 0) {
        return avret;
    }

    avret = av_bsf_init(priv->bsfc);
    if (avret != 0) {
        return avret;
    }

    /* Back up the extradata so it can be restored at close time. */
    priv->orig_extradata = avctx->extradata;
    priv->orig_extradata_size = avctx->extradata_size;

    size = priv->bsfc->par_out->extradata_size;
    extradata = av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!extradata) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to allocate copy of extradata\n");
        return AVERROR(ENOMEM);
    }
    memcpy(extradata, priv->bsfc->par_out->extradata, size);

    avctx->extradata = extradata;
    avctx->extradata_size = size;

    return 0;
}

static av_cold int init(AVCodecContext *avctx)
{
    CHDContext* priv;
    int avret;
    BC_STATUS ret;
    BC_INFO_CRYSTAL version;
    BC_INPUT_FORMAT format = {
        .FGTEnable   = FALSE,
        .Progressive = TRUE,
        .OptFlags    = 0x80000000 | vdecFrameRate59_94 | 0x40,
        .width       = avctx->width,
        .height      = avctx->height,
    };

    BC_MEDIA_SUBTYPE subtype;

    uint32_t mode = DTS_PLAYBACK_MODE |
                    DTS_LOAD_FILE_PLAY_FW |
                    DTS_SKIP_TX_CHK_CPB |
                    DTS_PLAYBACK_DROP_RPT_MODE |
                    DTS_SINGLE_THREADED_MODE |
                    DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);

    av_log(avctx, AV_LOG_VERBOSE, "CrystalHD Init for %s\n",
           avctx->codec->name);

    avctx->pix_fmt = AV_PIX_FMT_YUYV422;

    /* Initialize the library */
    priv               = avctx->priv_data;
    priv->avctx        = avctx;
    priv->is_nal       = avctx->extradata_size > 0 && *(avctx->extradata) == 1;
    priv->last_picture = -1;
    priv->pic          = av_frame_alloc();

    subtype = id2subtype(priv, avctx->codec->id);
    switch (subtype) {
    case BC_MSUBTYPE_AVC1:
        avret = init_bsf(avctx, "h264_mp4toannexb");
        if (avret != 0) {
            return avret;
        }
        subtype = BC_MSUBTYPE_H264;
        format.startCodeSz = 4;
        format.pMetaData  = avctx->extradata;
        format.metaDataSz = avctx->extradata_size;
        break;
    case BC_MSUBTYPE_DIVX:
        avret = init_bsf(avctx, "mpeg4_unpack_bframes");
        if (avret != 0) {
            return avret;
        }
        format.pMetaData  = avctx->extradata;
        format.metaDataSz = avctx->extradata_size;
        break;
    case BC_MSUBTYPE_H264:
        format.startCodeSz = 4;
        // Fall-through
    case BC_MSUBTYPE_VC1:
    case BC_MSUBTYPE_WVC1:
    case BC_MSUBTYPE_WMV3:
    case BC_MSUBTYPE_WMVA:
    case BC_MSUBTYPE_MPEG2VIDEO:
    case BC_MSUBTYPE_DIVX311:
        format.pMetaData  = avctx->extradata;
        format.metaDataSz = avctx->extradata_size;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "CrystalHD: Unknown codec name\n");
        return AVERROR(EINVAL);
    }
    format.mSubtype = subtype;

    if (priv->sWidth) {
        format.bEnableScaling = 1;
        format.ScalingParams.sWidth = priv->sWidth;
    }

    /* Get a decoder instance */
    av_log(avctx, AV_LOG_VERBOSE, "CrystalHD: starting up\n");
    // Initialize the Link and Decoder devices
    ret = DtsDeviceOpen(&priv->dev, mode);
    if (ret != BC_STS_SUCCESS) {
        av_log(avctx, AV_LOG_VERBOSE, "CrystalHD: DtsDeviceOpen failed\n");
        goto fail;
    }

    ret = DtsCrystalHDVersion(priv->dev, &version);
    if (ret != BC_STS_SUCCESS) {
        av_log(avctx, AV_LOG_VERBOSE,
               "CrystalHD: DtsCrystalHDVersion failed\n");
        goto fail;
    }
    priv->is_70012 = version.device == 0;

    if (priv->is_70012 &&
        (subtype == BC_MSUBTYPE_DIVX || subtype == BC_MSUBTYPE_DIVX311)) {
        av_log(avctx, AV_LOG_VERBOSE,
               "CrystalHD: BCM70012 doesn't support MPEG4-ASP/DivX/Xvid\n");
        goto fail;
    }

    ret = DtsSetInputFormat(priv->dev, &format);
    if (ret != BC_STS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "CrystalHD: SetInputFormat failed\n");
        goto fail;
    }

    ret = DtsOpenDecoder(priv->dev, BC_STREAM_TYPE_ES);
    if (ret != BC_STS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "CrystalHD: DtsOpenDecoder failed\n");
        goto fail;
    }

    ret = DtsSetColorSpace(priv->dev, OUTPUT_MODE422_YUY2);
    if (ret != BC_STS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "CrystalHD: DtsSetColorSpace failed\n");
        goto fail;
    }
    ret = DtsStartDecoder(priv->dev);
    if (ret != BC_STS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "CrystalHD: DtsStartDecoder failed\n");
        goto fail;
    }
    ret = DtsStartCapture(priv->dev);
    if (ret != BC_STS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "CrystalHD: DtsStartCapture failed\n");
        goto fail;
    }

    if (avctx->codec->id == AV_CODEC_ID_H264) {
        priv->parser = av_parser_init(avctx->codec->id);
        if (!priv->parser)
            av_log(avctx, AV_LOG_WARNING,
                   "Cannot open the h.264 parser! Interlaced h.264 content "
                   "will not be detected reliably.\n");
        priv->parser->flags = PARSER_FLAG_COMPLETE_FRAMES;
    }
    av_log(avctx, AV_LOG_VERBOSE, "CrystalHD: Init complete.\n");

    return 0;

 fail:
    uninit(avctx);
    return -1;
}


static inline CopyRet copy_frame(AVCodecContext *avctx,
                                 BC_DTS_PROC_OUT *output,
                                 void *data, int *got_frame)
{
    BC_STATUS ret;
    BC_DTS_STATUS decoder_status = { 0, };
    uint8_t trust_interlaced;
    uint8_t interlaced;

    CHDContext *priv = avctx->priv_data;
    int64_t pkt_pts  = AV_NOPTS_VALUE;
    uint8_t pic_type = 0;

    uint8_t bottom_field = (output->PicInfo.flags & VDEC_FLAG_BOTTOMFIELD) ==
                           VDEC_FLAG_BOTTOMFIELD;
    uint8_t bottom_first = !!(output->PicInfo.flags & VDEC_FLAG_BOTTOM_FIRST);

    int width    = output->PicInfo.width;
    int height   = output->PicInfo.height;
    int bwidth;
    uint8_t *src = output->Ybuff;
    int sStride;
    uint8_t *dst;
    int dStride;

    if (output->PicInfo.timeStamp != 0) {
        OpaqueList *node = opaque_list_pop(priv, output->PicInfo.timeStamp);
        if (node) {
            pkt_pts = node->reordered_opaque;
            pic_type = node->pic_type;
            av_free(node);
        } else {
            /*
             * We will encounter a situation where a timestamp cannot be
             * popped if a second field is being returned. In this case,
             * each field has the same timestamp and the first one will
             * cause it to be popped. To keep subsequent calculations
             * simple, pic_type should be set a FIELD value - doesn't
             * matter which, but I chose BOTTOM.
             */
            pic_type = PICT_BOTTOM_FIELD;
        }
        av_log(avctx, AV_LOG_VERBOSE, "output \"pts\": %"PRIu64"\n",
               output->PicInfo.timeStamp);
        av_log(avctx, AV_LOG_VERBOSE, "output picture type %d\n",
               pic_type);
    }

    ret = DtsGetDriverStatus(priv->dev, &decoder_status);
    if (ret != BC_STS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR,
               "CrystalHD: GetDriverStatus failed: %u\n", ret);
       return RET_ERROR;
    }

    /*
     * For most content, we can trust the interlaced flag returned
     * by the hardware, but sometimes we can't. These are the
     * conditions under which we can trust the flag:
     *
     * 1) It's not h.264 content
     * 2) The UNKNOWN_SRC flag is not set
     * 3) We know we're expecting a second field
     * 4) The hardware reports this picture and the next picture
     *    have the same picture number.
     *
     * Note that there can still be interlaced content that will
     * fail this check, if the hardware hasn't decoded the next
     * picture or if there is a corruption in the stream. (In either
     * case a 0 will be returned for the next picture number)
     */
    trust_interlaced = avctx->codec->id != AV_CODEC_ID_H264 ||
                       !(output->PicInfo.flags & VDEC_FLAG_UNKNOWN_SRC) ||
                       priv->need_second_field ||
                       (decoder_status.picNumFlags & ~0x40000000) ==
                       output->PicInfo.picture_number;

    /*
     * If we got a false negative for trust_interlaced on the first field,
     * we will realise our mistake here when we see that the picture number is that
     * of the previous picture. We cannot recover the frame and should discard the
     * second field to keep the correct number of output frames.
     */
    if (output->PicInfo.picture_number == priv->last_picture && !priv->need_second_field) {
        av_log(avctx, AV_LOG_WARNING,
               "Incorrectly guessed progressive frame. Discarding second field\n");
        /* Returning without providing a picture. */
        return RET_OK;
    }

    interlaced = (output->PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) &&
                 trust_interlaced;

    if (!trust_interlaced && (decoder_status.picNumFlags & ~0x40000000) == 0) {
        av_log(avctx, AV_LOG_VERBOSE,
               "Next picture number unknown. Assuming progressive frame.\n");
    }

    av_log(avctx, AV_LOG_VERBOSE, "Interlaced state: %d | trust_interlaced %d\n",
           interlaced, trust_interlaced);

    if (priv->pic->data[0] && !priv->need_second_field)
        av_frame_unref(priv->pic);

    priv->need_second_field = interlaced && !priv->need_second_field;

    if (!priv->pic->data[0]) {
        if (ff_get_buffer(avctx, priv->pic, AV_GET_BUFFER_FLAG_REF) < 0)
            return RET_ERROR;
    }

    bwidth = av_image_get_linesize(avctx->pix_fmt, width, 0);
    if (priv->is_70012) {
        int pStride;

        if (width <= 720)
            pStride = 720;
        else if (width <= 1280)
            pStride = 1280;
        else pStride = 1920;
        sStride = av_image_get_linesize(avctx->pix_fmt, pStride, 0);
    } else {
        sStride = bwidth;
    }

    dStride = priv->pic->linesize[0];
    dst     = priv->pic->data[0];

    av_log(priv->avctx, AV_LOG_VERBOSE, "CrystalHD: Copying out frame\n");

    /*
     * The hardware doesn't return the first sample of a picture.
     * Ignoring why it behaves this way, it's better to copy the sample from
     * the second line, rather than the next sample across because the chroma
     * values should be correct (assuming the decoded video was 4:2:0, which
     * it was).
     */
    *((uint32_t *)src) = *((uint32_t *)(src + sStride));

    if (interlaced) {
        int dY = 0;
        int sY = 0;

        height /= 2;
        if (bottom_field) {
            av_log(priv->avctx, AV_LOG_VERBOSE, "Interlaced: bottom field\n");
            dY = 1;
        } else {
            av_log(priv->avctx, AV_LOG_VERBOSE, "Interlaced: top field\n");
            dY = 0;
        }

        for (sY = 0; sY < height; dY++, sY++) {
            memcpy(&(dst[dY * dStride]), &(src[sY * sStride]), bwidth);
            dY++;
        }
    } else {
        av_image_copy_plane(dst, dStride, src, sStride, bwidth, height);
    }

    priv->pic->interlaced_frame = interlaced;
    if (interlaced)
        priv->pic->top_field_first = !bottom_first;

    if (pkt_pts != AV_NOPTS_VALUE) {
        priv->pic->pts = pkt_pts;
#if FF_API_PKT_PTS
FF_DISABLE_DEPRECATION_WARNINGS
        priv->pic->pkt_pts = pkt_pts;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    }
    av_frame_set_pkt_pos(priv->pic, -1);
    av_frame_set_pkt_duration(priv->pic, 0);
    av_frame_set_pkt_size(priv->pic, -1);

    if (!priv->need_second_field) {
        *got_frame       = 1;
        if ((ret = av_frame_ref(data, priv->pic)) < 0) {
            return ret;
        }
    }

    return RET_OK;
}


static inline CopyRet receive_frame(AVCodecContext *avctx,
                                    void *data, int *got_frame)
{
    BC_STATUS ret;
    BC_DTS_PROC_OUT output = {
        .PicInfo.width  = avctx->width,
        .PicInfo.height = avctx->height,
    };
    CHDContext *priv = avctx->priv_data;
    HANDLE dev       = priv->dev;

    *got_frame = 0;

    // Request decoded data from the driver
    ret = DtsProcOutputNoCopy(dev, OUTPUT_PROC_TIMEOUT, &output);
    if (ret == BC_STS_FMT_CHANGE) {
        av_log(avctx, AV_LOG_VERBOSE, "CrystalHD: Initial format change\n");
        avctx->width  = output.PicInfo.width;
        avctx->height = output.PicInfo.height;
        switch ( output.PicInfo.aspect_ratio ) {
        case vdecAspectRatioSquare:
            avctx->sample_aspect_ratio = (AVRational) {  1,  1};
            break;
        case vdecAspectRatio12_11:
            avctx->sample_aspect_ratio = (AVRational) { 12, 11};
            break;
        case vdecAspectRatio10_11:
            avctx->sample_aspect_ratio = (AVRational) { 10, 11};
            break;
        case vdecAspectRatio16_11:
            avctx->sample_aspect_ratio = (AVRational) { 16, 11};
            break;
        case vdecAspectRatio40_33:
            avctx->sample_aspect_ratio = (AVRational) { 40, 33};
            break;
        case vdecAspectRatio24_11:
            avctx->sample_aspect_ratio = (AVRational) { 24, 11};
            break;
        case vdecAspectRatio20_11:
            avctx->sample_aspect_ratio = (AVRational) { 20, 11};
            break;
        case vdecAspectRatio32_11:
            avctx->sample_aspect_ratio = (AVRational) { 32, 11};
            break;
        case vdecAspectRatio80_33:
            avctx->sample_aspect_ratio = (AVRational) { 80, 33};
            break;
        case vdecAspectRatio18_11:
            avctx->sample_aspect_ratio = (AVRational) { 18, 11};
            break;
        case vdecAspectRatio15_11:
            avctx->sample_aspect_ratio = (AVRational) { 15, 11};
            break;
        case vdecAspectRatio64_33:
            avctx->sample_aspect_ratio = (AVRational) { 64, 33};
            break;
        case vdecAspectRatio160_99:
            avctx->sample_aspect_ratio = (AVRational) {160, 99};
            break;
        case vdecAspectRatio4_3:
            avctx->sample_aspect_ratio = (AVRational) {  4,  3};
            break;
        case vdecAspectRatio16_9:
            avctx->sample_aspect_ratio = (AVRational) { 16,  9};
            break;
        case vdecAspectRatio221_1:
            avctx->sample_aspect_ratio = (AVRational) {221,  1};
            break;
        }
        return RET_OK;
    } else if (ret == BC_STS_SUCCESS) {
        int copy_ret = -1;
        if (output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) {
            if (priv->last_picture == -1) {
                /*
                 * Init to one less, so that the incrementing code doesn't
                 * need to be special-cased.
                 */
                priv->last_picture = output.PicInfo.picture_number - 1;
            }

            print_frame_info(priv, &output);

            if (priv->last_picture + 1 < output.PicInfo.picture_number) {
                av_log(avctx, AV_LOG_WARNING,
                       "CrystalHD: Picture Number discontinuity\n");
                /*
                 * XXX: I have no idea what the semantics of this situation
                 * are so I don't even know if we've lost frames or which
                 * ones.
                 */
               priv->last_picture = output.PicInfo.picture_number - 1;
            }

            copy_ret = copy_frame(avctx, &output, data, got_frame);
            if (*got_frame > 0) {
                priv->last_picture++;
            }
        } else {
            /*
             * An invalid frame has been consumed.
             */
            av_log(avctx, AV_LOG_ERROR, "CrystalHD: ProcOutput succeeded with "
                                        "invalid PIB\n");
            copy_ret = RET_OK;
        }
        DtsReleaseOutputBuffs(dev, NULL, FALSE);

        return copy_ret;
    } else if (ret == BC_STS_BUSY) {
        return RET_OK;
    } else {
        av_log(avctx, AV_LOG_ERROR, "CrystalHD: ProcOutput failed %d\n", ret);
        return RET_ERROR;
    }
}

static int crystalhd_decode_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    BC_STATUS bc_ret;
    CHDContext *priv   = avctx->priv_data;
    HANDLE dev         = priv->dev;
    uint8_t pic_type   = 0;
    AVPacket filtered_packet = { 0 };
    int ret = 0;

    av_log(avctx, AV_LOG_VERBOSE, "CrystalHD: decode_packet\n");

    if (avpkt && avpkt->size) {
        int32_t tx_free = (int32_t)DtsTxFreeSize(dev);

        if (priv->bsfc) {
            AVPacket filter_packet = { 0 };

            ret = av_packet_ref(&filter_packet, avpkt);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "CrystalHD: mpv4toannexb filter "
                       "failed to ref input packet\n");
                goto exit;
            }

            ret = av_bsf_send_packet(priv->bsfc, &filter_packet);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "CrystalHD: mpv4toannexb filter "
                       "failed to send input packet\n");
                goto exit;
            }

            ret = av_bsf_receive_packet(priv->bsfc, &filtered_packet);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "CrystalHD: mpv4toannexb filter "
                       "failed to receive output packet\n");
                goto exit;
            }

            avpkt = &filtered_packet;
            av_packet_unref(&filter_packet);
        }

        if (priv->parser) {
            uint8_t *pout;
            int psize;
            int index;
            H264Context *h = priv->parser->priv_data;

            index = av_parser_parse2(priv->parser, avctx, &pout, &psize,
                                     avpkt->data, avpkt->size, avpkt->pts,
                                     avpkt->dts, 0);
            if (index < 0) {
                av_log(avctx, AV_LOG_WARNING,
                       "CrystalHD: Failed to parse h.264 packet to "
                       "detect interlacing.\n");
            } else if (index != avpkt->size) {
                av_log(avctx, AV_LOG_WARNING,
                       "CrystalHD: Failed to parse h.264 packet "
                       "completely. Interlaced frames may be "
                       "incorrectly detected.\n");
            } else {
                av_log(avctx, AV_LOG_VERBOSE,
                       "CrystalHD: parser picture type %d\n",
                       h->picture_structure);
                pic_type = h->picture_structure;
            }
        }

        if (avpkt->size < tx_free) {
            /*
             * Despite being notionally opaque, either libcrystalhd or
             * the hardware itself will mangle pts values that are too
             * small or too large. The docs claim it should be in units
             * of 100ns. Given that we're nominally dealing with a black
             * box on both sides, any transform we do has no guarantee of
             * avoiding mangling so we need to build a mapping to values
             * we know will not be mangled.
             */
            int64_t safe_pts = avpkt->pts == AV_NOPTS_VALUE ? 0 : avpkt->pts;
            uint64_t pts = opaque_list_push(priv, safe_pts, pic_type);
            if (!pts) {
                ret = AVERROR(ENOMEM);
                goto exit;
            }
            av_log(priv->avctx, AV_LOG_VERBOSE,
                   "input \"pts\": %"PRIu64"\n", pts);
            bc_ret = DtsProcInput(dev, avpkt->data, avpkt->size, pts, 0);
            if (bc_ret == BC_STS_BUSY) {
                av_log(avctx, AV_LOG_WARNING,
                       "CrystalHD: ProcInput returned busy\n");
                ret = AVERROR(EAGAIN);
                goto exit;
            } else if (bc_ret != BC_STS_SUCCESS) {
                av_log(avctx, AV_LOG_ERROR,
                       "CrystalHD: ProcInput failed: %u\n", ret);
                ret = -1;
                goto exit;
            }
        } else {
            av_log(avctx, AV_LOG_VERBOSE, "CrystalHD: Input buffer full\n");
            ret = AVERROR(EAGAIN);
            goto exit;
        }
    } else {
        av_log(avctx, AV_LOG_INFO, "CrystalHD: No more input data\n");
        ret = AVERROR_EOF;
        goto exit;
    }
 exit:
    av_packet_unref(&filtered_packet);
    return ret;
}

static int crystalhd_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    BC_STATUS bc_ret;
    BC_DTS_STATUS decoder_status = { 0, };
    CopyRet rec_ret;
    CHDContext *priv   = avctx->priv_data;
    HANDLE dev         = priv->dev;
    int got_frame = 0;

    av_log(avctx, AV_LOG_VERBOSE, "CrystalHD: receive_frame\n");

    bc_ret = DtsGetDriverStatus(dev, &decoder_status);
    if (bc_ret != BC_STS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "CrystalHD: GetDriverStatus failed\n");
        return -1;
    }

    if (decoder_status.ReadyListCount == 0) {
        av_log(avctx, AV_LOG_INFO, "CrystalHD: Insufficient frames ready. Returning\n");
        return AVERROR(EAGAIN);
    }

    rec_ret = receive_frame(avctx, frame, &got_frame);
    if (rec_ret == RET_ERROR) {
        return -1;
    } else if (got_frame == 0) {
        return AVERROR(EAGAIN);
    } else {
        return 0;
    }
}

#define DEFINE_CRYSTALHD_DECODER(x, X) \
    static const AVClass x##_crystalhd_class = { \
        .class_name = #x "_crystalhd", \
        .item_name = av_default_item_name, \
        .option = options, \
        .version = LIBAVUTIL_VERSION_INT, \
    }; \
    AVCodec ff_##x##_crystalhd_decoder = { \
        .name           = #x "_crystalhd", \
        .long_name      = NULL_IF_CONFIG_SMALL("CrystalHD " #X " decoder"), \
        .type           = AVMEDIA_TYPE_VIDEO, \
        .id             = AV_CODEC_ID_##X, \
        .priv_data_size = sizeof(CHDContext), \
        .priv_class     = &x##_crystalhd_class, \
        .init           = init, \
        .close          = uninit, \
        .send_packet    = crystalhd_decode_packet, \
        .receive_frame  = crystalhd_receive_frame, \
        .flush          = flush, \
        .capabilities   = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING, \
        .pix_fmts       = (const enum AVPixelFormat[]){AV_PIX_FMT_YUYV422, AV_PIX_FMT_NONE}, \
    };

#if CONFIG_H264_CRYSTALHD_DECODER
DEFINE_CRYSTALHD_DECODER(h264, H264)
#endif

#if CONFIG_MPEG2_CRYSTALHD_DECODER
DEFINE_CRYSTALHD_DECODER(mpeg2, MPEG2VIDEO)
#endif

#if CONFIG_MPEG4_CRYSTALHD_DECODER
DEFINE_CRYSTALHD_DECODER(mpeg4, MPEG4)
#endif

#if CONFIG_MSMPEG4_CRYSTALHD_DECODER
DEFINE_CRYSTALHD_DECODER(msmpeg4, MSMPEG4V3)
#endif

#if CONFIG_VC1_CRYSTALHD_DECODER
DEFINE_CRYSTALHD_DECODER(vc1, VC1)
#endif

#if CONFIG_WMV3_CRYSTALHD_DECODER
DEFINE_CRYSTALHD_DECODER(wmv3, WMV3)
#endif
