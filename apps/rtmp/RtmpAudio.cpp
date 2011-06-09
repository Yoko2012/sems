#include "RtmpAudio.h"
#include "RtmpSender.h"

#include "amci/codecs.h"

#define SPEEX_WB_SAMPLES_PER_FRAME 320

RtmpAudio::RtmpAudio(RtmpSender* s, unsigned int play_stream_id)
  : playout_buffer(this), sender(s), play_stream_id(play_stream_id),
    recv_offset_i(false), recv_rtp_offset(0), recv_rtmp_offset(0),
    send_offset_i(false), send_rtmp_offset(0)
{
  init_codec();
}

RtmpAudio::~RtmpAudio()
{
  speex_encoder_destroy(enc_state.state);
  speex_bits_destroy(&enc_state.bits);

  speex_decoder_destroy(dec_state.state);
  speex_bits_destroy(&dec_state.bits);
}

void RtmpAudio::init_codec()
{
  int val;

  enc_state.state = speex_encoder_init(&speex_wb_mode);
  speex_bits_init(&enc_state.bits);

  val = 8;
  speex_encoder_ctl(enc_state.state, SPEEX_SET_QUALITY, &val);

  dec_state.state = speex_decoder_init(&speex_wb_mode);
  speex_bits_init(&dec_state.bits);
}


int RtmpAudio::wb_decode(unsigned int size)
{
  // - decode into back-buffer
  // - down-sample from back-buffer into front-buffer

  short* pcm = (short*)samples.back_buffer();
  speex_bits_read_from(&dec_state.bits, (char*)(unsigned char*)samples, size);

  /* We don't know where frame boundaries are,
     but the minimum frame size is 43 */
  int out_size = 0;
  while (speex_bits_remaining(&dec_state.bits)>40) {

    switch(speex_decode_int(dec_state.state, &dec_state.bits, pcm)) {
	
    case -2:
      ERROR("Corrupted stream!\n");
      return -1;
	
    case -1: goto downsample;
    default: break;
    }

    pcm      += SPEEX_WB_SAMPLES_PER_FRAME;
    out_size += SPEEX_WB_SAMPLES_PER_FRAME*sizeof(short)/2;
  }

 downsample:
  
  short* in  = (short*)samples.back_buffer();
  short* out = (short*)(unsigned char*)samples;

  for(int i=0; i<out_size;i++){
    out[i] = in[i<<1];
  }

  return out_size;
}

int RtmpAudio::wb_encode(unsigned int size)
{
  //TODO:
  // - up-sample into back-buffer
  // - encode into front-buffer
  
  DBG("size = %i\n",size);
  short* out = (short*)samples.back_buffer();
  short* in  = (short*)(unsigned char*)samples;

  for(unsigned int i=0; i<size;i++){
    out[i<<1]     = in[i];
    out[(i<<1)+1] = in[i];
  }
  size *= 2;
  DBG("size = %i\n",size);
  
  div_t blocks = div(size, sizeof(short)*SPEEX_WB_SAMPLES_PER_FRAME);
  if (blocks.rem) {
    ERROR("non integral number of blocks %d.%d\n", blocks.quot, blocks.rem);
    return -1;
  }
  DBG("blocks.quot = %i; blocks.rem = %i\n",blocks.quot,blocks.rem);
    
  in = (short*)samples.back_buffer();
  speex_bits_reset(&enc_state.bits);

  while (blocks.quot--) {

    speex_encode_int(enc_state.state, in, &enc_state.bits);
    in += SPEEX_WB_SAMPLES_PER_FRAME;
  }
    
  int out_size = speex_bits_write(&enc_state.bits, 
				  (char*)(unsigned char*)samples, 
				  AUDIO_BUFFER_SIZE);

  DBG("returning %u encoded bytes\n",out_size);
  return out_size;
}


// returns bytes read, else -1 if error (0 is OK)
int RtmpAudio::get(unsigned int user_ts, unsigned char* buffer, unsigned int nb_samples)
{
  // - buffer RTMP audio
  // - read from RTMP recv buffer

  DBG("get(%u, %u)\n",user_ts,nb_samples);
  process_recv_queue(user_ts);

  u_int32_t rlen = playout_buffer.
    read(user_ts<<1,
	 (ShortSample*)((unsigned char*)samples),
	 nb_samples);

  return PCM16_S2B(rlen);
}

int RtmpAudio::read(unsigned int user_ts, unsigned int size)
{
  assert(0);
  return 0;
}

#include <fcntl.h>
/*
static void dump_audio(unsigned char* buffer, unsigned int size)
{
  static int dump_fd=0;
  if(dump_fd == 0){
    dump_fd = open("pcm_out.raw",O_WRONLY|O_CREAT|O_TRUNC,
		   S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
    if(dump_fd < 0)
      ERROR("could not open pcm_out.raw: %s\n",strerror(errno));
  }
  if(dump_fd < 0) return;

  write(dump_fd,buffer,size);
}
*/

