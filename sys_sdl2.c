
#include <SDL.h>
#include "sys.h"
#include "util.h"

#define COPPER_BARS_H 80
#define MAX_SPRITES 256

static const int FADE_STEPS = 16;

struct spritesheet_t {
	int count;
	SDL_Rect *r;
	SDL_Surface *surface;
	SDL_Texture *texture;
};

static struct spritesheet_t _spritesheets[RENDER_SPR_COUNT];

struct sprite_t {
	int sheet;
	int num;
	int x, y;
	bool xflip;
};

static struct sprite_t _sprites[MAX_SPRITES];
static int _sprites_count;
static SDL_Rect _sprites_cliprect;

static int _screen_w, _screen_h;
static int _shake_dx, _shake_dy;
static SDL_Window *_window;
static SDL_Renderer *_renderer;
static SDL_Texture *_texture;
static SDL_Texture *_framebuffer; // new framebuffer for render game before aspect correction
static SDL_PixelFormat *_fmt;
static SDL_Palette *_palette;
static uint32_t _screen_palette[256];
static uint32_t *_screen_buffer;
static int _copper_color_key;
static uint32_t _copper_palette[COPPER_BARS_H];

static SDL_GameController *_controller;
static SDL_Joystick *_joystick;

static void sdl2_init() {
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
	SDL_ShowCursor(SDL_DISABLE);
	_screen_w = _screen_h = 0;
	memset(_screen_palette, 0, sizeof(_screen_palette));
	_palette = SDL_AllocPalette(256);
	_screen_buffer = 0;
	_copper_color_key = -1;
	SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
	_controller = 0;
	const int count = SDL_NumJoysticks();
	if (count > 0) {
		for (int i = 0; i < count; ++i) {
			if (SDL_IsGameController(i)) {
				_controller = SDL_GameControllerOpen(i);
				if (_controller) {
					fprintf(stdout, "Using controller '%s'\n", SDL_GameControllerName(_controller));
					break;
				}
			}
		}
		if (!_controller) {
			_joystick = SDL_JoystickOpen(0);
			if (_joystick) {
				fprintf(stdout, "Using joystick '%s'\n", SDL_JoystickName(_joystick));
			}
		}
	}
}

static void sdl2_fini() {
	if (_fmt) {
		SDL_FreeFormat(_fmt);
		_fmt = 0;
	}
	if (_palette) {
		SDL_FreePalette(_palette);
		_palette = 0;
	}
	if (_texture) {
		SDL_DestroyTexture(_texture);
		_texture = 0;
	}
	if (_framebuffer) {
		SDL_DestroyTexture(_framebuffer);
		_framebuffer = 0;
	}
	if (_renderer) {
		SDL_DestroyRenderer(_renderer);
		_renderer = 0;
	}
	if (_window) {
		SDL_DestroyWindow(_window);
		_window = 0;
	}
	free(_screen_buffer);
	if (_controller) {
		SDL_GameControllerClose(_controller);
		_controller = 0;
	}
	if (_joystick) {
		SDL_JoystickClose(_joystick);
		_joystick = 0;
	}
	SDL_Quit();
}

static void sdl2_set_screen_size(int w, int h, const char *caption, int scale, const char *filter, bool fullscreen) {
	assert(_screen_w == 0 && _screen_h == 0); // abort if called more than once
	_screen_w = w;
	_screen_h = h;
	if (!filter || strcmp(filter, "nearest") == 0) {
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	} else if (strcmp(filter, "linear") == 0) {
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
	} else {
		print_warning("Unhandled filter '%s'", filter);
	}
	const int window_w = w * scale;
	const int window_h = h * scale * 1.2f; // DOS game window aspect correction
	const int flags = fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_RESIZABLE;
	_window = SDL_CreateWindow(caption, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_w, window_h, flags);
	_renderer = SDL_CreateRenderer(_window, -1, 0);
	// SDL_RenderSetLogicalSize(_renderer, w, h); // It fights against aspect correction
	print_debug(DBG_SYSTEM, "set_screen_size %d,%d", _screen_w, _screen_h);
	_screen_buffer = (uint32_t *)calloc(_screen_w * _screen_h, sizeof(uint32_t));
	if (!_screen_buffer) {
		print_error("Failed to allocate screen buffer");
	}
	static const uint32_t pfmt = SDL_PIXELFORMAT_RGB888;
	_texture = SDL_CreateTexture(_renderer, pfmt, SDL_TEXTUREACCESS_STREAMING, _screen_w, _screen_h);
	_fmt = SDL_AllocFormat(pfmt);
	_sprites_cliprect.x = 0;
	_sprites_cliprect.y = 0;
	_sprites_cliprect.w = w;
	_sprites_cliprect.h = h;

	/* final game frame before aspect correction */
	_framebuffer = SDL_CreateTexture(_renderer, pfmt, SDL_TEXTUREACCESS_TARGET, _screen_w, _screen_h);
}

