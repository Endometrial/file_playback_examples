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
	long		frames;
	int16_t*	buffer;
} PcmRemainder;

// Contains all data required to decode an ogg/vorbis file
typedef struct {
	FILE* 		filepointer;
	ogg_stream_state stream_state;
	ogg_sync_state 	sync_state;
	ogg_packet 	packet;
	ogg_page 	page;
	vorbis_block 	block;
	vorbis_comment 	comments;
	vorbis_info 	info;
	vorbis_dsp_state state;
	PcmRemainder	remainder;
	int 		eos;
}	OggDecoder;

OggDecoder ogg_decoder_open(char* filepath);
void ogg_decoder_close(OggDecoder* decoder);
int ogg_decoder_get_pcm_i16(OggDecoder* decoder, int16_t** buffer, int frames);
int ogg_decoder_get_channels(OggDecoder* decoder);
int ogg_decoder_eos(OggDecoder* decoder);
int ogg_decoder_is_vorbis(char* filepath);
int ogg_decoder_get_rate(OggDecoder* decoder);
static int ogg_decoder_callback_i16(const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void* userData);
void audio_list_devices();

int main(int argc, char* argv[]) {
	OggDecoder decoder;
	PaError err;
	PaStreamParameters input, output;
	PaStreamFlags flags;
	PaStream* stream;
	char* filepath;
	int16_t* buffer;
	int in, out;

	// Initialize portaudio
	err = Pa_Initialize();
	if (err != paNoError) goto error;

	// Verify that we have all arguments from the user & give an error if otherwise
	if ((argc < 2) || (argc == 3)) {
		fprintf(stderr, "%s: please input all arguments!\n\t%s [char* filepath] [int input device(optional)] [int output device(optional)]\n",argv[0],argv[0]);
		audio_list_devices();
		exit(-1);
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
	fprintf(stderr, "Initializing audio device data types\n");
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
	
	// Check that the file is vorbis
	(ogg_decoder_is_vorbis(filepath)) ? 1 : fprintf(stderr, "File is not vorbis!\n");

	// Print out current devices
	fprintf(stderr, "Selected devices -> input [%i] output [%i]\n", input.device, output.device);

	// Open ogg decoder
	fprintf(stderr, "Opening ogg/vorbis decoder\n");
	decoder = ogg_decoder_open(filepath);

	// Play the audio file
	int rate = ogg_decoder_get_rate(&decoder);
	int channels = ogg_decoder_get_channels(&decoder);

	fprintf(stderr, "Opening stream\n");
	err = Pa_OpenStream(&stream, &input, &output, rate, 8192, paNoFlag, &ogg_decoder_callback_i16, &decoder);
	if (err != paNoError) goto error;

	fprintf(stderr, "Playing stream\n");
	err = Pa_StartStream(stream);
	if (err != paNoError) goto error;

	// Loop untill eos
	fprintf(stderr, "Looping untill EOS is reached\n");
	while (!ogg_decoder_eos(&decoder)) {}


	fprintf(stderr, "Stopping stream\n");
	err = Pa_StopStream(stream);
	if (err != paNoError) goto error;

	fprintf(stderr, "Closing stream\n");
	err = Pa_CloseStream(stream);
	if (err != paNoError) goto error;
		
	// Cleanup and return
	fprintf(stderr,"Closing ogg/vorbis decoder\n");
	Pa_Terminate();
	ogg_decoder_close(&decoder);
	fprintf(stderr,"Exiting program\n");
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

	// Read pcm data into the buffer :) (And all 0s if its over)
	if (ogg_decoder_get_pcm_i16(decoder, &out, framesPerBuffer)) {
		rewind(decoder->filepointer);
		for (int i=0; i<framesPerBuffer*ogg_decoder_get_channels(decoder); i++) {
			out[i] = 0;
		}
	}

	//fwrite(outputBuffer, sizeof(int16_t), framesPerBuffer, stdout);
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
	int page_eos = 0;

	// Initialize some variables
	channels = (int)decoder->info.channels;
	sample_data = (float)pow(2, bits) / 2.0f;

	// Reallocate the remainder as needed (aka everytime)
	decoder->remainder.buffer = realloc(decoder->remainder.buffer, MAX_OGG_SAMPLE_FRAMES * sizeof(int16_t) * channels);

	// Read in remainder (if it exists)
	if (decoder->remainder.frames > 0) {
		int16_t* bptr = (*buffer);						// Buffer
		int16_t* rptr = decoder->remainder.buffer;		// Remainder :3
		for (int i=0; i<(decoder->remainder.frames * channels); i++) {
			bptr[i] = rptr[i];
		}
		frames_read += decoder->remainder.frames;
	}

	// Loop until either there are no more samples or we have read all we need
	samples = !(0); // So it doesnt eval as false on first pass
	while ((frames_read < frames) && samples) {
		// Get next packet of pcm from executor vorbis unless another packet doesnt exist and the page is eos
		while (((samples=vorbis_synthesis_pcmout(&decoder->state, &raw_pcm)) == 0)) {
			if (page_eos == 1) {
				break; // If page is already eos then we know we are at eos
			}
			while((ogg_stream_packetout(&decoder->stream_state, &decoder->packet) == 0)) {
				if (ogg_sync_pageout(&decoder->sync_state, &decoder->page) == 1) {
					ogg_stream_pagein(&decoder->stream_state, &decoder->page);
				} else {
					load_buffer = ogg_sync_buffer(&decoder->sync_state, 4096l);
					bytes_read = fread(load_buffer, sizeof(char), 4096, decoder->filepointer);
					// (bytes_read < 4096) ? fprintf(stderr,"ogg_decoder_get_pcm_i16(): EOF!, EOS imminent\n") : 1;
					ogg_sync_wrote(&decoder->sync_state, bytes_read);
				}
				page_eos = (ogg_page_eos(&decoder->page)) ? 1 : 0; // Set page is eos based on if its eos
			}
			vorbis_synthesis(&decoder->block, &decoder->packet);
			vorbis_synthesis_blockin(&decoder->state, &decoder->block);
		}

		// Tell executor vorbis how many samples were read ^w^
		vorbis_synthesis_read(&decoder->state, samples);

		// Set available samples and remainder frames
		int available_samples = samples;
		decoder->remainder.frames = 0l;
		if ((frames_read + samples) >= frames) {
			available_samples = (frames-frames_read);
			decoder->remainder.frames = (long)(samples-available_samples);
		}

		// Read samples into the buffer
		for (int i=0; i<channels; i++) {
			int16_t* ptr = (*buffer) + i + frames_read*channels;
			float* mono = raw_pcm[i];
			for (int m=0; m<available_samples; m++) {
				int16_t val = floor(mono[m]*((float)sample_data/2.0f) + 0.5f);
      		    //val = (val > sample_data/2) ? sample_data/2 : val; // Guard against
      		    //val = (val < -sample_data/2) ? -sample_data/2 : val; // clipping
     		    *ptr=val;
     		    ptr+=channels;
			}
		}

		// Incriment frames_read by the ammount of samples
		frames_read+=available_samples;
	}

	// Read in the remainder
	if (decoder->remainder.frames > 0) {
		int offset = samples - decoder->remainder.frames;
		for (int i=0; i<channels; i++) {
			float* mono = raw_pcm[i];
			int16_t* rptr = decoder->remainder.buffer + i;
			for (int g=0; g<decoder->remainder.frames; g++) {
				int16_t val = floor(mono[g+offset]*((float)sample_data/2.0f) + 0.5f);
				//val = (val > sample_data/2) ? sample_data/2 : val; // Guard against
	   	   		//val = (val < -sample_data/2) ? -sample_data/2 : val; // clipping
	   	   		rptr[g<<1]=val;
			}
		}
	}

	// If data remains unread in finish out the packet and return eos
	decoder->eos = 0;
	if ((frames-frames_read) > 0) {
		for (int i=0; i<channels; i++) {
			int16_t* end_ptr = (*buffer) + (channels*frames_read);
			for (int m=0; m<(frames-frames_read); m++) {
				*end_ptr = 0;
				end_ptr+=channels;
			}
		}
		decoder->eos = 1;
		return 1; // EOS
	}
	return 0; // Not EOS
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

// Print available devices
void audio_list_devices() {
	int num_devices = Pa_GetDeviceCount();
	const PaDeviceInfo* device_info;
	for (int i=0; i<num_devices; i++) {
		device_info = Pa_GetDeviceInfo(i);
		fprintf(stderr, "Device [%i] %s\n",i, device_info->name);
		fprintf(stderr, "	in: %i out: %i\n",device_info->maxInputChannels, device_info->maxOutputChannels);
	}
}
