/*
 * RTP network protocol
 * Copyright (c) 2002 Fabrice Bellard
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
 * RTP protocol
 */

#include "libavutil/parseutils.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "avformat.h"
#include "avio_internal.h"
#include "rtp.h"
#include "rtpproto.h"
#include "url.h"

#include <stdarg.h>
#include "internal.h"
#include "network.h"
#include "os_support.h"
#include <fcntl.h>
#include "libavcodec/get_bits.h"
#if HAVE_POLL_H
#include <sys/poll.h>
#endif
#include <net/if.h>
#include <cutils/properties.h>
#include <sys/socket.h>
#include <netdb.h>
#include <itemlist.h>
#include <amthreadpool.h>
#include "RS_fec.h"
#include "rtpdec.h"
#include <sys/ioctl.h>
#include <ifaddrs.h>
#include "amconfigutils.h"

#define RTP_TX_BUF_SIZE  (64 * 1024)
#define RTP_RX_BUF_SIZE  (128 * 1024)
#define RTPPROTO_RECVBUF_SIZE 8192
#define MIN_CACHE_PACKET_SIZE 5
#define FEC_PAYLOAD_TYPE_1 127
#define FEC_PAYLOAD_TYPE_2 97
#define TRACE() av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);

extern int64_t av_gettime(void);

#ifndef min
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif
#define FEC_RECVBUF_SIZE 3000
#define MAX_FEC_RTP_PACKET_NUM 300
#define MAX_FEC_PACKET_NUM 10
#define MAX_FEC_MAP_NUM 310

#define EXTRA_BUFFER_PACKET_NUM 20

static int gd_report_error_enable = 0;
static int get_data_timeout_error = 0;
static float get_data_timeout = 0.2;

#define PLAYER_EVENTS_ERROR 3

/* +[SE] [REQ][IPTV-19][jungle.wang]:add fast channel switch module */
static int max_rtp_buf = 10000;//also used for rtp without fcc
static int wait_min_queue_size = 10;
static int wait_order_timeout = 100;    //100ms
static int wait_order_packet_low = 30;
static int sequence_order_range = 500;
static int normal_wait_first_rtcp_timeout = 600;
static int fast_wait_first_rtcp_timeout = 80;
static int wait_first_unicast_packet_timeout = 500; //10Minutes
static int fccread_wait_multicast_sync = 0;
static int threshold_of_read_drop_packet = 0;
static int unicast_data_without_fec_number = -1;
static int igmp3_enable = 0;
static int igmp_version = 0;
static int force_output_packet_num = 0;
static int receive_unicast_max_time = 600000;
static uint16_t first_multi_num = 0;
static int stop_receive_unicast = 0;
static uint16_t out_last_seq_num = 0; // the last seq num out rtp
static int out_packet_num = 0; //the number of packets out rtp
static int fcc_with_fec = 1;

typedef struct FEC_DATA_STRUCT {
    uint16_t rtp_begin_seq;
    uint16_t rtp_end_seq;
    uint8_t redund_num;
    uint8_t redund_idx;
    uint16_t fec_len;
    uint16_t rtp_len;
    uint16_t rsv;
    uint8_t *fec_data;					// point to rtp buffer
} FEC_DATA_STRUCT;

typedef struct RTPFECContext {
    URLContext *rtp_hd, *fec_hd;
    int rtp_fd, fec_fd;
    int pre_fec_lost, pre_fec_lost_last;
    int after_fec_lost, after_fec_lost_last;
    int total_num, total_num_last;
    long last_time;
    int pre_fec_ratio;
    int after_fec_ratio;


    volatile uint8_t brunning;
    pthread_t recv_thread;

    uint8_t bdecode;
    struct itemlist recvlist;
    struct itemlist outlist;
    struct itemlist feclist;
/*
    RtpFccFecPacket *fec_packet[MAX_FEC_PACKET_NUM];
    RtpFccFecPacket *rtp_packet[MAX_FEC_RTP_PACKET_NUM];

    uint8_t *fec_data_array[MAX_FEC_PACKET_NUM];
    uint8_t *rtp_data_array[MAX_FEC_RTP_PACKET_NUM];
    uint8_t lost_map[MAX_FEC_MAP_NUM];
*/
    FEC_DATA_STRUCT * cur_fec;
    uint16_t rtp_last_decode_seq;
    uint16_t rtp_media_packet_sum;
	uint8_t rtp_seq_discontinue;
	uint8_t fec_seq_discontinue;

    T_RS_FEC_MONDE *fec_handle;
    void* bandwidth_measure;

    int direct_output_in_decode;
    int use_multi_and_fec;
    int data_start_fec;
    int fecreport_flag;
} RTPFECContext;

typedef struct RtpFccFecPacket
{
    uint16_t seq;
    uint8_t payload_type;
    uint8_t *buf;                       //recv buffer
    int len;

    FEC_DATA_STRUCT * fec;          // fec struct

    int valid_data_offset;

} RtpFccFecPacket;

static PBYTE fec_data_array[MAX_FEC_PACKET_NUM];
static PBYTE rtp_data_array[MAX_FEC_RTP_PACKET_NUM];
static int lost_map[MAX_FEC_MAP_NUM]; // 0:lost, 1:received
#define MAXQSIZE 6

#define TS_FEC_PACKET_SIZE 204
#define TS_DVHS_PACKET_SIZE 192
#define TS_PACKET_SIZE 188

static int is_mpegts(uint8_t *data, int size)
{
    if (size < 612 )//204*3
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d] size =%d \n", __FUNCTION__, __LINE__, size);
        return 0;
    }

    if ((0x47 == data[0] && 0x47 == data[TS_PACKET_SIZE]  && 0x47 == data[TS_PACKET_SIZE*2])
        || (0x47 == data[0] && 0x47 == data[TS_DVHS_PACKET_SIZE]  && 0x47 == data[TS_DVHS_PACKET_SIZE*2])
        || (0x47 == data[0] && 0x47 == data[TS_FEC_PACKET_SIZE]  && 0x47 == data[TS_FEC_PACKET_SIZE*2]))
    {
        return 1;
    }
    else
        return 0;

    //TBD, add more logic for check valid mpegts

}
static int is_rtp_mpegts(uint8_t *data, int size)
{
    if (size < 12) {
        //Too short to be a valid RTP header.
        return 0;
    }

    if ((data[0] >> 6) != 2) {
        //Currently, the version is 2, if is not 2, unsupported.
        return 0;
    }

    if (data[0] & 0x20) {
        // Padding present.
        uint8_t paddingLength = data[size - 1];
        if (paddingLength + 12 > size) {
            return 0;
        }
        size -= paddingLength;
    }

    int numCSRCs = data[0] & 0x0f;
    int payloadOffset = 12 + 4 * numCSRCs;

    if (size < payloadOffset) {
        // Not enough data to fit the basic header and all the CSRC entries.
        return 0;
    }

    if (data[0] & 0x10) {
        // Header extension present.
        if (size < payloadOffset + 4) {
            // Not enough data to fit the basic header, all CSRC entries and the first 4 bytes of the extension header.
            return 0;
        }

        const uint8_t *extensionData = &data[payloadOffset];
        int extensionLength = 4 * (extensionData[2] << 8 | extensionData[3]);

        if (size < payloadOffset + 4 + extensionLength) {
            return 0;
        }
        payloadOffset += (4 + extensionLength);
    }

    if (33 != (data[1] & 0x7f))
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d] data[1]=%x \n", __FUNCTION__, __LINE__,data[1]);
        return 0;
    }
    //TBD, add more logic for check valid rtp header
    /*
    int rtpTime = data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];
    int srcId = data[8] << 24 | data[9] << 16 | data[10] << 8 | data[11];
    int seqNum = data[2] << 8 | data[3];
    */
    return 1;
}

#define MAXQSIZE 6
typedef struct {
    RTPPacket *base[MAXQSIZE];
    int front;
    int rear;
}SeQueue;//sequencequeue

static int InitQueue(SeQueue *Q)
{
    for (int i = 0; i < MAXQSIZE; i++)
        Q->base[i] = NULL;

    Q->front = Q->rear = 0;
    return 1;
}

static int EmptyQueue(SeQueue Q){

    if (Q.front == Q.rear)
        return 1;
    else
        return 0;
}

static int FullQueue(SeQueue Q){

    if ((Q.rear + 1) % MAXQSIZE == Q.front)
        return 1;
    else
        return 0;
}

static int EnQueue(SeQueue *Q, RTPPacket *e){

    if (FullQueue(*Q))
        return 0;
    Q->base[Q->rear] = e;
    Q->rear = (Q->rear + 1) % MAXQSIZE;
    return 1;
}

static int QueueLength(SeQueue Q){

    return (Q.rear - Q.front + MAXQSIZE) % MAXQSIZE;
}

static int DeQueue(SeQueue *Q, RTPPacket **e){

    if (EmptyQueue(*Q))
    {
        return 0;
    }
    *e = Q->base[Q->front];
    Q->front = (Q->front + 1) % MAXQSIZE;
    return 1;
}

// For RTP + fcc fec
typedef struct {
    RtpFccFecPacket *base[MAXQSIZE];
    int front;
    int rear;
}FccSeQueue;//sequencequeue

static int FccInitQueue(FccSeQueue *Q)
{
    for (int i = 0; i < MAXQSIZE; i++)
        Q->base[i] = NULL;

    Q->front = Q->rear = 0;
    return 1;
}

static int FccEmptyQueue(FccSeQueue Q){

    if (Q.front == Q.rear)
        return 1;
    else
        return 0;
}

static int FccFullQueue(FccSeQueue Q){

    if ((Q.rear + 1) % MAXQSIZE == Q.front)
        return 1;
    else
        return 0;
}

static int FccEnQueue(FccSeQueue *Q, RtpFccFecPacket *e){

    if (FccFullQueue(*Q))
        return 0;
    Q->base[Q->rear] = e;
    Q->rear = (Q->rear + 1) % MAXQSIZE;
    return 1;
}

static int FccQueueLength(FccSeQueue Q){

    return (Q.rear - Q.front + MAXQSIZE) % MAXQSIZE;
}

static int FccDeQueue(FccSeQueue *Q, RtpFccFecPacket **e){

    if (FccEmptyQueue(*Q))
    {
        return 0;
    }
    *e = Q->base[Q->front];
    Q->front = (Q->front + 1) % MAXQSIZE;
    return 1;
}

#define MAX_RTP_SEQ 65536
#define MAX_RTP_SEQ_SPAN 60000
static int seq_greater(int first,int second) {
    if (first == second) {
        return 0;
    }
    else if (abs(first-second) > MAX_RTP_SEQ_SPAN) {
    if (first < second)
        return 1;
    else
        return 0;
    }
    else if (first > second) {
        return 1;
    }
    else
        return 0;
}

static int seq_less(int first,int second) {
    if (first == second) {
        return 0;
    }
    else if (abs(first-second) > MAX_RTP_SEQ_SPAN) {
    if (first>second)
        return 1;
    else
        return 0;
    }
    else if (first < second) {
        return 1;
    }
    else
        return 0;
}

static int seq_greater_and_equal(int first,int second) {
    if (first == second)
        return 1;
    else
        return seq_greater(first,second);
}

static int seq_less_and_equal(int first,int second) {
    if (first == second)
        return 1;
    else
        return seq_less(first,second);
}

static int seq_subtraction(int first,int second) {
    if (first == second) {
        return 0;
    }
    else if(abs(first-second)>MAX_RTP_SEQ_SPAN){
        if (first < second)
            return first + MAX_RTP_SEQ - second;
    else
        return first - second - MAX_RTP_SEQ;
    }
    else {
        return first-second;
    }
}

static int rtp_free_packet(void * apkt)
{
    RTPPacket * lpkt = apkt;
    if (lpkt != NULL)
    {
        if (lpkt->buf != NULL)
            av_free(lpkt->buf);
        av_free(lpkt);
    }
    apkt = NULL;
    return 0;
}

static int rtpfec_free_packet(void * apkt)
{
    RtpFccFecPacket * lpkt = apkt;
    if (lpkt != NULL) {
        if (lpkt->buf != NULL)
        {
            av_free(lpkt->buf);
            lpkt->buf = NULL;
        }
        if (lpkt->fec != NULL)
        {
            av_free(lpkt->fec);
            lpkt->fec = NULL;
        }
        av_free(lpkt);
    }
    apkt = NULL;
    return 0;
}

//static int rtp_enqueue_packet(struct itemlist *itemlist, RtpFccFecPacket* lpkt)

static int rtp_enqueue_packet(struct itemlist *itemlist, RtpFccFecPacket* lpkt, int(*freefun)(void*))

{
    RtpFccFecPacket *ltailpkt=NULL;
    struct item *newitem=NULL;
    RtpFccFecPacket *headpkt=NULL;
    int ret = 0;
    int rt = -1;

    itemlist_peek_tail_data(itemlist, (unsigned long*)&ltailpkt);

    if (NULL == ltailpkt || (ltailpkt != NULL &&seq_less(ltailpkt->seq,lpkt->seq) == 1))
    {
        // append to the tail
        if (NULL != ltailpkt && NULL != lpkt && 1 != (lpkt->seq-ltailpkt->seq & MAX_RTP_SEQ-1))
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d],tailSeq:%d,insertSeq:%d\n", __FUNCTION__, __LINE__,ltailpkt->seq,lpkt->seq);
        }
        ret= itemlist_add_tail_data(itemlist, (unsigned long)lpkt);
        if (ret != 0)
            freefun(lpkt);
        return 0;
    }

    itemlist_peek_head_data(itemlist, (unsigned long*)&headpkt);
    if (headpkt != NULL && seq_less(lpkt->seq, headpkt->seq)) {
        av_log(NULL, AV_LOG_INFO, "[%s:%d],headSeq:%d,insertSeq:%d\n", __FUNCTION__, __LINE__,headpkt->seq,lpkt->seq);
        newitem = item_alloc(itemlist->item_ext_buf_size);
        if (newitem == NULL)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
            freefun(lpkt);
            return -12;//noMEM
        }
        newitem->item_data = (unsigned long)lpkt;
        ITEM_LOCK(itemlist);
        list_add(&(newitem->list), &(itemlist->list));
        itemlist->item_count++;
        ITEM_UNLOCK(itemlist);
        return 0;
    }

    // insert to the queue
    struct item *item = NULL;
    struct item *nextItem = NULL;
    struct list_head *llist=NULL, *tmplist=NULL;
    RtpFccFecPacket *nextRtpPac = NULL;
    RtpFccFecPacket *llistpkt=NULL;
    int CntList = 0;
    char used = 0;

    newitem = item_alloc(itemlist->item_ext_buf_size);
    if (newitem == NULL)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d],CntList:%d\n", __FUNCTION__, __LINE__,CntList);
        freefun(lpkt);
        return -12;//noMEM
    }
    newitem->item_data = (unsigned long)lpkt;


    ITEM_LOCK(itemlist);
    item = list_entry(itemlist->list.next, struct item, list);
    headpkt = (RtpFccFecPacket *)(item->item_data);

    if (seq_less(lpkt->seq,headpkt->seq) == 1)
    {
        // insert to head
        av_log(NULL, AV_LOG_INFO, "[%s:%d],try headSeq:%d,insertSeq:%d\n", __FUNCTION__, __LINE__,headpkt->seq,lpkt->seq);
        list_add(&(newitem->list), &(itemlist->list));
        itemlist->item_count++;
        used = 1;
        goto exit;
    }

    list_for_each_prev_safe(llist, tmplist, &itemlist->list)
    {
        CntList++;
        item = list_entry(llist, struct item, list);
        llistpkt = (RtpFccFecPacket *)(item->item_data);
        if (lpkt->seq == llistpkt->seq)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]The Replication packet, seq=%d\n", __FUNCTION__, __LINE__,lpkt->seq);
            item_free(newitem);
            freefun(lpkt);
            lpkt=NULL;
            used = 1;
            break;
        }
        else if (seq_less(llistpkt->seq, lpkt->seq)==1)
        {
            // insert to front
            if (NULL != nextItem)
            {
                nextRtpPac = (RtpFccFecPacket *)nextItem->item_data;
                av_log(NULL, AV_LOG_INFO, "[%s:%d],middle insert pre:%d,insert:%d,next:%d, item_count:%d\n", __FUNCTION__, __LINE__,llistpkt->seq, lpkt->seq,nextRtpPac->seq, itemlist->item_count);
            } else {
                av_log(NULL, AV_LOG_INFO, "[%s:%d],middle pac,lpkt->seq:%d,llistpkt->seq:%d\n", __FUNCTION__, __LINE__,lpkt->seq,llistpkt->seq);
            }

            list_add(&(newitem->list), &(item->list));
            itemlist->item_count++;
            used = 1;
            break;
        }
        nextItem = item;
    }

    if (!used) {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
        if (!list_empty(&itemlist->list)) {
            item = list_entry(itemlist->list.next, struct item, list);
            headpkt = (RtpFccFecPacket *)(item->item_data);
            av_log(NULL, AV_LOG_INFO, "[%s:%d] insert failed, try check head again! head seq:%d, pkt seq:%d\n", __FUNCTION__, __LINE__, headpkt->seq, lpkt->seq);
            if (seq_subtraction(headpkt->seq, lpkt->seq) != 1) {
                item_free(newitem);
                freefun(lpkt);
                goto exit;
            }
        }
        list_add(&(newitem->list), &(itemlist->list));
        itemlist->item_count++;
    }

exit:
    ITEM_UNLOCK(itemlist);

    return 0;
}

static int rtp_enqueue_packet1(struct itemlist *itemlist, RTPPacket* lpkt, int(*freefun)(void*))

{
    RTPPacket *ltailpkt=NULL;
    struct item *newitem=NULL;
    RTPPacket *headpkt=NULL;
    int ret = 0;
    int rt = -1;

    itemlist_peek_tail_data(itemlist, (unsigned long*)&ltailpkt);

    if (NULL == ltailpkt || (ltailpkt != NULL &&seq_less(ltailpkt->seq,lpkt->seq) == 1))
    {
        // append to the tail
        if (NULL != ltailpkt && NULL != lpkt && 1 != (lpkt->seq-ltailpkt->seq & MAX_RTP_SEQ-1))
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d],tailSeq:%d,insertSeq:%d\n", __FUNCTION__, __LINE__,ltailpkt->seq,lpkt->seq);
        }
        ret= itemlist_add_tail_data(itemlist, (unsigned long)lpkt);
        if (ret != 0)
            freefun(lpkt);
        return 0;
    }

    itemlist_peek_head_data(itemlist, (unsigned long*)&headpkt);
    if (headpkt != NULL && seq_less(lpkt->seq, headpkt->seq)) {
        av_log(NULL, AV_LOG_INFO, "[%s:%d],headSeq:%d,insertSeq:%d\n", __FUNCTION__, __LINE__,headpkt->seq,lpkt->seq);
        newitem = item_alloc(itemlist->item_ext_buf_size);
        if (newitem == NULL)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
            freefun(lpkt);
            return -12;//noMEM
        }
        newitem->item_data = (unsigned long)lpkt;
        ITEM_LOCK(itemlist);
        list_add(&(newitem->list), &(itemlist->list));
        itemlist->item_count++;
        ITEM_UNLOCK(itemlist);
        return 0;
    }

    // insert to the queue
    struct item *item = NULL;
    struct item *nextItem = NULL;
    struct list_head *llist=NULL, *tmplist=NULL;
    RTPPacket *nextRtpPac = NULL;
    RTPPacket *llistpkt=NULL;
    int CntList = 0;
    char used = 0;

    newitem = item_alloc(itemlist->item_ext_buf_size);
    if (newitem == NULL)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d],CntList:%d\n", __FUNCTION__, __LINE__,CntList);
        freefun(lpkt);
        return -12;//noMEM
    }
    newitem->item_data = (unsigned long)lpkt;


    ITEM_LOCK(itemlist);
    item = list_entry(itemlist->list.next, struct item, list);
    headpkt = (RTPPacket *)(item->item_data);

    if (seq_less(lpkt->seq,headpkt->seq) == 1)
    {
        // insert to head
        av_log(NULL, AV_LOG_INFO, "[%s:%d],try headSeq:%d,insertSeq:%d\n", __FUNCTION__, __LINE__,headpkt->seq,lpkt->seq);
        list_add(&(newitem->list), &(itemlist->list));
        itemlist->item_count++;
        used = 1;
        goto exit;
    }

    list_for_each_prev_safe(llist, tmplist, &itemlist->list)
    {
        CntList++;
        item = list_entry(llist, struct item, list);
        llistpkt = (RTPPacket *)(item->item_data);
        if (lpkt->seq == llistpkt->seq)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]The Replication packet, seq=%d\n", __FUNCTION__, __LINE__,lpkt->seq);
            item_free(newitem);
            freefun(lpkt);
            lpkt=NULL;
            used = 1;
            break;
        }
        else if (seq_less(llistpkt->seq, lpkt->seq)==1)
        {
            // insert to front
            if (NULL != nextItem)
            {
                nextRtpPac = (RTPPacket *)nextItem->item_data;
                av_log(NULL, AV_LOG_INFO, "[%s:%d],middle insert pre:%d,insert:%d,next:%d, item_count:%d\n", __FUNCTION__, __LINE__,llistpkt->seq, lpkt->seq,nextRtpPac->seq, itemlist->item_count);
            } else {
                av_log(NULL, AV_LOG_INFO, "[%s:%d],middle pac,lpkt->seq:%d,llistpkt->seq:%d\n", __FUNCTION__, __LINE__,lpkt->seq,llistpkt->seq);
            }

            list_add(&(newitem->list), &(item->list));
            itemlist->item_count++;
            used = 1;
            break;
        }
        nextItem = item;
    }

    if (!used) {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
        if (!list_empty(&itemlist->list)) {
            item = list_entry(itemlist->list.next, struct item, list);
            headpkt = (RTPPacket *)(item->item_data);
            av_log(NULL, AV_LOG_INFO, "[%s:%d] insert failed, try check head again! head seq:%d, pkt seq:%d\n", __FUNCTION__, __LINE__, headpkt->seq, lpkt->seq);
            if (seq_subtraction(headpkt->seq, lpkt->seq) != 1) {
                item_free(newitem);
                freefun(lpkt);
                goto exit;
            }
        }
        list_add(&(newitem->list), &(itemlist->list));
        itemlist->item_count++;
    }

exit:
    ITEM_UNLOCK(itemlist);

    return 0;
}


static int64_t ff_network_gettime(void)
{
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static int FreeSavedRtpPacket(SeQueue *Q){
      RTPPacket * savedlpkt = NULL;
      while (DeQueue(Q, &savedlpkt))
      {
           rtp_free_packet((void *)savedlpkt);
      }
      return 1;
}

static void ConstructSavedRtpPacket(RTPPacket **savedlpkt)
{
    uint8_t * lpoffset=NULL;
    int offset=0;
    uint8_t * lpkt_buf=NULL;
    int len=0;
    int ext=0;
    int csrc = 0;

    (*savedlpkt)->seq = AV_RB16((*savedlpkt)->buf + 2);
    lpkt_buf=(*savedlpkt)->buf;
    len=(*savedlpkt)->len;

    // output the playload data
    offset = 12 ;
    lpoffset = lpkt_buf + 12;
    csrc = lpkt_buf[0] & 0x0f;
    ext = lpkt_buf[0] & 0x10;
    if (ext > 0)
    {
        offset += 4*csrc;
        lpoffset += 4*csrc;
        if (len < offset + 4)
        {
            av_log(NULL, AV_LOG_ERROR, "[%s:%d]len < offset + 4\n",__FUNCTION__,__LINE__);
        }

        ext = (AV_RB16(lpoffset + 2) + 1) << 2;
        if (len < ext + offset)
        {
            av_log(NULL, AV_LOG_ERROR, "[%s:%d]len < ext + offset\n",__FUNCTION__,__LINE__);
        }
        offset+=ext;
        lpoffset+=ext;
        }
        (*savedlpkt)->valid_data_offset=offset;

}

static int FccFreeSavedRtpPacket(FccSeQueue *Q){
      RtpFccFecPacket * savedlpkt = NULL;
      while (FccDeQueue(Q, &savedlpkt))
      {
           rtp_free_packet((void *)savedlpkt);
      }
      return 1;
}

static void FccConstructSavedRtpPacket(RtpFccFecPacket **savedlpkt)
{
    uint8_t * lpoffset=NULL;
    int offset=0;
    uint8_t * lpkt_buf=NULL;
    int len=0;
    int ext=0;
    int csrc = 0;

    (*savedlpkt)->seq = AV_RB16((*savedlpkt)->buf + 2);
    lpkt_buf=(*savedlpkt)->buf;
    len=(*savedlpkt)->len;

    // output the playload data
    offset = 12 ;
    lpoffset = lpkt_buf + 12;
    csrc = lpkt_buf[0] & 0x0f;
    ext = lpkt_buf[0] & 0x10;
    if (ext > 0)
    {
        offset += 4*csrc;
        lpoffset += 4*csrc;
        if (len < offset + 4)
        {
            av_log(NULL, AV_LOG_ERROR, "[%s:%d]len < offset + 4\n",__FUNCTION__,__LINE__);
        }

        ext = (AV_RB16(lpoffset + 2) + 1) << 2;
        if (len < ext + offset)
        {
            av_log(NULL, AV_LOG_ERROR, "[%s:%d]len < ext + offset\n",__FUNCTION__,__LINE__);
        }
        offset+=ext;
        lpoffset+=ext;
        }
        (*savedlpkt)->valid_data_offset=offset;

}


typedef struct ContextItem
{
    int Fd;
    //-1 stop;0,init;1,soket setup
    //multicast:1,has join multicast;2,has left multicast
    //unicast:2,setup unicast stream socket;3,receive unicast stream;
    //signalling:2,had redirected, 4,have sent bye cmd;
    volatile int8_t Status;
    int LastSeqNum;
    int firstSeqNum;
    RtpFccFecPacket* bak_pkt;
    char stopReceive;
    uint32_t Cnt;
    //server
    uint32_t Ip;
    uint16_t Port;
    uint32_t Ipv6[4];
    char StrIp[256];
    char StrPort[50];
    //local
    uint16_t LocalPort;
    URLContext *Uc;
}ContextItem;

typedef enum {
    FCC_NORMAL_CONNECTING = 0,
    FCC_FAST_CONNECTING,
    FCC_CONNECT_FINISH
}FccConnectState;

/* We have two interruption reports in FCC, using the same flag, in order to distinguish between the two reports.
3 and 4 are used when data cannot be read completely.
If no multicast packet is received, use 1 and 2 */
typedef enum
{
    FCC_REPORT_NONE = 0,
    FCC_REPORT_MULTI_CUTOFF, // only multicase stream
    FCC_REPORT_MULTI_RECOVER,
    FCC_REPORT_CUTOFF,  // all stream, unicast & multicase
    FCC_REPORT_RECOVER
} FccReportState;

typedef enum {
    FCC_telecom         = 0, // telecom:       FMT 2,3,4,5
    FCC_huawei_value    = 1, // huawei value:  FMT 5,6,7,8,9
    FCC_huawei_tlv      = 2, // huawei TLV:    FMT 5,6,7,8,9
    FCC_fiberhome         = 3, // fenghuo:       FMT 2,3,4,5,  Commands are the same as telecom. But NAT is supported, IP address should be entered in the SSRC field
}FCC_VERSION;

static RtpFccFecPacket *fec_packet[MAX_FEC_PACKET_NUM];
static RtpFccFecPacket *rtp_packet[MAX_FEC_RTP_PACKET_NUM];

typedef struct RtpFccContext
{
    pthread_t RecvThread;
    //0,init;1,creat success;2,creat fail;3,during receive;4,sth wrong;5,process over;0xff quit receive loop.
    volatile int8_t ThreadStatus;
    struct itemlist Recvlist;
    struct item *CurItem;
    int FirstMulticastSeq;
    int LastSeqNum;
    //three socket context
    ContextItem Unicast;
    ContextItem Multicast;
    ContextItem MulticastAndFec;
    ContextItem Signalling;
    ContextItem *CurSock;
    unsigned int try_direct_read;
    char first_packet_get;
    char first_packet_read;
    int first_rtcp_request;
    char first_rtcp_response;
    int64_t first_rtcp_send_time;
    int unicast_packet_received;
    int64_t receive_unicast_begin_time;
    int64_t last_receive_multicast_time;
    int fccreport_flag;
    char url[MAX_URL_SIZE];
    int flags;
    int network_down;
    FccConnectState connectState;

    RTPFECContext *feccontext;

    int output_number;

    FCC_VERSION FCC_Version;
    uint32_t local_Ip;
    uint32_t local_Ipv6[4];
    char local_StrIpl[256];
    uint32_t Client_identifier;
    uint16_t First_Unicast_Seq;
    uint16_t Bitrate;
    int FCC_Server_validtime_default;  //second

    int isIpv6;
    int isMultiIpv6;
    int Response_state;
    int64_t last_readtime;
    uint32_t source_Ip; // add for IGMP V3
    char source_StrIpl[256];
    int alreadyNatTraversaled; // traversal flag
} RtpFccContext;

static int check_net_phy_conn_status(void)
{
    int nNetDownOrUp = am_getconfig_int_def("net.ethwifi.up",3);//0-eth&wifi both down, 1-eth up, 2-wifi up, 3-eth&wifi both up

    return nNetDownOrUp;
}

int judge_seq_discontinuity(int seq1, int seq2, int seq3);
int parse_rtp_ts_packet(RtpFccFecPacket* lpkt);

static int check_ip_string(const char* hostname, int size) {
    if (hostname == NULL || size <= 0)
        return -1;

    int i = 0;
    int dot = 0;
    while (i < size) {
        if (hostname[i] != '.' && (hostname[i] > '9' || hostname[i] < '0')) {
            return 0;
        } else if (hostname[i] == '.') {
            dot++;
        }
        i++;
    }
    if (dot != 3)
        return 0;
    int a = 0;
    int b = 0;
    int c = 0;
    int d = 0;

    int ret = sscanf(hostname, "%d.%d.%d.%d", &a, &b , &c, &d);
    if (ret == 4
        && (a >= 0 && a <= 255)
        && (b >= 0 && b <= 255)
        && (c >= 0 && c <= 255)
        && (d >= 0 && d <= 255)) {
        return 1;
    }
    return 0;
}


static inline time_t getMonotonicTime()
{
    struct timespec tc = {0};
    int ret = clock_gettime(CLOCK_MONOTONIC, &tc);

    return tc.tv_sec;
}

static FccConnectState initFccConnectState(const char* debug_str)
{
    int normal_mode = am_getconfig_int_def("libplayer.rtpfcc.normal", 1);
    av_log(NULL, AV_LOG_INFO, "[%s:%d]mode:%d\n", __FUNCTION__, __LINE__, normal_mode);
    return normal_mode? FCC_NORMAL_CONNECTING:FCC_FAST_CONNECTING;
}

static int SetupUdpSocket(URLContext **puc,char *StrIp,char *StrPort,int Port,int LocalPort,int flags, const AVIOInterruptCB *int_cb);
static int MakeNewRtcpPac(RtpFccContext *Rfc,uint8_t *BufPac,uint8_t Fmt,int Fmps);
static int fcc_url_write(URLContext *ctx, const unsigned char* buf, int len, int n);

typedef enum {
    FCCFMT_NULL = 0,
    FCCFMT_RSR = 5,         //RTCP Rapid Synchronization Request
    FCCFMT_RSI = 6,         //RTCP Rapid Synchronization Indication
    FCCFMT_SCN = 8,         //RTCP Synchronization Completed Notification
    FCCFMT_SCR = 9,         //RTCP Synchronization Completed Response
    FCCFMT_NAT = 12,        //NAT
}FCCFMT;
static int SendRTCPPacHW(RtpFccContext *Rfc, FCCFMT Fmt);

static int fccNormalStart(RtpFccContext *s)
{
    //new signalling socket
    if (NULL != s->Signalling.Uc)
    {
        ffurl_close(s->Signalling.Uc);
        s->Signalling.Fd = -1;
        s->Signalling.Uc = NULL;
    }

    int ret = SetupUdpSocket(&s->Signalling.Uc, s->Signalling.StrIp, s->Signalling.StrPort, s->Signalling.Port,-1,0, NULL);
    if (ret < 0) {
        return ret;
    }

    s->Signalling.Fd = ffurl_get_file_handle(s->Signalling.Uc);
    av_log(NULL, AV_LOG_INFO, "[%s:%d],s->Signalling.Fd:%d\n", __FUNCTION__, __LINE__,s->Signalling.Fd);
    s->Signalling.LocalPort =ff_udp_get_local_port(s->Signalling.Uc);
    s->Unicast.LocalPort = s->Signalling.LocalPort-1;
    av_log(NULL, AV_LOG_INFO, "[%s:%d],s->Signalling.LocalPort:%d,s->Unicast.LocalPort:%d\n", __FUNCTION__,__LINE__,s->Signalling.LocalPort,s->Unicast.LocalPort);
    //
    s->Signalling.Uc->flags = AVIO_FLAG_READ_WRITE;
    s->Signalling.Status = 1;

    if (NULL != s->Unicast.Uc) {
        ffurl_close(s->Unicast.Uc);
        s->Unicast.Uc = NULL;
        s->Unicast.Fd = -1;
    }

    av_log(NULL, AV_LOG_INFO, "[%s:%d]create unicast socket!\n", __FUNCTION__, __LINE__);
    //setup the unicast socket to receive the unicast stream //unicast stream local socket
    s->Unicast.Port = 0;
    ret = SetupUdpSocket(&s->Unicast.Uc, "", "", 0, s->Unicast.LocalPort,1,NULL);
    if (0 == ret)
    {
        s->Unicast.Fd = ffurl_get_file_handle(s->Unicast.Uc);
        s->Unicast.Status = 1;
        av_log(NULL, AV_LOG_INFO, "[%s:%d],s->Unicast.Fd:%d,s->Status:%d\n", __FUNCTION__, __LINE__,s->Unicast.Fd,s->Unicast.Status);
    }
    else
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d],build unicast socekt fail\n", __FUNCTION__, __LINE__);
    }

    //send rtcp request
    uint8_t RtcpPac[40];
    uint32_t RtcpLen = 40;
    if (s->FCC_Version == FCC_telecom || s->FCC_Version == FCC_fiberhome)
    {
        MakeNewRtcpPac(s, RtcpPac, 2, -1);
        av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
        ret = fcc_url_write(s->Signalling.Uc, RtcpPac, RtcpLen, 2);
        av_log(NULL, AV_LOG_INFO, "[%s:%d],ret:%d\n", __FUNCTION__, __LINE__, ret);
    }
    else {
        ret = SendRTCPPacHW(s, FCCFMT_RSR);
    }


    s->first_rtcp_send_time = av_gettime();
    s->first_rtcp_response = 0;

    return ret;
}