static uint32_t convert_amiga_color(uint16_t color) {
	uint8_t r = (color >> 8) & 15;
	r |= r << 4;
	uint8_t g = (color >> 4) & 15;
	g |= g << 4;
	uint8_t b =  color       & 15;
	b |= b << 4;
	return SDL_MapRGB(_fmt, r, g, b);
}

static void set_amiga_color(uint16_t color, SDL_Color *p) {
	const uint8_t r = (color >> 8) & 15;
	p->r = (r << 4) | r;
	const uint8_t g = (color >> 4) & 15;
	p->g = (g << 4) | g;
	const uint8_t b =  color       & 15;
	p->b = (b << 4) | b;
}

static void sdl2_set_palette_amiga(const uint16_t *colors, int offset) {
	SDL_Color *palette_colors = &_palette->colors[offset];
	for (int i = 0; i < 16; ++i) {
		_screen_palette[offset + i] = convert_amiga_color(colors[i]);
		set_amiga_color(colors[i], &palette_colors[i]);
	}
}

static void sdl2_set_copper_bars(const uint16_t *data) {
	if (!data) {
		_copper_color_key = -1;
	} else {
		_copper_color_key = (data[0] - 0x180) / 2;
		const uint16_t *src = data + 1;
		uint32_t *dst = _copper_palette;
		for (int i = 0; i < COPPER_BARS_H / 5; ++i) {
			const int j = i + 1;
			*dst++ = convert_amiga_color(src[j]);
			*dst++ = convert_amiga_color(src[i]);
			*dst++ = convert_amiga_color(src[j]);
			*dst++ = convert_amiga_color(src[i]);
			*dst++ = convert_amiga_color(src[j]);
		}
        }
}

static void sdl2_set_screen_palette(const uint8_t *colors, int offset, int count, int depth) {
	SDL_Color *palette_colors = &_palette->colors[offset];
	const int shift = 8 - depth;
	for (int i = 0; i < count; ++i) {
		int r = *colors++;
		int g = *colors++;
		int b = *colors++;
		if (depth != 8) {
			r = (r << shift) | (r >> (depth - shift));
			g = (g << shift) | (g >> (depth - shift));
			b = (b << shift) | (b >> (depth - shift));
		}
		_screen_palette[offset + i] = SDL_MapRGB(_fmt, r, g, b);
		palette_colors[i].r = r;
		palette_colors[i].g = g;
		palette_colors[i].b = b;
	}
	for (int i = 0; i < ARRAYSIZE(_spritesheets); ++i) {
		struct spritesheet_t *sheet = &_spritesheets[i];
		if (sheet->surface) {
			SDL_DestroyTexture(sheet->texture);
			sheet->texture = SDL_CreateTextureFromSurface(_renderer, sheet->surface);
		}
	}
}

static void sdl2_set_palette_color(int i, const uint8_t *colors) {
	int r = colors[0];
	r = (r << 2) | (r >> 4);
	int g = colors[1];
	g = (g << 2) | (g >> 4);
	int b = colors[2];
	b = (b << 2) | (b >> 4);
	_screen_palette[i] = SDL_MapRGB(_fmt, r, g, b);
}

static void fade_palette_helper(int in) {
	SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_BLEND);
	SDL_Rect r;
	r.x = r.y = 0;
	SDL_GetRendererOutputSize(_renderer, &r.w, &r.h);
	for (int i = 0; i <= FADE_STEPS; ++i) {
		int alpha = 255 * i / FADE_STEPS;
		if (in) {
			alpha = 255 - alpha;
		}
		SDL_SetRenderDrawColor(_renderer, 0, 0, 0, alpha);
		SDL_RenderClear(_renderer);
		SDL_RenderCopy(_renderer, _texture, 0, 0);
		SDL_RenderFillRect(_renderer, &r);
		SDL_RenderPresent(_renderer);
		SDL_Delay(30);
	}
}

