/*
 * RTMFP network protocol
 * Copyright (c) 2019 Thomas Jammet
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

/**
 * @file
 * RTMFP protocol based on https://github.com/MonaSolutions/librtmfp librtmfp
 */

#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "avformat.h"
#if CONFIG_NETWORK
#include "network.h"
#endif
#include <sys/time.h>

#include <librtmfp/librtmfp.h>

typedef struct LibRTMFPContext {
    const AVClass *class;
    RTMFPConfig rtmfp;
    unsigned int id;
    int audiounbuffered;
    int videounbuffered;
    int p2ppublishing;
    char *peerid;
    char *publication;
    unsigned short streamid;
    const char *swfurl;
    const char *app;
    const char *pageurl;
    const char *flashver;
    const char *host;
    const char *hostipv6;

    // General options
    int socketreceivesize;
    int socketsendsize;

    // NetGroup members
    RTMFPGroupConfig group;
    char *netgroup;
    unsigned int updateperiod;
    unsigned int windowduration;
    unsigned int pushlimit;
    char *fallbackurl;
    unsigned int fallbacktimeout;
    int disableratectl;
} LibRTMFPContext;

static void rtmfp_log(unsigned int level, const char* fileName, long line, const char* message)
{
    const char* strlevel = "";

    switch (level) {
    default:
    case 1: level = AV_LOG_FATAL; strlevel = "FATAL"; break;
    case 2:
    case 3: level = AV_LOG_ERROR; strlevel = "ERROR"; break;
    case 4: level = AV_LOG_WARNING; strlevel = "WARN"; break;
    case 5:
    case 6: level = AV_LOG_INFO; strlevel = "INFO"; break;
    case 7: level = AV_LOG_DEBUG; strlevel = "DEBUG"; break;
    case 8: level = AV_LOG_TRACE; strlevel = "TRACE"; break;
    }

    av_log(NULL, level, "[%s] %s\n", strlevel, message);
}

static int rtmfp_close(URLContext *s)
{
    LibRTMFPContext *ctx = s->priv_data;

    av_log(s, AV_LOG_INFO, "Closing RTMFP connection...\n");
    RTMFP_Close(ctx->id, 0);
    return 0;
}

/**
 * Open RTMFP connection and verify that the stream can be played.
 *
 * URL syntax: rtmp://server[:port][/app][/playpath][ keyword=value]...
 *             where 'app' is first one or two directories in the path
 *             (e.g. /ondemand/, /flash/live/, etc.)
 *             and 'playpath' is a file name (the rest of the path,
 *             may be prefixed with "mp4:")
 *
 *             Additional RTMFP library options may be appended as
 *             space-separated key-value pairs.
 */
static int rtmfp_open(URLContext *s, const char *uri, int flags)
{
    LibRTMFPContext *ctx = s->priv_data;
    int level;

    switch (av_log_get_level()) {
        case AV_LOG_FATAL:   level = 1; break;
        case AV_LOG_ERROR:   level = 3; break;
        case AV_LOG_WARNING: level = 4; break;
        default:
        case AV_LOG_INFO:    level = 6; break;
        case AV_LOG_DEBUG:   level = 7; break;
        case AV_LOG_VERBOSE: level = 8; break;
        case AV_LOG_TRACE:   level = 8; break;
    }

    RTMFP_SetIntParameter("socketReceiveSize", ctx->socketreceivesize);
    RTMFP_SetIntParameter("socketSendSize", ctx->socketsendsize);
    RTMFP_SetIntParameter("timeoutFallback", ctx->fallbacktimeout);
    RTMFP_SetIntParameter("logLevel", level);

    RTMFP_Init(&ctx->rtmfp, &ctx->group, 1);
    ctx->rtmfp.isBlocking = 1;
    ctx->rtmfp.swfUrl = ctx->swfurl;
    ctx->rtmfp.app = ctx->app;
    ctx->rtmfp.pageUrl = ctx->pageurl;
    ctx->rtmfp.flashVer = ctx->flashver;
    ctx->rtmfp.host = ctx->host;
    ctx->rtmfp.hostIPv6 = ctx->hostipv6;

    RTMFP_LogSetCallback(rtmfp_log);
    RTMFP_InterruptSetCallback(s->interrupt_callback.callback, s->interrupt_callback.opaque);

    RTMFP_GetPublicationAndUrlFromUri(uri, &ctx->publication);

    if ((ctx->id = RTMFP_Connect(uri, &ctx->rtmfp)) == 0)
        return -1;

    av_log(s, AV_LOG_INFO, "RTMFP Connect called : %d\n", ctx->id);

    // Wait for connection to happen
    if (RTMFP_WaitForEvent(ctx->id, RTMFP_CONNECTED) == 0)
        return -1;

    if (ctx->netgroup) {
        ctx->group.netGroup = ctx->netgroup;
        ctx->group.availabilityUpdatePeriod = ctx->updateperiod;
        ctx->group.windowDuration = ctx->windowduration;
        ctx->group.pushLimit = ctx->pushlimit;
        ctx->group.isPublisher = (flags & AVIO_FLAG_WRITE) > 1;
        ctx->group.isBlocking = 1;
        ctx->group.disableRateControl = ctx->disableratectl>0;
        ctx->streamid = RTMFP_Connect2Group(ctx->id, ctx->publication, &ctx->rtmfp, &ctx->group, !ctx->audiounbuffered, !ctx->videounbuffered, ctx->fallbackurl);
    } else if (ctx->peerid)
        ctx->streamid = RTMFP_Connect2Peer(ctx->id, ctx->peerid, ctx->publication, 1);
    else if (ctx->p2ppublishing)
        ctx->streamid = RTMFP_PublishP2P(ctx->id, ctx->publication, !ctx->audiounbuffered, !ctx->videounbuffered, 1);
    else if (flags & AVIO_FLAG_WRITE)
        ctx->streamid = RTMFP_Publish(ctx->id, ctx->publication, !ctx->audiounbuffered, !ctx->videounbuffered, 1);
    else
        ctx->streamid = RTMFP_Play(ctx->id, ctx->publication);

    if (!ctx->streamid)
        return -1;

    s->is_streamed = 1;
    return 0;
}