static int SendByeRtcp(RtpFccContext *Rfc,int LastSeq);
static void onFccFastStartFailure(RtpFccContext* s)
{
    SendByeRtcp(s, -1);


    s->Signalling.Status = 0;
    s->Unicast.Status = 0;
    s->Multicast.Status = 0;

    fccNormalStart(s);
}

/**
 * @brief fcc channel cache code start
 * use list as a container, like map;
 * (multicast_ip+multicast_port) stands for a channel, as map key
 * other as a map value.
 */
typedef struct _fcc_directed_node
{
    struct list_head m_list;
    union
    {
        struct in_addr ip4;
        struct in6_addr ip6;
    } multicast_ip;
    union
    {
        struct in_addr ip4;
        struct in6_addr ip6;
    } redirect_ip;
    uint16_t multicast_port;
    uint16_t redirect_port;
    uint16_t redirect_data_port;
    int validtime;
    int expiredtime;
} fcc_directed_node_t;

//g_aryChannelCache is a (channel:directed fcc server) map
static struct itemlist g_aryChannelCache;

static void channelcache_once_fun()
{
    struct itemlist* list = &g_aryChannelCache;
    list->max_items = am_getconfig_int_def("media.amplayer.fcc_cache_count", 100);
    list->item_ext_buf_size = 0;
    list->muti_threads_access = 0;
    list->reject_same_item_data = 0;
    itemlist_init(list);
}

//channel cache init, Can only be called once
static int channelcache_init(struct itemlist* list)
{
    static pthread_once_t init_once_20200909 = PTHREAD_ONCE_INIT;
    pthread_once(&init_once_20200909, channelcache_once_fun);
    return 0;
}

//insert new node to list
static int channelcache_add(struct itemlist* list, uint8_t* multicast_ip, uint16_t multicast_port,
    uint8_t* redirect_ip, uint16_t redirect_port, uint16_t redirect_data_port, int validtime, int isipv6, int isMultiIpv6)
{
    fcc_directed_node_t* node_1 = NULL;
    struct item *item = NULL;
    struct list_head* llist = NULL, *templist = NULL;
    //find if exist the channel(multicast_ip+multicast_port)
    list_for_each_safe(llist, templist, &list->list)
    {
        item = list_entry(llist, struct item, list);
        fcc_directed_node_t* node = (fcc_directed_node_t*)(item->item_data);
        if (node && node->multicast_port == multicast_port)
        {
            if (isMultiIpv6) {
                if (memcmp(&node->multicast_ip.ip6, multicast_ip, sizeof(struct in6_addr)) == 0)
                {
                    node_1 = node;
                    break;
                }
            } else {
                if (memcmp(&node->multicast_ip.ip4.s_addr, multicast_ip, sizeof(struct in_addr)) == 0)
                {
                    node_1 = node;
                    break;
                }
            }
        }
    }
    if (node_1 == NULL)
    {
        //if not exist this channel, will make new node and insert
        node_1 = av_mallocz(sizeof(fcc_directed_node_t));
        node_1->multicast_port = multicast_port;
        if (isMultiIpv6) {
            memcpy(&node_1->multicast_ip.ip6, multicast_ip, sizeof(struct in6_addr));
        } else {
            memcpy(&node_1->multicast_ip.ip4, multicast_ip, sizeof(struct in_addr));
        }
        if (isipv6) {
            memcpy(&node_1->redirect_ip.ip6, redirect_ip, sizeof(struct in6_addr));
        } else {
            memcpy(&node_1->redirect_ip.ip4, redirect_ip, sizeof(struct in_addr));
        }
        node_1->redirect_port = redirect_port;
        node_1->redirect_data_port = redirect_data_port;
        node_1->validtime = validtime;
        node_1->expiredtime = getMonotonicTime() + validtime;
        itemlist_add_tail_data(list, (unsigned long)node_1);
    }
    else
    {
        //if exist,will update this channel data
        if (isipv6) {
            memcpy(&node_1->redirect_ip.ip6, redirect_ip, sizeof(struct in6_addr));
        } else {
            memcpy(&node_1->redirect_ip.ip4, redirect_ip, sizeof(struct in_addr));
        }
        node_1->redirect_port = redirect_port;
        node_1->redirect_data_port = redirect_data_port;
        node_1->validtime = validtime;
        node_1->expiredtime = getMonotonicTime() + validtime;
    }

    return 0;
}

//channelcache_add overload, pass ip as string
static int channelcache_add2(struct itemlist* list, const char* s_multicast_ip, uint16_t multicast_port,
    const char* s_redirect_ip, uint16_t redirect_port, uint16_t redirect_data_port, int validtime, int isipv6, int isMultiIpv6)
{
    int ret = 0;
    uint8_t *multicast_ip = NULL;
    uint8_t *redirect_ip = NULL;

    if (isMultiIpv6) {
        multicast_ip = (uint8_t *)av_malloc(sizeof(struct in6_addr));
        ret = inet_pton(AF_INET6, s_multicast_ip, multicast_ip);
    }
    else {
        multicast_ip = (uint8_t *)av_malloc(sizeof(struct in_addr));
        ret = inet_pton(AF_INET, s_multicast_ip, multicast_ip);
    }

    if (isipv6)
    {
        redirect_ip = (uint8_t *)av_malloc(sizeof(struct in6_addr));
        ret = inet_pton(AF_INET6, s_redirect_ip, redirect_ip);
    }
    else {
        redirect_ip = (uint8_t *)av_malloc(sizeof(struct in_addr));
        ret = inet_pton(AF_INET, s_redirect_ip, redirect_ip);
    }

    ret = channelcache_add(list, multicast_ip, multicast_port, redirect_ip, redirect_port, redirect_data_port, validtime, isipv6, isMultiIpv6);
    if (multicast_ip)
    {
        av_freep(&multicast_ip);
    }
    if (redirect_ip)
    {
        av_freep(&redirect_ip);
    }
    return ret;
}

//get channel redirect info
static fcc_directed_node_t *channelcache_get(struct itemlist *list, uint8_t *multicast_ip, uint16_t multicast_port, int isMultiIpv6)
{
    struct item *item = NULL;
    struct list_head* llist = NULL, *templist = NULL;
    list_for_each_safe(llist, templist, &list->list)
    {
        item = list_entry(llist, struct item, list);
        fcc_directed_node_t* node = (fcc_directed_node_t*)(item->item_data);
        if (node && node->multicast_port == multicast_port)
        {
            int ipsame = 0;
            if (isMultiIpv6) {
                if (memcmp(&node->multicast_ip.ip6, multicast_ip, sizeof(struct in6_addr)) == 0)
                    ipsame = 1;
            } else {
                if (memcmp(&node->multicast_ip.ip4.s_addr, multicast_ip, sizeof(struct in_addr)) == 0)
                    ipsame = 1;
            }
            if (ipsame)
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d] node ok.", __FUNCTION__, __LINE__);
                if (getMonotonicTime() < node->expiredtime) {
                    return (fcc_directed_node_t*)item->item_data;
                } else {
                    av_log(NULL, AV_LOG_INFO, "[%s:%d] out expiredtime", __FUNCTION__, __LINE__);
                }
            } else if (node) {
                av_log(NULL, AV_LOG_INFO, "[%s:%d] node pass one", __FUNCTION__, __LINE__);

            }
        }
    }
    return NULL;
}

//channelcache_get overload, pass ip as string
static fcc_directed_node_t *channelcache_get2(struct itemlist *list, char *s_multicast_ip, uint16_t multicast_port, int isMultiIpv6)
{
    int ret;
    if (isMultiIpv6) {
        uint8_t multicast_ip[sizeof(struct in6_addr)];
        av_log(NULL, AV_LOG_INFO, "[%s:%d] s_multicast_ip:%s, multicast_port:%d", __FUNCTION__, __LINE__,
               s_multicast_ip, multicast_port);
        ret = inet_pton(AF_INET6, s_multicast_ip, multicast_ip);
        return channelcache_get(list, &multicast_ip, multicast_port, isMultiIpv6);
    } else {
        uint8_t multicast_ip[sizeof(struct in_addr)];
        av_log(NULL, AV_LOG_INFO, "[%s:%d] s_multicast_ip:%s, multicast_port:%d", __FUNCTION__, __LINE__,
            s_multicast_ip, multicast_port);
        ret = inet_pton(AF_INET, s_multicast_ip, multicast_ip);
        return channelcache_get(list, &multicast_ip, multicast_port, isMultiIpv6);
    }
}

//remove timeout redirect cache node
static int channelcache_refresh(struct itemlist* list)
{
    struct item *item = NULL;
    struct list_head* llist = NULL, *templist = NULL;
    list_for_each_safe(llist, templist, &list->list)
    {
        struct item* item = list_entry(llist, struct item, list);
        fcc_directed_node_t* node = (fcc_directed_node_t*)(item->item_data);
        if (node && getMonotonicTime() > node->expiredtime)
        {
            char ip[2][INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &node->multicast_ip.ip4, ip[0], INET_ADDRSTRLEN);
            inet_ntop(AF_INET, &node->redirect_ip.ip4, ip[1], INET_ADDRSTRLEN);
            av_log(NULL, AV_LOG_INFO, "channel cache node timeout, (%s:%d):(%s:%d)\n", ip[0], node->multicast_port,
                ip[1], node->redirect_port);
            itemlist_del_item_locked(list, item);
        }
    }
    return 0;
}

//for debug, print all node in g_aryChannelCache
static int channelcache_print(struct itemlist* list)
{
    struct item *item = NULL;
    struct list_head* llist = NULL, *templist = NULL;
    av_log(NULL, AV_LOG_INFO,"enter------------------------%d\n", list->item_count);
    list_for_each_safe(llist, templist, &list->list)
    {
        item = list_entry(llist, struct item, list);
        fcc_directed_node_t* node = (fcc_directed_node_t*)(item->item_data);
        char ip[2][INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &node->multicast_ip.ip4, ip[0], INET_ADDRSTRLEN);
        inet_ntop(AF_INET, &node->redirect_ip.ip4, ip[1], INET_ADDRSTRLEN);
        av_log(NULL, AV_LOG_INFO,"(%s:%d):(%s:%d), validtime:%d\n", ip[0], node->multicast_port,
            ip[1], node->redirect_port, node->validtime);
    }
    av_log(NULL, AV_LOG_INFO,"leave------------------------%d\n", list->item_count);
    return 0;
}
/**
 * @brief fcc channel cache code end
 */


/* -[SE] [REQ][IPTV-19][jungle.wang]:add fast channel switch module */

static int rtpfcc_close(URLContext *h);
static int init_def_settings()
{
    static int inited =0;
    if (inited>0)
        return 0;
    inited++;
    int ret = 0;
    int ecode = am_getconfig_int_def("media.player.errorcode", 0);
    gd_report_error_enable = ((ecode > 0) || (int)am_getconfig_bool_def("media.player.cmcc_report.enable",0));
    if (ecode > 0)
        get_data_timeout_error = ecode;
    else if (am_getconfig_bool_def("media.player.cmcc_report.enable",0))
        get_data_timeout_error = 10002;
    ret = am_getconfig_float("media.player.read_report.timeout", &get_data_timeout);
    if (ret < 0 || get_data_timeout < 0)
        get_data_timeout = 0.2;
    av_log(NULL, AV_LOG_INFO, "get_data_timeout=%f\n", get_data_timeout);
 //   rtp_queue_cnt = am_getconfig_int_def("media.player.rtpqueue",5);
    max_rtp_buf = am_getconfig_int_def("media.amplayer.rtp_max",10000);
    igmp_version = am_getconfig_int_def("sys.player.igmpversion", 2);
    igmp3_enable = am_getconfig_int_def("libplayer.igmp3.enable", 0);
    av_log(NULL, AV_LOG_ERROR, "udp config: gd_report enable:%d\n\n", gd_report_error_enable);
    av_log(NULL, AV_LOG_INFO, "get_data timeout error=%d get_data_timeout:%fs,max_rtp_buf:%d\n",get_data_timeout_error,get_data_timeout,max_rtp_buf);
    return 0;
}

typedef struct RTPContext {
    const AVClass *class;
    URLContext *rtp_hd, *rtcp_hd, *fec_hd;
    int rtp_fd, rtcp_fd, nb_ssm_include_addrs, nb_ssm_exclude_addrs;
    struct sockaddr_storage **ssm_include_addrs, **ssm_exclude_addrs;
    int write_to_source;
    struct sockaddr_storage last_rtp_source, last_rtcp_source;
    socklen_t last_rtp_source_len, last_rtcp_source_len;
    volatile uint8_t brunning;
    pthread_t recv_thread;
    struct itemlist recvlist;
    struct item *cur_item;
    ContextItem Signalling;
    int last_seq;
    int report_flag;
    int ttl;
    int buffer_size;
    int rtcp_port, local_rtpport, local_rtcpport;
    int connect;
    int pkt_size;
    int dscp;
    char *sources;
    char *block;
    char *fec_options_str;
    int64_t rw_timeout;
} RTPContext;

#define OFFSET(x) offsetof(RTPContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "ttl",                "Time to live (in milliseconds, multicast only)",                   OFFSET(ttl),             AV_OPT_TYPE_INT,    { .i64 = -1 },    -1, INT_MAX, .flags = D|E },
    { "buffer_size",        "Send/Receive buffer size (in bytes)",                              OFFSET(buffer_size),     AV_OPT_TYPE_INT,    { .i64 = -1 },    -1, INT_MAX, .flags = D|E },
    { "rtcp_port",          "Custom rtcp port",                                                 OFFSET(rtcp_port),       AV_OPT_TYPE_INT,    { .i64 = -1 },    -1, INT_MAX, .flags = D|E },
    { "local_rtpport",      "Local rtp port",                                                   OFFSET(local_rtpport),   AV_OPT_TYPE_INT,    { .i64 = -1 },    -1, INT_MAX, .flags = D|E },
    { "local_rtcpport",     "Local rtcp port",                                                  OFFSET(local_rtcpport),  AV_OPT_TYPE_INT,    { .i64 = -1 },    -1, INT_MAX, .flags = D|E },
    { "connect",            "Connect socket",                                                   OFFSET(connect),         AV_OPT_TYPE_BOOL,   { .i64 =  0 },     0, 1,       .flags = D|E },
    { "write_to_source",    "Send packets to the source address of the latest received packet", OFFSET(write_to_source), AV_OPT_TYPE_BOOL,   { .i64 =  0 },     0, 1,       .flags = D|E },
    { "pkt_size",           "Maximum packet size",                                              OFFSET(pkt_size),        AV_OPT_TYPE_INT,    { .i64 = -1 },    -1, INT_MAX, .flags = D|E },
    { "dscp",               "DSCP class",                                                       OFFSET(dscp),            AV_OPT_TYPE_INT,    { .i64 = -1 },    -1, INT_MAX, .flags = D|E },
    { "timeout",            "set timeout (in microseconds) of socket I/O operations",           OFFSET(rw_timeout),      AV_OPT_TYPE_INT64,  { .i64 = 30000000 },  -1, INT64_MAX, .flags = D|E },
    { "sources",            "Source list",                                                      OFFSET(sources),         AV_OPT_TYPE_STRING, { .str = NULL },               .flags = D|E },
    { "block",              "Block list",                                                       OFFSET(block),           AV_OPT_TYPE_STRING, { .str = NULL },               .flags = D|E },
    { "fec",                "FEC",                                                              OFFSET(fec_options_str), AV_OPT_TYPE_STRING, { .str = NULL },               .flags = E },
    { NULL }
};

