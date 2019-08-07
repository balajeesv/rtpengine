#include "jitter_buffer.h"
#include "media_socket.h"
#include "media_player.h"
#include "call.h"

#define CONT_SEQ_COUNT 0x64
#define CONT_MISS_COUNT 0x0A


static struct jitter_buffer_config *jb_config; 

struct jitter_buffer_config {
        unsigned int    min_jb_len;
        unsigned int    max_jb_len;
        int             enable_jb;
};

void jitter_buffer_init(unsigned int min, unsigned int max) {
        struct jitter_buffer_config *config;

        ilog(LOG_INFO, "jitter_buffer_init");

        if (jb_config)
                return;

        config = malloc(sizeof(*config));
        ZERO(*config);
        config->min_jb_len = min;
        config->max_jb_len = max;
        config->enable_jb  = 1;

        jb_config = config;

        return;
}

static void reset_jitter_buffer(struct jitter_buffer *jb){
        ilog(LOG_INFO, "reset_jitter_buffer");
        jb->first_send_ts  	= 0;
        jb->first_send.tv_sec 	= 0;
        jb->first_send.tv_usec 	= 0;
        jb->first_seq     	= 0;
        jb->rtptime_delta 	= 0;
        jb->buffer_len    	= 0;
        jb->cont_frames		= 0;
        jb->cont_miss		= 0;
        jb->next_exp_seq  	= 0;
        jb->clock_rate    	= 0;
        jb->payload_type  	= 0;

        if(jb->p)
	{
           free(jb->p->s.s);
           codec_packet_free(jb->p);
	}
        
        jb->num_resets++;

        if(jb->num_resets >= 2 && jb->call)
               jb->call->enable_jb = 0;
}

static int get_clock_rate(struct packet_handler_ctx *phc, int payload_type){
	const struct rtp_payload_type *rtp_pt = NULL;
        int clock_rate = 0;
        if(phc->sink->jb.clock_rate && phc->sink->jb.payload_type == payload_type)
        {
                return phc->sink->jb.clock_rate;
        }
	struct codec_handler *transcoder = codec_handler_get(phc->mp.media, payload_type);
	if(transcoder)
	{
		if(transcoder->source_pt.payload_type == payload_type)
			rtp_pt = &transcoder->source_pt;
		if(transcoder->dest_pt.payload_type == payload_type)
			rtp_pt = &transcoder->dest_pt;
	}
	if(rtp_pt)
        {
                clock_rate = phc->sink->jb.clock_rate = rtp_pt->clock_rate;
                phc->sink->jb.payload_type = payload_type;
       }
        else 
                ilog(LOG_ERROR, "ERROR clock_rate not present");
       
       return clock_rate;
}

static struct codec_packet* get_codec_packet(struct packet_handler_ctx *phc){
	struct codec_packet *p = g_slice_alloc0(sizeof(*p));
	p->s = phc->s;
	p->packet = g_slice_alloc0(sizeof(*p->packet));
	p->packet->sfd = phc->mp.sfd;
	p->packet->fsin = phc->mp.fsin;
	p->packet->tv = phc->mp.tv;
	p->free_func = NULL; // TODO check free
	p->packet->buffered =1;
        
        return p;
}

static void check_buffered_packets(struct jitter_buffer *jb, unsigned int len){
	if(len >= (2* jb_config->max_jb_len))
		reset_jitter_buffer(jb);
}

