#include <stdio.h>
#include <mutex>
#include <SDL2/SDL.h>
#include <SDL2/SDL_video.h>
#include <SDL2/SDL_events.h>
#include <SDL2/SDL_keyboard.h>
#include <SDL2/SDL_ttf.h>

#include "dbopl.h"

using namespace DBOPL;

static const uint8_t kChannels = 1;
static const Bitu kRate = 48000;
static const Bitu kBufferSize = 256;
static const Bit32u kPort = 0x220;
static const int16_t kGain = (1 << 15) / (1 << 12);
static const uint16_t kFNumberMask = (1 << 10) - 1;

enum operator_param
{
	OP_TREM,
	OP_VIB,
	OP_SUSTAIN,
	OP_KSR,
	OP_FMULTI,
	OP_KSL,
	OP_OLVL,
	OP_A,
	OP_D,
	OP_S,
	OP_R,
	OP_COUNT
};

enum channel_param
{
	CH_FNUMBER = 0,
	CH_FEEDBACK,
	CH_OCTAVE,
	CH_KEYON,
	CH_COUNT
};

uint8_t operator_param_mask[OP_COUNT] = {
	0x01,
	0x01,
	0x01,
	0x01,
	0x0F, // multi: 4 bits
	0x03, // ksl: 3 bits
	0x3f, // output level: 6 bits
	0x0F,
	0x0F,
	0x0F,
	0x0F
};

uint16_t channel_param_mask[CH_COUNT] = {
	0x03ff, // f-number: 10 bits
	0x0007, // feedback: 3 bits
	0x0007, // octave: 3 bits
	0x0001, // key-on: 1 bit
};

uint8_t operator_param_shortcut[OP_COUNT] = {
	20, // Q
	26, // W
	8,  // E
	21, // R
	23, // T
	28, // Y
	24, // U
	4,  // A
	22, // S
	7,  // D
	9   // F
};

uint8_t channel_param_shortcut[CH_COUNT] = {
	29, // Z
	27, // X
	6,  // C
	0   // No shortcut for key-on (handled separately)
};

const char * operator_param_str[OP_COUNT] = {
	"(Q) Tremolo",
	"(W) Vibrato",
	"(E) Sustain Mode",
	"(R) Key-scale Ratio",
	"(T) Frequency Multiplier",
	"(Y) Key-scale Level",
	"(U) Output Level",
	"(A) Attack",
	"(S) Decay",
	"(D) Sustain",
	"(F) Release"
};

const char * channel_param_str[CH_COUNT] = {
	"(Z) F-number",
	"(X) Feedback",
	"(C) Octave",
	"(Spacebar) Key-On/Off"
};

const uint8_t channel_operator_map[18] = {
	0, 1, 2, 
	6, 7, 8, 
	12, 13, 14, 
	18, 19, 20, 
	24, 25, 26, 
	30, 31, 32,
};

const uint8_t operator_register_offset[18] = {
	0x00,
	0x01,
	0x02,
	0x03,
	0x04,
	0x05,
	0x08,
	0x09,
	0x0a,
	0x0b,
	0x0c,
	0x0d,
	0x10,
	0x11,
	0x12,
	0x13,
	0x14,
	0x15
};

struct operator_state_t
{
	uint8_t params[OP_COUNT];
};

struct channel_state_t
{
	uint16_t params[CH_COUNT];
};

struct app_renderer_t
{
	SDL_Window * window;
	SDL_Renderer * renderer;
	SDL_Surface * surface;
	SDL_Rect dim;
	TTF_Font * font;
	int x;
	int y;
	int lineheight;
	int lineskip;
};

struct app_state_t
{
	channel_state_t channels[18];
	operator_state_t operators[36];
	uint8_t channel_dirty[18];
	uint8_t operator_dirty[36];
	uint8_t current_channel;
	uint8_t current_operator;
	uint8_t current_param_type;
	uint8_t current_param;
	Handler synth;
	Bit32s buffer[kBufferSize];
	app_renderer_t render_state;
	bool bContinue;
} app_state;

std::mutex synth_lock;

uint8_t get_operator(app_state_t &app_state)
{
	size_t op_index = channel_operator_map[app_state.current_channel];
	op_index += (app_state.current_operator * 3);
	return op_index;
}

int is_operator_shortcut(int sc)
{
	for (int i=0; i < OP_COUNT; i++) {
		if (operator_param_shortcut[i] == sc)
			return i;
	}
	return -1;
}

int is_channel_shortcut(int sc)
{
	for (int i=0; i < CH_COUNT; i++) {
		if (channel_param_shortcut[i] == sc)
			return i;
	}
	return -1;
}