static const AVClass rtp_class = {
    .class_name = "rtp",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/**
 * If no filename is given to av_open_input_file because you want to
 * get the local port first, then you must call this function to set
 * the remote server address.
 *
 * @param h media file context
 * @param uri of the remote server
 * @return zero if no error.
 */

int ff_rtp_set_remote_url(URLContext *h, const char *uri)
{
    RTPContext *s = h->priv_data;
    char hostname[256];
    int port, rtcp_port;
    const char *p;

    char buf[1024];
    char path[1024];

    av_url_split(NULL, 0, NULL, 0, hostname, sizeof(hostname), &port,
                 path, sizeof(path), uri);
    rtcp_port = port + 1;

    p = strchr(uri, '?');
    if (p) {
        if (av_find_info_tag(buf, sizeof(buf), "rtcpport", p)) {
            rtcp_port = strtol(buf, NULL, 10);
        }
    }

    ff_url_join(buf, sizeof(buf), "udp", NULL, hostname, port, "%s", path);
    ff_udp_set_remote_url(s->rtp_hd, buf);

    ff_url_join(buf, sizeof(buf), "udp", NULL, hostname, rtcp_port, "%s", path);
    ff_udp_set_remote_url(s->rtcp_hd, buf);
    return 0;
}

static struct addrinfo* rtp_resolve_host(const char *hostname, int port,
                                         int type, int family, int flags)
{
    struct addrinfo hints = { 0 }, *res = 0;
    int error;
    char service[16];

    snprintf(service, sizeof(service), "%d", port);
    hints.ai_socktype = type;
    hints.ai_family   = family;
    hints.ai_flags    = flags;
    if ((error = getaddrinfo(hostname, service, &hints, &res))) {
        res = NULL;
        av_log(NULL, AV_LOG_ERROR, "rtp_resolve_host: %s\n", gai_strerror(error));
    }

    return res;
}

static int compare_addr(const struct sockaddr_storage *a,
                        const struct sockaddr_storage *b)
{
    if (a->ss_family != b->ss_family)
        return 1;
    if (a->ss_family == AF_INET) {
        return (((const struct sockaddr_in *)a)->sin_addr.s_addr !=
                ((const struct sockaddr_in *)b)->sin_addr.s_addr);
    }

#if HAVE_STRUCT_SOCKADDR_IN6
    if (a->ss_family == AF_INET6) {
        const uint8_t *s6_addr_a = ((const struct sockaddr_in6 *)a)->sin6_addr.s6_addr;
        const uint8_t *s6_addr_b = ((const struct sockaddr_in6 *)b)->sin6_addr.s6_addr;
        return memcmp(s6_addr_a, s6_addr_b, 16);
    }
#endif
    return 1;
}

static int get_port(const struct sockaddr_storage *ss)
{
    if (ss->ss_family == AF_INET)
        return ntohs(((const struct sockaddr_in *)ss)->sin_port);
#if HAVE_STRUCT_SOCKADDR_IN6
    if (ss->ss_family == AF_INET6)
        return ntohs(((const struct sockaddr_in6 *)ss)->sin6_port);
#endif
    return 0;
}

static void set_port(struct sockaddr_storage *ss, int port)
{
    if (ss->ss_family == AF_INET)
        ((struct sockaddr_in *)ss)->sin_port = htons(port);
#if HAVE_STRUCT_SOCKADDR_IN6
    else if (ss->ss_family == AF_INET6)
        ((struct sockaddr_in6 *)ss)->sin6_port = htons(port);
#endif
}

static int rtp_check_source_lists(RTPContext *s, struct sockaddr_storage *source_addr_ptr)
{
    int i;
    if (s->nb_ssm_exclude_addrs) {
        for (i = 0; i < s->nb_ssm_exclude_addrs; i++) {
            if (!compare_addr(source_addr_ptr, s->ssm_exclude_addrs[i]))
                return 1;
        }
    }
    if (s->nb_ssm_include_addrs) {
        for (i = 0; i < s->nb_ssm_include_addrs; i++) {
            if (!compare_addr(source_addr_ptr, s->ssm_include_addrs[i]))
                return 0;
        }
        return 1;
    }
    return 0;
}

/**
 * add option to url of the form:
 * "http://host:port/path?option1=val1&option2=val2...
 */

static av_printf_format(3, 4) void url_add_option(char *buf, int buf_size, const char *fmt, ...)
{
    char buf1[1024];
    va_list ap;

    va_start(ap, fmt);
    if (strchr(buf, '?'))
        av_strlcat(buf, "&", buf_size);
    else
        av_strlcat(buf, "?", buf_size);
    vsnprintf(buf1, sizeof(buf1), fmt, ap);
    av_strlcat(buf, buf1, buf_size);
    va_end(ap);
}

static void build_udp_url(RTPContext *s,
                          char *buf, int buf_size,
                          const char *hostname,
                          int port, int local_port,
                          const char *include_sources,
                          const char *exclude_sources)
{
    ff_url_join(buf, buf_size, "udp", NULL, hostname, port, NULL);
    if (local_port >= 0)
        url_add_option(buf, buf_size, "localport=%d", local_port);
    if (s != NULL && s->ttl >= 0)
        url_add_option(buf, buf_size, "ttl=%d", s->ttl);
    if (s != NULL && s->buffer_size >= 0)
        url_add_option(buf, buf_size, "buffer_size=%d", s->buffer_size);
    if (s != NULL && s->pkt_size >= 0)
        url_add_option(buf, buf_size, "pkt_size=%d", s->pkt_size);
    if (s != NULL && s->connect)
        url_add_option(buf, buf_size, "connect=1");
    if (s != NULL && s->dscp >= 0)
        url_add_option(buf, buf_size, "dscp=%d", s->dscp);
    url_add_option(buf, buf_size, "fifo_size=0");
    if (include_sources && include_sources[0])
        url_add_option(buf, buf_size, "sources=%s", include_sources);
    if (exclude_sources && exclude_sources[0])
        url_add_option(buf, buf_size, "block=%s", exclude_sources);
}

static void rtp_parse_addr_list(URLContext *h, char *buf,
                                struct sockaddr_storage ***address_list_ptr,
                                int *address_list_size_ptr)
{
    struct addrinfo *ai = NULL;
    struct sockaddr_storage *source_addr;
    char tmp = '\0', *p = buf, *next;

    /* Resolve all of the IPs */

    while (p && p[0]) {
        next = strchr(p, ',');

        if (next) {
            tmp = *next;
            *next = '\0';
        }

        ai = rtp_resolve_host(p, 0, SOCK_DGRAM, AF_UNSPEC, 0);
        if (ai) {
            source_addr = av_mallocz(sizeof(struct sockaddr_storage));
            if (!source_addr) {
                freeaddrinfo(ai);
                break;
            }

            memcpy(source_addr, ai->ai_addr, ai->ai_addrlen);
            freeaddrinfo(ai);
            dynarray_add(address_list_ptr, address_list_size_ptr, source_addr);
        } else {
            av_log(h, AV_LOG_WARNING, "Unable to resolve %s\n", p);
        }

        if (next) {
            *next = tmp;
            p = next + 1;
        } else {
            p = NULL;
        }
    }
}

static int inner_rtp_read1(URLContext *_URLContext, uint8_t *buf, int size)
{
    URLContext * h = (URLContext *)_URLContext;
    RTPContext * s = h->priv_data;
    struct sockaddr_storage from;
    socklen_t from_len;
    int len, n, i;
    struct pollfd p[2] = {{s->rtp_fd, POLLIN, 0}, {s->rtcp_fd, POLLIN, 0}};
    struct sockaddr_storage *addrs[2] = { &s->last_rtp_source, &s->last_rtcp_source };
    socklen_t *addr_lens[2] = { &s->last_rtp_source_len, &s->last_rtcp_source_len };

    for (;;) {
        if (ff_check_interrupt(&h->interrupt_callback)) {
            return AVERROR_EXIT;
        }
        n = poll(p, 2, 100);
        if (n > 0) {
            /* first try RTCP, then RTP */
            for (i = 1; i >= 0; i--) {
                if (!(p[i].revents & POLLIN))
                    continue;
                *addr_lens[i] = sizeof(*addrs[i]);
                len = recvfrom(p[i].fd, buf, size, 0,
                                (struct sockaddr *)addrs[i], addr_lens[i]);
                if (len < 0) {
                    if (ff_neterrno() == AVERROR(EAGAIN) ||
                        ff_neterrno() == AVERROR(EINTR))
                        continue;
                    return AVERROR(EIO);
                }
                if (rtp_check_source_lists(s, addrs[i]))
                    continue;
                return len;
            }
        } else if (n < 0) {
            if (ff_neterrno() == AVERROR(EINTR))
                continue;
            return AVERROR(EIO);
        } else {
            return AVERROR(EAGAIN);
        }
    }
}

static void *rtp_recv_task( void *_URLContext)
{
    av_log(NULL, AV_LOG_INFO, "[%s:%d]rtp recv_buffer_task start running!!!\n", __FUNCTION__, __LINE__);
    URLContext * h = (URLContext *)_URLContext;
    RTPContext * s = h->priv_data;
    if (NULL == s)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]Null handle!!!\n", __FUNCTION__, __LINE__);
        goto rtp_thread_end;
    }
    RTPPacket * lpkt = NULL;
    int datalen=0 ;
    int payload_type=0;
    uint8_t * lpoffset=NULL;
    uint8_t * lpkt_buf=NULL;
    int len=0;
    int ext=0;
    int csrc = 0;
    int rtp_mpegts_num =0;
    int mpegts_num =0;
    int rtp_mpegts_flag = 1; // need to detec:0 rtp_mpegts:1;mpegts:2
    uint16_t sequence_numer = 0;
    int chk_pkt_num = 5;
    SeQueue RtpPacketQueue,MpegtsPacketQueue;
    RTPPacket * savedlpkt = NULL;

    rtp_mpegts_flag = am_getconfig_int_def("media.player.rtp_mpegts_flag",1);//default:1, 0-need to detect, 1-rtp, 2-mpegts
    if (0 == rtp_mpegts_flag)
    {
        InitQueue(&RtpPacketQueue);
        InitQueue(&MpegtsPacketQueue);
    }
    chk_pkt_num = am_getconfig_int_def("media.player.chk_pkt_num",2);//chk_pkt_num should be less than MAXQSIZE, more than zero
    av_log(NULL, AV_LOG_INFO, "[%s:%d]rtp_mpegts_flag=%d,chk_pkt_num =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,chk_pkt_num);

    while (s->brunning > 0)
    {
        if (ff_check_interrupt(&h->interrupt_callback))
        {
            goto rtp_thread_end;
        }

        if (s->recvlist.item_count >= 100)
        {
            usleep(10);
            continue;
        }

        if (lpkt != NULL)
        {
            rtp_free_packet((void *)lpkt);
            lpkt=NULL;
        }

        // malloc the packet buffer
        lpkt = av_mallocz(sizeof(RTPPacket));
        if (NULL == lpkt)
        {
            goto rtp_thread_end;
        }
        lpkt->buf= av_malloc(RTPPROTO_RECVBUF_SIZE);
        if (NULL == lpkt->buf)
        {
            goto rtp_thread_end;
        }
        // recv data
        lpkt->len = inner_rtp_read1(h, lpkt->buf, RTPPROTO_RECVBUF_SIZE);

        //detect rtp_mpegts or mpegts
        if (0 == rtp_mpegts_flag) //need to detect
        {
            if (is_rtp_mpegts(lpkt->buf, lpkt->len))
            {
                rtp_mpegts_num++;
                EnQueue(&RtpPacketQueue, lpkt);
                lpkt = NULL;
                av_log(NULL, AV_LOG_INFO, "[%s:%d]rtp_mpegts_flag =%d,is_rtp_mpegts true rtp_mpegts_num=%d,mpegts_num =%d,front =%d,rear =%d,length =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,rtp_mpegts_num,mpegts_num,RtpPacketQueue.front,RtpPacketQueue.rear,QueueLength(RtpPacketQueue));

            }
            else if (is_mpegts(lpkt->buf, lpkt->len))
            {
                mpegts_num++;
                EnQueue(&MpegtsPacketQueue, lpkt);
                lpkt = NULL;
                av_log(NULL, AV_LOG_INFO, "[%s:%d]rtp_mpegts_flag =%d, is_mpegts true rtp_mpegts_num=%d,mpegts_num =%d,front =%d,rear =%d,length =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,rtp_mpegts_num,mpegts_num,MpegtsPacketQueue.front,MpegtsPacketQueue.rear,QueueLength(MpegtsPacketQueue));
            }

            if (rtp_mpegts_num == chk_pkt_num)
            {
                rtp_mpegts_flag = 1;
                FreeSavedRtpPacket(&MpegtsPacketQueue);
                av_log(NULL, AV_LOG_INFO, "[%s:%d]rtp_mpegts_flag=%d,front =%d,rear =%d,length =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,RtpPacketQueue.front,RtpPacketQueue.rear,QueueLength(RtpPacketQueue));
                while (DeQueue(&RtpPacketQueue, &savedlpkt))
                {

                    ConstructSavedRtpPacket(&savedlpkt);
                    av_log(NULL, AV_LOG_INFO, "[%s:%d]rtp_mpegts_flag=%d,savedlpkt->valid_data_offset =%d,savedlpkt->seq =%d\n", __FUNCTION__, __LINE__,rtp_mpegts_flag,savedlpkt->valid_data_offset,savedlpkt->seq);
                    if (rtp_enqueue_packet1(&(s->recvlist), savedlpkt, rtp_free_packet)<0)
                    {
                        FreeSavedRtpPacket(&RtpPacketQueue);
                        goto rtp_thread_end;
                    }
                    av_log(NULL, AV_LOG_INFO, "[%s:%d]rtp_mpegts_flag=%d,front =%d,rear =%d,length =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,RtpPacketQueue.front,RtpPacketQueue.rear,QueueLength(RtpPacketQueue));
                }
            }
            else if (mpegts_num == chk_pkt_num)
            {
                rtp_mpegts_flag = 2;
                FreeSavedRtpPacket(&RtpPacketQueue);
                av_log(NULL, AV_LOG_INFO, "[%s:%d]rtp_mpegts_flag=%d,front =%d,rear =%d,length =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,MpegtsPacketQueue.front,MpegtsPacketQueue.rear,QueueLength(MpegtsPacketQueue));

                while (DeQueue(&MpegtsPacketQueue, &savedlpkt))
                {
                    savedlpkt->valid_data_offset=0;
                    savedlpkt->seq = sequence_numer ++;
                    av_log(NULL, AV_LOG_INFO, "[%s:%d]rtp_mpegts_flag=%d,savedlpkt->valid_data_offset =%d,savedlpkt->seq =%d\n", __FUNCTION__, __LINE__,rtp_mpegts_flag,savedlpkt->valid_data_offset,savedlpkt->seq);

                    if (rtp_enqueue_packet1(&(s->recvlist), savedlpkt, rtp_free_packet)<0)
                    {
                        FreeSavedRtpPacket(&MpegtsPacketQueue);
                        goto rtp_thread_end;
                    }
                    av_log(NULL, AV_LOG_INFO, "[%s:%d]rtp_mpegts_flag=%d,front =%d,rear =%d,length =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,MpegtsPacketQueue.front,MpegtsPacketQueue.rear,QueueLength(MpegtsPacketQueue));
                }
            }
            continue;
        }

        if (1 == rtp_mpegts_flag) //handle udp + rtp + mpegts
        {
            if (lpkt->len <=12)
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d]receive wrong packet len=%d \n", __FUNCTION__, __LINE__,lpkt->len);
                usleep(10);
                continue;
            }
            // paser data and buffer the packat
            payload_type = lpkt->buf[1] & 0x7f;
            lpkt->seq = AV_RB16(lpkt->buf + 2);
            if (33 == payload_type)
            {
                lpkt_buf=lpkt->buf;
                len=lpkt->len;
                lpkt->valid_data_offset = 0;
                if (rtp_enqueue_packet1(&(s->recvlist), lpkt, rtp_free_packet)<0)
                {
                    goto rtp_thread_end;
                }

            }
            else
            {
                av_log(NULL, AV_LOG_ERROR, "[%s:%d]unknown payload type = %d, seq=%d\n", __FUNCTION__, __LINE__, payload_type,lpkt->seq);
                continue;
            }
        }
        else if (2 == rtp_mpegts_flag) //handle udp + mpegts
        {
            if (lpkt->buf[0] == 0x47)
            {
                lpkt->valid_data_offset=0;
                lpkt->seq = sequence_numer ++;
                if (rtp_enqueue_packet1(&(s->recvlist), lpkt, rtp_free_packet)<0)
                {
                    goto rtp_thread_end;
                }
            }
            else
            {
                av_log(NULL, AV_LOG_ERROR, "[%s:%d]unknown mpegts payload = %d\n", __FUNCTION__, __LINE__, lpkt->buf[0]);
                continue;
            }
        }
        lpkt = NULL;
    }

    rtp_thread_end:
    s->brunning =0;
    av_log(NULL, AV_LOG_ERROR, "[%s:%d]rtp recv_buffer_task end!!!\n", __FUNCTION__, __LINE__);
    return NULL;
}

/**
 * url syntax: rtp://host:port[?option=val...]
 * option: 'ttl=n'            : set the ttl value (for multicast only)
 *         'rtcpport=n'       : set the remote rtcp port to n
 *         'localrtpport=n'   : set the local rtp port to n
 *         'localrtcpport=n'  : set the local rtcp port to n
 *         'pkt_size=n'       : set max packet size
 *         'connect=0/1'      : do a connect() on the UDP socket
 *         'sources=ip[,ip]'  : list allowed source IP addresses
 *         'block=ip[,ip]'    : list disallowed source IP addresses
 *         'write_to_source=0/1' : send packets to the source address of the latest received packet
 *         'dscp=n'           : set DSCP value to n (QoS)
 * deprecated option:
 *         'localport=n'      : set the local port to n
 *
 * if rtcpport isn't set the rtcp port will be the rtp port + 1
 * if local rtp port isn't set any available port will be used for the local
 * rtp and rtcp ports
 * if the local rtcp port is not set it will be the local rtp port + 1
 */

static int rtp_open(URLContext *h, const char *uri, int flags)
{
    RTPContext *s = h->priv_data;
    AVDictionary *fec_opts = NULL;
    int rtp_port;
    char hostname[256], include_sources[1024] = "", exclude_sources[1024] = "";
    char *sources = include_sources, *block = exclude_sources;
    char *fec_protocol = NULL;
    char buf[1024];
    char path[1024];
    const char *p;
    int i, max_retry_count = 3;
    int rtcpflags;

    av_url_split(NULL, 0, NULL, 0, hostname, sizeof(hostname), &rtp_port,
                 path, sizeof(path), uri);
    /* extract parameters */
    if (s->rtcp_port < 0)
        s->rtcp_port = rtp_port + 1;

    p = strchr(uri, '?');
    if (p) {
        if (av_find_info_tag(buf, sizeof(buf), "ttl", p)) {
            s->ttl = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "rtcpport", p)) {
            s->rtcp_port = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "localport", p)) {
            s->local_rtpport = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "localrtpport", p)) {
            s->local_rtpport = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "localrtcpport", p)) {
            s->local_rtcpport = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "pkt_size", p)) {
            s->pkt_size = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "connect", p)) {
            s->connect = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "write_to_source", p)) {
            s->write_to_source = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "dscp", p)) {
            s->dscp = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "timeout", p)) {
            s->rw_timeout = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "sources", p)) {
            av_strlcpy(include_sources, buf, sizeof(include_sources));

            rtp_parse_addr_list(h, buf, &s->ssm_include_addrs, &s->nb_ssm_include_addrs);
        } else {
            rtp_parse_addr_list(h, s->sources, &s->ssm_include_addrs, &s->nb_ssm_include_addrs);
            sources = s->sources;
        }
        if (av_find_info_tag(buf, sizeof(buf), "block", p)) {
            av_strlcpy(exclude_sources, buf, sizeof(exclude_sources));
            rtp_parse_addr_list(h, buf, &s->ssm_exclude_addrs, &s->nb_ssm_exclude_addrs);
        } else {
            rtp_parse_addr_list(h, s->block, &s->ssm_exclude_addrs, &s->nb_ssm_exclude_addrs);
            block = s->block;
        }
    }
    if (s->rw_timeout >= 0)
        h->rw_timeout = s->rw_timeout;

    if (s->fec_options_str) {
        p = s->fec_options_str;

        if (!(fec_protocol = av_get_token(&p, "="))) {
            av_log(h, AV_LOG_ERROR, "Failed to parse the FEC protocol value\n");
            goto fail;
        }
        if (strcmp(fec_protocol, "prompeg")) {
            av_log(h, AV_LOG_ERROR, "Unsupported FEC protocol %s\n", fec_protocol);
            goto fail;
        }

        p = s->fec_options_str + strlen(fec_protocol);
        while (*p && *p == '=') p++;

        if (av_dict_parse_string(&fec_opts, p, "=", ":", 0) < 0) {
            av_log(h, AV_LOG_ERROR, "Failed to parse the FEC options\n");
            goto fail;
        }
        if (s->ttl > 0) {
            snprintf(buf, sizeof (buf), "%d", s->ttl);
            av_dict_set(&fec_opts, "ttl", buf, 0);
        }
    }

    for (i = 0; i < max_retry_count; i++) {
        build_udp_url(s, buf, sizeof(buf),
                      hostname, rtp_port, s->local_rtpport,
                      sources, block);
        if (ffurl_open_whitelist(&s->rtp_hd, buf, flags, &h->interrupt_callback,
                                 NULL, h->protocol_whitelist, h->protocol_blacklist, h) < 0)
            goto fail;
        s->local_rtpport = ff_udp_get_local_port(s->rtp_hd);
        if(s->local_rtpport == 65535) {
            s->local_rtpport = -1;
            continue;
        }
        rtcpflags = flags | AVIO_FLAG_WRITE;
        if (s->local_rtcpport < 0) {
            s->local_rtcpport = s->local_rtpport + 1;
            build_udp_url(s, buf, sizeof(buf),
                          hostname, s->rtcp_port, s->local_rtcpport,
                          sources, block);
            if (ffurl_open_whitelist(&s->rtcp_hd, buf, rtcpflags,
                                     &h->interrupt_callback, NULL,
                                     h->protocol_whitelist, h->protocol_blacklist, h) < 0) {
                s->local_rtpport = s->local_rtcpport = -1;
                continue;
            }
            break;
        }
        build_udp_url(s, buf, sizeof(buf),
                      hostname, s->rtcp_port, s->local_rtcpport,
                      sources, block);
        if (ffurl_open_whitelist(&s->rtcp_hd, buf, rtcpflags, &h->interrupt_callback,
                                 NULL, h->protocol_whitelist, h->protocol_blacklist, h) < 0)
            goto fail;
        break;
    }

    s->fec_hd = NULL;
    if (fec_protocol) {
        ff_url_join(buf, sizeof(buf), fec_protocol, NULL, hostname, rtp_port, NULL);
        if (ffurl_open_whitelist(&s->fec_hd, buf, flags, &h->interrupt_callback,
                             &fec_opts, h->protocol_whitelist, h->protocol_blacklist, h) < 0)
            goto fail;
    }

    /* just to ease handle access. XXX: need to suppress direct handle
       access */
    s->rtp_fd = ffurl_get_file_handle(s->rtp_hd);
    s->rtcp_fd = ffurl_get_file_handle(s->rtcp_hd);
    if (am_getconfig_int_def("vendor.media.rtp.usethread",0)) {
        s->recvlist.max_items = max_rtp_buf;
        s->recvlist.item_ext_buf_size = 0;
        s->recvlist.muti_threads_access = 1;
        s->recvlist.reject_same_item_data = 0;
        itemlist_init(&s->recvlist) ;
        s->cur_item = NULL;
        s->brunning = 1;
        av_log(NULL, AV_LOG_INFO, "[%s:%d]use cache mode\n",__FUNCTION__,__LINE__);
        if (pthread_create(&(s->recv_thread), NULL, rtp_recv_task, h)) {
            av_log(NULL, AV_LOG_ERROR, "[%s:%d]ffmpeg_pthread_create failed\n",__FUNCTION__,__LINE__);
            goto fail;
        }
        pthread_setname_np(s->recv_thread, "ffmpeg_rtp");
    }
    h->max_packet_size = s->rtp_hd->max_packet_size;
    h->is_streamed = 1;

    av_free(fec_protocol);
    av_dict_free(&fec_opts);

    return 0;

 fail:
    if (s->rtp_hd)
        ffurl_close(s->rtp_hd);
    if (s->rtcp_hd)
        ffurl_close(s->rtcp_hd);
    ffurl_closep(&s->fec_hd);
    av_free(fec_protocol);
    av_dict_free(&fec_opts);
    return AVERROR(EIO);
}

static int rtp_read(URLContext *h, uint8_t *buf, int size)
{
    RTPContext *s = h->priv_data;
    int len, n, i;
    int64_t curtime;
    int64_t starttime = ff_network_gettime();
    struct pollfd p[2] = {{s->rtp_fd, POLLIN, 0}, {s->rtcp_fd, POLLIN, 0}};
    int poll_delay = h->flags & AVIO_FLAG_NONBLOCK ? 0 : 100;
    struct sockaddr_storage *addrs[2] = { &s->last_rtp_source, &s->last_rtcp_source };
    socklen_t *addr_lens[2] = { &s->last_rtp_source_len, &s->last_rtcp_source_len };
    int runs = h->rw_timeout / 1000 / POLLING_TIME;
    if (am_getconfig_int_def("vendor.media.rtp.usethread",0))  {
        RTPPacket *lpkt = NULL;
        int readsize=0;
        int single_readsize=0;

        while (s->brunning > 0 && readsize == 0) {
            if (ff_check_interrupt(&h->interrupt_callback))
                return AVERROR(EIO);

            if (check_net_phy_conn_status() == 0)
                break;

            if (s->recvlist.item_count <= 5) {
                curtime = ff_network_gettime();
                if (starttime <= 0)
                    starttime = curtime;
                if (gd_report_error_enable && (curtime > starttime + (int64_t)(get_data_timeout*1000*1000)) && !s->report_flag) {
                    //ffmpeg_notify(h, MEDIA_INFO_DOWNLOAD_ERROR, 10003, ERROR_EVNET_TYPE_RTP);
                    s->report_flag = 1;
                }
                usleep(10);
                continue;
            }
            usleep(10);

            if (s->cur_item == NULL)
                s->cur_item = itemlist_get_head(&s->recvlist);

            if (s->cur_item  == NULL) {
                usleep(10);
                continue;
            }
            lpkt = (RTPPacket*)s->cur_item->item_data;
            starttime = 0;
            s->report_flag = 0;
            int expect_seq = (s->last_seq+1)%MAX_RTP_SEQ;
            if (expect_seq != lpkt->seq) {
                RTPPacket *headpkt = NULL;
                itemlist_peek_head_data(&s->recvlist, (unsigned long*)&headpkt);
                if (headpkt && headpkt->seq == expect_seq) {
                    av_log(NULL, AV_LOG_ERROR, "[%s:%d] drop seq=%d,nextseq:%d the right seq=%d\n",__FUNCTION__,__LINE__, lpkt->seq,headpkt->seq,expect_seq);
                    item_free(s->cur_item);
                    s->cur_item = NULL;
                    rtp_free_packet((void *)lpkt);
                    lpkt=NULL;
                    continue;
                }
            }
            readsize = lpkt->len;
            memcpy(buf, lpkt->buf+lpkt->valid_data_offset, readsize);
            if (expect_seq != lpkt->seq) {
                av_log(NULL, AV_LOG_WARNING, "[%s:%d]discontinuity seq=%d, the right seq=%d\n",__FUNCTION__,__LINE__, lpkt->seq,expect_seq);
            }
            s->last_seq = lpkt->seq;
            // already read, no valid data clean it
            item_free(s->cur_item);
            s->cur_item = NULL;
            rtp_free_packet((void *)lpkt);
            lpkt=NULL;
        }
        return readsize;
    } else {
        for (;;) {
            if (ff_check_interrupt(&h->interrupt_callback))
                return AVERROR_EXIT;
            n = poll(p, 2, poll_delay);
            if (n > 0) {
                /* first try RTCP, then RTP */
                for (i = 1; i >= 0; i--) {
                    if (!(p[i].revents & POLLIN))
                        continue;
                    *addr_lens[i] = sizeof(*addrs[i]);
                    len = recvfrom(p[i].fd, buf, size, 0,
                                    (struct sockaddr *)addrs[i], addr_lens[i]);
                    if (len < 0) {
                        if (ff_neterrno() == AVERROR(EAGAIN) ||
                            ff_neterrno() == AVERROR(EINTR))
                            continue;
                        return AVERROR(EIO);
                    }
                    if (rtp_check_source_lists(s, addrs[i]))
                        continue;
                    return len;
                }
            } else if (n == 0 && h->rw_timeout > 0 && --runs <= 0) {
                return AVERROR(ETIMEDOUT);
            } else if (n < 0) {
                if (ff_neterrno() == AVERROR(EINTR))
                    continue;
                return AVERROR(EIO);
            }
            if (h->flags & AVIO_FLAG_NONBLOCK)
                return AVERROR(EAGAIN);
        }
    }
}

static int rtp_write(URLContext *h, const uint8_t *buf, int size)
{
    RTPContext *s = h->priv_data;
    int ret, ret_fec;
    URLContext *hd;

    if (size < 2)
        return AVERROR(EINVAL);

    if ((buf[0] & 0xc0) != (RTP_VERSION << 6))
        av_log(h, AV_LOG_WARNING, "Data doesn't look like RTP packets, "
                                  "make sure the RTP muxer is used\n");

    if (s->write_to_source) {
        int fd;
        struct sockaddr_storage *source, temp_source;
        socklen_t *source_len, temp_len;
        if (!s->last_rtp_source.ss_family && !s->last_rtcp_source.ss_family) {
            av_log(h, AV_LOG_ERROR,
                   "Unable to send packet to source, no packets received yet\n");
            // Intentionally not returning an error here
            return size;
        }

        if (RTP_PT_IS_RTCP(buf[1])) {
            fd = s->rtcp_fd;
            source     = &s->last_rtcp_source;
            source_len = &s->last_rtcp_source_len;
        } else {
            fd = s->rtp_fd;
            source     = &s->last_rtp_source;
            source_len = &s->last_rtp_source_len;
        }
        if (!source->ss_family) {
            source      = &temp_source;
            source_len  = &temp_len;
            if (RTP_PT_IS_RTCP(buf[1])) {
                temp_source = s->last_rtp_source;
                temp_len    = s->last_rtp_source_len;
                set_port(source, get_port(source) + 1);
                av_log(h, AV_LOG_INFO,
                       "Not received any RTCP packets yet, inferring peer port "
                       "from the RTP port\n");
            } else {
                temp_source = s->last_rtcp_source;
                temp_len    = s->last_rtcp_source_len;
                set_port(source, get_port(source) - 1);
                av_log(h, AV_LOG_INFO,
                       "Not received any RTP packets yet, inferring peer port "
                       "from the RTCP port\n");
            }
        }

        if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
            ret = ff_network_wait_fd(fd, 1);
            if (ret < 0)
                return ret;
        }
        ret = sendto(fd, buf, size, 0, (struct sockaddr *) source,
                     *source_len);

        return ret < 0 ? ff_neterrno() : ret;
    }

    if (RTP_PT_IS_RTCP(buf[1])) {
        /* RTCP payload type */
        hd = s->rtcp_hd;
    } else {
        /* RTP payload type */
        hd = s->rtp_hd;
    }

    if ((ret = ffurl_write(hd, buf, size)) < 0) {
        return ret;
    }

    if (s->fec_hd && !RTP_PT_IS_RTCP(buf[1])) {
        if ((ret_fec = ffurl_write(s->fec_hd, buf, size)) < 0) {
            av_log(h, AV_LOG_ERROR, "Failed to send FEC\n");
            return ret_fec;
        }
    }

    return ret;
}

static int rtp_close(URLContext *h)
{
    RTPContext *s = h->priv_data;
    int i;
    if (am_getconfig_int_def("vendor.media.rtp.usethread",0))  {
        s->brunning =0;
        pthread_join(s->recv_thread, NULL);
        s->recv_thread = 0;
        if (s->cur_item) {
            rtp_free_packet((void*)s->cur_item->item_data);
            s->cur_item->item_data = 0;
            item_free(s->cur_item);
            s->cur_item = NULL;
        }
        itemlist_clean(&s->recvlist, rtp_free_packet);
    }
    for (i = 0; i < s->nb_ssm_include_addrs; i++)
        av_freep(&s->ssm_include_addrs[i]);
    av_freep(&s->ssm_include_addrs);
    for (i = 0; i < s->nb_ssm_exclude_addrs; i++)
        av_freep(&s->ssm_exclude_addrs[i]);
    av_freep(&s->ssm_exclude_addrs);

    ffurl_close(s->rtp_hd);
    ffurl_close(s->rtcp_hd);
    ffurl_closep(&s->fec_hd);
    return 0;
}

/**
 * Return the local rtp port used by the RTP connection
 * @param h media file context
 * @return the local port number
 */

int ff_rtp_get_local_rtp_port(URLContext *h)
{
    RTPContext *s = h->priv_data;
    return ff_udp_get_local_port(s->rtp_hd);
}

/**
 * Return the local rtcp port used by the RTP connection
 * @param h media file context
 * @return the local port number
 */

int ff_rtp_get_local_rtcp_port(URLContext *h)
{
    RTPContext *s = h->priv_data;
    return ff_udp_get_local_port(s->rtcp_hd);
}

static int rtp_get_file_handle(URLContext *h)
{
    RTPContext *s = h->priv_data;
    return s->rtp_fd;
}

static int rtp_get_multi_file_handle(URLContext *h, int **handles,
                                     int *numhandles)
{
    RTPContext *s = h->priv_data;
    int *hs       = *handles = av_malloc(sizeof(**handles) * 2);
    if (!hs)
        return AVERROR(ENOMEM);
    hs[0] = s->rtp_fd;
    hs[1] = s->rtcp_fd;
    *numhandles = 2;
    return 0;
}

/* +[SE] [REQ][IPTV-19][jungle.wang]:add fast channel switch module */
static int RtpFccReadOnePac(RtpFccContext * s, uint8_t *buf, int size)
{
 //   av_log(NULL, AV_LOG_INFO, "[%s:%d],enter\n", __FUNCTION__, __LINE__);
    struct sockaddr_storage from;
    socklen_t from_len;
    int len, n;
    int CurFd = -1;
    len = n = 0;
    s->CurSock = NULL;
    struct pollfd p[4] = {{s->Unicast.Fd, POLLIN, 0}, {s->Multicast.Fd, POLLIN, 0},  {s->Signalling.Fd, POLLIN, 0}, {s->MulticastAndFec.Fd, POLLIN, 0}};
    if (s->Unicast.stopReceive) {
        p[0].fd = -1;
        s->try_direct_read &= ~(1<<0);
    }
    if (s->Signalling.stopReceive) {
        p[2].fd = -1;
        s->try_direct_read &= ~(2<<0);
    }

    int cnt = 0;
    while (s->try_direct_read) {
        if (!(s->try_direct_read & 1<<cnt)) {
            ++cnt;
            continue;
        }

        switch (cnt) {
        case 0:
            s->CurSock = &s->Unicast;
            break;
        case 1:
            s->CurSock = &s->Multicast;
            break;
        case 2:
            s->CurSock = &s->Signalling;
            break;
        case 3:
            s->CurSock = &s->MulticastAndFec;
            break;
        default:
            av_log(NULL, AV_LOG_ERROR, "[%s:%d] error try_direct_read:%x\n", __FUNCTION__, __LINE__, s->try_direct_read);
            s->try_direct_read = 0;
            goto try_poll;
        }

        from_len = sizeof(from);
        len = recvfrom (s->CurSock->Fd, buf, size, MSG_DONTWAIT, (struct sockaddr *)&from, &from_len);
        if (len < 0)
        {
            //av_log(NULL, AV_LOG_INFO, "direct read len:%d, try_direct_read:%#x, errno:%d\n", len, s->try_direct_read, errno);
            if (ff_neterrno() == AVERROR(EAGAIN) || ff_neterrno() == AVERROR(EWOULDBLOCK)) {
                s->try_direct_read &= ~(1<<cnt);
                ++cnt;
                continue;
            }
        }

        return len;
    }

try_poll:
    while (1)
    {
        if ((ff_check_interrupt(&s->Signalling.Uc->interrupt_callback)))
        {
            int ValueRet = AVERROR_EXIT;
            av_log(NULL, AV_LOG_INFO, "[%s:%d],ValueRet:%d\n", __FUNCTION__, __LINE__,ValueRet);
            return ValueRet;
        }
        if (s->ThreadStatus < 3) {
            av_log(NULL, AV_LOG_INFO, "fcc close quit read\n");
            return AVERROR_EXIT;
        }
        /* build fdset to listen to RTP, fcc packets */
        n = poll(p, 4, 10);
        if (n > 0)
        {
            int i;
            for (i = 0; i < 4; ++i) {
                if (p[i].revents & POLLIN) {
                    s->try_direct_read |= 1 << i;
                }
            }

            CurFd = -1;
            /* first try unicast */
            if (p[0].revents & POLLIN)
            {
                s->CurSock  = &s->Unicast;
                goto RecvOnePac;
            }

            /* then multicast media */
            else if (p[1].revents & POLLIN)
            {
                s->CurSock = &s->Multicast;
                goto RecvOnePac;

            }
            /* then multicast signalling */
            else if (p[2].revents & POLLIN)
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
                s->CurSock = &s->Signalling;
                goto RecvOnePac;
            }
            else if (p[3].revents & POLLIN)
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
                s->CurSock = &s->MulticastAndFec;
                goto RecvOnePac;
            }
            else
            {
                continue;
            }

RecvOnePac:

            from_len = sizeof(from);
            len = recvfrom (s->CurSock->Fd, buf, size, 0, (struct sockaddr *)&from, &from_len);
            if (len < 0)
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d],len :%d\n", __FUNCTION__, __LINE__,len);
                if (ff_neterrno() == AVERROR(EAGAIN) || ff_neterrno() == AVERROR(EINTR))
                {
                    TRACE()
                    usleep(10);
                    continue;
                }
                return AVERROR(EIO);
            }

            break;

        }
        else if (n < 0)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]network error n=%d, errno:%d\n", __FUNCTION__, __LINE__,n, errno);
            if (ff_neterrno() == AVERROR(EINTR))
            {
                usleep(10);
                continue;
            }
            return AVERROR(EIO);
        } else {
            //av_log(NULL, AV_LOG_INFO, "[%s:%d]poll timeout, break!\n", __FUNCTION__, __LINE__);
            s->CurSock = NULL;
            break;
        }
    }

    return len;
}

static int SetupUdpSocket(URLContext **puc,char *StrIp,char *StrPort,int Port,int LocalPort,int flags, const AVIOInterruptCB *int_cb)
{
    char buf[1024]={0};
    build_udp_url(NULL, buf, sizeof(buf), StrIp, Port, LocalPort, NULL, NULL);
    av_log(NULL, AV_LOG_INFO, "[%s:%d]build udp url %s\n",__FUNCTION__,__LINE__,buf);
    //signalling rtcp
    if (ffurl_open(puc, buf, flags, int_cb, NULL) < 0)
    {
       av_log(NULL, AV_LOG_INFO, "[%s:%d]build udp url fail\n",__FUNCTION__,__LINE__);
       return -1;
    }

    return 0;
}

