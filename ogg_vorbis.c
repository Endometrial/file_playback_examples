#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <portaudio.h>

// Largest packet size in frames that libvorbis can spit out
#define MAX_OGG_SAMPLE_FRAMES 1024

// Remainder struct for readability
typedef struct {
	long frames;
	int16_t* buffer;
} PcmRemainder;

// Contains all data required to decode an ogg/vorbis file
typedef struct {
	FILE* 				filepointer;
	ogg_stream_state 	stream_state;
	ogg_sync_state 		sync_state;
	ogg_packet 			packet;
	ogg_page 			page;
	vorbis_block 		block;
	vorbis_comment 		comments;
	vorbis_info 		info;
	vorbis_dsp_state 	state;
	PcmRemainder		remainder;
	int 				eos;
}	OggDecoder;

OggDecoder ogg_decoder_open(char* filepath);
void ogg_decoder_close(OggDecoder* decoder);
int ogg_decoder_get_pcm_i16(OggDecoder* decoder, int16_t** buffer, int frames);
int ogg_decoder_get_channels(OggDecoder* decoder);
int ogg_decoder_eos(OggDecoder* decoder);
int ogg_decoder_is_vorbis(char* filepath);
int ogg_decoder_get_rate(OggDecoder* decoder);
static int ogg_decoder_callback_i16(const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void* userData);

int main(int argc, char* argv[]) {
	OggDecoder decoder;
	PaError err;
	PaStreamParameters input, output;
	PaStreamFlags flags;
	PaStream* stream;
	char* filepath;
	int16_t* buffer;
	int in, out;

	// Verify that we have all arguments from the user
	if ((argc < 2) || (argc == 3)) {
		fprintf(stderr, "%s: please input all arguments!\n\t%s [char* filepath] [int input device(optional)] [int output device(optional)]\n",argv[0],argv[0]);
		exit(-1);
	}

	// Initialize portaudio
	err = Pa_Initialize();
	if (err != paNoError) goto error;

	// Print available devices
	int num_devices = Pa_GetDeviceCount();
	const PaDeviceInfo* device_info;
	for (int i=0; i<num_devices; i++) {
		device_info = Pa_GetDeviceInfo(i);
		fprintf(stderr, "Device [%i] %s\n",i, device_info->name);
		fprintf(stderr, "	in: %i out: %i\n",device_info->maxInputChannels, device_info->maxOutputChannels);
	}

	// Populate argument data types
	filepath = argv[1];
	in = Pa_GetDefaultInputDevice();
	out = Pa_GetDefaultOutputDevice();
	if (argc > 3) {
		in = *(argv[2]) - '0';
		out = *(argv[3]) - '0';
	}

	// Initialize devices
	input.device =  (int)in;
	const PaDeviceInfo* input_info = Pa_GetDeviceInfo(input.device);
	input.channelCount = input_info->maxInputChannels;
	input.sampleFormat = paFloat32;
	input.suggestedLatency = input_info->defaultHighInputLatency;
	input.hostApiSpecificStreamInfo = NULL;

	output.device =  (int)out;
	const PaDeviceInfo* output_info = Pa_GetDeviceInfo(output.device);
	output.channelCount = output_info->maxOutputChannels;
	output.sampleFormat = paInt16;
	output.suggestedLatency = output_info->defaultHighOutputLatency;
	output.hostApiSpecificStreamInfo = NULL;

	// Print out current devices
	fprintf(stderr, "Input [%i]\n", input.device);
	fprintf(stderr, "Output [%i]\n", output.device);
	
	// Check that the file is vorbis
	char vorb = (ogg_decoder_is_vorbis(filepath)) ? 'y' : 'n';
	fprintf(stderr, "Is vorbis? : [%c]\n", vorb);

	// Open ogg decoder
	decoder = ogg_decoder_open(filepath);

	// Play the audio file
	int rate = ogg_decoder_get_rate(&decoder);
	int channels = ogg_decoder_get_channels(&decoder);

	err = Pa_OpenStream(&stream, &input, &output, rate, 4096, paNoFlag, &ogg_decoder_callback_i16, &decoder);
	if (err != paNoError) goto error;

	err = Pa_StartStream(stream);
	if (err != paNoError) goto error;

	// Loop untill eos
	while (!ogg_decoder_eos(&decoder)) {}

	err = Pa_StopStream(stream);
	if (err != paNoError) goto error;

	err = Pa_CloseStream(stream);
	if (err != paNoError) goto error;
		
	// Cleanup and return
	fprintf(stderr,"EOS!\n");
	Pa_Terminate();
	ogg_decoder_close(&decoder);
	fprintf(stderr,"Done\n");
	return 0;

	// Label for errors with portaudio
	error:
		fprintf(stderr, "An error occured : %s\n",Pa_GetErrorText(err));
		
		exit(-1);
}