void select_channel_param(app_state_t &app_state, int param)
{
	app_state.current_param_type = 0;
	app_state.current_param = param;
}

void select_operator_param(app_state_t &app_state, int param)
{
	app_state.current_param_type = 1;
	app_state.current_param = param;
}

void set_channel_param(app_state_t & app_state, int param, uint16_t val)
{
	uint8_t dirty = app_state.channels[app_state.current_channel].params[param] == val ? 0 : 1;
	app_state.channels[app_state.current_channel].params[param] = val;
	app_state.channel_dirty[app_state.current_channel] |= dirty;
}

void set_operator_param(app_state_t &app_state, int param, uint8_t val)
{
	int op_index = get_operator(app_state);
	uint8_t dirty = app_state.operators[op_index].params[param] == val ? 0 : 1;
	app_state.operators[op_index].params[param] = val;
	app_state.operator_dirty[op_index] |= dirty;
}

void step_channel_param(app_state_t &app_state, int param, int step)
{
	uint16_t * val = app_state.channels[app_state.current_channel].params + param;
	*val = (*val + step) & channel_param_mask[param];
	app_state.channel_dirty[app_state.current_channel] = 1;
}

void step_operator_param(app_state_t &app_state, int param, int step)
{
	uint8_t op_index = get_operator(app_state);
	uint8_t * val = app_state.operators[op_index].params + param;
	*val = (*val + step) & operator_param_mask[param];
	app_state.operator_dirty[op_index] = 1;
}

void step_param(app_state_t &app_state, int step)
{
	if (app_state.current_param_type == 0) {
		step_channel_param(app_state, app_state.current_param, step);
	} else {
		step_operator_param(app_state, app_state.current_param, step);
	}
}

void audio_render_cb(void* userdata, Uint8* stream, int)
{
	app_state_t * state = (app_state_t*)userdata;
	synth_lock.lock();
	state->synth.Generate(state->buffer, kBufferSize);
	synth_lock.unlock();
	uint16_t * pcm = (uint16_t*)stream;
	for (int i=0; i < (int)kBufferSize; i++)
		pcm[i] = (int16_t)state->buffer[i] * kGain;
}

void write_register(app_state_t &state, Bit32u addr, Bit32u reg, Bit8u val)
{
	printf("WRITE %d-0x%02x: 0x%02x\n", addr, reg, val);
	state.synth.WriteReg(state.synth.WriteAddr(addr, reg), val);
}

void handle_events(app_state_t &app_state)
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		int sc = event.key.keysym.scancode;
		int param;
		switch (event.type) {
			case SDL_KEYDOWN:
				printf("SDL KEYDOWN: %d\n", sc);
				if (sc >= 58 && sc < 70) {
					// F1-F12; select a channel
					app_state.current_channel = sc - 58;
					printf("Selected channel: %d\n", app_state.current_channel);
				}
				else if (sc >= 30 && sc < 34) {
					// 1-4; select channel operator
					app_state.current_operator = sc - 30;
					printf("Selected operator: %d\n", app_state.current_operator);
				}
				else if ((param = is_channel_shortcut(sc)) >= 0) {
					select_channel_param(app_state, param);
				}
				else if ((param = is_operator_shortcut(sc)) >= 0) {
					select_operator_param(app_state, param);
				}
				else
				{
					switch (sc) {
						case 81: // Arrow-Down
							step_param(app_state, -1);
							break;
						case 82: // Arrow-Up
							step_param(app_state, 1);
							break;
						case 44: // Spacebar
							set_channel_param(app_state, CH_KEYON, 1);
							break;
					}
				}
				break;
			case SDL_KEYUP:
				printf("SDL KEYUP: %d\n", event.key.keysym.scancode);
				switch (sc) {
					case 44:
						set_channel_param(app_state, CH_KEYON, 0);
						break;
				}
				break;
			case SDL_QUIT:
				app_state.bContinue = false;
		}
	}
}