static int MakeNewRTCPPacHWV1(RtpFccContext *Rfc, uint8_t **pBufPac, uint32_t bufsize, FCCFMT Fmt)
{
    if (NULL == Rfc)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d],NULL == Rfc\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if (pBufPac == NULL || *pBufPac == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "[%s:%d],pBufPac == NULL\n", __FUNCTION__, __LINE__);
        return -1;
    }
    uint8_t *BufPac = *pBufPac;

    int16_t LenPac = 0;
    int8_t SignallingStatus = Rfc->Signalling.Status;
    int8_t MulticastStatus = Rfc->Multicast.Status;
    uint32_t MulticastIp = Rfc->Multicast.Ip;
    uint16_t UnicastPort = Rfc->Unicast.LocalPort;
    uint16_t SignallingPort = Rfc->Signalling.LocalPort;
    uint16_t MulticastPort = Rfc->Multicast.Port;
    uint32_t local_Ip = Rfc->local_Ip;
    uint8_t StbId[16];
    uint8_t LoopCnt = 0;
    if (FCCFMT_RSR == Fmt)
    {
        LenPac = 7;

        BufPac[12] = 0;
        BufPac[13] = 0;
        BufPac[14] = 0;
        BufPac[15] = 0;

        BufPac[16] = 0;
        BufPac[17] = 0;
        BufPac[18] = 0;
        BufPac[19] = 0;

        BufPac[20] = (local_Ip & 0xff000000) >> 24;
        BufPac[21] = (local_Ip & 0x00ff0000) >> 16;
        BufPac[22] = (local_Ip & 0x0000ff00) >> 8;
        BufPac[23] = local_Ip & 0xff;

        BufPac[24] = (SignallingPort & 0xff00) >> 8;
        BufPac[25] = SignallingPort & 0xff;
        BufPac[26] = 0x80;
        BufPac[27] = 0;

        if (SignallingStatus == 2)
        {
            //not support redirected
            BufPac[28] = 0;
        }
        else
        {
            BufPac[28] = 0x20;
        }
        BufPac[29] = 0;
        BufPac[30] = 0;
        BufPac[31] = 0;
    }
    else if (FCCFMT_SCR == Fmt)
    {
        LenPac = 3;
        int firstSeqNum = Rfc->Multicast.firstSeqNum;
        if (MulticastStatus == 1 && firstSeqNum >= 0)
        {
            BufPac[12] = 0x01;
            BufPac[13] = 0;
            BufPac[14] = (firstSeqNum & 0xff00) >> 8;
            BufPac[15] = firstSeqNum & 0xff;
        }
        else
        {
            BufPac[12] = 0x02;
            BufPac[13] = 0;
            BufPac[14] = 0;
            BufPac[15] = 0;
        }
    }
    else if (FCCFMT_NAT == Fmt)
    {
        uint32_t Client_identifier = Rfc->Client_identifier;
        BufPac[0] = 0x00;
        BufPac[1] = 0x03;
        BufPac[2] = 0x00;
        BufPac[3] = 0x00;

        BufPac[4] = (Client_identifier & 0xff000000) >> 24;
        BufPac[5] = (Client_identifier & 0x00ff0000) >> 16;
        BufPac[6] = (Client_identifier & 0x0000ff00) >> 8;
        BufPac[7] = Client_identifier & 0xff;
        av_log(NULL, AV_LOG_INFO, "[%s:%d] Make NAT traversal Pac\n", __FUNCTION__, __LINE__);
        return 8;
    }
    else
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]not supported\n", __FUNCTION__, __LINE__);
        return -1;
    }
    //
    BufPac[0] = Fmt;
    BufPac[0] |= 0x80;
    BufPac[1] = 0xcd;
    BufPac[2] = (LenPac & 0xff00) >> 8;
    BufPac[3] = LenPac & 0xff;
    BufPac[4] = 0;
    BufPac[5] = 0;
    BufPac[6] = 0;
    BufPac[7] = 0;
    BufPac[8] = (MulticastIp & 0xff000000) >> 24;
    BufPac[9] = (MulticastIp & 0x00ff0000) >> 16;
    BufPac[10] = (MulticastIp & 0x0000ff00) >> 8;
    BufPac[11] = MulticastIp & 0xff;

    LenPac++;
    int ret = LenPac * 4;
    av_log(NULL, AV_LOG_INFO, "[%s:%d]MakeNewRTCPPacHW, Fmt:%d, ret:%d\n", __FUNCTION__, __LINE__, Fmt, ret);

    if (ret > bufsize)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d] RTCPPac over size, data len:%d, max len:%d\n", __FUNCTION__, __LINE__, ret, bufsize);
    }
    return ret;
}

typedef enum {
    TLV_TYPE_Head = 0,
    TLV_TYPE_First_Unicast_Sequence_Number = 32,
    TLV_TYPE_First_Multicast_Sequence_Number = 61,
    TLV_TYPE_MultiIP = 128,
    TLV_TYPE_SrcIP = 129,
    TLV_TYPE_UserIP = 130,
    TLV_TYPE_UserPort = 131,
    TLV_TYPE_Bitrate = 132,
    TLV_TYPE_ReasonType = 133,
    TLV_TYPE_StatusType = 134,
    TLV_TYPE_Server_IP = 139,
    TLV_TYPE_Redirect_Flag = 140,
    TLV_TYPE_NAT_Flag = 141,
    TLV_TYPE_Server_Port = 148,
    TLV_TYPE_Server_ValidTime = 149,
    TLV_TYPE_NAT_Traversal_Type = 150,
    TLV_TYPE_SessionID = 151,
    TLV_TYPE_Bandwidth = 154,

}FCC_TLV_TYPE;

static void resize_TLV(uint8_t **pBufPac, uint32_t *pbufsizemax, uint16_t LenPac, int encodelen)
{
    if (LenPac + encodelen >= *pbufsizemax)
    {
        *pbufsizemax += encodelen;
        *pBufPac = av_realloc(*pBufPac, *pbufsizemax);
    }
}

static int EncodeOne_TLV(FCC_TLV_TYPE type, uint8_t **pBufPac, uint32_t *pbufsizemax, uint16_t *pLenPac,
        RtpFccContext *Rfc, FCCFMT Fmt)
{
    int encodelen = 8;
    uint32_t bufsizemax = *pbufsizemax;
    uint16_t LenPac = *pLenPac;

    switch (type) {
        case TLV_TYPE_Head:
        {
            encodelen = 16;
            resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

//            typedef enum {
//                FCCFMT_NULL = 0,
//                FCCFMT_RSR = 5,         //RTCP Rapid Synchronization Request
//                FCCFMT_RSI = 6,         //RTCP Rapid Synchronization Indication
//                FCCFMT_SCN = 8,         //RTCP Synchronization Completed Notification
//                FCCFMT_SCR = 9,         //RTCP Synchronization Completed Response
//                FCCFMT_NAT = 12,        //NAT
//            }FCCFMT;
            uint16_t SFMT = 0;
            switch (Fmt) {
                case FCCFMT_RSR:
                {
                    SFMT = 1;
                    break;
                }
                case FCCFMT_SCR:
                {
                    SFMT = 4;
                    break;
                }
                case FCCFMT_NAT:
                {
                    SFMT = 12;
                    break;
                }
                default:
                    av_log(NULL, AV_LOG_ERROR, "[%s:%d] err Fmt:%d\n", __FUNCTION__, __LINE__, Fmt);
                    return -1;
                    break;
            }

            (*pBufPac)[LenPac++] = 0x8a;
            (*pBufPac)[LenPac++] = 0xcd;
            //length
            //add at last
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            //SSRC of packet sender
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            //SSRC of media source
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            //SFMT
            (*pBufPac)[LenPac++] = SFMT;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            break;
        }
        case TLV_TYPE_MultiIP:
        {
            if (!Rfc->isMultiIpv6) {  //IPV4
                encodelen = 8;
                resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

                uint32_t IP = Rfc->Multicast.Ip;
                (*pBufPac)[LenPac++] = 128;
                (*pBufPac)[LenPac++] = 0;
                (*pBufPac)[LenPac++] = 0;
                (*pBufPac)[LenPac++] = 4;
                (*pBufPac)[LenPac++] = (IP & 0xff000000) >> 24;
                (*pBufPac)[LenPac++] = (IP & 0x00ff0000) >> 16;
                (*pBufPac)[LenPac++] = (IP & 0x0000ff00) >> 8;
                (*pBufPac)[LenPac++] = IP & 0xff;

            } else { // IPV6
                encodelen = 20;
                resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

                (*pBufPac)[LenPac++] = 130;
                (*pBufPac)[LenPac++] = 0;
                (*pBufPac)[LenPac++] = 0;
                (*pBufPac)[LenPac++] = 16;
                uint32_t IPV6[4];
                int i;
                for (i = 0; i<4; i++) {
                    IPV6[i] = Rfc->Multicast.Ipv6[i];
                    (*pBufPac)[LenPac++] = IPV6[i] & 0xff;
                    (*pBufPac)[LenPac++] = (IPV6[i] & 0x0000ff00) >> 8;
                    (*pBufPac)[LenPac++] = (IPV6[i] & 0x00ff0000) >> 16;
                    (*pBufPac)[LenPac++] = (IPV6[i] & 0xff000000) >> 24;
                }
            }
            break;
        }
        case TLV_TYPE_SrcIP:
        {
            if (true)  // IPV4
            {
                encodelen = 8;
                resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

                uint32_t IP = Rfc->source_Ip;
                (*pBufPac)[LenPac++] = 129;
                (*pBufPac)[LenPac++] = 0;
                (*pBufPac)[LenPac++] = 0;
                (*pBufPac)[LenPac++] = 4;
                (*pBufPac)[LenPac++] = (IP & 0xff000000) >> 24;
                (*pBufPac)[LenPac++] = (IP & 0x00ff0000) >> 16;
                (*pBufPac)[LenPac++] = (IP & 0x0000ff00) >> 8;
                (*pBufPac)[LenPac++] = IP & 0xff;
            }
            // todo V6
            break;
        }
        case TLV_TYPE_UserIP:
        {
            if (!Rfc->isIpv6) {  //IPV4
                encodelen = 8;
                resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

                uint32_t IP = Rfc->local_Ip;
                (*pBufPac)[LenPac++] = 130;
                (*pBufPac)[LenPac++] = 0;
                (*pBufPac)[LenPac++] = 0;
                (*pBufPac)[LenPac++] = 4;
                (*pBufPac)[LenPac++] = (IP & 0xff000000) >> 24;
                (*pBufPac)[LenPac++] = (IP & 0x00ff0000) >> 16;
                (*pBufPac)[LenPac++] = (IP & 0x0000ff00) >> 8;
                (*pBufPac)[LenPac++] = IP & 0xff;
            } else { // IPV6
                encodelen = 20;
                resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);
                (*pBufPac)[LenPac++] = 130;
                (*pBufPac)[LenPac++] = 0;
                (*pBufPac)[LenPac++] = 0;
                (*pBufPac)[LenPac++] = 16;
                uint32_t IPV6[4];
                int i;
                for (i = 0; i<4; i++) {
                    IPV6[i] = Rfc->local_Ipv6[i];
                    (*pBufPac)[LenPac++] = IPV6[i] & 0xff;
                    (*pBufPac)[LenPac++] = (IPV6[i] & 0x0000ff00) >> 8;
                    (*pBufPac)[LenPac++] = (IPV6[i] & 0x00ff0000) >> 16;
                    (*pBufPac)[LenPac++] = (IPV6[i] & 0xff000000) >> 24;
                }
            }
            break;
        }
        case TLV_TYPE_UserPort:
        {
            encodelen = 8;
            resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

            uint16_t LocalPort = Rfc->Signalling.LocalPort;
            (*pBufPac)[LenPac++] = 131;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 2;
            (*pBufPac)[LenPac++] = (LocalPort & 0xff00) >> 8;
            (*pBufPac)[LenPac++] = LocalPort & 0xff;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            break;
        }
        case TLV_TYPE_Redirect_Flag:
        {
            encodelen = 8;
            resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

            (*pBufPac)[LenPac++] = 140;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 1;
            (*pBufPac)[LenPac++] = 1;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            break;
        }
        case TLV_TYPE_NAT_Flag:
        {
            encodelen = 8;
            resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

            (*pBufPac)[LenPac++] = 141;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 2;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 1;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            break;
        }
        case TLV_TYPE_Bandwidth:
        {
            encodelen = 8;
            resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

            (*pBufPac)[LenPac++] = 154;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 2;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            break;
        }
        case TLV_TYPE_First_Multicast_Sequence_Number:
        {
            int firstSeqNum = Rfc->Multicast.firstSeqNum;
            if (firstSeqNum < 0) {
                firstSeqNum = 0;
            }
            encodelen = 8;
            resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

            (*pBufPac)[LenPac++] = 61;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 4;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = (firstSeqNum & 0xff00) >> 8;
            (*pBufPac)[LenPac++] = firstSeqNum & 0xff;
            break;
        }
        case TLV_TYPE_StatusType:
        {
            int8_t MulticastStatus = Rfc->Multicast.Status;
            encodelen = 8;
            resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

            (*pBufPac)[LenPac++] = 134;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 1;
            if (MulticastStatus == 1)
            {
                (*pBufPac)[LenPac++] = 1;
            } else {
                (*pBufPac)[LenPac++] = 2;
            }
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            break;
        }
        case TLV_TYPE_NAT_Traversal_Type:
        {
            encodelen = 8;
            resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

            (*pBufPac)[LenPac++] = 150;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 1;
            (*pBufPac)[LenPac++] = 3;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            break;
        }
        case TLV_TYPE_SessionID:
        {
            uint32_t Client_identifier = Rfc->Client_identifier;
            encodelen = 8;
            resize_TLV(pBufPac, &bufsizemax, LenPac, encodelen);

            (*pBufPac)[LenPac++] = 151;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 0;
            (*pBufPac)[LenPac++] = 4;
            (*pBufPac)[LenPac++] = (Client_identifier & 0xff000000) >> 24;
            (*pBufPac)[LenPac++] = (Client_identifier & 0x00ff0000) >> 16;
            (*pBufPac)[LenPac++] = (Client_identifier & 0x0000ff00) >> 8;
            (*pBufPac)[LenPac++] = Client_identifier & 0xff;

            break;
        }
    }

    *pLenPac = LenPac;
    *pbufsizemax = bufsizemax;
    return 0;
}

static int MakeNewRTCPPacHWV2(RtpFccContext *Rfc, uint8_t **pBufPac, uint32_t bufsize, FCCFMT Fmt)
{
    if (NULL == Rfc)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d],NULL == Rfc\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if (pBufPac == NULL || *pBufPac == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "[%s:%d],pBufPac == NULL\n", __FUNCTION__, __LINE__);
        return -1;
    }
    uint8_t *BufPac = *pBufPac;

    uint16_t LenPac = 0;
    uint32_t bufsizemax = bufsize;


    EncodeOne_TLV(TLV_TYPE_Head, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);

    if (FCCFMT_RSR == Fmt)
    {
        //128 MulticastIp
        EncodeOne_TLV(TLV_TYPE_MultiIP, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
        if (igmp_version == 3)
            EncodeOne_TLV(TLV_TYPE_SrcIP, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
        //130 UserIP local_Ip
        EncodeOne_TLV(TLV_TYPE_UserIP, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
        //131 User Port  SignallingPort
        EncodeOne_TLV(TLV_TYPE_UserPort, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
        //140 re locol yes
        EncodeOne_TLV(TLV_TYPE_Redirect_Flag, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
        //141 NAT   yes
        EncodeOne_TLV(TLV_TYPE_NAT_Flag, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
        //154 net   0
        //no need
        //EncodeOne_TLV(TLV_TYPE_Bandwidth, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
    } else if (FCCFMT_SCR == Fmt)
    {
        //128 MulticastIp
        EncodeOne_TLV(TLV_TYPE_MultiIP, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
        av_log(NULL, AV_LOG_INFO, "[%s:%d],FCCFMT_SCR TLV_TYPE_MultiIP ,LenPac:%d\n", __FUNCTION__, __LINE__, (int)LenPac);
        if (igmp_version == 3)
            EncodeOne_TLV(TLV_TYPE_SrcIP, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
        //
        EncodeOne_TLV(TLV_TYPE_First_Multicast_Sequence_Number, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
        av_log(NULL, AV_LOG_INFO, "[%s:%d],FCCFMT_SCR First_Multicast_Sequence_Number ,LenPac:%d\n", __FUNCTION__, __LINE__, (int)LenPac);
        //
        EncodeOne_TLV(TLV_TYPE_StatusType, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
        av_log(NULL, AV_LOG_INFO, "[%s:%d],FCCFMT_SCR TLV_TYPE_StatusType ,LenPac:%d\n", __FUNCTION__, __LINE__, (int)LenPac);
    } else if (FCCFMT_NAT == Fmt)
    {
        //
        EncodeOne_TLV(TLV_TYPE_NAT_Traversal_Type, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
        //
        EncodeOne_TLV(TLV_TYPE_SessionID, pBufPac, &bufsizemax, &LenPac, Rfc, Fmt);
    }
    else
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]not supported\n", __FUNCTION__, __LINE__);
        return -1;
    }
    uint16_t LengthPac = (LenPac - 4) / 4;
    (*pBufPac)[2] = (LengthPac & 0xff00) >> 8;
    (*pBufPac)[3] = LengthPac & 0xff;
    return LenPac;
}

static int SendRTCPPacHW(RtpFccContext *Rfc, FCCFMT Fmt)
{
    int ret;
    int n = 2;
    uint32_t RtcpLen = 96;
    uint8_t *RtcpPac = av_malloc(RtcpLen);

    if (Rfc->FCC_Version == FCC_huawei_value)
    {
        RtcpLen = MakeNewRTCPPacHWV1(Rfc, &RtcpPac, RtcpLen, Fmt);
    }
    else if (Rfc->FCC_Version == FCC_huawei_tlv)
    {
        RtcpLen = MakeNewRTCPPacHWV2(Rfc, &RtcpPac, RtcpLen, Fmt);
    } else {
        av_log(NULL, AV_LOG_ERROR, "[%s:%d]SendRTCPPacHW, err Version:%d\n", __FUNCTION__, __LINE__, Rfc->FCC_Version);
    }

    if (RtcpLen <= 0)
    {
        av_log(NULL, AV_LOG_ERROR, "[%s:%d]SendRTCPPacHW, err RtcpLen:%d\n", __FUNCTION__, __LINE__, RtcpLen);
    }
    Rfc->Signalling.Uc->flags = AVIO_FLAG_WRITE;
    if ((Rfc->FCC_Version == FCC_huawei_value || Rfc->FCC_Version == FCC_huawei_tlv) && Fmt == FCCFMT_NAT)
    {
        ret = fcc_url_write(Rfc->Unicast.Uc, RtcpPac, RtcpLen, n);
    } else {
        ret = fcc_url_write(Rfc->Signalling.Uc, RtcpPac, RtcpLen, n);
    }
    return ret;
}

//
static int MakeNewRtcpPac(RtpFccContext *Rfc,uint8_t *BufPac,uint8_t Fmt,int Fmps)
{
    if (NULL == Rfc)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]o<NULL == Rfc\n", __FUNCTION__, __LINE__);
    }
    uint16_t LenPac = 0;
    uint32_t MulticastIp = Rfc->Multicast.Ip;
    uint16_t UnicastPort = Rfc->Unicast.LocalPort;
    uint16_t MulticastPort = Rfc->Multicast.Port;
    uint32_t local_Ip = Rfc->local_Ip;
    uint8_t  StbId[16];
    uint8_t  LoopCnt = 0;
    //h/7f1f6f/
    if (2 == Fmt)
    {
        LenPac      =   9;

        BufPac[12]  =   0;
        BufPac[13]  =   0xff;
        BufPac[14]  =   0xff;
        BufPac[15]  =   0xff;

        BufPac[16]  =   (UnicastPort&0xff00)>>8;
        BufPac[17]  =   UnicastPort&0xff;
        BufPac[18]  =   (MulticastPort&0xff00)>>8;
        BufPac[19]  =   MulticastPort&0xff;

        BufPac[20]  =   (MulticastIp&0xff000000)>>24;
        BufPac[21]  =   (MulticastIp&0x00ff0000)>>16;
        BufPac[22]  =   (MulticastIp&0x0000ff00)>>8;
        BufPac[23]  =   MulticastIp&0xff;


        //2 get serial code
        char Value[100];
        if (property_get("ro.serialno",Value,NULL) > 0)
        {
            for (LoopCnt = 0;LoopCnt  < 16;LoopCnt++)
            {
                BufPac[24+LoopCnt] = Value[2*LoopCnt]<<4;
                BufPac[24+LoopCnt] += Value[2*LoopCnt+1];
                BufPac[24+LoopCnt] -= 0x30;
                if (10 == LoopCnt || 15 == LoopCnt)
                {
                    BufPac[24+LoopCnt] += 0x90;
                }
            }
        }
    }
    //g;ff6f/
    else if (5 == Fmt)
    {
        LenPac      =   3;

        BufPac[13]  =   0;
        if (Fmps >= 0) {
            BufPac[12]  =   0;
            BufPac[14]  =   (Fmps&0xff00)>>8;
            BufPac[15]  =   Fmps&0xff;
        } else {
            BufPac[12]  =   1;
            BufPac[14]  =   0;
            BufPac[15]  =   0;
        }
    }
    else if (Rfc->FCC_Version == FCC_fiberhome && 31 == Fmt) // nat
    {
        LenPac      =   20;
        char Value[100];
        if (property_get("ro.serialno",Value,NULL) > 0)
        {
            for (LoopCnt = 0;LoopCnt < 16;LoopCnt++)
            {
                BufPac[12+LoopCnt] = Value[2*LoopCnt]<<4;
                BufPac[12+LoopCnt] += Value[2*LoopCnt+1];
                BufPac[12+LoopCnt] -= 0x30;
                if (10 == LoopCnt || 15 == LoopCnt)
                {
                    BufPac[12+LoopCnt] += 0x90;
                }
            }
            for (LoopCnt; LoopCnt < 20; LoopCnt++)
            {
                BufPac[12+LoopCnt] = 0;
            }
        }
    }
    //not supported
    else
    {
        return -1;
    }

    BufPac[0]   =   Fmt;
    BufPac[0]   |=  0x80;
    BufPac[1]   =   0xcd;
    BufPac[2]   =   (LenPac&0xff00)>>8;
    BufPac[3]   =   LenPac&0xff;
    if (Rfc->FCC_Version == FCC_telecom) // SSRC of packet sender, telecom should be 0
    {
        BufPac[4]   =   0;
        BufPac[5]   =   0;
        BufPac[6]   =   0;
        BufPac[7]   =   0;
    }
    else if (Rfc->FCC_Version == FCC_fiberhome) // SSRC of packet sender, fenghuo nat need local ip
    {
        BufPac[4]   =   (local_Ip & 0xff000000) >> 24;
        BufPac[5]   =   (local_Ip & 0x00ff0000) >> 16;
        BufPac[6]   =   (local_Ip & 0x0000ff00) >> 8;
        BufPac[7]   =   local_Ip & 0xff;
    }
    BufPac[8]   =   (MulticastIp&0xff000000)>>24;
    BufPac[9]   =   (MulticastIp&0x00ff0000)>>16;
    BufPac[10]  =   (MulticastIp&0x0000ff00)>>8;
    BufPac[11]  =   MulticastIp&0xff;

    return 0;
}

//for multi times send fcc request, fmt 2 and fmt 5
static int fcc_url_write(URLContext *ctx, const unsigned char* buf, int len, int n)
{
    if (n == 0)
        n = 2;
    int succeed_times = 0, ret = 0;
    for (int i = 0; i < n; i++)
    {
        ret = ffurl_write(ctx, buf, len);
        if (ret == len)
            succeed_times++;
        else {
            av_log(NULL, AV_LOG_INFO, "[%s,%d], url_write err, len:%d, ret:%d", __FUNCTION__, __LINE__, len, ret);
        }
    }
    av_log(NULL, AV_LOG_INFO, "[%s,%d]fcc url_write, ret=%d, succeed_times:%d\n", __FUNCTION__,__LINE__, ret, succeed_times);
    if (ret < 0)
        return ret;
    return succeed_times;
}

//leave unicast
static int SendByeRtcp(RtpFccContext *Rfc,int LastSeq)
{
    int ret = 0;
    if (Rfc->Signalling.Status < 4)
    {
        uint8_t RtcpPac[16];
        uint32_t RtcpLen = 16;

        if (-1 == LastSeq)
        {
            av_log(NULL, AV_LOG_INFO, "[%s,%d],make bye cmd, stop fcc service!\n", __FUNCTION__,__LINE__);
        }
        else
        {
            av_log(NULL, AV_LOG_INFO, "[%s,%d],make bye cmd ,LastSeq:%d\n", __FUNCTION__,__LINE__,LastSeq);
        }
        if (NULL != Rfc->Signalling.Uc) {
            Rfc->Signalling.Uc->flags = AVIO_FLAG_WRITE;
        }

        if (Rfc->FCC_Version == FCC_telecom || Rfc->FCC_Version == FCC_fiberhome)
        {
            uint8_t RtcpPac[16];
            uint32_t RtcpLen = 16;
            MakeNewRtcpPac(Rfc, RtcpPac, 5, LastSeq);
            ret = fcc_url_write(Rfc->Signalling.Uc, RtcpPac, RtcpLen, 2);
        }
        else
        {
            ret = SendRTCPPacHW(Rfc, FCCFMT_SCR);
        }
        av_log(NULL, AV_LOG_INFO, "[%s,%d],send bye cmd ,ret:%d,s->Signalling.Status:%d \n", __FUNCTION__, __LINE__, ret, Rfc->Signalling.Status);
        Rfc->Signalling.Status = 4;
        return ret;
    }
    else
    {
        av_log(NULL,AV_LOG_INFO,"[%s,%d],the bye cmd has already been sent,Rfc->Signalling.Status:%d, Seq:%d\n",__FUNCTION__,__LINE__,Rfc->Signalling.Status, LastSeq);
        return -1;
    }
}


//join multicast
static int JoinMulticast(RtpFccContext *Rfc)
{
    if (Rfc->Multicast.Status == 1)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d] already build Multicast return 0\n", __FUNCTION__, __LINE__);
        return 0;
    }
    //setup the multicast socket to receive the multicast stream
    URLContext* ptmpMultUc = NULL;
    URLContext* ptmpMultAndFecUc = NULL;

    int ret = SetupUdpSocket(&ptmpMultUc, Rfc->Multicast.StrIp, Rfc->Multicast.StrPort, Rfc->Multicast.Port,-1,1,NULL);
    if (0 == ret)
    {
        Rfc->Multicast.Fd = ffurl_get_file_handle(ptmpMultUc);
        Rfc->Multicast.Status = 1;
        Rfc->Multicast.Uc = ptmpMultUc;
        av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Multicast.Fd:%d,Rfc->MultiCastStatus:%d\n", __FUNCTION__,__LINE__,Rfc->Multicast.Fd,Rfc->Multicast.Status);
    }
    else
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d],build Multicast socekt fail\n", __FUNCTION__, __LINE__);
    }

    if (Rfc->feccontext->use_multi_and_fec && -1 == unicast_data_without_fec_number)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]fec, will join multicast\n", __FUNCTION__,__LINE__);
        URLContext* ptmpMultAndFecUc = NULL;
        ret = SetupUdpSocket(&ptmpMultAndFecUc, Rfc->Multicast.StrIp, Rfc->MulticastAndFec.StrPort, Rfc->MulticastAndFec.Port,-1,1,NULL);
        if (0 == ret)
        {
            Rfc->MulticastAndFec.Fd = ffurl_get_file_handle(ptmpMultAndFecUc);
            Rfc->MulticastAndFec.Uc = ptmpMultAndFecUc;
            av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->MulticastAndFec.Fd:%d,Rfc->MultiCastStatus:%d\n", __FUNCTION__,__LINE__, Rfc->MulticastAndFec.Fd, Rfc->Multicast.Status);
        }
        else
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d],build Multicast And Fec socekt fail\n", __FUNCTION__, __LINE__);
        }
    }

    Rfc->last_receive_multicast_time = av_gettime();
    av_log(NULL, AV_LOG_INFO, "[%s:%d] init last_receive_multicast_time=%lld\n", __FUNCTION__, __LINE__, Rfc->last_receive_multicast_time);
    //Rfc->feccontext->join_multicast = 1;
    return ret;
}
#define IPV6CONTAINSHORT 4
static unsigned int Ipv6ToInt(const char* pszIP, int b_hton, unsigned int ipv6[])
{
    //unsigned int ui_ip[IPV6CONTAINSHORT] = {0};
    char* psz_token = NULL;
    char szBuf[256] = {'\0'};
    struct in6_addr ip;
    int i = 0;

    if ((NULL == pszIP)  || (strlen(pszIP) >= sizeof(szBuf)))
    {
        return 0;
    }

    strcpy(szBuf, pszIP); // attention!

    int ret = inet_pton(AF_INET6, szBuf, &ip);
    if (ret < 1) {
        return 0;
    }

    for (i = 0; i < IPV6CONTAINSHORT; i++) {
        ipv6[i] = ip.s6_addr32[i] & 0xffffffff;
    }

    return 0;
}


static unsigned int IpToInt(const char* pszIP, int b_hton)
{
    int b_first = 1;
    unsigned int ui_ip = 0;
    unsigned char ucTmp = 0;
    char* psz_token = NULL;
    char szBuf[100] = {'\0'};

    do {
        if ((NULL == pszIP)  || (strlen(pszIP) >= sizeof(szBuf)))
        {
            break;
        }

        strcpy(szBuf, pszIP); // attention!

        psz_token = strtok(szBuf, ".");
        while (NULL != psz_token)
        {
            if (b_first)
            {
                b_first = 0;
            }
            else
            {
                ui_ip <<= 8;
            }

            ucTmp = (unsigned char)atoi(psz_token);
            ui_ip |= ucTmp;

            psz_token = strtok(NULL, ".");
        }
    } while (0);

    if (b_hton)
    {
        ui_ip = htonl(ui_ip);
    }

    return ui_ip;
}
//

static unsigned int IntToIpv6(char StrIp[], int LenStr,uint32_t IntIp[])
{
    av_log(NULL, AV_LOG_INFO, "[%s:%d]strlen(StrIp):%d\n", __FUNCTION__, __LINE__,strlen(StrIp));

    inet_ntop(AF_INET6,IntIp,StrIp,LenStr);

    return 0;
}