static int rtmfp_write(URLContext *s, const uint8_t *buf, int size)
{
    LibRTMFPContext *ctx = s->priv_data;
    int res;

    res = RTMFP_Write(ctx->id, buf, size);
    return (res < 0)? AVERROR(EIO) : res;
}

static int rtmfp_read(URLContext *s, uint8_t *buf, int size)
{
    LibRTMFPContext *ctx = s->priv_data;
    int res;

    res = RTMFP_Read(ctx->streamid, ctx->id, buf, size);

    return (res < 0)? AVERROR(EIO) : res;
}

#define OFFSET(x) offsetof(LibRTMFPContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    {"socketreceivesize", "Socket receive buffer size", OFFSET(socketreceivesize), AV_OPT_TYPE_INT, {.i64 = 212992}, 0, 0x0FFFFFFF, DEC|ENC},
    {"socketsendsize", "Socket send buffer size", OFFSET(socketsendsize), AV_OPT_TYPE_INT, {.i64 = 212992}, 0, 0x0FFFFFFF, DEC|ENC},
    {"audiounbuffered", "Unbuffered audio mode", OFFSET(audiounbuffered), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, DEC|ENC},
    {"videounbuffered", "Unbuffered video mode", OFFSET(videounbuffered), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, DEC|ENC},
    {"peerid", "Connect to a peer for playing", OFFSET(peerid), AV_OPT_TYPE_STRING, {.str = NULL }, 0, 0, DEC|ENC},
    {"p2ppublishing", "Publish the stream in p2p mode", OFFSET(p2ppublishing), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, DEC|ENC},
    {"netgroup", "NetGroup id to connect or create a p2p multicast group", OFFSET(netgroup), AV_OPT_TYPE_STRING, {.str = NULL }, 0, 0, DEC|ENC},
    {"fallbackurl", "Try to play a unicast stream url until the NetGroup connection is not ready (can produce undefined behavior if the stream codecs are different)",
        OFFSET(fallbackurl), AV_OPT_TYPE_STRING, {.str = NULL }, 0, 0, DEC|ENC},
    {"fallbacktimeout", "Set the timeout in milliseconds to start fallback to unicast", OFFSET(fallbacktimeout), AV_OPT_TYPE_INT, {.i64 = 8000 }, 0, 120000, DEC|ENC},
    {"disableratecontrol", "For Netgroup disable the P2P connection rate control to avoid disconnection", OFFSET(disableratectl), AV_OPT_TYPE_BOOL, {.i64 = 0 }, 0, 1, DEC|ENC},
    {"pushlimit", "Specifies the maximum number (minus one) of peers to which the peer will send push fragments", OFFSET(pushlimit), AV_OPT_TYPE_INT, {.i64 = 4 }, 0, 255, DEC|ENC},
    {"updateperiod", "Interval in milliseconds between media fragments availability messages", OFFSET(updateperiod), AV_OPT_TYPE_INT, {.i64 = 100 }, 100, 10000, DEC|ENC},
    {"windowduration", "Duration in milliseconds of the p2p multicast reassembly window", OFFSET(windowduration), AV_OPT_TYPE_INT, {.i64 = 8000 }, 1000, 60000, DEC|ENC},
    {"rtmfp_swfurl", "URL of the SWF player. By default no value will be sent", OFFSET(swfurl), AV_OPT_TYPE_STRING, {.str = NULL }, 0, 0, DEC|ENC},
    {"rtmfp_app", "Name of application to connect to on the RTMFP server (by default 'live')", OFFSET(app), AV_OPT_TYPE_STRING, {.str = NULL }, 0, 0, DEC|ENC},
    {"rtmfp_pageurl", "URL of the web page in which the media was embedded. By default no value will be sent.", OFFSET(pageurl), AV_OPT_TYPE_STRING, {.str = NULL }, 0, 0, DEC},
    {"rtmfp_flashver", "Version of the Flash plugin used to run the SWF player. By default 'WIN 20,0,0,286'", OFFSET(flashver), AV_OPT_TYPE_STRING, {.str = NULL }, 0, 0, DEC|ENC},
    {"rtmfp_host", "IPv4 host address to bind to (use this if you ave multiple interfaces)", OFFSET(host), AV_OPT_TYPE_STRING, {.str = NULL }, 0, 0, DEC|ENC},
    {"rtmfp_hostipv6", "IPv6 host address to bind to (use this if you ave multiple interfaces)", OFFSET(hostipv6), AV_OPT_TYPE_STRING, {.str = NULL }, 0, 0, DEC|ENC},
    { NULL },
};

static const AVClass librtmfp_class = {
    .class_name = "librtmfp protocol",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

URLProtocol ff_librtmfp_protocol = {
    .name                = "rtmfp",
    .url_open            = rtmfp_open,
    .url_read            = rtmfp_read,
    .url_write           = rtmfp_write,
    .url_close           = rtmfp_close,
    .priv_data_size      = sizeof(LibRTMFPContext),
    .priv_data_class     = &librtmfp_class,
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
};