void update_synth(app_state_t &app_state)
{
	synth_lock.lock();
	for (int i=0; i < 18; i++) {
		if (app_state.channel_dirty[i]) {
			uint32_t addr = i <= 8 ? 0 : 1;
			uint32_t reg_offset = i - (addr * 9);
			channel_state_t * chan = app_state.channels + i;
			uint16_t fnumber = chan->params[CH_FNUMBER];
			uint8_t fnlo = fnumber & 0xff;
			write_register(app_state, addr, 0xa0 | reg_offset, fnlo);
			uint8_t keyon = chan->params[CH_KEYON] << 5;
			uint8_t block = chan->params[CH_OCTAVE] << 2;
			uint8_t fnhi = fnumber >> 8;
			write_register(app_state, addr, 0xb0 | reg_offset, keyon | block | fnhi);
			uint8_t fb = chan->params[CH_FEEDBACK] << 1;
			write_register(app_state, addr, 0xc0 | reg_offset, fb);
			app_state.channel_dirty[i] = 0;
		}
	}
	for (int i=0; i < 36; i++) {
		if (app_state.operator_dirty[i]) {
			operator_state_t * op = app_state.operators + i;
			uint8_t addr = i < 18 ? 0 : 1;
			uint8_t reg_offset = operator_register_offset[i % 18];
			uint8_t trem = op->params[OP_TREM] << 7;
			uint8_t vib = op->params[OP_VIB] << 6;
			uint8_t sus = op->params[OP_SUSTAIN] << 5;
			uint8_t ksr = op->params[OP_KSR] << 4;
			uint8_t mul = op->params[OP_FMULTI];
			write_register(app_state, addr, 0x20 + reg_offset, trem | vib | sus | ksr | mul);
			uint8_t ksl = op->params[OP_KSL] << 6;
			uint8_t olvl = op->params[OP_OLVL];
			write_register(app_state, addr, 0x40 + reg_offset, ksl | olvl);
			uint8_t a = op->params[OP_A] << 4;
			uint8_t d = op->params[OP_D];
			write_register(app_state, addr, 0x60 + reg_offset, a | d);
			uint8_t s = op->params[OP_S] << 4;
			uint8_t r = op->params[OP_R];
			write_register(app_state, addr, 0x80 + reg_offset, s | r);
			app_state.operator_dirty[i] = 0;
		}
	}
	synth_lock.unlock();
}

void setup_patch(app_state_t &app)
{
	app.current_channel = 0;
	app.current_operator = 0;
	set_operator_param(app, OP_VIB, 0x01);
	set_operator_param(app, OP_SUSTAIN, 0x01);
	set_operator_param(app, OP_FMULTI, 0x08);
	set_operator_param(app, OP_OLVL, 0x1f);
	set_operator_param(app, OP_A, 0x0e);
	set_operator_param(app, OP_D, 0x04);
	set_operator_param(app, OP_S, 0x09);
	set_operator_param(app, OP_R, 0x06);
	app.current_operator = 1;
	set_operator_param(app, OP_SUSTAIN, 0x01);
	set_operator_param(app, OP_FMULTI, 0x02);
	set_operator_param(app, OP_OLVL, 0x00);
	set_operator_param(app, OP_A, 0x0e);
	set_operator_param(app, OP_D, 0x04);
	set_operator_param(app, OP_S, 0x04);
	set_operator_param(app, OP_R, 0x04);

	set_channel_param(app, CH_FNUMBER, 0x03FF);
}

int init_video(app_state_t &app);
void term_video(app_state_t &app);
void render_video(app_state_t &app);