static unsigned int IntToIp(char StrIp[], int LenStr,int IntIp)
{
    av_log(NULL, AV_LOG_INFO, "[%s:%d]sizeof(StrIp):%d\n", __FUNCTION__, __LINE__,sizeof(StrIp));

    char Ip[10] = {0};
    uint8_t *p = (uint8_t *)&IntIp;

    snprintf(Ip, 10, "%d",p[3]&0xff);
    strcpy(StrIp,Ip);
    av_strlcat(StrIp,".",LenStr);
    snprintf(Ip, 10, "%d",p[2]&0xff);
    av_strlcat(StrIp,Ip,LenStr);
    av_strlcat(StrIp,".",LenStr);
    snprintf(Ip, 10, "%d",p[1]&0xff);
    av_strlcat(StrIp,Ip,LenStr);
    av_strlcat(StrIp,".",LenStr);
    snprintf(Ip, 10, "%d",p[0]&0xff);
    av_strlcat(StrIp,Ip,LenStr);

    return 0;
}

static int ParseOneRtcpPacketHWV1(RtpFccContext *Rfc, uint8_t *Buf)
{
    if (NULL == Buf)
    {
        return -1;
    }

    uint8_t RTP_Version = (Buf[0] & 0xc0) >> 6;

    if (2 != RTP_Version)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]invalid rtcp version number:%d!!!\n", __FUNCTION__, __LINE__, RTP_Version);
        return -1;
    }

    uint8_t Fmt = Buf[0] & 0x1f;
    uint16_t length = AV_RB16(Buf + 2); //<= 5: support non; >5: support NAT or redirected;
    av_log(NULL, AV_LOG_INFO, "[%s:%d]receive rtcp respones fmt type:%d, length:%u.\n", __FUNCTION__, __LINE__, Fmt, length);
    Rfc->first_rtcp_response = 1;

    if (6 == Fmt)
    {
        uint8_t Result = Buf[12];
        uint16_t Type = AV_RB16(Buf + 14);
        av_log(NULL, AV_LOG_INFO, "[%s:%d]Fmt == 6, Result:%d,Type:%d\n", __FUNCTION__, __LINE__, Result, Type);
        if (1 != Result)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]the fcc server not process correctly,Result:%d,Type:%d\n", __FUNCTION__, __LINE__, Result, Type);
            av_log(NULL, AV_LOG_INFO, "[%s:%d]connectState:%d,Multicast.Status:%d,Signalling.Status:%d\n", __FUNCTION__, __LINE__,
                   Rfc->connectState, Rfc->Multicast.Status, Rfc->Signalling.Status);
            if (Rfc->connectState == FCC_FAST_CONNECTING)
            {
                onFccFastStartFailure(Rfc);
                Rfc->connectState = FCC_NORMAL_CONNECTING;
                return 0;
            }
            else if (Rfc->connectState == FCC_NORMAL_CONNECTING && Rfc->Multicast.Status < 1 && Rfc->Signalling.Status > 0)
            {
                JoinMulticast(Rfc);
                SendByeRtcp(Rfc, -1);
                stop_receive_unicast = 1;
            }
            else
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d]can't reach here!\n", __FUNCTION__, __LINE__);
            }
            return -1;
        }

        if (1 == Type)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]no need to obtain unicast,igmp join immediately!!!\n", __FUNCTION__, __LINE__);
            Rfc->connectState = FCC_NORMAL_CONNECTING;
            /* +[SE] [BUG][IPTV-4070][jipeng.zhao] if server not support, use multicast*/
            JoinMulticast(Rfc);
            SendByeRtcp(Rfc, -1);
            stop_receive_unicast = 1;
            return 0;
        }
        uint16_t ServerPort = AV_RB16(Buf + 26);
        uint32_t ServerIpAddress = AV_RB32(Buf + 32);
        //
        char StrServerIp[100] = {0};
        char StrServerPort[50] = {0};
        int ret = -1;
        //

        IntToIp(StrServerIp, 100, ServerIpAddress);
        snprintf(StrServerPort, sizeof(StrServerPort), "%d", ServerPort);
        av_log(NULL, AV_LOG_INFO, "[%s:%d]StrServerIp:%s, StrServerPort:%s\n", __FUNCTION__, __LINE__, StrServerIp, StrServerPort);
        if (2 == Type && Rfc->Unicast.Status < 3)
        {
            Rfc->First_Unicast_Seq = AV_RB32(Buf + 16);
            Rfc->Bitrate = AV_RB32(Buf + 20);

            if (length > 5)
            {
                Rfc->Client_identifier = AV_RB32(Buf + 28);

                if (NULL != Rfc->Unicast.Uc)
                {
                    av_log(NULL, AV_LOG_INFO, "[%s:%d] close old unicast socket in NAT traversal!\n", __FUNCTION__, __LINE__);
                    ffurl_close(Rfc->Unicast.Uc);
                    Rfc->Unicast.Uc = NULL;
                    Rfc->Unicast.Fd = -1;
                }

                uint8_t Nat_support = (Buf[24] << 2) >> 7;

                av_log(NULL, AV_LOG_INFO, "[%s:%d] Nat_support :%u, %u\n", __FUNCTION__, __LINE__, Buf[24], Nat_support);

                if (Nat_support == 1)
                {
                    //setup the unicast socket to receive the unicast stream //unicast stream local socket
                    ret = SetupUdpSocket(&Rfc->Unicast.Uc, StrServerIp, StrServerPort, ServerPort, Rfc->Unicast.LocalPort, 3,NULL);
                    if (0 == ret)
                    {
                        Rfc->Unicast.Fd = ffurl_get_file_handle(Rfc->Unicast.Uc);
                        Rfc->Unicast.Status = 2;
                        av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Unicast.Fd:%d,Rfc->Status:%d\n", __FUNCTION__, __LINE__, Rfc->Unicast.Fd, Rfc->Unicast.Status);
                        av_log(NULL, AV_LOG_INFO, "[%s:%d]StrServerIp:%s, StrServerPort:%s\n", __FUNCTION__, __LINE__, StrServerIp, StrServerPort);
                    }
                    else
                    {
                        av_log(NULL, AV_LOG_INFO, "[%s:%d],build unicast socekt fail\n", __FUNCTION__, __LINE__);
                    }

                    ret = SendRTCPPacHW(Rfc, FCCFMT_NAT);
                    av_log(NULL, AV_LOG_INFO, "[%s:%d],ret:%d\n", __FUNCTION__, __LINE__, ret);
                }
            }
            //length <= 5
            //length > 5 and not support NAT
            if (Rfc->Unicast.Uc == NULL)
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d]FATAL, try rereate unicast socket!\n", __FUNCTION__, __LINE__);
                //setup the unicast socket to receive the unicast stream //unicast stream local socket
                ret = SetupUdpSocket(&Rfc->Unicast.Uc, "", "", 0, Rfc->Unicast.LocalPort, 1,NULL);
                if (0 == ret)
                {
                    Rfc->Unicast.Fd = ffurl_get_file_handle(Rfc->Unicast.Uc);
                    Rfc->Unicast.Status = 2;
                    av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Unicast.Fd:%d,Rfc->Status:%d\n", __FUNCTION__, __LINE__, Rfc->Unicast.Fd, Rfc->Unicast.Status);
                }
                else
                {
                    av_log(NULL, AV_LOG_INFO, "[%s:%d],build unicast socekt fail\n", __FUNCTION__, __LINE__);
                }
            }

            Rfc->Unicast.Status = 3;
            Rfc->connectState = FCC_CONNECT_FINISH;
            Rfc->unicast_packet_received = 0;
            Rfc->receive_unicast_begin_time = av_gettime();
        }
        else if (3 == Type && Rfc->Unicast.Status < 3 && Rfc->Signalling.Status < 2)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]NATtraversal\n", __FUNCTION__, __LINE__);
            {
                //TODO
                //send goodbye rtcp
            }

            //send to the NAT traversal server
            av_log(NULL, AV_LOG_INFO, "[%s:%d]the fcc server is redirected!!!!\n", __FUNCTION__, __LINE__);
            //new signalling socket
            if (NULL != Rfc->Signalling.Uc)
            {
                ffurl_close(Rfc->Signalling.Uc);
                //        s->Signalling.Uc = NULL;
            }

            strcpy(Rfc->Signalling.StrIp, StrServerIp);
            if (ServerPort != 0)
            {
                Rfc->Signalling.Port = ServerPort;
                strcpy(Rfc->Signalling.StrPort, StrServerPort);
            }

            //
            //SetupUdpSocket(&s->Signalling.Uc, s->Signalling.StrIp, s->Signalling.StrPort, s->Signalling.Port, -1, 0);
            SetupUdpSocket(&Rfc->Signalling.Uc, Rfc->Signalling.StrIp, Rfc->Signalling.StrPort, Rfc->Signalling.Port, -1, 0, NULL);
            Rfc->Signalling.Fd = ffurl_get_file_handle(Rfc->Signalling.Uc);
            Rfc->Signalling.Uc->flags = AVIO_FLAG_READ_WRITE;
            av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Signalling.Fd:%d\n", __FUNCTION__, __LINE__, Rfc->Signalling.Fd);
            Rfc->Signalling.LocalPort = ff_udp_get_local_port(Rfc->Signalling.Uc);
            Rfc->Signalling.Status = 2;
            Rfc->Unicast.LocalPort = Rfc->Signalling.LocalPort - 1;
            av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Signalling redirected, Signalling.LocalPort:%d, Unicast.LocalPort:%d\n", __FUNCTION__, __LINE__, Rfc->Signalling.LocalPort, Rfc->Unicast.LocalPort);
            {
                //send new request rtcp pac
                ret = SendRTCPPacHW(Rfc, FCCFMT_RSR);
                if (ret < 0)
                {
                    av_log(NULL, AV_LOG_INFO, "[%s:%d],ret:%d, send request failed\n", __FUNCTION__, __LINE__, ret);
                    Rfc->first_rtcp_request = 0;
                }
            }
        }
        else
        {
            av_log(NULL, AV_LOG_ERROR, "[%s:%d]not supported type:%d!!!, Unicast.Signalling:%d, Unicast.Status:%d, Multicast.Status:%d\n", __FUNCTION__, __LINE__,
                   Type, Rfc->Signalling.Status, Rfc->Unicast.Status, Rfc->Multicast.Status);
            return -1;
        }
    }
    else if (8 == Fmt)
    {
        if (Rfc->Multicast.Status < 1)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d] NAT receive rtcp sync fmt type:%d!!!\n", __FUNCTION__, __LINE__, Fmt);
            //setup the multicast socket to receive the multicast stream
            JoinMulticast(Rfc);
        } else {
            av_log(NULL, AV_LOG_WARNING, "[%s:%d] sync fmt type:%d, but Multicast.Status:%d err!\n",
                   __FUNCTION__, __LINE__, Fmt, Rfc->Multicast.Status);
        }
    }
    //not supported
    else
    {
        av_log(NULL, AV_LOG_ERROR, "[%s:%d]invalid rtcp fmt:%d!!!, Unicast.Signalling:%d, Unicast.Status:%d, Multicast.Status:%d\n", __FUNCTION__, __LINE__,
               Fmt, Rfc->Signalling.Status, Rfc->Unicast.Status, Rfc->Multicast.Status);
        return -1;
    }

    return 0;
}

typedef struct FccTLVContext
{
    FCC_TLV_TYPE Type;
    uint16_t length;
} FccTLVContext;

typedef struct FccRSIContext
{
    uint32_t Multicast_IP;
    uint32_t Multicast_IPV6[4];
    uint16_t ReasonType;
    uint32_t Server_IP;
    uint32_t Server_IPV6[4];
    uint16_t Server_Port;
    uint32_t Server_ValidTime;
    uint32_t SessionID;
    uint16_t First_Unicast_Sequence_Number;
    uint16_t Bitrate;
    uint16_t N_Flag;
} FccRSIContext;


static int ParseOneTLV(uint8_t *Buf, FccTLVContext *tlvcontext)
{
    tlvcontext->Type = Buf[0];
    tlvcontext->length = AV_RB16(Buf + 2);
    return 0;
}

static int ParseOneRSI(uint8_t *Buf, FccRSIContext *rsicontext)
{
    int ret = 0;
    uint16_t position = 0;
    FccTLVContext tlvcontext;
    /* express the data length,read from reply of server */
    uint16_t length = (AV_RB16(Buf + 2) + 1) * 4;
    position += 16;
    if (length >= RTPPROTO_RECVBUF_SIZE) {
        return -2;
    }

    while (position < length)
    {
        ret = ParseOneTLV(Buf + position, &tlvcontext);
        if (ret == 0)
        {
            switch (tlvcontext.Type) {
                case TLV_TYPE_First_Unicast_Sequence_Number:
                {
                    rsicontext->First_Unicast_Sequence_Number = AV_RB16(Buf + 4 + position);
                    break;
                }
                case TLV_TYPE_MultiIP:
                {
                   if (tlvcontext.length == 4) {
                       rsicontext->Multicast_IP = AV_RB32(Buf + 4 + position);
                   } else {
                       int i;
                       for (i=0; i<4; i++) {
                           rsicontext->Multicast_IPV6[i] = ntohl(AV_RB32(Buf + 4 + position + 4*i));
                       }
                   }
                   break;
                }
                case TLV_TYPE_Bitrate:
                {
                    rsicontext->Bitrate = AV_RB16(Buf + 4 + position);
                    break;
                }
                case TLV_TYPE_ReasonType:
                {
                    rsicontext->ReasonType = AV_RB16(Buf + 4 + position);
                    break;
                }
                case TLV_TYPE_Server_IP:
                {
                    if (tlvcontext.length == 4) {
                        rsicontext->Server_IP = AV_RB32(Buf + 4 + position);
                    } else {
                        int i;
                        for (i=0; i<4; i++) {
                            rsicontext->Server_IPV6[i] = ntohl(AV_RB32(Buf + 4 + position + 4*i));
                        }
                    }
                    break;
                }
                case TLV_TYPE_Server_Port:
                {
                    rsicontext->Server_Port = AV_RB16(Buf + 4 + position);
                    break;
                }
                // case TLV_TYPE_Server_ValidTime:
                // {
                //     if (tlvcontext.length == 2) {
                //         rsicontext->Server_ValidTime = AV_RB16(Buf + 4 + position);
                //     } else if (tlvcontext.length == 4) {
                //         rsicontext->Server_ValidTime = AV_RB32(Buf + 4 + position);
                //     } else {
                //         av_log(NULL, AV_LOG_ERROR, "[%s:%d] Server_ValidTime length:%d err!.\n", __FUNCTION__, __LINE__, tlvcontext.length);
                //     }
                //     break;
                // }
                case TLV_TYPE_SessionID:
                {
                    rsicontext->SessionID = AV_RB32(Buf + 4 + position);
                    break;
                }
                case TLV_TYPE_NAT_Flag:
                {
                    rsicontext->N_Flag = AV_RB16(Buf + 4 + position);
                    break;
                }
                default:
                {
                    av_log(NULL, AV_LOG_ERROR, "[%s:%d] unsuppport type:%d.\n", __FUNCTION__, __LINE__, tlvcontext.Type);
                    break;
                }
            }
            position += ((tlvcontext.length < 4 ? 4 : tlvcontext.length) + 4);
            if (tlvcontext.length > 4)
                av_log(NULL, AV_LOG_ERROR, "[%s:%d] tlvcontext length exceed 4 is :%u.\n", __FUNCTION__, __LINE__, tlvcontext.length);
        } else {
            ret = -2;
        }
    }
    return ret;
}



static int ParseOneRtcpPacketHWV2(RtpFccContext *Rfc, uint8_t *Buf)
{
    if (NULL == Buf)
    {
        return -1;
    }

    uint8_t RTP_Version = (Buf[0] & 0xc0) >> 6;

    if (2 != RTP_Version)
    {
        av_log(NULL, AV_LOG_ERROR, "[%s:%d]invalid rtcp version number:%d!!!\n", __FUNCTION__, __LINE__, RTP_Version);
        return -1;
    }

    uint8_t SFMT = Buf[12];
    uint8_t MSN = Buf[13];  //ignore
    uint16_t length = AV_RB16(Buf + 2);
    uint16_t Response = AV_RB16(Buf + 14);

    if (SFMT == 2) {
        //RSI
        if (Response == 200) {
            //succeed
            Rfc->Response_state = 200;
        } else {
            //onFccFastStartFailure(Rfc);
            Rfc->connectState = FCC_NORMAL_CONNECTING;
            if (Response == 400)
            {
                //fail
                av_log(NULL, AV_LOG_ERROR, "[%s:%d] RSI failed, Response:%d!\n", __FUNCTION__, __LINE__, Response);
                av_log(NULL, AV_LOG_ERROR, "[%s:%d]connectState:%d,Multicast.Status:%d,Signalling.Status:%d\n",
                       __FUNCTION__, __LINE__, Rfc->connectState, Rfc->Multicast.Status, Rfc->Signalling.Status);
                Rfc->Response_state = 400;
                return -1;
            } else {
                av_log(NULL, AV_LOG_ERROR, "[%s:%d] RSI not support, Response:%d!\n", __FUNCTION__, __LINE__, Response);
                av_log(NULL, AV_LOG_ERROR, "[%s:%d]connectState:%d,Multicast.Status:%d,Signalling.Status:%d\n",
                    __FUNCTION__, __LINE__, Rfc->connectState, Rfc->Multicast.Status, Rfc->Signalling.Status);
            }
        }
        FccRSIContext rsicontext;
        memset(&rsicontext, 0, sizeof(rsicontext));
        ParseOneRSI(Buf, &rsicontext);

        if (rsicontext.ReasonType == 1) {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]no need to obtain unicast,igmp join immediately!!!\n",
                    __FUNCTION__, __LINE__);
            Rfc->connectState = FCC_NORMAL_CONNECTING;
            /* +[SE] [BUG][IPTV-4070][jipeng.zhao] if server not support, use multicast*/
            JoinMulticast(Rfc);
            SendByeRtcp(Rfc, -1);
            stop_receive_unicast = 1;
            return 0;
        }
        else if (rsicontext.ReasonType == 2) {
            if (Rfc->Unicast.Status < 3)
            {
                int ret = -1;
                Rfc->First_Unicast_Seq = rsicontext.First_Unicast_Sequence_Number;
                Rfc->Bitrate = rsicontext.Bitrate;

                if (length > 5)
                {
                    Rfc->Client_identifier = rsicontext.SessionID;

                    uint16_t Nat_support = rsicontext.N_Flag;

                    av_log(NULL, AV_LOG_INFO, "[%s:%d] Nat_support :%u, %u\n", __FUNCTION__, __LINE__, Buf[24], Nat_support);

                    if (Nat_support > 0)
                    {
                        if (NULL != Rfc->Unicast.Uc)
                        {
                            av_log(NULL, AV_LOG_INFO, "[%s:%d] close old unicast socket in NAT traversal!\n", __FUNCTION__, __LINE__);
                            ffurl_close(Rfc->Unicast.Uc);
                            Rfc->Unicast.Uc = NULL;
                            Rfc->Unicast.Fd = -1;
                        }
                        char StrServerIp[100] = {0};
                        char StrServerPort[50] = {0};
                        if (!Rfc->isIpv6) {
                            IntToIp(StrServerIp, 100, rsicontext.Server_IP);
                        } else {
                            IntToIpv6(StrServerIp, 100, rsicontext.Server_IPV6);
                        }
                        snprintf(StrServerPort, sizeof(StrServerPort), "%d", rsicontext.Server_Port);
                        av_log(NULL, AV_LOG_INFO, "[%s:%d]StrServerIp:%s, StrServerPort:%s\n", __FUNCTION__, __LINE__,
                                StrServerIp, StrServerPort);
                        //setup the unicast socket to receive the unicast stream //unicast stream local socket
                        ret = SetupUdpSocket(&Rfc->Unicast.Uc, StrServerIp, StrServerPort, rsicontext.Server_Port, Rfc->Unicast.LocalPort, 3, NULL);
                        if (0 == ret)
                        {
                            Rfc->Unicast.Fd = ffurl_get_file_handle(Rfc->Unicast.Uc);
                            Rfc->Unicast.Status = 2;
                            av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Unicast.Fd:%d,Rfc->Status:%d\n", __FUNCTION__, __LINE__,
                                    Rfc->Unicast.Fd, Rfc->Unicast.Status);
                        }
                        else
                        {
                            av_log(NULL, AV_LOG_INFO, "[%s:%d],build unicast socekt fail\n", __FUNCTION__, __LINE__);
                        }

                        ret = SendRTCPPacHW(Rfc, FCCFMT_NAT);
                        av_log(NULL, AV_LOG_INFO, "[%s:%d],ret:%d\n", __FUNCTION__, __LINE__, ret);
                    }
                }
                //length <= 5
                //length > 5 and not support NAT
                if (Rfc->Unicast.Uc == NULL)
                {
                    av_log(NULL, AV_LOG_INFO, "[%s:%d]FATAL, try rereate unicast socket!\n", __FUNCTION__, __LINE__);
                    //setup the unicast socket to receive the unicast stream //unicast stream local socket
                    ret = SetupUdpSocket(&Rfc->Unicast.Uc, "", "", 0, Rfc->Unicast.LocalPort, 1, NULL);
                    if (0 == ret)
                    {
                        Rfc->Unicast.Fd = ffurl_get_file_handle(Rfc->Unicast.Uc);
                        Rfc->Unicast.Status = 2;
                        av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Unicast.Fd:%d,Rfc->Status:%d\n", __FUNCTION__, __LINE__,
                                Rfc->Unicast.Fd, Rfc->Unicast.Status);
                    }
                    else
                    {
                        av_log(NULL, AV_LOG_INFO, "[%s:%d],build unicast socekt fail\n", __FUNCTION__, __LINE__);
                    }

                    Rfc->Unicast.Status = 3;
                    Rfc->connectState = FCC_CONNECT_FINISH;
                    Rfc->unicast_packet_received = 0;
                    Rfc->receive_unicast_begin_time = av_gettime();
                }
            }
        } else if (rsicontext.ReasonType == 3) {
            if (Rfc->Unicast.Status < 3 && Rfc->Signalling.Status < 2)
            {
                int ret = -1;
                av_log(NULL, AV_LOG_INFO, "[%s:%d]NATtraversal\n", __FUNCTION__, __LINE__);
                {
                    //TODO
                    //send goodbye rtcp
                }

                char StrServerIp[100] = {0};
                char StrServerPort[50] = {0};
                if (!Rfc->isIpv6) {
                    IntToIp(StrServerIp, 100, rsicontext.Server_IP);
                } else {
                    IntToIpv6(StrServerIp, 100, rsicontext.Server_IPV6);
                }
                snprintf(StrServerPort, sizeof(StrServerPort), "%d", rsicontext.Server_Port);
                av_log(NULL, AV_LOG_INFO, "[%s:%d]StrServerIp:%s, StrServerPort:%s\n", __FUNCTION__, __LINE__,
                       StrServerIp, StrServerPort);

                //send to the NAT traversal server
                av_log(NULL, AV_LOG_INFO, "[%s:%d]the fcc server is redirected!!!!\n", __FUNCTION__, __LINE__);
                //new signalling socket
                if (NULL != Rfc->Signalling.Uc)
                {
                    ffurl_close(Rfc->Signalling.Uc);
                    //        s->Signalling.Uc = NULL;
                }

                strcpy(Rfc->Signalling.StrIp, StrServerIp);
                if (rsicontext.Server_IP != 0)
                {
                    if (!Rfc->isIpv6) {
                        Rfc->Signalling.Ip = rsicontext.Server_IP;
                    } else {
                        int i;
                        for (i=0; i<4;i++) {
                            Rfc->Signalling.Ipv6[i] = rsicontext.Server_IPV6[i];
                        }
                    }
                    strcpy(Rfc->Signalling.StrIp, StrServerIp);
                }
                if (rsicontext.Server_Port != 0)
                {
                    Rfc->Signalling.Port = rsicontext.Server_Port;
                    strcpy(Rfc->Signalling.StrPort, StrServerPort);
                }

                channelcache_print(&g_aryChannelCache);
                av_log(NULL, AV_LOG_INFO, "%s:%d, new channel cache:(%s:%d-%d), FccValidTime:%d\n", Rfc->Multicast.StrIp, Rfc->Multicast.Port, Rfc->Signalling.StrIp, Rfc->Signalling.Port, 0, 0);
                channelcache_add2(&g_aryChannelCache, Rfc->Multicast.StrIp, Rfc->Multicast.Port, Rfc->Signalling.StrIp,
                                  Rfc->Signalling.Port, 0, Rfc->FCC_Server_validtime_default, Rfc->isIpv6, Rfc->isMultiIpv6);
                channelcache_print(&g_aryChannelCache);

                //
                //SetupUdpSocket(&s->Signalling.Uc, s->Signalling.StrIp, s->Signalling.StrPort, s->Signalling.Port, -1, 0);
                SetupUdpSocket(&Rfc->Signalling.Uc, Rfc->Signalling.StrIp, Rfc->Signalling.StrPort, Rfc->Signalling.Port, Rfc->Signalling.LocalPort, 0, NULL);
                Rfc->Signalling.Fd = ffurl_get_file_handle(Rfc->Signalling.Uc);
                Rfc->Signalling.Uc->flags = AVIO_FLAG_READ_WRITE;
                av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Signalling.Fd:%d\n", __FUNCTION__, __LINE__, Rfc->Signalling.Fd);
                Rfc->Signalling.LocalPort = ff_udp_get_local_port(Rfc->Signalling.Uc);
                Rfc->Signalling.Status = 2;
                Rfc->Unicast.LocalPort = Rfc->Signalling.LocalPort - 1;
                av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Signalling redirected, Signalling.LocalPort:%d, Unicast.LocalPort:%d\n", __FUNCTION__, __LINE__, Rfc->Signalling.LocalPort, Rfc->Unicast.LocalPort);
                {
                    //send new request rtcp pac
                    ret = SendRTCPPacHW(Rfc, FCCFMT_RSR);
                    if (ret < 0)
                    {
                        av_log(NULL, AV_LOG_INFO, "[%s:%d],ret:%d, send request failed\n", __FUNCTION__, __LINE__, ret);
                        Rfc->first_rtcp_request = 0;
                    }
                }
            }
        } else {
            av_log(NULL, AV_LOG_ERROR, "[%s:%d]not supported ReasonType:%d!!!, Unicast.Signalling:%d, Unicast.Status:%d, Multicast.Status:%d\n",
                    __FUNCTION__, __LINE__, rsicontext.ReasonType, Rfc->Signalling.Status, Rfc->Unicast.Status, Rfc->Multicast.Status);
            return -1;
        }

    } else if (SFMT == 3) {
        //RCN
        if (Response == 200) {
            //succeed
        } else if (Response == 400) {
            //fail
            av_log(NULL, AV_LOG_ERROR, "[%s:%d] RCN failed, Response:%d!!!\n", __FUNCTION__, __LINE__, Response);
            return -1;
        } else {
            av_log(NULL, AV_LOG_WARNING, "[%s:%d] RCN not support Response:%d!!!\n", __FUNCTION__, __LINE__, Response);
        }
        av_log(NULL, AV_LOG_INFO, "[%s:%d] NAT receive rtcp sync SFMT type:%d!!!\n", __FUNCTION__, __LINE__, SFMT);
        if (Rfc->Multicast.Status < 1)
        {
            JoinMulticast(Rfc);
        }
        else {
            av_log(NULL, AV_LOG_WARNING, "[%s:%d], JoinMulticast again!", __FUNCTION__, __LINE__);
        }
    }
    return 0;
}

static int ParseOneRtcpPacketHW(RtpFccContext *Rfc, uint8_t *Buf)
{
    if (NULL == Buf)
    {
        return -1;
    }

    uint8_t RTP_Version = (Buf[0] & 0xc0) >> 6;

    if (2 != RTP_Version)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]invalid rtcp version number:%d!!!\n", __FUNCTION__, __LINE__, RTP_Version);
        return -1;
    }

    uint8_t Fmt = Buf[0] & 0x1f;
    uint16_t length = AV_RB16(Buf + 2); //<= 5: support non; >5: support NAT or redirected;
    av_log(NULL, AV_LOG_INFO, "[%s:%d]receive rtcp respones fmt type:%d, length:%u.\n", __FUNCTION__, __LINE__, Fmt, length);
    Rfc->first_rtcp_response = 1;

    if (10 == Fmt)
    {
        return ParseOneRtcpPacketHWV2(Rfc,Buf);
    } else {
        return ParseOneRtcpPacketHWV1(Rfc,Buf);
    }
}