static void sdl2_fade_in_palette() {
	if (!g_sys.input.quit) {
		fade_palette_helper(1);
	}
}

static void sdl2_fade_out_palette() {
	if (!g_sys.input.quit) {
		fade_palette_helper(0);
	}
}

static void sdl2_transition_screen(enum sys_transition_e type, bool open) {
	const int step_w = _screen_w / FADE_STEPS;
	const int step_h = _screen_h / FADE_STEPS;
	SDL_Rect r;
	r.x = 0;
	r.w = 0;
	r.y = 0;
	r.h = (type == TRANSITION_CURTAIN) ? _screen_h : 0;
	do {
		r.x = (_screen_w - r.w) / 2;
		if (r.x < 0) {
			r.x = 0;
		}
		r.w += step_w;
		if (r.x + r.w > _screen_w) {
			r.w = _screen_w - r.x;
		}
		if (type == TRANSITION_SQUARE) {
			r.y = (_screen_h - r.h) / 2;
			if (r.y < 0) {
				r.y = 0;
			}
			r.h += step_h;
			if (r.y + r.h > _screen_h) {
				r.h = _screen_h - r.y;
			}
		}
		SDL_RenderClear(_renderer);
		SDL_RenderCopy(_renderer, _texture, &r, &r);
		SDL_RenderPresent(_renderer);
		SDL_Delay(30);
	} while (r.x > 0 && (type == TRANSITION_CURTAIN || r.y > 0));
}

static void sdl2_copy_bitmap(const uint8_t *p, int w, int h) {
	if (w != _screen_w || h != _screen_h) {
		memset(_screen_buffer, 0, _screen_w * _screen_h * sizeof(uint32_t));
		const int offset_x = (_screen_w - w) / 2;
		const int offset_y = (_screen_h - h) / 2;
		for (int j = 0; j < h; ++j) {
			for (int i = 0; i < w; ++i) {
				_screen_buffer[(offset_y + j) * _screen_w + (offset_x + i)] = _screen_palette[*p++];
			}
		}
	} else if (_copper_color_key != -1) {
		for (int j = 0; j < _screen_h; ++j) {
			const int color = (j * 200 / _screen_h) / 2;
			if (color < COPPER_BARS_H) {
				const uint32_t line_color = _copper_palette[color];
				for (int i = 0; i < _screen_w; ++i) {
					_screen_buffer[j * _screen_w + i] = (p[i] == _copper_color_key) ? line_color : _screen_palette[p[i]];
				}
			} else {
				for (int i = 0; i < _screen_w; ++i) {
					_screen_buffer[j * _screen_w + i] = _screen_palette[p[i]];
				}
			}
			p += _screen_w;
		}
	} else {
		for (int i = 0; i < _screen_w * _screen_h; ++i) {
			_screen_buffer[i] = _screen_palette[p[i]];
		}
	}
	SDL_UpdateTexture(_texture, 0, _screen_buffer, _screen_w * sizeof(uint32_t));
}

static void sdl2_update_game_screen() {
	SDL_Rect r;
	r.x = _shake_dx;
	r.y = _shake_dy;
	r.w = _screen_w;
	r.h = _screen_h;
	SDL_RenderClear(_renderer);
	SDL_RenderCopy(_renderer, _texture, 0, &r);

	// sprites
	SDL_RenderSetClipRect(_renderer, &_sprites_cliprect);
	for (int i = 0; i < _sprites_count; ++i) {
		const struct sprite_t *spr = &_sprites[i];
		struct spritesheet_t *sheet = &_spritesheets[spr->sheet];
		if (spr->num >= sheet->count) {
			continue;
		}
		SDL_Rect r;
		r.x = spr->x + _shake_dx;
		r.y = spr->y + _shake_dy;
		r.w = sheet->r[spr->num].w;
		r.h = sheet->r[spr->num].h;
		if (!spr->xflip) {
			SDL_RenderCopy(_renderer, sheet->texture, &sheet->r[spr->num], &r);
		} else {
			SDL_RenderCopyEx(_renderer, sheet->texture, &sheet->r[spr->num], &r, 0., 0, SDL_FLIP_HORIZONTAL);
		}
	}
	SDL_RenderSetClipRect(_renderer, 0);
}