// Callback used by portaudio to play the file
static int ogg_decoder_callback_i16(const void *inputBuffer, void *outputBuffer,
                    unsigned long framesPerBuffer,
                    const PaStreamCallbackTimeInfo* timeInfo,
                    PaStreamCallbackFlags statusFlags,
                    void* userData ) {

	OggDecoder* decoder = (OggDecoder*)userData;
	int16_t* out = (int16_t*)outputBuffer;

	// Read pcm data into the buffer :)
	ogg_decoder_get_pcm_i16(decoder, &out, framesPerBuffer);
	return 0;
}

// Returns 1 if is vorbis
int ogg_decoder_is_vorbis(char* filepath) {
	OggDecoder decoder;
	char* audio_buffer;
	int bytes_read;

	// Open the file
	decoder.filepointer = fopen(filepath, "rb");
	if (!decoder.filepointer) {
		fprintf(stderr,"ogg_decoder_is_vorbis(): cannot open %s ;-;\n", filepath);
		fclose(decoder.filepointer); // Close file
		exit(-1);
	}

	// Read data into libogg & use it to create a page
	ogg_sync_init(&decoder.sync_state);
	audio_buffer = ogg_sync_buffer(&decoder.sync_state, 4096l);
	bytes_read = fread(audio_buffer, sizeof(char), 4096, decoder.filepointer);
	(bytes_read < 4096) ? fprintf(stderr, "ogg_decoder_is_vorbis(): %s too short (early EOF)\n", filepath) : 1;
	fclose(decoder.filepointer); // Close file
	ogg_sync_wrote(&decoder.sync_state, bytes_read);
	if (ogg_sync_pageout(&decoder.sync_state, &decoder.page) != 1) {
		fprintf(stderr, "ogg_decoder_is_vorbis(): %s is not an ogg file\n", filepath);
		
		// Cleanup remaining data
		ogg_sync_clear(&decoder.sync_state);
		return 0;
	}

	// Initialize data types
	ogg_stream_init(&decoder.stream_state, ogg_page_serialno(&decoder.page));
	vorbis_info_init(&decoder.info);
	vorbis_comment_init(&decoder.comments);

	// Submit packet to vorbis
	ogg_stream_pagein(&decoder.stream_state, &decoder.page);
	ogg_stream_packetout(&decoder.stream_state, &decoder.packet);

	// Check if is vorbis
	if (vorbis_synthesis_headerin(&decoder.info, &decoder.comments, &decoder.packet) < 0 ) {
		fprintf(stderr, "ogg_decoder_is_vorbis(): %s is not an ogg/vorbis file\n", filepath);
		
		// Cleanup remaining data
		ogg_stream_clear(&decoder.stream_state);
		ogg_sync_clear(&decoder.sync_state);
		vorbis_info_clear(&decoder.info);
		vorbis_comment_clear(&decoder.comments);
		return 0;
	}

	// Cleanup remaining data
	ogg_stream_clear(&decoder.stream_state);
	ogg_sync_clear(&decoder.sync_state);
	vorbis_info_clear(&decoder.info);
	vorbis_comment_clear(&decoder.comments);

	// File is vorbis :)
	return 1;
}