static int ParseOneRtcpPacket(RtpFccContext *Rfc,uint8_t *Buf)
{
    if (NULL == Buf)
    {
        return -1;
    }

    uint8_t Version =   (Buf[0]&0xc0)>>6;

    if (2 != Version)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]invalid rtcp version number:%d!!!\n", __FUNCTION__, __LINE__,Version);
        return -1;
    }

    uint8_t Padding =   (Buf[0]&0x20)>>5;

    if (0 != Padding)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]invalid rtcp padding:%d!!!\n", __FUNCTION__, __LINE__,Padding);
        return -1;
    }


    uint8_t Fmt =   Buf[0]&0x1f;
    av_log(NULL, AV_LOG_INFO, "[%s:%d]receive rtcp respones fmt type:%d!!!\n", __FUNCTION__, __LINE__,Fmt);
    Rfc->first_rtcp_response = 1;

    /* ?????? */
    if (3 == Fmt)
    {
        uint8_t Result  = Buf[12];
        uint8_t Type    = Buf[13];
        if (0 != Result)
        {
            //TODO: how to avoid duplicated error packet?
            av_log(NULL, AV_LOG_INFO, "[%s:%d]the fcc server not process correctly,Result:%d,Type:%d\n",__FUNCTION__, __LINE__,Result,Type);
            av_log(NULL, AV_LOG_INFO, "[%s:%d]connectState:%d,Multicast.Status:%d,Signalling.Status:%d\n",__FUNCTION__, __LINE__,
                Rfc->connectState, Rfc->Multicast.Status, Rfc->Signalling.Status);
            if (Rfc->connectState == FCC_FAST_CONNECTING) {
                onFccFastStartFailure(Rfc);
                Rfc->connectState = FCC_NORMAL_CONNECTING;
                return 0;
            } else if (Rfc->connectState == FCC_NORMAL_CONNECTING && Rfc->Multicast.Status < 1 && Rfc->Signalling.Status > 0) {
                JoinMulticast(Rfc);
                SendByeRtcp(Rfc, -1);
                stop_receive_unicast = 1;
                av_log(NULL, AV_LOG_INFO, "[%s:%d]join multicast\n", __FUNCTION__, __LINE__);
            } else {
                av_log(NULL, AV_LOG_INFO, "[%s:%d]can't reach here!\n", __FUNCTION__, __LINE__);
            }

            return -1;
        }

        if (1 == Type)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]no need to obtain unicast,igmp join immediately!!!\n", __FUNCTION__, __LINE__);
            Rfc->connectState = FCC_NORMAL_CONNECTING;
            /* +[SE] [BUG][IPTV-4070][jipeng.zhao] if server not support, use multicast*/
            JoinMulticast(Rfc);
            SendByeRtcp(Rfc, -1);
            stop_receive_unicast = 1;
            return 0;
        }
        uint16_t FccSignalPort  = AV_RB16(Buf+14);
        uint16_t FccMediaPort   = AV_RB16(Buf+16);
        uint32_t FccIpAddress   = AV_RB32(Buf+20);
        uint32_t FccValidTime = AV_RB32(Buf+24);
        //
        char StrFccIp[100] = {0};
        char StrFccPort[50] = {0};
        char StrFccMediaPort[50] = {0};
        int ret = -1;
        //
        IntToIp(StrFccIp, 100,FccIpAddress);
        snprintf(StrFccPort,sizeof(StrFccPort),"%d",FccSignalPort);
        snprintf(StrFccMediaPort,sizeof(StrFccMediaPort),"%d",FccMediaPort);
        av_log(NULL, AV_LOG_INFO, "[%s:%d] StrFccIp:%s, FccSignalPort:%s, FccMediaPort:%s\n", __FUNCTION__, __LINE__,StrFccIp, StrFccPort, StrFccMediaPort);
        //unicast media socket
        char hostname[100] = {0};
        char strport[50] = {0};
        //
        if (2 == Type && Rfc->Unicast.Status < 3)
        {
            uint8_t nat_support = Buf[18] >> 7;
            av_log(NULL, AV_LOG_INFO, "[%s:%d] the normal type to obatin unicast stream!!!, unicast fd:%d, nat_support :%u, %u\n",
                __FUNCTION__, __LINE__, Rfc->Unicast.Fd, Buf[18], nat_support);
            if (Rfc->FCC_Version == FCC_fiberhome && nat_support)
            {
                if (NULL != Rfc->Unicast.Uc)
                {
                    av_log(NULL, AV_LOG_INFO, "[%s:%d] close old unicast socket in NAT traversal!\n", __FUNCTION__, __LINE__);
                    av_log(NULL, AV_LOG_INFO, "[%s:%d] close old unicast port=%d(%s), ip=%d(%s)\n",
                        __FUNCTION__, __LINE__, Rfc->Unicast.Port, Rfc->Unicast.StrPort, Rfc->Unicast.Ip, Rfc->Unicast.StrIp);
                    ffurl_close(Rfc->Unicast.Uc);
                    Rfc->Unicast.Uc = NULL;
                    Rfc->Unicast.Fd = -1;
                }

                //setup the unicast socket to receive the unicast stream //unicast stream local socket
                ret = SetupUdpSocket(&Rfc->Unicast.Uc, StrFccIp, StrFccMediaPort, FccMediaPort, Rfc->Unicast.LocalPort, 3,NULL);
                if (0 == ret)
                {
                    Rfc->Unicast.Fd = ffurl_get_file_handle(Rfc->Unicast.Uc);
                    Rfc->Unicast.Status = 2;
                    av_log(NULL, AV_LOG_INFO, "[%s:%d] Rfc->Unicast.Fd:%d,Rfc->Status:%d\n", __FUNCTION__, __LINE__, Rfc->Unicast.Fd, Rfc->Unicast.Status);
                    av_log(NULL, AV_LOG_INFO, "[%s:%d] StrServerIp:%s, StrServerPort:%s\n", __FUNCTION__, __LINE__, StrFccIp, StrFccMediaPort);
                }
                else
                {
                    av_log(NULL, AV_LOG_INFO, "[%s:%d] build unicast socekt fail\n", __FUNCTION__, __LINE__);
                }

                //send nat traversal pac
                uint8_t RtcpPac[32];  // rtcp 12B + stbid 128b + reserved 32b = 32B
                uint32_t RtcpLen = 32;
                MakeNewRtcpPac(Rfc,RtcpPac, 31,-1);
                ret = fcc_url_write(Rfc->Unicast.Uc, RtcpPac, RtcpLen, 2);
                if (ret < 0)
                {
                    av_log(NULL, AV_LOG_INFO, "[%s:%d] ret:%d, send request failed\n",__FUNCTION__,__LINE__,ret);
                    Rfc->first_rtcp_request = 0;
                }
            }

            if (Rfc->Unicast.Uc == NULL) {
                av_log(NULL, AV_LOG_INFO, "[%s:%d]FATAL, try rereate unicast socket!\n", __FUNCTION__, __LINE__);
                //setup the unicast socket to receive the unicast stream //unicast stream local socket
                Rfc->Unicast.Port = FccMediaPort;
                ret = SetupUdpSocket(&Rfc->Unicast.Uc, hostname, strport, FccMediaPort,Rfc->Unicast.LocalPort,1, NULL);
                if (0 == ret)
                {
                    Rfc->Unicast.Fd = ffurl_get_file_handle(Rfc->Unicast.Uc);
                    Rfc->Unicast.Status = 2;
                    av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Unicast.Fd:%d,Rfc->Status:%d\n", __FUNCTION__, __LINE__,Rfc->Unicast.Fd,Rfc->Unicast.Status);
                }
                else
                {
                    av_log(NULL, AV_LOG_INFO, "[%s:%d],build unicast socekt fail\n", __FUNCTION__, __LINE__);
                }
            }

//our udp socket is connectless, so we can receive packet from this new rtcp port
//don't need to recreate it
//BUT when port change, we need to reconnect to FCC Server
#if 1
            if (0 != FccSignalPort && Rfc->Signalling.Port != FccSignalPort)
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d],FccSignalPort:%d,Rfc->Signalling.Port:%d,make new signalling socket !!!\n", __FUNCTION__,__LINE__,FccSignalPort,Rfc->Signalling.Port);
                //new signalling socket
                if (NULL != Rfc->Signalling.Uc)
                {
                    ffurl_close(Rfc->Signalling.Uc);
//                    s->Signalling.Uc = NULL;
                }
                Rfc->Signalling.Port = FccSignalPort;
                strcpy(Rfc->Signalling.StrPort,StrFccPort);
                //
                av_log(NULL, AV_LOG_INFO,"connectState:%d, mip:(%s:%d)\n",Rfc->connectState,Rfc->Multicast.StrIp, Rfc->Multicast.Port);
                channelcache_print(&g_aryChannelCache);
                fcc_directed_node_t* node = NULL;
                if (Rfc->connectState == FCC_FAST_CONNECTING &&
                    (node = channelcache_get2(&g_aryChannelCache, Rfc->Multicast.StrIp, Rfc->Multicast.Port, 0)) )
                {
                    char s_ip[INET_ADDRSTRLEN]={0};
                    char s_port[8]={0};
                    inet_ntop(AF_INET, &node->redirect_ip, s_ip, INET_ADDRSTRLEN);
                    snprintf(s_port, 8, "%d", node->redirect_port);
                    SetupUdpSocket(&Rfc->Signalling.Uc, s_ip, s_port,
                        node->redirect_data_port, Rfc->Signalling.LocalPort ,0,NULL);
                    av_log(NULL, AV_LOG_INFO,"redirect to (%s:%s)\n",s_ip, s_port);
                } else//if (Rfc->connectState == FCC_NORMAL_CONNECTING)
                {
                    SetupUdpSocket(&Rfc->Signalling.Uc, Rfc->Signalling.StrIp, StrFccPort, FccSignalPort,Rfc->Signalling.LocalPort ,0,NULL);
                }

                av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Signalling.LocalPort:%dn", __FUNCTION__,__LINE__,Rfc->Signalling.LocalPort);
                Rfc->Signalling.Fd = ffurl_get_file_handle(Rfc->Signalling.Uc);
                Rfc->Signalling.Uc->flags = AVIO_FLAG_READ_WRITE;
                av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Signalling.Fd:%d\n", __FUNCTION__, __LINE__,Rfc->Signalling.Fd);
                Rfc->Signalling.LocalPort =ff_udp_get_local_port(Rfc->Signalling.Uc);
                av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Signalling.LocalPort:%d,Rfc->LocalUnicastStreamPort:%d\n", __FUNCTION__,__LINE__,Rfc->Signalling.LocalPort,Rfc->Unicast.LocalPort);
            }
#else
            if (0 != FccSignalPort)
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d],new signal port:%d, Rfc->Signalling.Port:%d", __FUNCTION__,__LINE__,FccSignalPort, Rfc->Signalling.Port);
                Rfc->Signalling.Port = FccSignalPort;
                strcpy(Rfc->Signalling.StrPort,StrFccPort);
                //
                av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Signalling.LocalPort:%d,Rfc->LocalUnicastStreamPort:%d\n", __FUNCTION__,__LINE__,Rfc->Signalling.LocalPort,Rfc->Unicast.LocalPort);
            }
#endif

            Rfc->Unicast.Status = 3;
            Rfc->connectState = FCC_CONNECT_FINISH;
            Rfc->unicast_packet_received = 0;
            Rfc->receive_unicast_begin_time = av_gettime();
        }
        else if (3 == Type && Rfc->Unicast.Status < 2)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]the fcc server is redirected!!!!\n", __FUNCTION__, __LINE__);
            //new signalling socket
            if (NULL != Rfc->Signalling.Uc)
            {
                ffurl_close(Rfc->Signalling.Uc);
            //        s->Signalling.Uc = NULL;
            }
            Rfc->Signalling.Port = FccSignalPort;
            strcpy(Rfc->Signalling.StrIp,StrFccIp);
            strcpy(Rfc->Signalling.StrPort,StrFccPort);

            channelcache_print(&g_aryChannelCache);

            av_log(NULL, AV_LOG_INFO,"new channel cache:(%s:%d-%d), FccValidTime:%d\n", StrFccIp, FccSignalPort, FccMediaPort, FccValidTime);
            channelcache_add2(&g_aryChannelCache, Rfc->Multicast.StrIp, Rfc->Multicast.Port, StrFccIp,
                FccSignalPort, FccMediaPort, FccValidTime, 0, 0);
            channelcache_print(&g_aryChannelCache);
            //
            SetupUdpSocket(&Rfc->Signalling.Uc, StrFccIp, StrFccPort, FccSignalPort,-1,0,NULL);
            Rfc->Signalling.Fd = ffurl_get_file_handle(Rfc->Signalling.Uc);
            Rfc->Signalling.Uc->flags = AVIO_FLAG_READ_WRITE;
            av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Signalling.Fd:%d\n", __FUNCTION__, __LINE__,Rfc->Signalling.Fd);
            Rfc->Signalling.LocalPort =ff_udp_get_local_port(Rfc->Signalling.Uc);
            Rfc->Unicast.LocalPort = Rfc->Signalling.LocalPort-1;
            av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Signalling.LocalPort:%d,Rfc->LocalUnicastStreamPort:%d\n", __FUNCTION__, __LINE__,Rfc->Signalling.LocalPort,Rfc->Unicast.LocalPort);

            if (NULL != Rfc->Unicast.Uc) {
                av_log(NULL, AV_LOG_INFO, "[%s:%d] close old unicast socket in redirect!\n", __FUNCTION__, __LINE__);
                ffurl_close(Rfc->Unicast.Uc);
                Rfc->Unicast.Uc = NULL;
                Rfc->Unicast.Fd = -1;
            }

            //setup the unicast socket to receive the unicast stream //unicast stream local socket
            Rfc->Unicast.Port = FccMediaPort;
            ret = SetupUdpSocket(&Rfc->Unicast.Uc, hostname, strport, FccMediaPort,Rfc->Unicast.LocalPort,1, NULL);
            if (0 == ret)
            {
                Rfc->Unicast.Fd = ffurl_get_file_handle(Rfc->Unicast.Uc);
                Rfc->Unicast.Status = 2;
                av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->Unicast.Fd:%d,Rfc->Status:%d\n", __FUNCTION__, __LINE__,Rfc->Unicast.Fd,Rfc->Unicast.Status);
            }
            else
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d],build unicast socekt fail\n", __FUNCTION__, __LINE__);
            }
            //send new request rtcp pac
            uint8_t RtcpPac[40];
            uint32_t RtcpLen = 40;
            MakeNewRtcpPac(Rfc,RtcpPac, 2,-1);
            ret = fcc_url_write(Rfc->Signalling.Uc, RtcpPac, RtcpLen, 2);
            if (ret < 0)
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d],ret:%d, send request failed\n",__FUNCTION__,__LINE__,ret);
                Rfc->first_rtcp_request = 0;
            }
        }
        else
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]not supported type:%d!!!\n", __FUNCTION__, __LINE__,Type);
            return -1;
        }
    }
    //ef-%i
    else if (4 == Fmt &&  Rfc->Multicast.Status < 1)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]receive rtcp sync fmt type:%d!!!\n", __FUNCTION__, __LINE__,Fmt);
        //setup the multicast socket to receive the multicast stream
        JoinMulticast(Rfc);
    }
    //not supported
    else
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]invalid rtcp fmt type:%d!!!\n", __FUNCTION__, __LINE__,Fmt);
        return -1;
    }

    return 0;
}

//seq1: most old
//seq2: old
//seq3: now
int judge_seq_discontinuity(int seq1, int seq2, int seq3)
{
    int diff1 = abs(seq_subtraction(seq1, seq3));
    int diff2 = abs(seq_subtraction(seq2, seq3));

    if (diff1 < diff2) {
        return 1;
    }

    return 0;
}

int parse_rtp_ts_packet(RtpFccFecPacket* lpkt)
{
    int payload_type = lpkt->buf[1] & 0x7f;
    uint8_t * lpoffset=NULL;
    int offset=0;
    uint8_t * lpkt_buf=NULL;
    int len=0;
    int ext=0;
    int csrc = 0;

    if (33 == payload_type) {
        // mpegts packet, parse the rtp playload data
        lpkt_buf=lpkt->buf;
        len=lpkt->len;

        if (lpkt_buf[0] & 0x20)
        {
            // remove the padding data
            int padding = lpkt_buf[len - 1];
            if (len >= 12 + padding)
            {
                len -= padding;
            }
        }

        if (len <= 12)
        {
            av_log(NULL, AV_LOG_ERROR, "[%s:%d]len<=12,len=%d\n",__FUNCTION__,__LINE__,len);
            return 0;
        }
        // output the playload data
        offset = 12 ;
        lpoffset = lpkt_buf + 12;

        csrc = lpkt_buf[0] & 0x0f;
        ext = lpkt_buf[0] & 0x10;
        if (ext > 0)
        {
            offset += 4*csrc;
            lpoffset += 4*csrc;
            if (len < offset + 4)
            {
                av_log(NULL, AV_LOG_ERROR, "[%s:%d]len < offset + 4\n",__FUNCTION__,__LINE__);
                return 0;
            }

            ext = (AV_RB16(lpoffset + 2) + 1) << 2;
            if (len < ext + offset)
            {
                av_log(NULL, AV_LOG_ERROR, "[%s:%d]len < ext + offset\n",__FUNCTION__,__LINE__);
                return 0;
            }
            offset+=ext ;
            lpoffset+=ext ;
        }
        lpkt->valid_data_offset=offset;

    } else {
        av_log(NULL, AV_LOG_ERROR, "[%s:%d]unknown payload type = %d, seq=%d\n", __FUNCTION__, __LINE__, payload_type,lpkt->seq);
        return 0;
    }

    return 1;
}
static int closeRtpFccSinal_Unicast(RtpFccContext * s)
{
    int ret = -1;
    if (NULL != s->Unicast.Uc) {
        ret = ffurl_close(s->Unicast.Uc);
        av_log(NULL, AV_LOG_INFO, "[%s,%d] close Unicast ret:%d\n",
                __FUNCTION__,__LINE__, ret);
        s->Unicast.Uc = NULL;
        s->Unicast.stopReceive = 1;
        stop_receive_unicast = 1;
    }
    if (NULL != s->Signalling.Uc) {
        ret = ffurl_close(s->Signalling.Uc);
        av_log(NULL, AV_LOG_INFO, "[%s,%d] close Signalling ret:%d\n",
                __FUNCTION__,__LINE__, ret);
        s->Signalling.Uc = NULL;
        s->Signalling.stopReceive = 1;
    }
    return ret;
}
static int rtpfcc_multicastpacket_process(RtpFccContext * s,RtpFccFecPacket * lpkt)
{
    if (-1 == s->Multicast.LastSeqNum)
    {
        s->Multicast.LastSeqNum = lpkt->seq;
        // this is conflict with stopReceive logic, disable it temporary
        //s->FirstMulticastSeq    = lpkt->seq;
        s->feccontext->data_start_fec = 1;
        s->Multicast.firstSeqNum = lpkt->seq;
        first_multi_num = lpkt->seq;

        SendByeRtcp(s, lpkt->seq);
        av_log(NULL, AV_LOG_INFO, "[%s:%d]the first multicast seq:%d, current unicast seq:%d,all CntUm:%d\n", __FUNCTION__,
            __LINE__,lpkt->seq, s->Unicast.LastSeqNum,s->Unicast.Cnt);
        if (s->Unicast.Status > 0)
        {
//                        av_log(NULL, AV_LOG_INFO, "[%s:%d]discard the first mulitcast pac\n", __FUNCTION__, __LINE__);
//                        continue;
        }
        else
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]unicast is not setted up,the first pac is needed\n", __FUNCTION__, __LINE__);
        }
    }

    if (s->Multicast.LastSeqNum != -1) {
        if (s->Multicast.bak_pkt != NULL) {
            if (judge_seq_discontinuity(s->Multicast.LastSeqNum, s->Multicast.bak_pkt->seq, lpkt->seq)) {
                av_log(NULL, AV_LOG_INFO,"[%s:%d] multicast discard discontinuity packet, LastSeqNum:%d, discard pkt seq:%d, new pkt seq:%d\n", __FUNCTION__, __LINE__,
                s->Multicast.LastSeqNum, s->Multicast.bak_pkt->seq, lpkt->seq);
                rtpfec_free_packet(s->Multicast.bak_pkt);
            } else {
                //enque bak_pkt
                av_log(NULL, AV_LOG_INFO, "[%s:%d] enqueue discontinuity packet seq:%d, new pkt seq:%d\n", __FUNCTION__, __LINE__,
                s->Multicast.bak_pkt->seq, lpkt->seq);
                if (parse_rtp_ts_packet(s->Multicast.bak_pkt)) {
                    if (s->feccontext->use_multi_and_fec) {
                        rtp_enqueue_packet(&(s->feccontext->recvlist), s->Multicast.bak_pkt, rtpfec_free_packet);
                    } else {
                        rtp_enqueue_packet(&(s->feccontext->outlist), s->Multicast.bak_pkt,rtpfec_free_packet);
                    }
                } else {
                    rtpfec_free_packet(s->Multicast.bak_pkt);
                }
            }
            s->Multicast.bak_pkt = NULL;
        } else if (abs(seq_subtraction(s->Multicast.LastSeqNum, lpkt->seq)) >= sequence_order_range) {
            av_log(NULL, AV_LOG_INFO,"[%s:%d] multi packet sequence out of range, seq:%d, lastSeq:%d\n", __FUNCTION__, __LINE__,lpkt->seq, s->Multicast.LastSeqNum);
            s->Multicast.bak_pkt = lpkt;
            lpkt = NULL;
            //continue;
            return 1;
        }
    }

    s->Multicast.LastSeqNum = lpkt->seq;
    if (0 == s->Multicast.Cnt%1000)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]receive multicast pac:%d,recvlist.item_count:%d,outlist.item_count:%d! LastSeqNum:%d\n", __FUNCTION__, __LINE__,
        s->Multicast.Cnt,s->feccontext->recvlist.item_count,s->feccontext->outlist.item_count, s->Multicast.LastSeqNum);
    }
    s->Multicast.Cnt++;
    s->last_receive_multicast_time = av_gettime();
    if (s->fccreport_flag == FCC_REPORT_MULTI_CUTOFF)
        s->fccreport_flag = FCC_REPORT_MULTI_RECOVER;
    if (s->Unicast.stopReceive != 1 && s->Multicast.firstSeqNum != -1 && seq_greater_and_equal((s->Unicast.LastSeqNum+1)%MAX_RTP_SEQ, s->Multicast.firstSeqNum))
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d] stopReceive unicast, unicast seq:%d, multicast seq:%d\n", __FUNCTION__, __LINE__, s->Unicast.LastSeqNum, s->Multicast.firstSeqNum);
        s->Unicast.stopReceive = 1;
        stop_receive_unicast = 1;
        s->receive_unicast_begin_time = -1;
    }
    return 0;
}
static void *RtpFccRecvTask( void *_RtpFccContext)
{
    av_log(NULL, AV_LOG_INFO, "[%s:%d]rtp rtp fcc receive task start running!!!\n", __FUNCTION__, __LINE__);
    RtpFccContext * s=(RtpFccContext *)_RtpFccContext;
    if (NULL == s)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]Null handle!!!\n", __FUNCTION__, __LINE__);
        goto EndAbnormal;
    }
    RtpFccFecPacket * lpkt = NULL;
    int payload_type=0;

// add fec fun
    int datalen = 0;
    int try_cnt = 0;
    int ret = 0;