static void sdl2_shake_screen(int dx, int dy) {
	_shake_dx = dx;
	_shake_dy = dy;
}

static void sdl2_present_framebuffer() {
	int ww;
	int wh;

	SDL_GetWindowSize(_window, &ww, &wh);
	SDL_RenderClear(_renderer);

	SDL_Rect dst;

	dst.w = ww; // fit in window width
	dst.h = ww * 3 / 4; // get 4:3 height from the width.
	dst.x = 0;
	dst.y = (wh - dst.h) / 2; // center vertically

	SDL_RenderCopy(_renderer, _framebuffer, NULL, &dst);

	SDL_RenderPresent(_renderer);
}

static void sdl2_update_screen() {
	SDL_SetRenderTarget(_renderer, _framebuffer); // First pass: render game into offscreen framebuffer

	sdl2_update_game_screen();

	SDL_SetRenderTarget(_renderer, NULL); // Second pass: render framebuffer to window with DOS aspect correction (320x200 -> 320x240)

	sdl2_present_framebuffer();
}

static void handle_keyevent(int keysym, bool keydown, struct input_t *input, bool *paused) {
	switch (keysym) {
	case SDLK_ESCAPE:
		if (keydown) {
			g_sys.input.quit = true;
		}
		break;
	case SDLK_1:
		input->digit1 = keydown;
		break;
	case SDLK_2:
		input->digit2 = keydown;
		break;
	case SDLK_3:
		input->digit3 = keydown;
		break;
	case SDLK_p:
		if (!keydown) {
			*paused = !*paused;
		}
		break;
	}
}

static int handle_event(const SDL_Event *ev, bool *paused) {
	switch (ev->type) {
	case SDL_QUIT:
		g_sys.input.quit = true;
		break;
	case SDL_WINDOWEVENT:
		switch (ev->window.event) {
		case SDL_WINDOWEVENT_FOCUS_GAINED:
		case SDL_WINDOWEVENT_FOCUS_LOST:
			*paused = (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST);
			break;
		}
		break;
	case SDL_KEYUP:
		switch (ev->key.keysym.scancode) {
		case SDL_SCANCODE_LEFT:
		case SDL_SCANCODE_RIGHT:
		case SDL_SCANCODE_UP:
		case SDL_SCANCODE_DOWN:
			/* movement handled by SDL_GetKeyboardState() */
			break;

		default:
			handle_keyevent(ev->key.keysym.sym, 0, &g_sys.input, paused);
			break;
		}
		break;
		case SDL_KEYDOWN:
			if (!ev->key.repeat) {
				switch (ev->key.keysym.scancode) {

				case SDL_SCANCODE_LEFT:
				case SDL_SCANCODE_RIGHT:
				case SDL_SCANCODE_UP:
				case SDL_SCANCODE_DOWN:
					/* movement handled by SDL_GetKeyboardState() */
					break;

				default:
					handle_keyevent(ev->key.keysym.sym, 1, &g_sys.input, paused);
					break;
				}
			}
			break;
	case SDL_CONTROLLERDEVICEADDED:
		if (!_controller) {
			_controller = SDL_GameControllerOpen(ev->cdevice.which);
			if (_controller) {
				fprintf(stdout, "Using controller '%s'\n", SDL_GameControllerName(_controller));
			}
		}
		break;
	case SDL_CONTROLLERDEVICEREMOVED:
		if (_controller == SDL_GameControllerFromInstanceID(ev->cdevice.which)) {
			fprintf(stdout, "Removed controller '%s'\n", SDL_GameControllerName(_controller));
			SDL_GameControllerClose(_controller);
			_controller = 0;
		}
		break;
	default:
		return -1;
	}
	return 0;
}

