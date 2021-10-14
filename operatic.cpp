#include <stdio.h>
#include <signal.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#include "dbopl.h"

using namespace DBOPL;

static const uint8_t kChannels = 1;
static const Bitu kRate = 48000;
static const Bitu kBufferSize = 256;
static const Bit32u kPort = 0x220;
static const int16_t kGain = (1 << 15) / (1 << 12);

bool continueMainLoop = true;

void sig_handler(int sig_num)
{
	if(sig_num == SIGINT) {
		printf("\n Caught the SIGINT signal\n");
	}
	else {
		printf("\n Caught the signal number [%d]\n", sig_num);
	}
	continueMainLoop = false;
}

int main()
{
	// First, setup the PulseAudio connection
	pa_simple *s;
	pa_sample_spec ss;
	int pacode;
	int16_t pcm[kBufferSize];
	
	ss.format = PA_SAMPLE_S16NE;
	ss.channels = kChannels;
	ss.rate = kRate;
	
	s = pa_simple_new(NULL,               // Use the default server.
					"operatic",           // Our application's name.
					PA_STREAM_PLAYBACK,
					NULL,               // Use the default device.
					"OPL3 Synthesizer", // Description of our stream.
					&ss,                // Our sample format.
					NULL,               // Use default channel map
					NULL,               // Use default buffering attributes.
					&pacode             // Ignore error code.
					);
	if (s == nullptr) {
		fprintf(stderr, "Could not open PulseAudio connection: %s\n", pa_strerror(pacode));
		return -1;
	}

	// Initialize synthesizer
	Bit32s buffer[kBufferSize];
	Handler handler;
	handler.Init(kRate);

	// Setup patch
	handler.WriteReg(handler.WriteAddr(0, 0x01), 0); // Disable waveform mask, clear test register
	handler.WriteReg(handler.WriteAddr(0, 0x20), 0x68); // Set operator 0 Tremolo/Vibrato/Sustain/KSR/FreqScale
	handler.WriteReg(handler.WriteAddr(0, 0x23), 0x22); // Set operator 3 Tremolo/Vibrato/Sustain/KSR/FreqScale
	handler.WriteReg(handler.WriteAddr(0, 0x40), 0x1f);  // Set operator 0 output volume
	handler.WriteReg(handler.WriteAddr(0, 0x43), 0x0);  // Set operator 3 output volume
	handler.WriteReg(handler.WriteAddr(0, 0x60), 0xe4); // Set operator 0 AD envelope
	handler.WriteReg(handler.WriteAddr(0, 0x63), 0xe4); // Set operator 3 AD envelope
	handler.WriteReg(handler.WriteAddr(0, 0x80), 0x9d); // Set operator 0 SR envelope
	handler.WriteReg(handler.WriteAddr(0, 0x83), 0x4d); // Set operator 3 SR envelope


	handler.WriteReg(handler.WriteAddr(0, 0xA0), 0x81); // Set channel 0 F-number
	handler.WriteReg(handler.WriteAddr(0, 0xB0), 0x2a); // Set channel 0 KEY ON
	handler.WriteReg(handler.WriteAddr(0, 0xC0), 0x06); // Set channel 0 FEEDBACK

	// Register signal handler before entering main loop
	signal(SIGINT, sig_handler);

	// Render!
	printf("Rendering...\n");
	while (continueMainLoop) {
		handler.Generate(buffer, kBufferSize);
		// Convert to signed 16-bit PCM
		for (int i=0; i < (int)kBufferSize; i++)
			pcm[i] = (int16_t)buffer[i] * kGain;
		int res = pa_simple_write(s, pcm, sizeof(pcm), &pacode);
		if (res < 0) {
			fprintf(stderr, "Failed to write to PulseAudio: %s\n", pa_strerror(pacode));
			continueMainLoop = false;
		}
	}
	printf("Rendering complete.\n");

	// Clean up
	pa_simple_free(s);

	return 0;
}