// Opens the file and prepares an OggDecoder struct
OggDecoder ogg_decoder_open(char* filepath) {
	OggDecoder decoder;

	char* audio_buffer;
	int bytes_read;

	// Load the file
	decoder.filepointer = fopen(filepath, "rb");
	if (!decoder.filepointer) {
		fprintf(stderr,"ogg_decoder_open(): cannot open %s ;-;\n", filepath);
		exit(-1);
	}

	// Initialize libogg & read in some bytes
	ogg_sync_init(&decoder.sync_state);
	audio_buffer = ogg_sync_buffer(&decoder.sync_state, 4096l);
	bytes_read = fread(audio_buffer, sizeof(char), 4096, decoder.filepointer);
	(bytes_read < 4096) ? fprintf(stderr, "ogg_decoder_open(): EOF!\n") : 1;

	// Submit data and get the first page
	ogg_sync_wrote(&decoder.sync_state, bytes_read);
	if (ogg_sync_pageout(&decoder.sync_state, &decoder.page) != 1) {
		fprintf(stderr, "ogg_decoder_open(): %s not ogg >:3\n", filepath);
		exit(-1);
	}

	// Initialize stream and data types
	ogg_stream_init(&decoder.stream_state, ogg_page_serialno(&decoder.page));
	vorbis_info_init(&decoder.info);
	vorbis_comment_init(&decoder.comments);

	// Read and interpret first header packet
	ogg_stream_pagein(&decoder.stream_state, &decoder.page);
	ogg_stream_packetout(&decoder.stream_state, &decoder.packet);
	if (vorbis_synthesis_headerin(&decoder.info, &decoder.comments, &decoder.packet) < 0) {
		fprintf(stderr, "ogg_decoder_open(): %s not vorbis! >:3\n", filepath);
		exit(-1);
	}

	// Get Comment and Codebook headers
	int headers = 0;
	while (headers<2) {
		// Get a page out :3
		if (ogg_sync_pageout(&decoder.sync_state, &decoder.page) == 1) {
			ogg_stream_pagein(&decoder.stream_state, &decoder.page);
			ogg_stream_packetout(&decoder.stream_state, &decoder.packet);
			if (vorbis_synthesis_headerin(&decoder.info, &decoder.comments, &decoder.packet)) {
				fprintf(stderr, "ogg_decoder_open(): Error in header #%i ",headers);
			}
			headers++;
		}
		// Read data in :3
		audio_buffer = ogg_sync_buffer(&decoder.sync_state, 4096l);
		bytes_read = fread(audio_buffer, sizeof(char), 4096, decoder.filepointer);	
		(bytes_read < 4096) ? fprintf(stderr, "ogg_decoder_open(): EOF!") : 1;
		ogg_sync_wrote(&decoder.sync_state, bytes_read);
	}

	// Initialize vorbis data types
	vorbis_synthesis_init(&decoder.state, &decoder.info);
	vorbis_block_init(&decoder.state, &decoder.block);

	// Clear remainder & allocate some data for the remainder buffer
	decoder.remainder.buffer = malloc(0);
	decoder.remainder.frames = 0;

	// Set eos
	decoder.eos = 0;

	return decoder;
}