//////
    uint8_t * lpoffset=NULL;
    int offset=0;
    uint8_t * lpkt_buf=NULL;
    int len=0;
    int ext=0;
    int csrc = 0;
    int SleepTime = 0;
    int rtp_mpegts_num =0;
    int mpegts_num =0;
    int rtp_mpegts_flag = 1; // need to detec:0 rtp_mpegts:1;mpegts:2
    int report_cutoff_nostop = 0;
    uint16_t sequence_numer = 0;
    int chk_pkt_num = 5;
    FccSeQueue RtpPacketQueue,MpegtsPacketQueue;
    RtpFccFecPacket * savedlpkt = NULL;
    s->Response_state = 0;

    rtp_mpegts_flag = am_getconfig_int_def("media.player.rtp_mpegts_flag",1);//default:1, 0-need to detect, 1-rtp, 2-mpegts
    if (0 == rtp_mpegts_flag)
    {
        FccInitQueue(&RtpPacketQueue);
        FccInitQueue(&MpegtsPacketQueue);
    }
    chk_pkt_num = am_getconfig_int_def("media.player.chk_pkt_num",2);//chk_pkt_num should be less than MAXQSIZE, more than zero
    report_cutoff_nostop = am_getconfig_int_def("media.player.rtp_cutoff_nostop",0); // set it when need report cutoff by no muti-data recv.
    av_log(NULL, AV_LOG_INFO, "[%s:%d]rtpfcc_mpegts_flag=%d,chk_pkt_num =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,chk_pkt_num);

    while (3 <= s->ThreadStatus)
    {
        if (ff_check_interrupt(&s->Signalling.Uc->interrupt_callback))
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d],call url_interrupt_cb\n", __FUNCTION__, __LINE__);
            goto EndAbnormal;
        }

        if (s->feccontext->outlist.item_count >= max_rtp_buf)
        {
            if (0 == SleepTime ||  1000 <= SleepTime)
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d]two much rtp pac in buffer,s->outlist.item_count:%d,SleepTime:%d\n", __FUNCTION__,  __LINE__,s->feccontext->outlist.item_count,SleepTime);
                SleepTime = 0;
            }

            usleep(10);
            SleepTime++;
            continue;
        }

        if (lpkt != NULL)
        {
            rtpfec_free_packet((void *)lpkt);
            lpkt=NULL;
        }

        // malloc the packet buffer
        lpkt = av_mallocz(sizeof(RtpFccFecPacket));
        if (NULL == lpkt)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
            goto EndAbnormal;
        }
        lpkt->buf = av_mallocz(RTPPROTO_RECVBUF_SIZE);
        if (NULL == lpkt->buf)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
            goto EndAbnormal;
        }
        // recv data
        /* +[SE] [BUG][IPTV-819][yinli.xia] added: add fcc function to calculate bandwidth*/
        lpkt->len = RtpFccReadOnePac(s, lpkt->buf, RTPPROTO_RECVBUF_SIZE);
        if (AVERROR_EXIT == lpkt->len)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
            goto EndAbnormal;
        }
        if (s->first_rtcp_request == 0 && s->Multicast.Status != 1)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d] first_rtcp_request=false, force join multicast\n", __FUNCTION__, __LINE__);
            stop_receive_unicast = 1;
            s->Unicast.stopReceive = 1;
            SendByeRtcp(s, -1);
            JoinMulticast(s);
        }

        if (!s->first_rtcp_response && lpkt->len <= 0 && !s->Unicast.stopReceive) {
            int64_t cur_time = av_gettime();
            long diff_time = (cur_time - s->first_rtcp_send_time) / 1000;
            if (s->connectState == FCC_NORMAL_CONNECTING) {
                if (diff_time > normal_wait_first_rtcp_timeout) {
                    av_log(NULL, AV_LOG_WARNING, "[%s:%d] normal_wait_first_rtcp_timeout:%d ms, force join multicast!",
                        __FUNCTION__, __LINE__, diff_time);
                    s->first_rtcp_response = 1;
                    JoinMulticast(s);
                    SendByeRtcp(s, -1);
                    s->connectState = FCC_CONNECT_FINISH;
                    closeRtpFccSinal_Unicast(s);
                    continue;
                }
            } else if (s->connectState == FCC_FAST_CONNECTING) {
                if (diff_time > fast_wait_first_rtcp_timeout) {
                    av_log(NULL, AV_LOG_WARNING, "[%s:%d] fast_wait_first_rtcp_timeout:%d ms, maybe retry with normal mode!",
                        __FUNCTION__, __LINE__, diff_time);
                    onFccFastStartFailure(s);
                    s->connectState = FCC_NORMAL_CONNECTING;
                    continue;
                }
            } else {
                av_log(NULL, AV_LOG_ERROR, "[%s:%d]can't reach here!\n", __FUNCTION__, __LINE__);
            }
        }

        //
        if (s->CurSock == &s->Signalling)
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]receive signalling pac, buf:%p, len:%d\n", __FUNCTION__, __LINE__, lpkt->buf, lpkt->len);
            int parse_ret = 0;
            if (s->FCC_Version == FCC_telecom || s->FCC_Version == FCC_fiberhome)
                parse_ret = ParseOneRtcpPacket(s, lpkt->buf);
            else
                parse_ret = ParseOneRtcpPacketHW(s, lpkt->buf);
            rtpfec_free_packet((void *)lpkt);
            if (parse_ret == -1 && ( s->Response_state == 400)) {
                av_log(NULL, AV_LOG_INFO, "[%s:%d] add multi cast,stop unicast!!!\n", __FUNCTION__, __LINE__);
                stop_receive_unicast = 1;
                s->Unicast.stopReceive = 1;
                SendByeRtcp(s, -1);
                JoinMulticast(s);
            }
        }
        else if (s->CurSock == &s->Unicast || s->CurSock == &s->Multicast || s->CurSock == &s->MulticastAndFec)
        {
            if ((s->CurSock == &s->Multicast) && (!s->feccontext->use_multi_and_fec)) {
                //detect multicast(not include fec multicast) payload rtp_mpegts or mpegts
                //need to detect
                if (0 == rtp_mpegts_flag) {
                    if (is_rtp_mpegts(lpkt->buf, lpkt->len)) {
                        rtp_mpegts_num++;
                        FccEnQueue(&RtpPacketQueue, lpkt);
                        lpkt = NULL;
                        av_log(NULL, AV_LOG_INFO, "[%s:%d]rtpfcc_mpegts_flag =%d,is_rtp_mpegts true rtp_mpegts_num=%d,mpegts_num =%d,front =%d,rear =%d,length =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,rtp_mpegts_num,mpegts_num,RtpPacketQueue.front,RtpPacketQueue.rear,FccQueueLength(RtpPacketQueue));
                    }
                    else if (is_mpegts(lpkt->buf, lpkt->len)) {
                        mpegts_num++;
                        FccEnQueue(&MpegtsPacketQueue, lpkt);
                        lpkt = NULL;
                        av_log(NULL, AV_LOG_INFO, "[%s:%d]rtpfcc_mpegts_flag =%d, is_mpegts true rtp_mpegts_num=%d,mpegts_num =%d,front =%d,rear =%d,length =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,rtp_mpegts_num,mpegts_num,MpegtsPacketQueue.front,MpegtsPacketQueue.rear,FccQueueLength(MpegtsPacketQueue));
                    }

                    if (rtp_mpegts_num == chk_pkt_num)
                    {
                        rtp_mpegts_flag = 1;
                        FccFreeSavedRtpPacket(&MpegtsPacketQueue);
                        av_log(NULL, AV_LOG_INFO, "[%s:%d]rtpfcc_mpegts_flag=%d,front =%d,rear =%d,length =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,RtpPacketQueue.front,RtpPacketQueue.rear,FccQueueLength(RtpPacketQueue));
                        while (FccDeQueue(&RtpPacketQueue, &savedlpkt))
                        {
                            if (ff_check_interrupt(&s->Signalling.Uc->interrupt_callback))
                            {
                                rtpfec_free_packet((void *)savedlpkt);
                                FccFreeSavedRtpPacket(&RtpPacketQueue);
                                av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
                                goto EndAbnormal;
                            }
                            FccConstructSavedRtpPacket(&savedlpkt);
                            if (rtpfcc_multicastpacket_process (s, savedlpkt) == 1)
                                continue;
                            av_log(NULL, AV_LOG_INFO, "[%s:%d]rtpfcc_mpegts_flag=%d,savedlpkt->valid_data_offset =%d,savedlpkt->seq =%d\n", __FUNCTION__, __LINE__,rtp_mpegts_flag,savedlpkt->valid_data_offset,savedlpkt->seq);
                            if (s->feccontext->use_multi_and_fec)
                            {
                                ret = rtp_enqueue_packet(&(s->feccontext->recvlist), savedlpkt, rtpfec_free_packet);
                            }
                            else
                            {
                                ret = rtp_enqueue_packet(&(s->feccontext->outlist), savedlpkt, rtpfec_free_packet);
                            }
                            if (ret < 0)
                            {
                                FccFreeSavedRtpPacket(&RtpPacketQueue);
                                av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
                                goto EndAbnormal;
                            }
                            av_log(NULL, AV_LOG_INFO, "[%s:%d]rtpfcc_mpegts_flag=%d,front =%d,rear =%d,length =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,RtpPacketQueue.front,RtpPacketQueue.rear, FccQueueLength(RtpPacketQueue));
                        }
                    }
                    else if (mpegts_num == chk_pkt_num)
                    {
                        rtp_mpegts_flag = 2;
                        FccFreeSavedRtpPacket(&RtpPacketQueue);
                        av_log(NULL, AV_LOG_INFO, "[%s:%d]rtpfcc_mpegts_flag=%d,front =%d,rear =%d,length =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,MpegtsPacketQueue.front,MpegtsPacketQueue.rear, FccQueueLength(MpegtsPacketQueue));
                        while (FccDeQueue(&MpegtsPacketQueue, &savedlpkt))
                        {
                            if (ff_check_interrupt(&s->Signalling.Uc->interrupt_callback))
                            {
                                rtpfec_free_packet((void *)savedlpkt);
                                FccFreeSavedRtpPacket(&MpegtsPacketQueue);
                                av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
                                goto EndAbnormal;
                            }
                            savedlpkt->seq = sequence_numer++;
                            savedlpkt->valid_data_offset=0;
                            if (rtpfcc_multicastpacket_process (s, savedlpkt) == 1)
                                continue;
                            av_log(NULL, AV_LOG_INFO, "[%s:%d]rtpfcc_mpegts_flag=%d,savedlpkt->valid_data_offset =%d,savedlpkt->seq =%d\n", __FUNCTION__, __LINE__,rtp_mpegts_flag,savedlpkt->valid_data_offset,savedlpkt->seq);

                            if (rtp_enqueue_packet(&(s->feccontext->outlist), savedlpkt, rtpfec_free_packet)<0)
                            {
                                FccFreeSavedRtpPacket(&MpegtsPacketQueue);
                                av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
                                goto EndAbnormal;
                            }
                            av_log(NULL, AV_LOG_INFO, "[%s:%d]rtpfcc_mpegts_flag=%d,front =%d,rear =%d,length =%d \n", __FUNCTION__, __LINE__,rtp_mpegts_flag,MpegtsPacketQueue.front,MpegtsPacketQueue.rear,FccQueueLength(MpegtsPacketQueue));
                        }
                    }
                    continue;
                }
                // paser data and buffer the packat
                //handle udp + rtp + mpegts
                if (1 == rtp_mpegts_flag) {
                    payload_type = lpkt->buf[1] & 0x7f;
                    lpkt->seq = AV_RB16(lpkt->buf + 2);
                }
                //handle udp + mpegts
                if (2 == rtp_mpegts_flag) {
                    payload_type = lpkt->buf[0];
                    lpkt->seq = sequence_numer ++;
                }
            }
            else {
                payload_type = lpkt->buf[1] & 0x7f;
                lpkt->seq = AV_RB16(lpkt->buf + 2);
            }
            if (((s->CurSock == &s->Multicast || s->CurSock == &s->MulticastAndFec) && 33 == payload_type) || (s->CurSock == &s->Multicast && (!s->feccontext->use_multi_and_fec) && 0x47 == payload_type))
            {
                #if 1
                if (rtpfcc_multicastpacket_process (s, lpkt) == 1)
                {
                    //IPTV-8723,jungle.wang,remember to set null when it is inserted
                    av_log(NULL, AV_LOG_INFO, "[%s:%d],lpkt:%p,lpkt->seq:%d\n", __FUNCTION__, __LINE__,lpkt,lpkt->seq);
                    lpkt = NULL;
                    continue;
                }
                #endif
            }//unicast data
            else if (33 == payload_type) {
                if (s->Unicast.LastSeqNum != -1) {
                    if (s->Unicast.bak_pkt != NULL) {
                        if (judge_seq_discontinuity(s->Unicast.LastSeqNum, s->Unicast.bak_pkt->seq, lpkt->seq)) {
                            av_log(NULL, AV_LOG_INFO,"[%s:%d] Unicast discard discontinuity packet, LastSeqNum:%d, discard pkt seq:%d, new pkt seq:%d\n", __FUNCTION__, __LINE__,
                            s->Unicast.LastSeqNum, s->Unicast.bak_pkt->seq, lpkt->seq);
                            rtpfec_free_packet(s->Unicast.bak_pkt);
                        } else {
                            //enque bak_pkt
                            av_log(NULL, AV_LOG_INFO, "[%s:%d] enqueue discontinuity packet seq:%d, new pkt seq:%d\n", __FUNCTION__, __LINE__, s->Unicast.bak_pkt->seq, lpkt->seq);
                            if (parse_rtp_ts_packet(s->Unicast.bak_pkt)) {
                                if (s->feccontext->use_multi_and_fec)
                                    rtp_enqueue_packet(&(s->feccontext->recvlist), s->Unicast.bak_pkt, rtpfec_free_packet);
                                else
                                    rtp_enqueue_packet(&(s->feccontext->outlist), s->Unicast.bak_pkt, rtpfec_free_packet);
                            } else {
                                rtpfec_free_packet(s->Unicast.bak_pkt);
                            }
                        }
                        s->Unicast.bak_pkt = NULL;
                    } else if (abs(seq_subtraction(s->Unicast.LastSeqNum, lpkt->seq)) >= sequence_order_range) {
                        av_log(NULL, AV_LOG_INFO,"[%s:%d] Unicast packet sequence out of range, seq:%d, lastSeq:%d\n", __FUNCTION__, __LINE__, lpkt->seq, s->Unicast.LastSeqNum);
                        s->Unicast.bak_pkt = lpkt;
                        lpkt = NULL;
                        continue;
                    }
                }

                s->Unicast.LastSeqNum = lpkt->seq;
                //
                if (0 == s->Unicast.Cnt%1000)
                {
                    av_log(NULL, AV_LOG_INFO, "[%s:%d]receive unicast pac:%d,s->outlist.item_count:%d LastSeqNum:%d\n", __FUNCTION__, __LINE__,s->Unicast.Cnt,s->feccontext->outlist.item_count, s->Unicast.LastSeqNum);
                }
                s->Unicast.Cnt++;
                s->receive_unicast_begin_time = av_gettime();
                s->unicast_packet_received = 1;

                if (s->Multicast.firstSeqNum !=-1 && seq_greater_and_equal((lpkt->seq)%MAX_RTP_SEQ, s->Multicast.firstSeqNum)) {
                    av_log(NULL, AV_LOG_INFO, "[%s:%d] stopReceive unicast, unicast seq:%d, multicast seq:%d\n", __FUNCTION__, __LINE__, lpkt->seq, s->Multicast.firstSeqNum);
                    s->Unicast.stopReceive = 1;
                    s->receive_unicast_begin_time = -1;
                    stop_receive_unicast = 1;
                    if (NULL != s->Unicast.Uc)
                    {
                        ffurl_close(s->Unicast.Uc);
                        s->Unicast.Uc = NULL;
                        av_log(NULL, AV_LOG_INFO, "[%s:%d]it is time to close the unicast,seq num:%d\n", __FUNCTION__, __LINE__,s->FirstMulticastSeq);
                    }
                }
            }

            if (payload_type == FEC_PAYLOAD_TYPE_1 || payload_type == FEC_PAYLOAD_TYPE_2) { // fec packet
                // parse the fec header
                datalen = lpkt->len ;
                if (lpkt->buf[0] & 0x20) {          // remove the padding padding (P): 1 bit
                    int padding = lpkt->buf[datalen - 1];
                    if (datalen >= 12 + padding)
                        datalen -= padding;
                    av_log(NULL, AV_LOG_INFO, "[%s:%d]padding=%d\n", __FUNCTION__, __LINE__,padding);
                }

                datalen-=12; // The first twelve octets are present in every RTP packet
                lpoffset = lpkt->buf + 12;

                // RFC 3550 Section 5.3.1 RTP Header Extension handling
                ext = lpkt->buf[0] & 0x10;
                if (ext > 0) {
                    TRACE()
                    if (datalen < 4) {
                        av_log(NULL, AV_LOG_ERROR, "[%s:%d]datalen<4\n", __FUNCTION__, __LINE__);
                        continue;
                    }
                    ext = (AV_RB16(lpoffset + 2) + 1) << 2;
                    if (datalen < ext) {
                        av_log(NULL, AV_LOG_ERROR, "[%s:%d]ext = %d\n", __FUNCTION__, __LINE__, ext);
                        continue;
                    }

                    datalen -= ext;
                    lpoffset += ext;
                    av_log(NULL, AV_LOG_INFO, "[%s:%d]ext=%d\n", __FUNCTION__, __LINE__,ext);
                }

                if (NULL == lpkt->fec) {
                    lpkt->fec= av_mallocz(sizeof(FEC_DATA_STRUCT));
                    if (NULL == lpkt->fec)
                    {
                    // goto thread_end;
                        goto EndAbnormal;
                    }
                }
                else
                {
                    memset(lpkt->fec,0,sizeof(FEC_DATA_STRUCT));
                }

                lpkt->fec->rtp_begin_seq=AV_RB16(lpoffset);
                lpkt->fec->rtp_end_seq=AV_RB16(lpoffset+2);
                lpkt->fec->redund_num=*(lpoffset+4);
                lpkt->fec->redund_idx=*(lpoffset+5);
                lpkt->fec->fec_len=AV_RB16(lpoffset+6);
                lpkt->fec->rtp_len=AV_RB16(lpoffset+8);
                lpkt->fec->fec_data=lpoffset+12;
                //rtp_fec_calculate(s->feccontext);

                av_log(NULL, AV_LOG_ERROR, "[%s:%d]seq=%d,rtp_begin_seq=%d,rtp_end_seq=%d,redund_num=%d,redund_idx=%d,rtp_len=%d\n", __FUNCTION__, __LINE__,
            lpkt->seq,lpkt->fec->rtp_begin_seq,lpkt->fec->rtp_end_seq,lpkt->fec->redund_num,lpkt->fec->redund_idx,lpkt->fec->rtp_len);

                try_cnt = 1;
                ret=rtp_enqueue_packet(&(s->feccontext->feclist), lpkt, rtpfec_free_packet);

                if (s->output_number >= unicast_data_without_fec_number && -1 != unicast_data_without_fec_number) {
                    s->feccontext->data_start_fec = 1;
                }

                while (ret < 0 && try_cnt <= 6) {       // keyinfo try 6
                    if (ff_check_interrupt(&s->Signalling.Uc->interrupt_callback))
                    {
                        //  goto thread_end;
                        goto EndAbnormal;
                    }
                    usleep(10);
                    // retry
                    ret=rtp_enqueue_packet(&(s->feccontext->feclist), lpkt, rtpfec_free_packet);
                    try_cnt++;
                }

                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "[%s:%d]feclist have no room. timeout\n", __FUNCTION__, __LINE__);
                    continue;
                }
            }
            else if (33 == payload_type)
            {
                // mpegts packet, parse the rtp playload data
                lpkt_buf=lpkt->buf;
                len=lpkt->len;

                if (lpkt_buf[0] & 0x20)
                {
                    // remove the padding data
                    int padding = lpkt_buf[len - 1];
                    if (len >= 12 + padding)
                    {
                        len -= padding;
                    }
                }

                if (len <= 12)
                {
                    av_log(NULL, AV_LOG_ERROR, "[%s:%d]len<=12,len=%d\n",__FUNCTION__,__LINE__,len);
                    continue;
                }
                // output the playload data
                offset = 12 ;
                lpoffset = lpkt_buf + 12;

                csrc = lpkt_buf[0] & 0x0f;
                ext = lpkt_buf[0] & 0x10;
                if (ext > 0)
                {
                    offset += 4*csrc;
                    lpoffset += 4*csrc;
                    if (len < offset + 4)
                    {
                        av_log(NULL, AV_LOG_ERROR, "[%s:%d]len < offset + 4\n",__FUNCTION__,__LINE__);
                        continue;
                    }

                    ext = (AV_RB16(lpoffset + 2) + 1) << 2;
                    if (len < ext + offset)
                    {
                        av_log(NULL, AV_LOG_ERROR, "[%s:%d]len < ext + offset\n",__FUNCTION__,__LINE__);
                        continue;
                    }
                    offset+=ext ;
                    lpoffset+=ext ;
                }
                lpkt->valid_data_offset=offset;

                if (s->first_packet_get == 0) {
                    s->first_packet_get = 1;
                    av_log(NULL, AV_LOG_INFO, "[%s:%d] first_packet_get, size:%d\n", __FUNCTION__, __LINE__, lpkt->len);
                }
                if (s->feccontext->use_multi_and_fec)
                {
                    /* av_log(NULL, AV_LOG_INFO, "[%s:%d]will enqueue seq:%d, from %s\n", __FUNCTION__, __LINE__,
                        lpkt->seq, s->CurSock == &s->Unicast? "unicast":"multicast");*/
                     struct itemlist *list = NULL;
                     if (s->CurSock == &s->Unicast)
                        list =&(s->feccontext->outlist);
                     else
                        list =&(s->feccontext->recvlist);

                    if (rtp_enqueue_packet(list, lpkt, rtpfec_free_packet)<0)
                    {
                        av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
                        goto EndAbnormal;
                    }
                }
                else
                {
                  /* av_log(NULL, AV_LOG_INFO, "[%s:%d]will enqueue seq:%d, from %s\n", __FUNCTION__, __LINE__,
                        lpkt->seq, s->CurSock == &s->Unicast? "unicast":"multicast");*/
                    if (rtp_enqueue_packet(&(s->feccontext->outlist), lpkt, rtpfec_free_packet)<0)
                    {
                        av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
                        goto EndAbnormal;
                    }
                }

            } else if (0x47 == payload_type) {
                av_log(NULL, AV_LOG_INFO, "[%s:%d] payload_type = 0x%x, lpkt->valid_data_offset =%d, lpkt->seq = sequence_numer = %d\n", __FUNCTION__, __LINE__, payload_type, lpkt->valid_data_offset, lpkt->seq = sequence_numer);
                if (!s->feccontext->use_multi_and_fec) {
                    if (rtp_enqueue_packet(&(s->feccontext->outlist), lpkt, rtpfec_free_packet)<0) {
                        av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
                        goto EndAbnormal;
                    }
                }
                else
                    av_log(NULL, AV_LOG_ERROR, "[%s:%d]not support fec multicast udp + ts payload\n",__FUNCTION__,__LINE__);
            }
            else
            {
                av_log(NULL, AV_LOG_ERROR, "[%s:%d]unknown payload type = %d, seq=%d\n", __FUNCTION__, __LINE__, payload_type,lpkt->seq);
                if (lpkt != NULL)
                {
                    rtpfec_free_packet((void *)lpkt);
                    lpkt=NULL;
                }
                continue;
            }

            if (s->feccontext->use_multi_and_fec)
            {
                TRACE();
                //rtpfccfec_output_packet(s->feccontext);
                TRACE()
            }


        } else {
            if (lpkt != NULL)
            {
                rtpfec_free_packet((void *)lpkt);
                lpkt=NULL;
            }
        }
        lpkt = NULL;

        if (/*s->unicast_packet_received == 0 &&*/ s->receive_unicast_begin_time >= 0 && !s->Unicast.stopReceive && !stop_receive_unicast) {
            int64_t cur_time = av_gettime();
            long diff_time = (cur_time - s->receive_unicast_begin_time) / 1000;
            if (diff_time > wait_first_unicast_packet_timeout) {
                av_log(NULL, AV_LOG_WARNING, "[%s:%d] wait_first_unicast_packet_timeout:%d ms, force join multicast!",
                    __FUNCTION__, __LINE__, diff_time);
                s->unicast_packet_received = -1;    //don't check unicast anymore
                s->receive_unicast_begin_time = -1;
                s->Unicast.stopReceive = 1;
                stop_receive_unicast = 1;
                SendByeRtcp(s, -1);
                JoinMulticast(s);
            }
        }
        if (s->first_rtcp_send_time && !s->Unicast.stopReceive && !stop_receive_unicast)
        {
            int64_t cur_time = av_gettime();
            long diff_time = (cur_time - s->first_rtcp_send_time) / 1000;
            if (diff_time > receive_unicast_max_time && s->Multicast.Status != 1) {
                av_log(NULL, AV_LOG_WARNING, "[%s:%d] receive_unicast_max_time:%d ms, force join multicast!", __FUNCTION__, __LINE__, diff_time);
                s->unicast_packet_received = -1;    //don't check unicast anymore
                s->receive_unicast_begin_time = -1;
                s->Unicast.stopReceive = 1;
                stop_receive_unicast = 1;
                SendByeRtcp(s, -1);
                JoinMulticast(s);
            }
        }

        if (report_cutoff_nostop && !stop_receive_unicast && gd_report_error_enable && s->last_receive_multicast_time >= 0 &&
            (s->fccreport_flag == FCC_REPORT_NONE || s->fccreport_flag == FCC_REPORT_MULTI_RECOVER))
        {
            int64_t cur_time = av_gettime();
            long diff_time = (cur_time - s->last_receive_multicast_time) / 1000;
            //av_log(NULL, AV_LOG_ERROR, "[%s:%d] URLContext=%p, cur_time=%lld, last_receive_multicast_time=%lld, diff_time=%ld, get_data_timeout=%d\n",
            //    __FUNCTION__, __LINE__, s->Multicast.Uc, cur_time, s->last_receive_multicast_time, diff_time, get_data_timeout);
            if (diff_time > get_data_timeout*1000)
            {
                s->fccreport_flag = FCC_REPORT_MULTI_CUTOFF;
                //ffmpeg_id_notify(PLAYER_EVENTS_ERROR, get_data_timeout_error, 0);
                av_log(NULL, AV_LOG_ERROR, "%d %s, Both unicast and multicast exist, but multicast has no data streams, PLAYER_EVENTS_ERROR=%d, s=%p\n",
                    __LINE__, __FUNCTION__, PLAYER_EVENTS_ERROR, s);
            }
        }
    }
    EndAbnormal:
    if (lpkt != NULL)
    {
        rtpfec_free_packet((void *)lpkt);
        lpkt=NULL;
    }
    av_log(NULL, AV_LOG_ERROR, "[%s:%d]rtp fcc receive task end!!!,s->ThreadStatus:%x\n", __FUNCTION__, __LINE__,s->ThreadStatus);
    s->ThreadStatus = 0xb;
    return NULL;
}

#define IPV6_ADDR_GLOBAL        0x0000U
#define IPV6_ADDR_LOOPBACK      0x0010U
#define IPV6_ADDR_LINKLOCAL     0x0020U
#define IPV6_ADDR_SITELOCAL     0x0040U
#define IPV6_ADDR_COMPATv4      0x0080U

//
static int rtpfcc_open(URLContext *h, const char *uri, int flags)

{
    av_log(NULL, AV_LOG_INFO, "[%s,%d]rtpfcc_open %s\n", __FUNCTION__,__LINE__,uri);
    init_def_settings();

    //max_rtp_buf = am_getconfig_int_def("media.amplayer.rtp_max",10000);
    fcc_with_fec = am_getconfig_int_def("media.amplayer.fcc_with_fec",0);
    wait_order_timeout = am_getconfig_int_def("media.amplayer.rtp_wait_order_time",100);
    wait_min_queue_size = am_getconfig_int_def("media.amplayer.rtp_min_queue_size", 10);
    wait_order_packet_low = am_getconfig_int_def("media.amplayer.rtp_wait_order_pkt",30);
    sequence_order_range = am_getconfig_int_def("media.amplayer.rtp_seq_order_range", 500);
    normal_wait_first_rtcp_timeout = am_getconfig_int_def("media.amplayer.n_wait_first_rtcp_timeout", 600);
    fast_wait_first_rtcp_timeout = am_getconfig_int_def("media.amplayer.f_wait_first_rtcp_timeout", 80);
    unicast_data_without_fec_number = am_getconfig_int_def("media.amplayer.unicastwithfec",-1); // -1: unicast does not do FEC. other numbers: Start FEC after number packets have been read.
    wait_first_unicast_packet_timeout = am_getconfig_int_def("media.amplayer.wait_first_unicast_timeout", 500);
    fccread_wait_multicast_sync = am_getconfig_int_def("media.amplayer.read_wait_multi_sync", 1);
    threshold_of_read_drop_packet = am_getconfig_int_def("media.amplayer.read_drop_packet", -20);
    force_output_packet_num = am_getconfig_int_def("media.amplayer.force_output_packet_num", 200);
    receive_unicast_max_time = am_getconfig_int_def("media.amplayer.receive_unicast_max_time", 600000);

    av_log(NULL, AV_LOG_INFO, "[%s:%d] max_rtp_buf:%d, wait_order_timeout:%dms, wait_min_queue_size:%d, wait_order_packet_low:%d,"
        "sequence_order_range:%d, normal_wait_first_rtcp_timeout:%d, fast_wait_first_rtcp_timeout:%d, unicast_data_without_fec_number:%d "
        "wait_first_unicast_packet_timeout:%d, fccread_wait_multicast_sync:%d, threshold_of_read_drop_packet:%d, receive_unicast_max_time:%d",
        __FUNCTION__, __LINE__,max_rtp_buf, wait_order_timeout, wait_min_queue_size, wait_order_packet_low, sequence_order_range,
        normal_wait_first_rtcp_timeout, fast_wait_first_rtcp_timeout, unicast_data_without_fec_number, wait_first_unicast_packet_timeout,
    fccread_wait_multicast_sync, threshold_of_read_drop_packet, receive_unicast_max_time);

    RtpFccContext *s = h->priv_data;
    s = av_mallocz(sizeof(RtpFccContext));
    s->feccontext = av_mallocz(sizeof(RTPFECContext));
    if (NULL == s || NULL == s->feccontext)
    {
        return AVERROR(ENOMEM);
    }
    s->ThreadStatus = 0;
    s->Unicast.Status = 0;
    s->Multicast.Status = 0;
    s->Signalling.Status = 0;
    s->MulticastAndFec.Status = 0;
    s->FCC_Version = am_getconfig_int_def("media.amplayer.FCC_Version", 0);
    s->FCC_Server_validtime_default = am_getconfig_int_def("media.amplayer.FCC_validtime", 3600);
    s->Client_identifier = 0;
    s->First_Unicast_Seq = 0;
    s->Bitrate = 0;
    av_log(NULL, AV_LOG_INFO, "FCC_Version:%d\n", s->FCC_Version);
    if (s->FCC_Version != FCC_telecom && wait_first_unicast_packet_timeout < 30000)
    {
        wait_first_unicast_packet_timeout = 30000;
        av_log(NULL, AV_LOG_INFO, "wait_first_unicast_packet_timeout add to %d, when NAT traversal\n", wait_first_unicast_packet_timeout);
    }
    struct in_addr addr;
    char buf[1024]={0};
    char path[1024]={0};
    char hostname[256]={0};
    strcpy(s->url, uri);
    s->flags = flags;
    s->source_Ip = 0;
    const char *p;
    memset(s->Multicast.StrIp,0,sizeof(s->Multicast.StrIp));
    av_url_split(NULL, 0, s->source_StrIpl, sizeof(s->source_StrIpl), hostname, sizeof(hostname), &s->Multicast.Port, path, sizeof(path), uri);
    if (1 != check_ip_string(s->source_StrIpl, strlen(s->source_StrIpl))) // todo check v6
    {
        memset(s->source_StrIpl, 0x00, sizeof(s->source_StrIpl)/sizeof(char));
        igmp_version = 0;
    }
    else
    {
        igmp_version = 3;
        av_log(NULL, AV_LOG_INFO, "rtpfcc_open sources: %s, igmp_version=3\n", s->source_StrIpl);
        s->source_Ip = IpToInt(s->source_StrIpl, 0);
        av_log(NULL, AV_LOG_INFO, "[%s:%d] s->source_StrIpl: %s, s->source_Ip:%x\n", __FUNCTION__, __LINE__, s->source_StrIpl, s->source_Ip);
    }

    strcpy(s->Multicast.StrIp,hostname);
    av_log(NULL, AV_LOG_INFO, "[%s,%d],s->Multicast.StrIp %s,hostname:%s\n", __FUNCTION__,__LINE__,s->Multicast.StrIp,hostname);
    if (s->FCC_Version == FCC_huawei_tlv && strstr(s->Multicast.StrIp,":") != NULL) {
        s->isMultiIpv6 = 1;
    }
    if (!s->isMultiIpv6) {
        s->Multicast.Ip = IpToInt(s->Multicast.StrIp,0);
    } else {
        Ipv6ToInt(s->Multicast.StrIp,0,s->Multicast.Ipv6);
    }


    av_log(NULL, AV_LOG_INFO, "[%s,%d],s->Multicast.Ip %x\n", __FUNCTION__,__LINE__,s->Multicast.Ip);
    snprintf(s->Multicast.StrPort,sizeof(s->Multicast.StrPort),"%d",s->Multicast.Port);
    av_log(NULL, AV_LOG_INFO,
    "[%s:%d]s->Multicast.StrIp:%s,MulticastIp:%#x,rtp_port:%d\n",__FUNCTION__,__LINE__,s->Multicast.StrIp,s->Multicast.Ip,s->Multicast.Port);

    h->priv_data = s;
    p = strchr(uri, '?');
    if (NULL != p)
    {
        if (av_find_info_tag(buf, sizeof(buf), "ChannelFCCIP", p))
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d]buf: %s,p:%s\n",__FUNCTION__,__LINE__,buf,p);
            memcpy(s->Signalling.StrIp, buf,sizeof(s->Signalling.StrIp));
            if (s->FCC_Version == FCC_huawei_tlv && strstr(s->Signalling.StrIp,":") != NULL) {
                s->isIpv6 = 1;
            }
            if (s->isIpv6 != 1) {
                s->Signalling.Ip = IpToInt(s->Signalling.StrIp,0);
                av_log(NULL, AV_LOG_INFO, "[%s:%d]s->Signalling.StrIp: %s,s->Signalling.Ip:%x\n",__FUNCTION__,__LINE__,s->Signalling.StrIp,s->Signalling.Ip);
            } else {
                Ipv6ToInt(s->Signalling.StrIp,0,s->Signalling.Ipv6);
                av_log(NULL, AV_LOG_INFO, "[%s:%d]s->Signalling.StrIp: %s,s->Signalling.Ip:%8x%8x%8x%8x\n",__FUNCTION__,__LINE__,s->Signalling.StrIp,ntohl(s->Signalling.Ipv6[0]),ntohl(s->Signalling.Ipv6[1]),ntohl(s->Signalling.Ipv6[2]),ntohl(s->Signalling.Ipv6[3]));
            }
        }

        if (av_find_info_tag(buf, sizeof(buf), "ChannelFCCPort", p))
        {
            s->Signalling.Port = strtol(buf, NULL, 10);
            snprintf(s->Signalling.StrPort ,sizeof(s->Signalling.StrPort),"%d",s->Signalling.Port);
            av_log(NULL, AV_LOG_INFO, "[%s:%d]s->Signalling.StrPort: %s,s->Signalling.Port:%d\n", __FUNCTION__,__LINE__,s->Signalling.StrPort,s->Signalling.Port);
        }

        if (fcc_with_fec && av_find_info_tag(buf, sizeof(buf), "ChannelFECPort", p))
        {
            s->feccontext->use_multi_and_fec = 1;
            s->feccontext->data_start_fec = 0;
            s->MulticastAndFec.Port = strtol(buf, NULL, 10);
            snprintf(s->MulticastAndFec.StrPort ,sizeof(s->MulticastAndFec.StrPort),"%d",s->MulticastAndFec.Port);
            av_log(NULL, AV_LOG_INFO, "[%s:%d]s->MulticastAndFec.StrPort: %s,s->MulticastAndFec.Port:%d\n", __FUNCTION__,__LINE__,s->MulticastAndFec.StrPort,s->MulticastAndFec.Port);
            memset(rtp_data_array,0,sizeof(rtp_data_array));
            memset(rtp_packet,0,sizeof(rtp_packet));
            memset(fec_data_array,0,sizeof(fec_data_array));
            memset(fec_packet,0,sizeof(fec_packet));
            memset(lost_map,0,sizeof(lost_map));
        }
    }

    if (s->Signalling.Port < 0)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d],s->Signalling.Port:%d\n",__FUNCTION__,__LINE__,s->Signalling.Port);
        goto fail;
    }

    channelcache_init(&g_aryChannelCache);
    channelcache_print(&g_aryChannelCache);
    s->connectState = initFccConnectState(__FUNCTION__);

    int RetSock = -1;
    fcc_directed_node_t* node = NULL;
    if (s->connectState == FCC_FAST_CONNECTING &&
        (node = channelcache_get2(&g_aryChannelCache, s->Multicast.StrIp, s->Multicast.Port, s->isMultiIpv6)))
    {
        char s_port[8] = {0};
        snprintf(s_port, 8, "%d", node->redirect_port);
        if (s->isIpv6) {
            char redirected_ipv6[INET6_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET6, &node->redirect_ip.ip6, redirected_ipv6, INET6_ADDRSTRLEN);
            RetSock = SetupUdpSocket(&s->Signalling.Uc, redirected_ipv6, s_port, node->redirect_port, -1, 0, &h->interrupt_callback);
            av_log(NULL, AV_LOG_INFO, "[%s:%d] setup unicast,use cache, %s:%s(%d)\n", __FUNCTION__, __LINE__, redirected_ipv6, s_port, node->redirect_port);
        } else {
            char redirected_ip[INET_ADDRSTRLEN]={0};
            inet_ntop(AF_INET, &node->redirect_ip.ip4, redirected_ip, INET_ADDRSTRLEN);
            RetSock = SetupUdpSocket(&s->Signalling.Uc, redirected_ip, s_port, node->redirect_port,-1,0, &h->interrupt_callback);
            av_log(NULL, AV_LOG_INFO, "[%s:%d] setup unicast,use cache, %s:%s(%d)\n", __FUNCTION__, __LINE__, redirected_ip, s_port, node->redirect_port);
        }
    } else// if (s->connectState == FCC_NORMAL_CONNECTING)
    {
        RetSock = SetupUdpSocket(&s->Signalling.Uc, s->Signalling.StrIp, s->Signalling.StrPort, s->Signalling.Port,-1,0, &h->interrupt_callback);
        av_log(NULL, AV_LOG_INFO, "[%s:%d] setup unicast, %s:%s(%d)\n", __FUNCTION__, __LINE__, s->Signalling.StrIp, s->Signalling.StrPort, s->Signalling.Port);
    }
    av_log(NULL, AV_LOG_INFO, "[%s:%d] use channel cache %s\n", __FUNCTION__, __LINE__, node ? "yes" : "no");
    if (-1 == RetSock)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d],setup socket fail\n",__FUNCTION__,__LINE__);
        goto fail;
    }
    //
    s->Signalling.Fd = ffurl_get_file_handle(s->Signalling.Uc);
    av_log(NULL, AV_LOG_INFO, "[%s:%d],s->Signalling.Fd:%d\n", __FUNCTION__, __LINE__, s->Signalling.Fd);
    s->Signalling.LocalPort =ff_udp_get_local_port(s->Signalling.Uc);
    s->Unicast.LocalPort = s->Signalling.LocalPort-1;
    av_log(NULL, AV_LOG_INFO, "[%s:%d],s->Signalling.LocalPort:%d,s->Unicast.LocalPort:%d\n", __FUNCTION__,__LINE__,s->Signalling.LocalPort,s->Unicast.LocalPort);
    //
    s->Signalling.Uc->flags = AVIO_FLAG_READ_WRITE;
    s->Signalling.Status = 1;

    av_log(NULL, AV_LOG_INFO, "[%s:%d]create unicast socket!\n", __FUNCTION__, __LINE__);
    //setup the unicast socket to receive the unicast stream //unicast stream local socket
    s->Unicast.Port = 0;
    int ret = SetupUdpSocket(&s->Unicast.Uc, "", "", 0, s->Unicast.LocalPort,1,&h->interrupt_callback);
    if (0 == ret)
    {
        s->Unicast.Fd = ffurl_get_file_handle(s->Unicast.Uc);
        s->Unicast.Status = 1;
        av_log(NULL, AV_LOG_INFO, "[%s:%d],s->Unicast.Fd:%d,s->Status:%d\n", __FUNCTION__, __LINE__,s->Unicast.Fd,s->Unicast.Status);
    }
    else
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d],build unicast socekt fail\n", __FUNCTION__, __LINE__);
    }

    //get the local IP
    memset(s->local_StrIpl, 0, sizeof(s->local_StrIpl));
    s->local_Ip = 0;
    if (s->FCC_Version != FCC_telecom) // nat need local IP
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d],get the local IP, isIpv6=%d\n", __FUNCTION__, __LINE__, s->isIpv6);
        if (!s->isIpv6) {
            do
            {
                int i = 0;
                int sockfd;
                struct ifconf ifconf;
                char buf[512];
                struct ifreq *ifreq;
                char *ip;
                ifconf.ifc_len = 512;
                ifconf.ifc_buf = buf;

                if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
                {
                    break;
                }
                ioctl(sockfd, SIOCGIFCONF, &ifconf);
                close(sockfd);
                ifreq = (struct ifreq *)buf;
                for (i = (ifconf.ifc_len / sizeof(struct ifreq)); i > 0; i--)
                {
                    ip = inet_ntoa(((struct sockaddr_in *)&(ifreq->ifr_addr))->sin_addr);

                    if (strcmp(ip, "127.0.0.1") == 0)
                    {
                        av_log(NULL, AV_LOG_INFO, "[%s,%d], 127.0.0.1\n", __FUNCTION__, __LINE__);
                        ifreq++;
                        continue;
                    }
                    memset(s->local_StrIpl, 0, sizeof(s->local_StrIpl));
                    strcpy(s->local_StrIpl, ip);
                    s->local_Ip = IpToInt(s->local_StrIpl, 0);
                    av_log(NULL, AV_LOG_INFO, "[%s,%d], local_StrIpl %s, local_Ip:%x\n", __FUNCTION__, __LINE__, s->local_StrIpl, s->local_Ip);
                    break;
                }
            } while (0);
        }
        else {
            do {
                FILE *f;
                int ret, scope, prefix;
                unsigned char ipv6[16];
                char dname[IFNAMSIZ];
                char address[INET6_ADDRSTRLEN];
                char *scopestr;

                f = fopen("/proc/net/if_inet6", "r");
                if (f == NULL) {
                    return -1;
                }
                /* info get from /proc/net/if_inet6 like:
                *00000000000000000000000000000001 01 80 10 80       lo
                *but the length of name is not certain
                */
                while (18 == fscanf(f,
                    " %2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx %*x %x %x %*x %*s",
                    &ipv6[0],
                    &ipv6[1],
                    &ipv6[2],
                    &ipv6[3],
                    &ipv6[4],
                    &ipv6[5],
                    &ipv6[6],
                    &ipv6[7],
                    &ipv6[8],
                    &ipv6[9],
                    &ipv6[10],
                    &ipv6[11],
                    &ipv6[12],
                    &ipv6[13],
                    &ipv6[14],
                    &ipv6[15],
                    &prefix,
                    &scope)) {

                    if (inet_ntop(AF_INET6, ipv6, address, sizeof(address)) == NULL) {
                        continue;
                    }
                    if (strcmp(address, "::1") == 0) {
                        av_log(NULL, AV_LOG_INFO, "[%s,%d], ::1, skip it\n", __FUNCTION__, __LINE__);
                        continue;
                    }

                    if (scope == IPV6_ADDR_LINKLOCAL) {
                        av_log(NULL, AV_LOG_INFO, "[%s,%d], the addr is link local, skip it\n", __FUNCTION__, __LINE__);
                        continue;
                    }

                    memset(s->local_StrIpl, 0, sizeof(s->local_StrIpl));
                    strcpy(s->local_StrIpl, address);
                    Ipv6ToInt(s->local_StrIpl, 0, s->local_Ipv6);
                    av_log(NULL, AV_LOG_INFO, "[%s,%d], local_StrIpl %s, local_Ip:%8x%8x%8x%8x\n", __FUNCTION__, __LINE__,
                        s->local_StrIpl, htonl(s->local_Ipv6[0]), htonl(s->local_Ipv6[1]), htonl(s->local_Ipv6[2]), htonl(s->local_Ipv6[3]));
                    break;

                }

                fclose(f);

            }while(0);
        }

    }

    //send rtcp request
    uint8_t RtcpPac[40];
    uint32_t RtcpLen = 40;
    if (s->FCC_Version == FCC_telecom || s->FCC_Version == FCC_fiberhome)
    {
        MakeNewRtcpPac(s, RtcpPac, 2, -1);
        ret = fcc_url_write(s->Signalling.Uc, RtcpPac, RtcpLen, 2);
    }
    else {
        ret = SendRTCPPacHW(s, FCCFMT_RSR);
    }
    if (ret < 0)
    {
        s->first_rtcp_request = 0;
    }
    else
    {
        s->first_rtcp_request = 1;
    }

    s->first_rtcp_send_time = av_gettime();
    s->first_rtcp_response = 0;
    s->feccontext->outlist.max_items               = -1;
    s->feccontext->outlist.item_ext_buf_size       = 0;
    s->feccontext->outlist.muti_threads_access     = 1;
    s->feccontext->outlist.reject_same_item_data   = 0;
    s->CurItem = NULL;
    itemlist_init(&s->feccontext->outlist) ;
    av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
    //

    s->feccontext->recvlist.max_items = 2000;
    s->feccontext->recvlist.item_ext_buf_size = 0;
    s->feccontext->recvlist.muti_threads_access = 1;
    s->feccontext->recvlist.reject_same_item_data = 0;
    itemlist_init(&s->feccontext->recvlist) ;

    s->feccontext->feclist.max_items = 500;
    s->feccontext->feclist.item_ext_buf_size = 0;
    s->feccontext->feclist.muti_threads_access = 1;
    s->feccontext->feclist.reject_same_item_data = 0;
    itemlist_init(&s->feccontext->feclist) ;

    s->feccontext->rtp_seq_discontinue=0;
    s->feccontext->fec_seq_discontinue=0;
    s->feccontext->cur_fec=NULL;
    s->feccontext->bdecode=1;       // 0:test 1:decode
    s->feccontext->brunning = 1;
    s->feccontext->total_num = 0;
    s->feccontext->pre_fec_ratio = 0;
    s->feccontext->after_fec_ratio = 0;
    s->feccontext->total_num_last = 0;
    s->feccontext->pre_fec_lost = 0;
    s->feccontext->after_fec_lost = 0;
    s->feccontext->pre_fec_lost_last = 0;
    s->feccontext->after_fec_lost_last = 0;
    s->feccontext->last_time = 0;

    s->MulticastAndFec.Fd = -1;
    s->MulticastAndFec.Uc = NULL;
    s->MulticastAndFec.Cnt = 0;
    s->MulticastAndFec.LastSeqNum = -1;
    s->MulticastAndFec.firstSeqNum = -1;

    first_multi_num = 0;
    stop_receive_unicast = 0;
    out_last_seq_num = 0;
    out_packet_num = 0;
    //fcc should not join multicast
    av_log(NULL, AV_LOG_INFO, "[%s:%d]How many packets will start using FEC:%d\n", __FUNCTION__,__LINE__, unicast_data_without_fec_number);
    if (s->feccontext->use_multi_and_fec && -1 != unicast_data_without_fec_number)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d]fec, will join multicast\n", __FUNCTION__,__LINE__);
        URLContext* ptmpMultAndFecUc = NULL;
        ret = SetupUdpSocket(&ptmpMultAndFecUc, s->Multicast.StrIp, s->MulticastAndFec.StrPort, s->MulticastAndFec.Port,-1,1,&h->interrupt_callback);
        if (0 == ret)
        {
            s->MulticastAndFec.Fd = ffurl_get_file_handle(ptmpMultAndFecUc);
        // Rfc->Multicast.Status = 1;
            s->MulticastAndFec.Uc = ptmpMultAndFecUc;
            av_log(NULL, AV_LOG_INFO, "[%s:%d],Rfc->MulticastAndFec.Fd:%d,Rfc->MultiCastStatus:%d\n", __FUNCTION__,__LINE__, s->MulticastAndFec.Fd, s->Multicast.Status);
        }
        else
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d],build Multicast And Fec socekt fail\n", __FUNCTION__, __LINE__);
        }
    }

    s->fccreport_flag = FCC_REPORT_NONE;
    //s->Unicast.Fd   = -1;
    s->Multicast.Fd = -1;
    s->Signalling.Status = 1;
    s->Unicast.Cnt = 0;
    s->Unicast.stopReceive = 0;
    s->Multicast.Cnt = 0;
    s->Signalling.Cnt = 0;
    s->unicast_packet_received = 0;
    s->receive_unicast_begin_time = -1;
    s->last_receive_multicast_time = -1;
    //
    s->LastSeqNum = -1;
    s->output_number = -1;
    s->FirstMulticastSeq = -1;
    s->Signalling.LastSeqNum = -1;
    s->Unicast.LastSeqNum = -1;
    s->Multicast.LastSeqNum = -1;
    s->Multicast.firstSeqNum = -1;
    //
    if (s->first_rtcp_request == 0 && !s->feccontext->use_multi_and_fec)
    {
        av_log(NULL, AV_LOG_INFO, "[%s:%d] send first rtcp request failed, force join Multicast\n", __FUNCTION__, __LINE__);
        s->Unicast.stopReceive = 1;
        stop_receive_unicast = 1;
        SendByeRtcp(s, -1);
        if (JoinMulticast(s))
        {
            av_log(NULL, AV_LOG_INFO, "[%s:%d] send first rtcp request failed, and force join Multicast failed\n", __FUNCTION__, __LINE__);
            goto fail;
        }
    }

    av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
    s->ThreadStatus = 3;
    if (pthread_create(&(s->RecvThread), NULL, RtpFccRecvTask, s))
    {
        av_log(NULL, AV_LOG_ERROR, "[%s:%d]creat receive thread failed\n",__FUNCTION__,__LINE__);
        s->ThreadStatus = 2;
        goto fail;
    }
    pthread_setname_np(s->RecvThread, "RecvThread");
    av_log(NULL, AV_LOG_INFO, "[%s:%d]\n", __FUNCTION__, __LINE__);
    return 0;

    fail:

    av_log(NULL, AV_LOG_INFO, "[%s:%d]\n",__FUNCTION__,__LINE__);
    if (NULL != s->Unicast.Uc)
    {
        ffurl_close(s->Unicast.Uc);
    }
    if (NULL != s->Multicast.Uc)
    {
        ffurl_close(s->Multicast.Uc);
    }
    if (NULL != s->Signalling.Uc)
    {
        ffurl_close(s->Signalling.Uc);
    }
    if (NULL != s->MulticastAndFec.Uc)
    {
        ffurl_close(s->MulticastAndFec.Uc);
    }

    av_free(s);
    return AVERROR(EIO);
}