static void update_keyboard_input(void) {
	SDL_PumpEvents();

	const Uint8 *state = SDL_GetKeyboardState(NULL);

	g_sys.input.direction = 0;

	if (state[SDL_SCANCODE_LEFT] || state[SDL_SCANCODE_A]) {
		g_sys.input.direction |= INPUT_DIRECTION_LEFT;
	}
	if (state[SDL_SCANCODE_RIGHT] || state[SDL_SCANCODE_D]) {
		g_sys.input.direction |= INPUT_DIRECTION_RIGHT;
	}
	if (state[SDL_SCANCODE_UP] || state[SDL_SCANCODE_W]) {
		g_sys.input.direction |= INPUT_DIRECTION_UP;
	}
	if (state[SDL_SCANCODE_DOWN] || state[SDL_SCANCODE_S]) {
		g_sys.input.direction |= INPUT_DIRECTION_DOWN;
	}

	g_sys.input.space =
		state[SDL_SCANCODE_SPACE] ||
		state[SDL_SCANCODE_RETURN];

	g_sys.input.jump =
		state[SDL_SCANCODE_LSHIFT] ||
		state[SDL_SCANCODE_RSHIFT];
}

static void update_controller_input(bool *paused) {
	if (!_controller) {
		return;
	}

	if (SDL_GameControllerGetButton(
		_controller,
		SDL_CONTROLLER_BUTTON_DPAD_LEFT)) {
		g_sys.input.direction |= INPUT_DIRECTION_LEFT;
	}

	if (SDL_GameControllerGetButton(
		_controller,
		SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
		g_sys.input.direction |= INPUT_DIRECTION_RIGHT;
	}

	if (SDL_GameControllerGetButton(
		_controller,
		SDL_CONTROLLER_BUTTON_DPAD_UP)) {
		g_sys.input.direction |= INPUT_DIRECTION_UP;
	}

	if (SDL_GameControllerGetButton(
		_controller,
		SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
		g_sys.input.direction |= INPUT_DIRECTION_DOWN;
	}

	const int lx = SDL_GameControllerGetAxis(
		_controller,
		SDL_CONTROLLER_AXIS_LEFTX);

	const int ly = SDL_GameControllerGetAxis(
		_controller,
		SDL_CONTROLLER_AXIS_LEFTY);

	const int THRESHOLD = 6400;

	if (lx < -THRESHOLD) {
		g_sys.input.direction |= INPUT_DIRECTION_LEFT;
	}
	if (lx > THRESHOLD) {
		g_sys.input.direction |= INPUT_DIRECTION_RIGHT;
	}
	if (ly < -THRESHOLD) {
		g_sys.input.direction |= INPUT_DIRECTION_UP;
	}
	if (ly > THRESHOLD) {
		g_sys.input.direction |= INPUT_DIRECTION_DOWN;
	}

	g_sys.input.space |= SDL_GameControllerGetButton(
		_controller,
		SDL_CONTROLLER_BUTTON_A);

	g_sys.input.jump |= SDL_GameControllerGetButton(
		_controller,
		SDL_CONTROLLER_BUTTON_B);

	/* START button edge detection */
	static bool prev_start = false;

	const bool start = SDL_GameControllerGetButton(
		_controller,
		SDL_CONTROLLER_BUTTON_START);

	if (start && !prev_start) {
		*paused = !*paused;
		SDL_PauseAudio(*paused);
	}

	prev_start = start;
}

static void edge_triggered_down() {
		static uint8_t prev_down = 0;

		uint8_t down_now = (g_sys.input.direction & INPUT_DIRECTION_DOWN) ? 1 : 0;

		g_sys.input.down_pressed = down_now && !prev_down;

		prev_down = down_now;
}

static void sdl2_process_events() {
	bool paused = false;
	while (1) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			const bool prev = paused;
			handle_event(&ev, &paused);
			if (g_sys.input.quit) {
				break;
			}
			if (prev != paused) {
				SDL_PauseAudio(paused);
			}
		}

		update_keyboard_input();
		update_controller_input(&paused);
		edge_triggered_down();

		if (!paused) {
			break;
		}
		SDL_Delay(10);
	}
}

static void sdl2_sleep(int duration) {
	SDL_Delay(duration);
}

static uint32_t sdl2_get_timestamp() {
	return SDL_GetTicks();
}

static void sdl2_start_audio(sys_audio_cb callback, void *param) {
	SDL_AudioSpec desired;
	memset(&desired, 0, sizeof(desired));
	desired.freq = SYS_AUDIO_FREQ;
	desired.format = AUDIO_S16;
	desired.channels = 1;
	desired.samples = 2048;
	desired.callback = callback;
	desired.userdata = param;
	if (SDL_OpenAudio(&desired, 0) == 0) {
		SDL_PauseAudio(0);
	}
}