int main()
{
	app_state = app_state_t{};

	// Setup SDL
	int sdlcode = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
	if (sdlcode < 0) {
		fprintf(stderr, "Could not initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	if (0 != init_video(app_state)) {
		return -1;
	}

	SDL_AudioSpec spec{}, obtained_spec;
	spec.freq = kRate;
	spec.format = AUDIO_S16;
	spec.channels = kChannels;
	spec.samples = kBufferSize;
	spec.callback = audio_render_cb;
	spec.userdata = &app_state;
	SDL_AudioDeviceID aid = SDL_OpenAudioDevice(nullptr, 0, &spec, &obtained_spec, 0);
	if (!aid) {
		fprintf(stderr, "Could not open audio device: %s\n", SDL_GetError());
		return -1;
	}

	// Initialize synthesizer
	app_state.synth.Init(kRate);

	// Setup patch
	setup_patch(app_state);


	//app_state.synth.WriteReg(app_state.synth.WriteAddr(0, 0xC0), 0x06); // Set channel 0 FEEDBACK


	// Render!
	printf("Rendering...\n");
	SDL_PauseAudioDevice(aid, 0);
	app_state.bContinue = true;
	while (app_state.bContinue) {
		handle_events(app_state);
		update_synth(app_state);
		render_video(app_state);
	}
	printf("Rendering complete.\n");

	// Clean up
	SDL_CloseAudioDevice(aid);

	term_video(app_state);

	SDL_Quit();

	return 0;
}

void render_line(app_state_t &app, const char * msg, SDL_Color * color)
{
	SDL_Rect srcrect{0, 0, 0, 0};
	TTF_SizeText(app.render_state.font, msg, &srcrect.w, &srcrect.h);
	SDL_Rect dstrect{app.render_state.x, app.render_state.y, srcrect.w, srcrect.h};
	SDL_Surface * text = TTF_RenderText_Solid(app.render_state.font, msg, *color);
	if (text != nullptr) {
		SDL_BlitSurface(text, &srcrect, app.render_state.surface, &dstrect);
		app.render_state.x = 0;
		app.render_state.y += srcrect.h + app.render_state.lineskip;
	} else {
		fprintf(stderr, "Could not render text: %s\n", TTF_GetError());
	}

}

void render_video(app_state_t &app)
{
	SDL_Renderer * renderer = app.render_state.renderer;
	SDL_RenderClear(renderer);

	// Draw black
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderFillRect(renderer, &app.render_state.dim);

	SDL_LockSurface(app.render_state.surface);
	SDL_memset(app.render_state.surface->pixels, 0, app.render_state.dim.h * app.render_state.surface->pitch);
	SDL_UnlockSurface(app.render_state.surface);

	SDL_Color normal = SDL_Color{192, 192, 192, 255};
	SDL_Color opcolor = SDL_Color{192, 192, 255, 255};
	SDL_Color selected = SDL_Color{192, 255, 192, 255};
	app.render_state.x = 0;
	app.render_state.y = 0;
	char msg[1024];
	uint8_t op_index = get_operator(app);
	sprintf(msg, "Channel: #%d; Operator %d (#%d)", app.current_channel, app.current_operator, op_index);
	render_line(app, msg, &normal);
	for (int i=0; i < OP_COUNT; i++) {
		SDL_Color * color = app_state.current_param_type == 1 && app_state.current_param == i ? &selected : &opcolor;
		sprintf(msg, "  %s: 0x%02x", operator_param_str[i], app_state.operators[op_index].params[i]);
		render_line(app, msg, color);
	}
	for (int i=0; i < CH_COUNT; i++) {
		SDL_Color * color = app_state.current_param_type == 0 && app_state.current_param == i ? &selected : &normal;
		sprintf(msg, "  %s: 0x%04x", channel_param_str[i], app_state.channels[app_state.current_channel].params[i]);
		render_line(app, msg, color);
	}

	render_line(app, "Press F1-F12 to select a channel", &normal);
	render_line(app, "Press 1-4 to select channel operator", &normal);
	render_line(app, "Press letter shortcut to select a parameter", &normal);
	render_line(app, "Use the arrow up/down keys to change parameter values", &normal);
	render_line(app, "Press spacebar for Note ON/OFF", &normal);


	// Render whole texture
	SDL_Texture * tex = SDL_CreateTextureFromSurface(renderer, app_state.render_state.surface);
	SDL_RenderCopy(renderer, tex, nullptr, &app.render_state.dim);
	SDL_RenderPresent(renderer);
}

int init_video(app_state_t &app)
{
	if (-1 == TTF_Init())
	{
		fprintf(stderr, "Coult not initialize TTF: %s\n", TTF_GetError());
		return -1;
	}

	app.render_state.font = TTF_OpenFont("/usr/share/fonts/truetype/freefont/FreeMono.ttf", 16);
	if (app.render_state.font == nullptr)
	{
		fprintf(stderr, "Could not load font: %s\n", TTF_GetError());
		return -1;
	}

	app.render_state.lineheight = TTF_FontHeight(app_state.render_state.font);
	app_state.render_state.lineskip = TTF_FontLineSkip(app_state.render_state.font);

	app_state.render_state.dim = {0, 0, 800, 600};
	app_state.render_state.window = SDL_CreateWindow(
		"operatic",                         // window title
		SDL_WINDOWPOS_UNDEFINED,           // initial x position
		SDL_WINDOWPOS_UNDEFINED,           // initial y position
		app_state.render_state.dim.w,                         // width, in pixels
		app_state.render_state.dim.h,                        // height, in pixels
		SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI // flags - see below
	);
	if (app_state.render_state.window == nullptr) {
		fprintf(stderr, "Could not create window: %s", SDL_GetError());
		SDL_Quit();
		return -1;
	}

	app.render_state.renderer = SDL_CreateRenderer(app.render_state.window, -1, SDL_RENDERER_PRESENTVSYNC);
	if (app.render_state.renderer == nullptr) {
		fprintf(stderr, "Could not create renderer: %s", SDL_GetError());
		SDL_Quit();
		return -1;
	}

	app_state.render_state.surface = SDL_CreateRGBSurface(0, app_state.render_state.dim.w, app_state.render_state.dim.h, 32, 0, 0, 0, 255);
	if (app_state.render_state.surface == nullptr) {
		fprintf(stderr, "Could not create surface: %s", SDL_GetError());
		SDL_Quit();
		return -1;
	}
	return 0;
}

void term_video(app_state_t &app)
{
	TTF_CloseFont(app.render_state.font);

	TTF_Quit();
}