static int rtpfcc_reset(URLContext *h)
{
    RtpFccContext *s = h->priv_data;
    char uri[MAX_URL_SIZE];
    strcpy(uri, s->url);
    int flags = s->flags;

    rtpfcc_close(h);
    rtpfcc_open(h,uri,flags);
    return 0;
}

static void judge_report_error(URLContext *h, int64_t* starttime)
{
    RtpFccContext *s = h->priv_data;
    int64_t curtime;
    curtime = ff_network_gettime();
    if (*starttime <= 0)
        *starttime = curtime;
    if (gd_report_error_enable && (curtime > *starttime + (int64_t)(get_data_timeout*1000*1000)) &&
        (s->fccreport_flag == FCC_REPORT_NONE || s->fccreport_flag == FCC_REPORT_RECOVER))
    {
        s->fccreport_flag = FCC_REPORT_CUTOFF;
        //ffmpeg_notify(h, PLAYER_EVENTS_ERROR, get_data_timeout_error, 0);
        av_log(NULL, AV_LOG_ERROR, "PLAYER_EVENTS_ERROR= %d, s=%p, s->fccreport_flag= %d\n",PLAYER_EVENTS_ERROR, s, s->fccreport_flag);
    }
}


static int rtpfcc_read(URLContext *h, uint8_t *buf, int size)
{
    RtpFccContext *s = h->priv_data;
    struct item *HeadItem = NULL;
    struct item *NextItem = NULL;
    RtpFccFecPacket *HeadRtp = NULL;
    RtpFccFecPacket *NextRtp = NULL;
    struct list_head *NextList = NULL;
    RtpFccFecPacket *lpkt = NULL;
    int readsize=0;
    int single_readsize=0;
    int TimeSleep = 0;
    char isNULL = 0;
    char out_of_sequence = 0;
    //real=valuetime*70
    int64_t starttime = ff_network_gettime();
    int64_t curtime;
    uint8_t * lpoffset=NULL;
    int offset=0;
    uint8_t * lpkt_buf=NULL;
    int len=0;
    int ext=0;
    int csrc = 0;

    s->Signalling.Cnt++;
    int64_t start = av_gettime();
    int64_t last_no_singal_time = av_gettime();

    while (s->Signalling.Status > 0 && size>readsize)
    {
        if (ff_check_interrupt(&h->interrupt_callback))
        {
            av_log(NULL, AV_LOG_ERROR, "[%s:%d]call url_interrupt_cb\n",__FUNCTION__,__LINE__);
            return AVERROR(EIO);
        }

        if (check_net_phy_conn_status() == 0)
        {
            s->network_down = 1;
            break;
        }
        else
        {
            if (s->network_down == 1)
            {
                rtpfcc_reset(h);
                //IPTV-8723,jungle.wang,remember to revalue "s" after reset
                av_log(NULL, AV_LOG_ERROR, "[%s:%d],h->priv_data:%p,s:%p\n",__FUNCTION__,__LINE__,h->priv_data,s);
                s = h->priv_data;
            }
            s->network_down = 0;
        }

        if (s->CurItem != NULL)
            goto do_read;

      //  av_log(NULL, AV_LOG_ERROR, "[%s:%d] HeadItem = itemlist_peek_head(&s->feccontext->outlist) \n",__FUNCTION__,__LINE__);
        HeadItem = itemlist_peek_head(&s->feccontext->outlist);
        if (NULL == HeadItem)
        {
//            av_log(NULL, AV_LOG_ERROR, "[%s:%d](NULL == HeadItem)\n",__FUNCTION__,__LINE__);
            usleep(1);
            ++TimeSleep;
            judge_report_error(h, &starttime);
            continue;
        }
        //

        HeadRtp = (RtpFccFecPacket*)HeadItem->item_data;
        NextList = HeadItem->list.next;
        isNULL = &s->feccontext->outlist.list == NextList;
        if (isNULL)
        {
 //           av_log(NULL, AV_LOG_ERROR, "[%s:%d],s->recvlist.item_count:%d\n",__FUNCTION__,__LINE__, s->feccontext->outlist.item_count);
            usleep(1);
            ++TimeSleep;
            judge_report_error(h, &starttime);
            continue;
        }
        NextItem = list_entry(NextList, struct item, list);

        if (NULL == NextItem)
        {
 //           av_log(NULL, AV_LOG_ERROR, "[%s:%d](NULL == NextItem)\n",__FUNCTION__,__LINE__);
            usleep(1);
            ++TimeSleep;
            judge_report_error(h, &starttime);
            continue;
        }

        /* +[SE] [BUG][TV-14279][dewei.yao] Read unicast data on unicast multicast synchronization or after no unicast stream for a period of time */
        if (fccread_wait_multicast_sync && -1 != s->Multicast.firstSeqNum && seq_greater_and_equal(HeadRtp->seq, s->Multicast.firstSeqNum) && stop_receive_unicast != 1)
        {
            if ((int)(av_gettime() - last_no_singal_time)/1000 >= 10)
            {
                av_log(NULL, AV_LOG_INFO, "[%s:%d] Cannot read multicast data before receiving stopReceive\n",__FUNCTION__,__LINE__);
                last_no_singal_time = av_gettime();
            }
            usleep(10);
            judge_report_error(h, &starttime);
            continue;
        }

        //after fec success and the out_last_seq_num has not update, the delayed packet came.
        if ((int16_t)(HeadRtp->seq - out_last_seq_num) <= 0 && (int16_t)(HeadRtp->seq - out_last_seq_num) >= threshold_of_read_drop_packet && out_packet_num != 0)
        {
            struct item *freeitem = itemlist_get_head(&s->feccontext->outlist);
            if (freeitem->item_data)
            {
                rtpfec_free_packet((void *)freeitem->item_data);
                freeitem->item_data = 0;
            }
            item_free(freeitem);
            freeitem = NULL;
            av_log(NULL, AV_LOG_INFO, "[%s,%d] free one packet in rtpfcc_read. HeadRtp->seq %d <= s->LastSeqNum %d\n", __FUNCTION__,__LINE__, HeadRtp->seq, s->LastSeqNum);
            judge_report_error(h, &starttime);
            continue;
        }

        if ( s->LastSeqNum != -1 && (s->feccontext->outlist.item_count < wait_min_queue_size) && ((s->LastSeqNum + 1) % MAX_RTP_SEQ != HeadRtp->seq))
        {
            usleep(10);
            ++TimeSleep;
            av_log(NULL, AV_LOG_ERROR, "[%s:%d]one discontinuity seq=%d, the right seq=%d, waiting...\n",__FUNCTION__,__LINE__, HeadRtp->seq,(s->LastSeqNum+1)%MAX_RTP_SEQ);
            judge_report_error(h, &starttime);
            continue;
        }
        if (NULL == s->CurItem)
        {
            s->CurItem = itemlist_get_head(&s->feccontext->outlist);

            if (NULL == s->CurItem)
            {
                av_log(NULL, AV_LOG_INFO, "[%s,%d] \n", __FUNCTION__,__LINE__);
                usleep(1);
                judge_report_error(h, &starttime);
                continue;
            }
        }

do_read:
        lpkt = (RtpFccFecPacket*)s->CurItem->item_data;
        if (lpkt->valid_data_offset == 0)
        {
            lpkt_buf=lpkt->buf;
            len=lpkt->len;
            if (lpkt_buf[0] & 0x20)
            {
                // remove the padding data
                int padding = lpkt_buf[len - 1];
                if (len >= 12 + padding)
                {
                    len -= padding;
                }
            }

            if (len <= 12)
            {
                av_log(NULL, AV_LOG_ERROR, "[%s:%d]len<=12,len=%d\n",__FUNCTION__,__LINE__,len);
                judge_report_error(h, &starttime);
                continue;
            }
            offset = 12;
            lpoffset = lpkt_buf + 12;

            csrc = lpkt_buf[0] & 0x0f;
            ext = lpkt_buf[0] & 0x10;
            if (ext > 0)
            {
                offset += 4*csrc;
                lpoffset += 4*csrc;
                if (len < offset + 4)
                {
                    av_log(NULL, AV_LOG_ERROR, "[%s:%d]len < offset + 4\n",__FUNCTION__,__LINE__);
                    judge_report_error(h, &starttime);
                    continue;
                }

                ext = (AV_RB16(lpoffset + 2) + 1) << 2;
                if (len < ext + offset)
                {
                    av_log(NULL, AV_LOG_ERROR, "[%s:%d]len < ext + offset\n",__FUNCTION__,__LINE__);
                    judge_report_error(h, &starttime);
                    continue;
                }
                offset += ext;
                lpoffset += ext;
            }
            lpkt->valid_data_offset = offset;
        }
        // output the playload data
        starttime = 0;
        if (s->fccreport_flag == FCC_REPORT_CUTOFF)
            s->fccreport_flag = FCC_REPORT_RECOVER;
        single_readsize=min(lpkt->len-lpkt->valid_data_offset, size-readsize);
        memcpy(buf+readsize,lpkt->buf+lpkt->valid_data_offset,single_readsize);
        s->output_number++;
        readsize+=single_readsize;
        lpkt->valid_data_offset+=single_readsize;

        if (lpkt->valid_data_offset >= lpkt->len)
        {
            if ((s->LastSeqNum + 1) % MAX_RTP_SEQ != lpkt->seq && -1 != s->LastSeqNum)
            {
                av_log(NULL, AV_LOG_ERROR, "[%s:%d]discontinuity seq=%d, the right seq=%d\n",__FUNCTION__,__LINE__, lpkt->seq,(s->LastSeqNum+1)%MAX_RTP_SEQ);
            }
           // av_log(NULL, AV_LOG_ERROR, "[%s:%d] print the rtpfcc_read seq=%d\n",__FUNCTION__,__LINE__, lpkt->seq);
            s->LastSeqNum = lpkt->seq;
            out_last_seq_num = lpkt->seq;
            out_packet_num++;
            // already read, no valid data clean it
            item_free(s->CurItem);
            s->CurItem = NULL;
            rtpfec_free_packet((void *)lpkt);
            lpkt = NULL;
        }
        if (TimeSleep > 0)
        {
            //av_log(NULL, AV_LOG_ERROR, "[%s:%d]TimeSleep:%d, ThreadStatus:%d\n",__FUNCTION__,__LINE__, TimeSleep, s->ThreadStatus);
            TimeSleep = 0;
        }
    }

    int TimeCost = (int)(av_gettime() - start)/1000;
    if (TimeCost >= 50)
    {
        av_log(NULL, AV_LOG_INFO, "[%s,%d],size:%d/%d, seq:%d,(Uni:%d,outlist:%d),(multi:%d,recvlist:%d)CntRead:%d,use %d ms \n", __FUNCTION__,__LINE__,
        size,readsize,s->LastSeqNum,s->Unicast.LastSeqNum,s->feccontext->outlist.item_count,s->Multicast.LastSeqNum,
        s->feccontext->recvlist.item_count, s->Signalling.Cnt,TimeCost);
    }
    if (s->last_readtime && (start/1000 -s->last_readtime) > 1000)
    {
        av_log(NULL, AV_LOG_INFO, "[%s,%d],size:%d/%d, seq:%d,(Uni:%d,outlist:%d),(multi:%d,recvlist:%d) ,dur:%lld\n", __FUNCTION__,__LINE__,
        size,readsize,s->LastSeqNum,s->Unicast.LastSeqNum,s->feccontext->outlist.item_count,s->Multicast.LastSeqNum,
        s->feccontext->recvlist.item_count,start/1000 -s->last_readtime);
    }
    s->last_readtime = av_gettime()/1000;


    if (readsize <= 0)
    {
        av_log(NULL, AV_LOG_ERROR, "[%s:%d]readsize <= 0:%d\n",__FUNCTION__,__LINE__,readsize);
        return AVERROR(EAGAIN);
    }

    if (s->first_packet_read == 0) {
        s->first_packet_read = 1;
        av_log(NULL, AV_LOG_INFO, "[%s:%d] first_packet_read, size:%d\n", __FUNCTION__, __LINE__, readsize);
    }

    return readsize;
}


static int rtpfcc_close(URLContext *h)
{

    RtpFccContext *s = h->priv_data;
    SendByeRtcp(s,-1);
    s->Unicast.Status = -1;
    s->Multicast.Status = -1;
    s->Signalling.Status = -1;
    s->ThreadStatus = 2;
    #if 1
    pthread_join(s->RecvThread, NULL);
    #else
    //wait for receive thread quit
    if (3 <= s->ThreadStatus)
    {
        int SleepTime = 0;

        while (0xb != s->ThreadStatus)
        {
            usleep(1);
            SleepTime++;
            if (SleepTime >= 1000000)
            {
                av_log(NULL, AV_LOG_INFO, "[%s,%d],error:wait for receive thread quit timeout,SleepTime:%d,s->ThreadStatus:%x \n", __FUNCTION__,__LINE__,SleepTime,s->ThreadStatus);
                return -1;
            }
        }
        av_log(NULL, AV_LOG_INFO, "[%s,%d],wait for receive thread quit,SleepTime:%d,s->ThreadStatus:%x \n", __FUNCTION__,__LINE__,SleepTime,s->ThreadStatus);
    }
    #endif
    av_log(NULL, AV_LOG_INFO, "[%s,%d],s->ThreadStatus:%x \n", __FUNCTION__,__LINE__,s->ThreadStatus);
    s->RecvThread = 0;
    itemlist_clean(&s->feccontext->outlist, rtpfec_free_packet);
    itemlist_clean(&s->feccontext->recvlist, rtpfec_free_packet);
    itemlist_clean(&s->feccontext->feclist, rtpfec_free_packet);
    channelcache_refresh(&g_aryChannelCache);
    out_last_seq_num = 0;
    out_packet_num = 0;
    if (NULL != s->MulticastAndFec.Uc)
    {
        int ret = -1;
        ret = ffurl_close(s->MulticastAndFec.Uc);
        av_log(NULL, AV_LOG_INFO, "[%s,%d] close MulticastAndFec ret:%d\n", __FUNCTION__,__LINE__, ret);
        s->MulticastAndFec.Uc = NULL;
    }
    if (NULL != s->Unicast.Uc)
    {
        int ret = -1;
        ret = ffurl_close(s->Unicast.Uc);
        av_log(NULL, AV_LOG_INFO, "[%s,%d] close Unicast ret:%d\n", __FUNCTION__,__LINE__, ret);
        s->Unicast.Uc = NULL;
    }
    if (NULL != s->Multicast.Uc)
    {
        int ret  = -1;

        ret = ffurl_close(s->Multicast.Uc);
        av_log(NULL, AV_LOG_INFO, "[%s,%d] close Multicast ret:%d\n", __FUNCTION__,__LINE__, ret);

        s->Multicast.Uc = NULL;
    }
    if (NULL != s->Signalling.Uc)
    {
        int ret = -1;
        ret = ffurl_close(s->Signalling.Uc);
        av_log(NULL, AV_LOG_INFO, "[%s,%d] close Signalling ret:%d\n", __FUNCTION__,__LINE__, ret);
        s->Signalling.Uc = NULL;
    }
    av_free(s);
    h->priv_data = NULL;
    av_log(NULL, AV_LOG_INFO, "[%s,%d] \n", __FUNCTION__,__LINE__);
    return 0;
}

const URLProtocol ff_rtp_protocol = {
    .name                      = "rtp",
    .url_open                  = rtp_open,
    .url_read                  = rtp_read,
    .url_write                 = rtp_write,
    .url_close                 = rtp_close,
    .url_get_file_handle       = rtp_get_file_handle,
    .url_get_multi_file_handle = rtp_get_multi_file_handle,
    .priv_data_size            = sizeof(RTPContext),
    .flags                     = URL_PROTOCOL_FLAG_NETWORK,
    .priv_data_class           = &rtp_class,
};

URLProtocol ff_rtpfcc_protocol =
{
    .name           = "rtpfcc",
    .url_open       = rtpfcc_open,
    .url_read       = rtpfcc_read,
    .url_write      = NULL,
   .url_close      = rtpfcc_close,
};

/*URLProtocol ff_rtpfec_protocol = {
    .name                = "rtpfec",
    .url_open            = rtpfec_open,
    .url_read            = rtpfec_read,
    .url_write           = NULL,
    .url_close           = rtpfec_close,
};*/