static int queue_packet(struct packet_handler_ctx *phc, struct codec_packet *p){

	rtp_payload(&phc->mp.rtp, &phc->mp.payload, &p->s);
	unsigned long ts = ntohl(phc->mp.rtp->timestamp);
	int payload_type =  (phc->mp.rtp->m_pt & 0x7f);
        int clockrate = get_clock_rate(phc, payload_type);

        if(!clockrate)
        {
            reset_jitter_buffer(&phc->sink->jb);
            return 1;
        }
	uint32_t ts_diff = (uint32_t) ts - (uint32_t) phc->sink->jb.first_send_ts; // allow for wrap-around
	if(!phc->sink->jb.rtptime_delta)
	{
		int seq_diff = ntohs(phc->mp.rtp->seq_num) - phc->sink->jb.first_seq;
		phc->sink->jb.rtptime_delta = ts_diff/seq_diff;
	}
	p->to_send = phc->sink->jb.first_send;
	unsigned long long ts_diff_us =
		(unsigned long long) (ts_diff + (phc->sink->jb.rtptime_delta * phc->sink->jb.buffer_len))* 1000000 / clockrate; //phc->sink->encoder_format.clockrate
	timeval_add_usec(&p->to_send, ts_diff_us);

	// how far in the future is this?
	ts_diff_us = timeval_diff(&p->to_send, &rtpe_now); // negative wrap-around to positive OK

	if (ts_diff_us > 1000000) // more than one second, can't be right
		phc->sink->jb.first_send.tv_sec = 0; // fix it up below

	g_queue_push_tail(&phc->mp.packets_out, p);
	return media_socket_dequeue(&phc->mp, phc->sink);
}

int buffer_packet(struct packet_handler_ctx *phc) {
        int ret=0;
        char *buffer;
	buffer = malloc(phc->s.len);
	memcpy(buffer, phc->s.s, phc->s.len);
	str_init_len(&phc->s, buffer, phc->s.len);

        phc->mp.stream = phc->mp.sfd->stream;
        phc->sink = phc->mp.stream->rtp_sink;
        phc->mp.media = phc->mp.stream->media;
        __C_DBG("Handling packet on: %s:%d", sockaddr_print_buf(&phc->mp.stream->endpoint.address),
                        phc->mp.stream->endpoint.port);
        if(phc->sink)
        {
                mutex_lock(&phc->sink->jb.lock);
                struct codec_packet *p = get_codec_packet(phc);
                if (phc->sink->jb.first_send.tv_sec) {
                        queue_packet(phc,p);
                        if(phc->sink->jb.p)
                         {
				 queue_packet(phc,phc->sink->jb.p);
                                 phc->sink->jb.p = NULL;
                         }
                }
                else {
			rtp_payload(&phc->mp.rtp, &phc->mp.payload, &p->s);
			unsigned long ts = ntohl(phc->mp.rtp->timestamp);
                        p->to_send = phc->sink->jb.first_send = rtpe_now;
                        phc->sink->jb.first_send_ts = ts;
                        phc->sink->jb.first_seq = ntohs(phc->mp.rtp->seq_num);
                        phc->sink->jb.call = phc->sink->call;
                }
                check_buffered_packets(&phc->sink->jb, get_queue_length(phc->sink->buffer_timer));
                mutex_unlock(&phc->sink->jb.lock);
        }
        else
        {
               ilog(LOG_INFO, "phc->sink is NULL");
        }
        return ret;
}

static void increment_buffer(unsigned int* buffer_len)
{
    if(*buffer_len < jb_config->max_jb_len)
           (*buffer_len)++;
}

static void decrement_buffer(unsigned int *buffer_len)
{
    if(*buffer_len > jb_config->min_jb_len)
          (*buffer_len)--;
}

int set_jitter_values(struct jitter_buffer *jb, int curr_seq)
{
        int ret=0;
	if(jb->next_exp_seq)
	{
                mutex_lock(&jb->lock);
		if(curr_seq > jb->next_exp_seq)
		{
			ilog(LOG_DEBUG, "missing seq exp seq =%d, received seq= %d", jb->next_exp_seq, curr_seq);
                        increment_buffer(&jb->buffer_len);
			jb->cont_frames = 0;
			jb->cont_miss++;
                        if(jb->cont_miss >= CONT_MISS_COUNT)
				reset_jitter_buffer(jb);
		}
		else if(curr_seq < jb->next_exp_seq) //Might be duplicate or sequence already crossed
			ret=1;
		else
		{
			jb->cont_frames++;
			jb->cont_miss = 0;
			if(jb->cont_frames >= CONT_SEQ_COUNT)
			{
                                decrement_buffer(&jb->buffer_len);   
				jb->cont_frames = 0;
				ilog(LOG_DEBUG, "Received continous frames Buffer len=%d", jb->buffer_len);
			}
		}
                mutex_unlock(&jb->lock);
	}
	jb->next_exp_seq = curr_seq + 1;

        return ret;
}