// returns bytes written, else -1 if error (0 is OK)
int RtmpAudio::put(unsigned int user_ts, unsigned char* buffer, unsigned int size)
{
  //dump_audio((unsigned char*)buffer,size);
  DBG("size = %i\n",size);

  if(!size){
    return 0;
  }

  if(size>640)
    size = 640;

  // copy into internal buffer
  memcpy((unsigned char*)samples,buffer,size);

  // encode
  int s = wb_encode(size);
  if(s>0){
    // send
    return write(user_ts,(unsigned int)s);
  }
  else{
    return s;
  }
}


static void dump_audio(RTMPPacket *packet)
{
  static int dump_fd=0;
  if(dump_fd == 0){
    dump_fd = open("speex_out.raw",O_WRONLY|O_CREAT|O_TRUNC,
		   S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
    if(dump_fd < 0)
      ERROR("could not open speex_out.raw: %s\n",strerror(errno));
  }
  if(dump_fd < 0) return;

  uint32_t pkg_size = packet->m_nBodySize-1;
  write(dump_fd,&pkg_size,sizeof(uint32_t));
  write(dump_fd,packet->m_body+1,pkg_size);
}

int RtmpAudio::write(unsigned int user_ts, unsigned int size)
{
  m_sender.lock();

  if(!sender) {
    m_sender.unlock();
    return 0;
  }

  // - generate a new RTMP audio packet
  // - send packet

  RTMPPacket packet;
  RTMPPacket_Reset(&packet);

  packet.m_headerType  = send_offset_i ? 
    RTMP_PACKET_SIZE_MEDIUM 
    : RTMP_PACKET_SIZE_LARGE;

  packet.m_packetType  = RTMP_PACKET_TYPE_AUDIO;
  packet.m_nChannel    = 4;//TODO
  packet.m_nInfoField2 = play_stream_id;

  if(!send_offset_i){
    send_rtmp_offset = user_ts;
    send_offset_i = true;
  }
  
  unsigned int rtmp_ts = (user_ts - send_rtmp_offset) / (8000/1000);
  packet.m_nTimeStamp  = rtmp_ts;

  RTMPPacket_Alloc(&packet,size+1);
  packet.m_nBodySize = size+1;

// soundType 	(byte & 0x01) � 0 	
//   0: mono, 1: stereo
// soundSize 	(byte & 0x02) � 1 	
//   0: 8-bit, 1: 16-bit
// soundRate 	(byte & 0x0C) � 2 	
//   0: 5.5 kHz, 1: 11 kHz, 2: 22 kHz, 3: 44 kHz
// soundFormat 	(byte & 0xf0) � 4 	
//   0: Uncompressed, 1: ADPCM, 2: MP3, 5: Nellymoser 8kHz mono, 6: Nellymoser, 11: Speex 


  // 0xB2: speex, 16kHz
  packet.m_body[0] = 0xB2;
  memcpy(packet.m_body+1,(unsigned char*)samples,size);

  DBG("sending audio packet: size=%u rtmp_ts=%u StreamID=%u (rtp_ts=%u)\n",
      size+1,rtmp_ts,play_stream_id,user_ts);

  sender->push_back(packet);
  m_sender.unlock();

  //dump_audio(&packet);
  RTMPPacket_Free(&packet);

  return size;
}

void RtmpAudio::bufferPacket(const RTMPPacket& p)
{
  RTMPPacket np = p;
  if(!RTMPPacket_Alloc(&np,np.m_nBodySize)){
    ERROR("could not allocate packet.\n");
    return;
  }
  memcpy(np.m_body,p.m_body,p.m_nBodySize);

  m_q_recv.lock();
  q_recv.push(np);
  m_q_recv.unlock();
}

void RtmpAudio::process_recv_queue(unsigned int ref_ts)
{
  int size;

  // flush the recv queue into the playout buffer
  m_q_recv.lock();
  while(!q_recv.empty()){

    RTMPPacket p = q_recv.front();
    q_recv.pop();
    m_q_recv.unlock();

    //TODO:
    // - copy RTMP payload into this->samples
    // - decode
    // - put packet in playout buffer

    if(p.m_nBodySize <= (unsigned int)AUDIO_BUFFER_SIZE){
      

      size = p.m_nBodySize-1;
      memcpy((unsigned char*)samples, p.m_body+1, size);

      size = wb_decode(size);
      if(size <= 0){
	ERROR("decode() returned %i\n",size);
	return;
      }
      
      // TODO: generate some reasonable RTP timestamp
      //
      if(!recv_offset_i){
	recv_rtp_offset  = ref_ts;
	recv_rtmp_offset = p.m_nTimeStamp;
	recv_offset_i = true;
      }

      unsigned int rtp_ts = (p.m_nTimeStamp - recv_rtmp_offset) * (8000/1000);

      playout_buffer.write(ref_ts, rtp_ts, (ShortSample*)((unsigned char *)samples), 
			   PCM16_B2S(size), true/*begin_talk*/);
      
      RTMPPacket_Free(&p);    
    }

    m_q_recv.lock();
  }
  m_q_recv.unlock();
}

void RtmpAudio::add_to_history(int16_t *, unsigned int)
{
  return;
}

unsigned int RtmpAudio::conceal_loss(unsigned int, unsigned char *)
{
  return 0;
}

void RtmpAudio::setSenderPtr(RtmpSender* s)
{
  m_sender.lock();
  DBG("sender ptr = %p\n",s);
  sender = s;
  m_sender.unlock();
}
