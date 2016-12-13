
// -----------------------------------------------------------------------
// Standard Librarys
// -----------------------------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>


// -----------------------------------------------------------------------
// Linux Networking
// -----------------------------------------------------------------------

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

// -----------------------------------------------------------------------
// WebRTC Librarys
// -----------------------------------------------------------------------

#include "webrtc/modules/audio_device/include/audio_device.h"
#include "webrtc/modules/audio_device/linux/audio_device_pulse_linux.h"
#include "webrtc/common_audio/signal_processing/include/signal_processing_library.h"
#include "webrtc/modules/audio_processing/include/audio_processing.h"

#include "opus/src/include/opus.h"


int GetTickCount()
{
	struct timeval tv;
	gettimeofday( &tv, 0 );
	return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

double prc( double a, double b, double dist )
{
	double out;
	double diff;
	
	diff = b - a;
	out = a + (diff*dist);
	
	return out;
}

void rsmpl( float *inpcm, float *outpcm, int inln, int outln )
{
	float q;
	q = (float)inln / (float)outln;
	
	int i;
	i = 0;
	int n;
	n = 0;
	int y;
	float x;
	x = 0.0;
	
	float a;
	float b;
	
	for( i = 0; i < outln; i++ )
	{
		a = inpcm[n];
		y = n + 1;
		if( y >= inln )
			y--;
		b = inpcm[y];
		outpcm[i] = prc( a, b, x );
		x += q;
		while( x >= 1.0 )
		{
			x -= 1.0;
			n++;
		}
	}
}

int decibel( float in )
{
	int out;
	float max;
	max = 0.99f;
	
	for( out = 20; in < max; max /= 1.25f )
	{
		out--;
		if( out < 1 )
			return 0;
	}
	
	return out;
}

void fade( short *pcm, bool in, bool out )
{
	int i;
	float v;
	float vinc;
	float f;

	v = 0.0f;
	vinc = 1.0f / 60.0f;
	for( i = 0; i < 60; i++ )
	{
		if( in )
		{
			f = (float)pcm[i] / 32768.0;
			f *= v;
			pcm[i] = (short)(f * 32766.0);
		}
		
		if( out )
		{
			f = (float)pcm[479 - i] / 32768.0;
			f *= v;
			pcm[479 - i] = (short)(f * 32766.0);
		}
		
		v += vinc;
	}
}


webrtc::AudioProcessing *apm;
int sock;
struct sockaddr_in addr;
bool connected;
int miclevel;
int micl2;
bool micmuted;
bool playmuted;


typedef struct {
	bool online;
	bool speedup;
	int offcount;
	short pcm[480];
	OpusDecoder *dec;
	int pos;
	int lvl;
	int l2;
} iclnt, *pclnt;
iclnt clients[30];

typedef struct {
	unsigned char a;
	unsigned char b;
	unsigned char opus[500];
} isnt, *psnt;

typedef struct {
	unsigned char id;
	unsigned char opus[500];
} idta, *pdta;



class NetSending
{
	private:
	char buffer[50][505];
	int buflen[50];
	int bufstat[50];
	int spos;
	int wpos;

	public:
	NetSending()
	{
		memset( bufstat, 0, 50 );
		spos = 0;
		wpos = 0;
	}
	
	void write( psnt snt )
	{
		int len;
		len = ((int)snt->a * 100) + (int)snt->b + 2;
		
		if( bufstat[wpos] == 0 )
		{
			memcpy( buffer[wpos], (char*)snt, len );
			buflen[wpos] = len;
			bufstat[wpos] = 1;
			wpos = (wpos + 1) % 50;
		}
	}
	
	int transfer()
	{
		int rc;
		while( bufstat[spos] == 1 )
		{
			rc = send( sock, buffer[spos], buflen[spos], 0 );
			if( rc != buflen[spos] )
				return -1;
			bufstat[spos] = 0;
			spos = (spos + 1) % 50;
		}

		return 0;
	}
};
NetSending *sender;


class AudioTransportImpl: public webrtc::AudioTransport
{
	private:
	short ultrabuffer[50000];
	int ubrd;
	float nmul;
	float hi;
	isnt snt;
	OpusEncoder *enc;
	
	public:
	
	AudioTransportImpl()
	{
		int i;

		enc = opus_encoder_create( 48000, 1, OPUS_APPLICATION_VOIP, &i );
		opus_encoder_ctl( enc, OPUS_SET_BITRATE(56000) );
		opus_encoder_ctl( enc, OPUS_SET_COMPLEXITY(10) );
		opus_encoder_ctl( enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE) );

		for( i = 0; i < 50000; i++ )
			ultrabuffer[i] = 0;
		ubrd = 24000;
		nmul = 1.0;
		hi = 0.0;
	}
	
	~AudioTransportImpl()
	override {
	}
	
	int put( short *pcm, int pos )
	{
		int i;
		int x;
		int l;
		l = 0;
		for( i = 0; i < 480; i++ )
		{
			x = (int)ultrabuffer[i + pos];
			x += (int)pcm[i];
			if( x < -32766 ) x = -32766;
			if( x > 32766 ) x = 32766;
			ultrabuffer[i + pos] = (short)x;
			
			x = (int)pcm[i];
			if( x < 0 ) x = -x;
			if( x > l ) l = x;
		}
		
		return decibel( (float)l / 32768.0 );
	}
	
	int32_t RecordedDataIsAvailable(
						const void* audioSamples,
						const size_t nSamples,
						const size_t nBytesPerSample,
						const size_t nChannels,
						const uint32_t samplesPerSec,
						const uint32_t totalDelayMS,
						const int32_t clockDrift,
						const uint32_t currentMicLevel,
						const bool keyPressed,
						uint32_t& newMicLevel )
	override {
		short *s = (short*)audioSamples;
		
		float inpcm[960];
		float outpcm[960];
		float opcm[960];
		short endpcm[960];

		int vol;
		float f;
		float max;
		max = 0.25f;
		float mul;
		float inc;
		
		size_t i;
		for( i = 0; i < nSamples * nChannels; i += nChannels )
		{
			f = (float)s[i] / 32768.0;
			if( micmuted ) f = 0.0;
			hi += (f - hi) * 0.02;
			f -= hi;
			inpcm[i / nChannels] = f;
			if( f < 0.0 ) f = -f;
			if( f > max ) max = f;
		}

		mul = 0.9 / max;
		inc = (mul - nmul) / 480.0;
		
		rsmpl( inpcm, outpcm, (int)nSamples, 480 );

		const float* const buffers[] = {outpcm, 0};
		float* const output[] = {opcm, 0};
		
		apm->set_stream_delay_ms( totalDelayMS );
		//apm->echo_cancellation()->set_stream_drift_samples( clockDrift );
		apm->ProcessStream( buffers, 480, 48000, webrtc::AudioProcessing::kMono, 48000, webrtc::AudioProcessing::kMono, output );
		
		
		float lvl;
		lvl = 0.0;
		
		for( i = 0; i < 480; i++ )
		{
			nmul += inc;
			f = opcm[i] * nmul;
			
			if( f < -1.0 ) f = -1.0;
			if( f > 1.0 ) f = 1.0;
			
			endpcm[i] = (short)(f * 32766.0);
			
			if( f < 0.0 ) f = -f;
			if( f > lvl ) lvl = f;
		}
		
		vol = decibel( lvl );
		if( vol > miclevel )
			miclevel = vol;
		if( vol > micl2 )
			micl2 = vol;
		
		int rc;
		rc = opus_encode( enc, endpcm, 480, snt.opus, 500 );
		snt.b = (unsigned char)(rc % 100);
		snt.a = (unsigned char)((rc - (int)snt.b) / 100);
		sender->write( &snt );
		
		return 0;
	}


	int32_t NeedMorePlayData(const size_t nSamples,
					   const size_t nBytesPerSample,
					   const size_t nChannels,
					   const uint32_t samplesPerSec,
					   void* audioSamples,
					   size_t& nSamplesOut,
					   int64_t* elapsed_time_ms,
					   int64_t* ntp_time_ms)
	override {
		float f;
		float inpcm[960];
		float outpcm[960];
		short *endpcm = (short*)audioSamples;

		size_t i;
		
		for( i = 0; i < 480; i++ )
		{
			f = (float)ultrabuffer[i] / 32768.0;
			if( playmuted ) f = 0.0;
			inpcm[i] = f;
		}

		for( i = 0; i < 48000; i++ )
			ultrabuffer[i] = ultrabuffer[i+480];
		
		for( i = 0; i < 30; i++ )
		{
			if( clients[i].online )
			{
				if( clients[i].pos >= 480 )
					clients[i].pos -= 480;
			}
		}
		
		const float* const buffers[] = {inpcm, 0};
		apm->AnalyzeReverseStream( buffers, 480, 48000, webrtc::AudioProcessing::kMono );

		rsmpl( inpcm, outpcm, 480, (int)nSamples );
		
		if( nChannels == 2 )
		{
			for( i = 0; i < nSamples; i++ )
			{
				endpcm[i+i] = (short)(outpcm[i] * 32766.0);
				endpcm[i+i+1] = endpcm[i+i];
			}
		}
		if( nChannels == 1 )
		{
			for( i = 0; i < nSamples; i++ )
				endpcm[i] = (short)(outpcm[i] * 32766.0);
		}
		
		nSamplesOut = nSamples;
		return 0;
	}

	void PushCaptureData(int voe_channel,
					   const void* audio_data,
					   int bits_per_sample,
					   int sample_rate,
					   size_t number_of_channels,
					   size_t number_of_frames)
	override {
		puts( "PushCaptureData" );
		sleep( 1 );
	}

	void PullRenderData(int bits_per_sample,
					  int sample_rate,
					  size_t number_of_channels,
					  size_t number_of_frames,
					  void* audio_data,
					  int64_t* elapsed_time_ms,
					  int64_t* ntp_time_ms)
	override {
		puts( "PullRenderData" );
		sleep( 1 );
	}

};