static void sdl2_stop_audio() {
	SDL_CloseAudio();
}

static void sdl2_lock_audio() {
	SDL_LockAudio();
}

static void sdl2_unlock_audio() {
	SDL_UnlockAudio();
}

static void render_load_sprites(int spr_type, int count, const struct sys_rect_t *r, const uint8_t *data, int w, int h, uint8_t color_key, bool update_pal) {
	assert(spr_type < ARRAYSIZE(_spritesheets));
	struct spritesheet_t *sheet = &_spritesheets[spr_type];
	sheet->count = count;
	sheet->r = (SDL_Rect *)malloc(count * sizeof(SDL_Rect));
	for (int i = 0; i < count; ++i) {
		SDL_Rect *rect = &sheet->r[i];
		rect->x = r[i].x;
		rect->y = r[i].y;
		rect->w = r[i].w;
		rect->h = r[i].h;
	}
	SDL_Surface *surface = SDL_CreateRGBSurface(0, w, h, 8, 0x0, 0x0, 0x0, 0x0);
	SDL_SetSurfacePalette(surface, _palette);
	SDL_SetColorKey(surface, 1, color_key);
	SDL_LockSurface(surface);
	for (int y = 0; y < h; ++y) {
		memcpy(((uint8_t *)surface->pixels) + y * surface->pitch, data + y * w, w);
	}
	SDL_UnlockSurface(surface);
	sheet->texture = SDL_CreateTextureFromSurface(_renderer, surface);
	if (update_pal) { /* update texture on palette change */
		sheet->surface = surface;
	} else  {
		SDL_FreeSurface(surface);
	}
}

static void render_unload_sprites(int spr_type) {
	struct spritesheet_t *sheet = &_spritesheets[spr_type];
	free(sheet->r);
	if (sheet->surface) {
		SDL_FreeSurface(sheet->surface);
	}
	if (sheet->texture) {
		SDL_DestroyTexture(sheet->texture);
	}
	memset(sheet, 0, sizeof(struct spritesheet_t));
}

static void render_add_sprite(int spr_type, int frame, int x, int y, int xflip) {
	assert(_sprites_count < ARRAYSIZE(_sprites));
	struct sprite_t *spr = &_sprites[_sprites_count];
	spr->sheet = spr_type;
	spr->num = frame;
	spr->x = x;
	spr->y = y;
	spr->xflip = xflip;
	++_sprites_count;
}

static void render_clear_sprites() {
	_sprites_count = 0;
}

static void render_set_sprites_clipping_rect(int x, int y, int w, int h) {
	_sprites_cliprect.x = x;
	_sprites_cliprect.y = y;
	_sprites_cliprect.w = w;
	_sprites_cliprect.h = h;
}

static void print_log(FILE *fp, const char *s) {
}

struct sys_t g_sys = {
	.init = sdl2_init,
	.fini = sdl2_fini,
	.set_screen_size = sdl2_set_screen_size,
	.set_screen_palette = sdl2_set_screen_palette,
	.set_palette_amiga = sdl2_set_palette_amiga,
	.set_copper_bars = sdl2_set_copper_bars,
	.set_palette_color = sdl2_set_palette_color,
	.fade_in_palette = sdl2_fade_in_palette,
	.fade_out_palette = sdl2_fade_out_palette,
	.copy_bitmap = sdl2_copy_bitmap,
	.update_screen = sdl2_update_screen,
	.shake_screen = sdl2_shake_screen,
	.transition_screen = sdl2_transition_screen,
	.process_events = sdl2_process_events,
	.sleep = sdl2_sleep,
	.get_timestamp = sdl2_get_timestamp,
	.start_audio = sdl2_start_audio,
	.stop_audio = sdl2_stop_audio,
	.lock_audio = sdl2_lock_audio,
	.unlock_audio = sdl2_unlock_audio,
	.render_load_sprites = render_load_sprites,
	.render_unload_sprites = render_unload_sprites,
	.render_add_sprite = render_add_sprite,
	.render_clear_sprites = render_clear_sprites,
	.render_set_sprites_clipping_rect = render_set_sprites_clipping_rect,
	.print_log = print_log,
};