// Loads int16_ts into buffer of sizeof(int16_t) * number of frames * channels and returns value of internal eos flag
int ogg_decoder_get_pcm_i16(OggDecoder* decoder, int16_t** buffer, int frames) {
	int channels, samples, bytes_read, len, sample_data, frames_read;
	char* load_buffer = (char*)0;
	float** raw_pcm;

	// Error if not enough data is provided
	// yes i know i could make it work anyway but like.... why....
	if (frames < MAX_OGG_SAMPLE_FRAMES) {
		fprintf(stderr, "ogg_decoder_get_pcm_i16(): frames must be greater than %i\n", MAX_OGG_SAMPLE_FRAMES);
	}

	// Set the bits and other small variables
	short bits = sizeof(int16_t) * 8;
	frames_read = 0;
	int eos = 0;

	// Initialize some variables
	channels = (int)decoder->info.channels;
	sample_data = (float)pow(2, bits) / 2.0f;

	// Read in remainder (if it exists)
	if (decoder->remainder.frames > 0) {
		int16_t* bptr = (*buffer);						// Buffer
		int16_t* rptr = decoder->remainder.buffer;		// Remainder :3
		for (int i=0; i<(decoder->remainder.frames * channels); i++) {
			bptr[i] = rptr[i];
		}
		frames_read += decoder->remainder.frames;
	}

	while (!eos) {
		// Get next packet of pcm from executor vorbis
		while (((samples=vorbis_synthesis_pcmout(&decoder->state, &raw_pcm)) == 0)) {
			while(ogg_stream_packetout(&decoder->stream_state, &decoder->packet) != 1) {
				if (ogg_sync_pageout(&decoder->sync_state, &decoder->page) == 1) {
					ogg_stream_pagein(&decoder->stream_state, &decoder->page);
					if (ogg_page_eos(&decoder->page)) {
					eos = 1;
					}
				} else {
					load_buffer = ogg_sync_buffer(&decoder->sync_state, 4096l);
					bytes_read = fread(load_buffer, sizeof(char), 4096, decoder->filepointer);
					// (bytes_read < 4096) ? fprintf(stderr,"ogg_decoder_get_pcm_i16(): EOF!, EOS imminent\n") : 1;
					ogg_sync_wrote(&decoder->sync_state, bytes_read);
				}
			}
			vorbis_synthesis(&decoder->block, &decoder->packet);
			vorbis_synthesis_blockin(&decoder->state, &decoder->block);
		}

		// Tell executor vorbis how many samples were read ^w^
		vorbis_synthesis_read(&decoder->state, samples);

		// If more frames have been gathered than are requested break the loop
		if ((frames_read + samples) >= frames) {
			// Set ammount of remainder frames
			decoder->remainder.frames = ((frames_read + samples)-frames);
			break;
		}

		// Read samples into the buffer
		for (int i=0; i<channels; i++) {
			int16_t* ptr = (*buffer) + i + frames_read*channels;
			float* mono = raw_pcm[i];
			for (int m=0; m<samples; m++) {
				int16_t val = floor(mono[m]*((float)sample_data/2.0f) + 0.5f);
      		    //val = (val > sample_data/2) ? sample_data/2 : val; // Guard against
      		    //val = (val < -sample_data/2) ? -sample_data/2 : val; // clipping
     		    *ptr=val;
     		    ptr+=channels;
   
			}
		}

		// Incriment frames_read by the ammount of samples
		frames_read+=samples;
	}

	// Read all remaining data into its relevent location
	if (decoder->remainder.frames > 0) {
		int m;
		for (int i=0; i<channels; i++) {
			m = 0;
			float* mono = raw_pcm[i];
			int16_t* bptr = (*buffer) + i + frames_read*channels;
			for (int l=0; l<frames-frames_read; l++) {
				int16_t val = floor(mono[m]*((float)sample_data/2.0f) + 0.5f);
				//val = (val > sample_data/2) ? sample_data/2 : val; // Guard against
	   	   		//val = (val < -sample_data/2) ? -sample_data/2 : val; // clipping
				bptr[l<<1] = val; // l * 2
				m++;
			}
			int16_t* rptr = decoder->remainder.buffer + i;
			for (int l=0; l<decoder->remainder.frames; l++) {
				int16_t val = floor(mono[m]*((float)sample_data/2.0f) + 0.5f);
				//val = (val > sample_data/2) ? sample_data/2 : val; // Guard against
   		   		//val = (val < -sample_data/2) ? -sample_data/2 : val; // clipping
				rptr[l<<1] = val; // l * 2
				m++;
			}
		}
	}

	// At eos read all null data into the rest of the buffer
	if (eos) {
		decoder->eos = eos;
		while((frames-frames_read) > 0) {
			for (int i=0; i<channels; i++) {
				(*buffer)[(channels * frames_read) + i] = 0;
			}
			frames_read++;
		}
	}

	return eos;
}

// Clean up and close OggDecoder struct
void ogg_decoder_close(OggDecoder* decoder) {
	vorbis_block_clear(&decoder->block);
	vorbis_dsp_clear(&decoder->state);
	ogg_stream_clear(&decoder->stream_state);
	vorbis_comment_clear(&decoder->comments);
	vorbis_info_clear(&decoder->info);
	ogg_sync_clear(&decoder->sync_state);
	fclose(decoder->filepointer);
	free(decoder->remainder.buffer);
}

// Is song at eos?
int ogg_decoder_eos(OggDecoder* decoder) {
	return decoder->eos;
}

// Get files channels
int ogg_decoder_get_channels(OggDecoder* decoder) {
	return decoder->info.channels;
}

// Get rate of file
int ogg_decoder_get_rate(OggDecoder* decoder) {
	return decoder->info.rate;
}