//WSADATA wsa;

typedef struct {
	unsigned char a;
	unsigned char b;
} ilen, *plen;


int coco_recv( pdta dta, int len )
{
	char data[505];
	int i;
	int rc;
	int pos;

	struct timeval tv;

	fd_set master, readfd;
	FD_ZERO( &master );
	FD_ZERO( &readfd );
	FD_SET( sock, &master );

	pos = 0;
	for( i = 0; i < 8; i++ )
	{
		readfd = master;
		tv.tv_sec = 0;
		tv.tv_usec = 16000;
		select( sock+1, &readfd, 0, 0, &tv );

		if( FD_ISSET(sock, &readfd) )
		{
			rc = recv( sock, &data[pos], len-pos, 0 );
			if( rc <= 0 )
				return -1;
			pos += rc;
			if( pos == len )
				break;
		}
	}
	
	if( pos != len )
		return -1;
	
	memcpy( (char*)dta, data, len );
	return 0;
}


int main()
{
	int rc;
	int x;
	int i;
	//char c;
	
	//system( "mode con cols=70 lines=25" );

	//WSAStartup( MAKEWORD(2,2), &wsa );
	sender = new NetSending();

	for( x = 0; x < 30; x++ )
	{
		clients[x].online = false;
		clients[x].speedup = false;
		clients[x].offcount = 16;
		clients[x].dec = opus_decoder_create( 48000, 1, &rc );
		clients[x].pos = 1920;
	}
	miclevel = 0;
	micl2 = 0;

	micmuted = false;
	playmuted = false;
	
	sock = socket( AF_INET, SOCK_STREAM, 0 );
	memset( &addr, 0, sizeof(struct sockaddr_in) );
	addr.sin_family = AF_INET;
	addr.sin_port = htons( 4999 );
	addr.sin_addr.s_addr = inet_addr( "188.68.38.124" );
	
	if( connect( sock, (struct sockaddr*)&addr, sizeof(addr) ) < 0 )
		connected = false;
	else
		connected = true;

	apm = webrtc::AudioProcessing::Create();
	apm->noise_suppression()->set_level( webrtc::NoiseSuppression::kHigh );
	apm->noise_suppression()->Enable( true );
	apm->echo_cancellation()->set_suppression_level( webrtc::EchoCancellation::kHighSuppression );
	//apm->echo_cancellation()->enable_drift_compensation( true );
	apm->echo_cancellation()->Enable( true );
	apm->Initialize( 48000, 48000, 48000, webrtc::AudioProcessing::kMono, webrtc::AudioProcessing::kMono, webrtc::AudioProcessing::kMono );

	webrtc::AudioDeviceLinuxPulse *audio;
	audio = new webrtc::AudioDeviceLinuxPulse( 1 );
	
	WebRtcSpl_Init();
	
	webrtc::AudioDeviceBuffer *buffer;
	buffer = new webrtc::AudioDeviceBuffer();
	
	buffer->SetId( 1 );

	audio->Init();
	audio->AttachAudioBuffer( buffer );

	audio->SetPlayoutDevice( 0 );
	audio->InitPlayout();
	
	audio->SetRecordingDevice( 0 );
	audio->InitRecording();

	AudioTransportImpl *callback;
	callback = new AudioTransportImpl();
	buffer->RegisterAudioCallback( callback );

	audio->StartRecording();
	audio->StartPlayout();
	
	struct timeval tv;
	
	fd_set master, readfd;
	FD_ZERO( &master );
	FD_ZERO( &readfd );
	FD_SET( sock, &master );

	ilen len;
	int lab;
	lab = 0;
	idta dta;
	int ID;

	//HANDLE hCon;
	//hCon = GetStdHandle(STD_OUTPUT_HANDLE);
	//COORD crd;
	//crd.X = 0;
	//crd.Y = 0;
	
	char levelz[2000];
	int start;
	int end;
	int diff;
	
	int done[32];
	
	start = GetTickCount();
	fputs( "\033[2J", stdout );
	
	while( true )
	{
		if( !connected )
		{
			sleep( 1 );
			sock = socket( AF_INET, SOCK_STREAM, 0 );
			if( connect( sock, (struct sockaddr*)&addr, sizeof(addr) ) < 0 )
			{
				connected = false;
				//MessageBoxA( 0, "cannot connect", "Error", MB_OK );
			}
			else
			{
				connected = true;
				FD_ZERO( &master );
				FD_ZERO( &readfd );
				FD_SET( sock, &master );
			}
		}
		readfd = master;
		tv.tv_sec = 0;
		tv.tv_usec = 16000;
		select( sock+1, &readfd, 0, 0, &tv );
		
		for( i = 0; i < 30; i++ )
		{
			if( clients[i].online )
			{
				if( clients[i].pos <= 960 )
				{
					callback->put( clients[i].pcm, clients[i].pos );
					clients[i].pos += 480;
					clients[i].offcount--;
					if( clients[i].offcount <= 0 )
						clients[i].online = false;
				}
			}
		}
		
		if( FD_ISSET(sock, &readfd) )
		{
			if( lab == 0 )
			{
				rc = recv( sock, (char*)&len.a, 1, 0 );
				if( rc != 1 )
				{
					close( sock );
					connected = false;
					//MessageBoxA( 0, "cannot recv 1", "Error", MB_OK );
				}
				else
					lab++;
				continue;
			}
			else
			{
				rc = recv( sock, (char*)&len.b, 1, 0 );
				if( rc != 1 )
				{
					close( sock );
					connected = false;
					//MessageBoxA( 0, "cannot recv 2", "Error", MB_OK );
				}
				else
					lab++;
			}
			if( lab == 2 )
			{
				lab = 0;
				x = ((int)len.a * 100) + (int)len.b;
				if( x < 3 || x > 490 )
				{
					close( sock );
					connected = false;
					//MessageBoxA( 0, "Wrong length", "Error", MB_OK );
					continue;
				}
				if( coco_recv( &dta, x ) == 0 )
				{
					ID = (int)dta.id;
					if( ID < 0 || ID >= 30 )
					{
						close( sock );
						connected = false;
						//MessageBoxA( 0, "Wrong ID", "Error", MB_OK );
						continue;
					}
					clients[ID].online = true;
					rc = opus_decode( clients[ID].dec, dta.opus, x-1, clients[ID].pcm, 480, 0 );
					if( clients[ID].pos < 47040 )
						clients[ID].pos += 480;
					
					if( clients[ID].speedup )
					{
						if( clients[ID].pos <= 3840 )
						{
							clients[ID].speedup = false;
							fade( clients[ID].pcm, true, false );
						}
						else
							fade( clients[ID].pcm, true, true );
						clients[ID].pos -= 60;
					}

					if( clients[ID].pos >= 14400 && !clients[ID].speedup )
					{
						clients[ID].speedup = true;
						fade( clients[ID].pcm, false, true );
					}

					i = callback->put( clients[ID].pcm, clients[ID].pos );
					if( i > clients[ID].lvl )
						clients[ID].lvl = i;
					if( i > clients[ID].l2 )
						clients[ID].l2 = i;
				}
				else
				{
					close( sock );
					connected = false;
				}
			}
			else
			{
				close( sock );
				connected = false;
			}
		}
		
		end = GetTickCount();
		diff = end - start;
		
		if( diff > 60 )
		{
			start = end;

			for( x = 0; x < 32; x++ )
				done[x] = 0;
			
			//SetConsoleCursorPosition( hCon, crd );
			fputs( "\033[0;0H", stdout );
			for( i = 0; i < 20; i++ )
			{
				rc = 0;
				memset( levelz, 0, 2000 );
				
				if( i == 0 )
					fputs( "\033[31;1m", stdout );
				if( i == 3 )
					fputs( "\033[33;1m", stdout );
				if( i == 10 )
					fputs( "\033[32;1m", stdout );

				for( x = 0; x < 32; x++ )
				{
					if( x == 30 )
					{
						levelz[rc] = ' ';
						rc++;
						continue;
					}
					if( x == 31 )
					{
						if( miclevel < 20-i )
						{
							if( micl2 < 20-i )
								levelz[rc] = ' ';
							else
							{
								if( done[x] == 0 )
								{
									levelz[rc] = '=';
									done[x] = 1;
								}
								else
									levelz[rc] = ' ';
							}
						}
						else
							levelz[rc] = '#';
						
						rc++;
						continue;
					}
					if( clients[x].online )
					{
						if( clients[x].lvl < 20-i )
						{
							if( clients[x].l2 < 20-i )
								levelz[rc] = ' ';
							else
							{
								if( done[x] == 0 )
								{
									levelz[rc] = '=';
									done[x] = 1;
								}
								else
									levelz[rc] = ' ';
							}
						}
						else
							levelz[rc] = '#';
					}
					else
						levelz[rc] = ' ';
					
					rc++;
					levelz[rc] = ' ';
					rc++;
				}
				puts( levelz );
			}
			
			rc = 0;
			memset( levelz, 0, 2000 );
			
			for( x = 0; x < 32; x++ )
			{
				if( x == 30 )
				{
					levelz[rc] = ' ';
					rc++;
					continue;
				}
				if( x == 31 )
				{
					levelz[rc] = 'Y';
					rc++;
					continue;
				}
				
				if( clients[x].online )
					levelz[rc] = 'X';
				else
					levelz[rc] = '-';
				rc++;
				levelz[rc] = ' ';
				rc++;
			}
			
			fputs( "\033[36;1m", stdout );			
			//SetConsoleTextAttribute( hCon, FOREGROUND_GREEN|FOREGROUND_BLUE|FOREGROUND_INTENSITY );
			puts( levelz );
			//fflush( stdout );
			
			for( i = 0; i < 30; i++ )
			{
				clients[i].lvl /= 2;
				if( clients[i].l2 > 0 )
					clients[i].l2--;
			}
			
			miclevel /= 2;
			if( micl2 > 0 )
				micl2--;
		}

		if( connected )
		{
			if( sender->transfer() == -1 )
			{
				close( sock );
				connected = false;
				//MessageBoxA( 0, "unable to send", "Error", MB_OK );
			}
		}
	}
	
	audio->StopPlayout();
	audio->StopRecording();
	
	delete callback;

	return 0;
}
