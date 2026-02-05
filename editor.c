/*
	Each frame stores non-unique pointer for buffers.
	Each buffer stores unique pointer for text. For each file there should be only one buffer.
	There's a global list of frames inside ctx.
	Althrough ctx stores dynamic array of frames, frames shouldn't be moved or resized into smaller size.
		- TODO(c4llv07e): Think about frames "defragmentation procedure".
	"Ask buffer" (aka emacs minibuffer) should be the frame too to work with keybings.
	Frame type should work as a smaill and controllable polymorphism, i.e. cast the meaning of the buffer,
		not the "class".
*/

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_CHUNK_SIZE 256

#define CHAR_SIZE SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE
#define LINE_HEIGHT ((int)(TEXT_SIZE * 1.5))

typedef struct {
	Uint32 text_size;
	Uint32 text_capacity;
	char *text;
} TextBuffer;

typedef enum {
	Frame_Type_memory = 0, // The most safe one
	Frame_Type_file,
	Frame_Type_ask,
	Frame_Type_length,
} Frame_Type;

typedef struct {
	Uint32 is_deleted;
	Frame_Type frame_type;
	char *filename;
	SDL_FRect bounds;
	SDL_FPoint scroll;
	// I want edit one files in multiple frames, so cursor is needed here
	Uint32 cursor;
	TextBuffer *buffer;
} Frame;

#define ASK_BUFFER_SIZE 0x1000

typedef struct {
	SDL_Renderer *renderer;
	SDL_Window *window;
	Uint32 last_row;
	int win_w, win_h;
	int linebar_length;
	const bool *keys;
	const char *opened_file;
	SDL_Keymod keymod;
	bool running;
	bool moving_col; // When cursor was just moving up and down
	Uint32 frames_count;
	Uint32 frames_capacity;
	Frame *frames;
	Uint32 focused_frame;
} Ctx;

static const SDL_Color text_color = {0xe6, 0xe6, 0xe6, 0xff};

static void buffer_insert_text(Ctx *ctx, TextBuffer *buffer, const char *in, size_t in_len, Uint32 pos) {
	(void)ctx;
	if (in_len == 0) return;
	if (pos > buffer->text_size) pos = buffer->text_size;
	size_t new_size = (size_t)buffer->text_size + in_len;
	if (new_size + 1 > buffer->text_capacity) {
		size_t new_capacity = ((new_size + 1 + TEXT_CHUNK_SIZE - 1) / TEXT_CHUNK_SIZE) * TEXT_CHUNK_SIZE;
		char *new_buf = realloc(buffer->text, new_capacity);
		if (!new_buf) {
			SDL_Log("Error, failed to reallocate new buffer");
			return;
		}
		buffer->text = new_buf;
		buffer->text_capacity = (Uint32)new_capacity;
	}
	memmove(buffer->text + pos + in_len,
		buffer->text + pos,
		(size_t)buffer->text_size - (size_t)pos);
	memcpy(buffer->text + pos, in, in_len);
	buffer->text_size = (Uint32)new_size;
	buffer->text[buffer->text_size] = '\0';
}

#define TAB_WIDTH 8

static void render_line(Ctx *ctx, SDL_FRect frame, const char *buffer, size_t len) {
	char tmp[1024];
	size_t out = 0;
	for (size_t i = 0; i < len && out < sizeof(tmp) - 1 && out < frame.w / 8; ++i) {
		if (buffer[i] == '\t') {
			for (int s = 0; s < TAB_WIDTH && out < sizeof(tmp) - 1; ++s) {
				tmp[out++] = ' ';
			}
		} else {
			tmp[out++] = buffer[i];
		}
	}
	tmp[out] = '\0';
	SDL_SetRenderDrawColor(ctx->renderer, text_color.r, text_color.g, text_color.b, text_color.a);
	SDL_RenderDebugText(ctx->renderer, frame.x, frame.y, tmp);
#ifdef DEBUG_LAYOUT
	SDL_SetRenderDrawColor(ctx->renderer, 0xff, 0x00, 0x00, 0xff);
	SDL_RenderRect(ctx->renderer, &frame);
#endif
}

static void render_frame(Ctx *ctx, Frame *frame) {
	int line = 0;
	const char *text = frame->buffer->text;
	Uint32 length = frame->buffer->text_size;
	const char *start = text;
	const char *end = start;
	while (end - text <= length) {
		while ((end - text < length) && *end != '\n') {
			end++;
		}
		render_line(ctx, (SDL_FRect) {
			.x = frame->bounds.x,
			.y = frame->bounds.y + line * 12,
			.w = frame->bounds.w,
			.h = 12,
		}, start, end - start);
		if (frame->cursor >= (start - frame->buffer->text) && frame->cursor <= (end - frame->buffer->text)) {
			int col = 0;
			const char *cur = start;
			while (frame->cursor > (cur - frame->buffer->text)) {
				if (*cur == '\t') {
					col += TAB_WIDTH;
					cur++;
				} else {
					col++;
					cur++;
				}
			}
			SDL_SetRenderDrawColor(ctx->renderer, text_color.r, text_color.g, text_color.b, text_color.a);
			SDL_RenderRect(ctx->renderer, &(SDL_FRect) {
				.x = frame->bounds.x + col * 8,
				.y = frame->bounds.y + line * 12,
				.w = 2,
				.h = 12,
			});
		}
		start = end = end + 1;
		if (end - text >= length) break;
		line++;
	}
#ifdef DEBUG_LAYOUT
	SDL_SetRenderDrawColor(ctx->renderer, 0x00, 0xff, 0x00, 0xff);
	SDL_RenderRect(ctx->renderer, &frame->bounds);
#endif
}

static TextBuffer *allocate_buffer(Ctx *ctx) {
	(void)ctx;
	TextBuffer *buffer;
	buffer = malloc(sizeof *buffer);
	if (buffer == NULL) {
		SDL_Log("Error, can't allocate buffer");
		return NULL;
	}
	buffer->text_capacity = 0;
	buffer->text = NULL;
	return buffer;
}

static Uint32 append_frame(Ctx *ctx, TextBuffer *buffer, SDL_FRect bounds) {
	if (ctx->frames_capacity <= ctx->frames_count) {
		size_t new_cap = ctx->frames_capacity * 2;
		if (new_cap == 0) {
			new_cap = 8;
		}
		Frame *new_frames = realloc(ctx->frames, new_cap * (sizeof *ctx->frames));
		if (new_frames == NULL) {
			SDL_Log("Can't reallocate frames array");
			return -1;
		}
		ctx->frames = new_frames;
		ctx->frames_capacity = new_cap;
	}
	Uint32 frame_ind = ctx->frames_count++;
	ctx->frames[ctx->frames_count++] = (Frame){
		.cursor = 0,
		.scroll = 0,
		.bounds = bounds,
		.buffer = buffer,
	};
	return frame_ind;
}

static Uint32 create_ask_frame(Ctx *ctx) {
	TextBuffer *buffer = allocate_buffer(&ctx);
	if (buffer == NULL) {
		SDL_Log("Error, can't allocate ask buffer");
		return -1;
	}
	Uint32 frame = append_frame(ctx, buffer, (SDL_FRect){0, ctx->win_h - LINE_HEIGHT, ctx->win_w, LINE_HEIGHT});
	if (frame == -1) {
		SDL_Log("Error, can't append ask buffer");
		return -1;
	}
	return frame;
}

int main(int argc, char *argv[argc]) {
	(void)argv;
	Ctx ctx = {0};
	TextBuffer *buffer = allocate_buffer(&ctx);
	if (buffer == NULL) {
		SDL_Log("Error, can't allocate first buffer");
		return -1;
	}
	if (append_frame(&ctx, buffer, (SDL_FRect){0, 0, 100, 100}) == -1) {
		SDL_Log("Error, can't create first frame");
		return -1;
	}
	ctx.frames[0].buffer->text_size = 0;
	ctx.frames[0].buffer->text_capacity = 0;
	ctx.frames[0].buffer->text = NULL;
	ctx.keys = NULL;
	ctx.linebar_length = 3;
	ctx.opened_file = NULL;
	Uint64 window_flags =  SDL_WINDOW_RESIZABLE;
#if defined(DEBUG) && !defined(_WIN32)
	window_flags |= SDL_WINDOW_UTILITY;
#endif
	if (!SDL_CreateWindowAndRenderer("test", 0x300, 0x200, window_flags, &ctx.window, &ctx.renderer)) {
		SDL_Log("Error, can't init renderer: %s", SDL_GetError());
		return -1;
	}
	if (!SDL_SetRenderVSync(ctx.renderer, 1)) {
		SDL_Log("Warning, can't enable vsync: %s", SDL_GetError());
	}
#ifdef DEBUG
	const char *text = "int main(void) {\n\treturn 0;\n}\n";
	buffer_insert_text(&ctx, buffer, text, strlen(text), 0);
	ctx.frames[ctx.focused_frame].cursor = strlen(text);
	text = "aaaaaaa\n";
	buffer_insert_text(&ctx, buffer, text, strlen(text), 0);
	text = "aa\n";
	buffer_insert_text(&ctx, buffer, text, strlen(text), 0);
	text = "aaaaaaa\n";
	buffer_insert_text(&ctx, buffer, text, strlen(text), 0);
#endif
	ctx.running = true;
	while (ctx.running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			switch (ev.type) {
				case SDL_EVENT_QUIT: {
					ctx.running = false;
					break;
				}; break;
				case SDL_EVENT_KEY_DOWN: {
					switch (ev.key.scancode) {
						case SDL_SCANCODE_LEFT: {
							ctx.moving_col = false;
							if (ctx.frames[ctx.focused_frame].cursor > 0) ctx.frames[ctx.focused_frame].cursor -= 1;
						}; break;
						case SDL_SCANCODE_RIGHT: {
							ctx.moving_col = false;
							if (ctx.frames[ctx.focused_frame].cursor < ctx.frames[ctx.focused_frame].buffer->text_size) ctx.frames[ctx.focused_frame].cursor += 1;
						}; break;
						case SDL_SCANCODE_BACKSPACE: {
							ctx.moving_col = false;
							if (ctx.frames[ctx.focused_frame].cursor > 0 && ctx.frames[ctx.focused_frame].buffer->text_size > 0) {
								memmove(ctx.frames[ctx.focused_frame].buffer->text + ctx.frames[ctx.focused_frame].cursor - 1,
									ctx.frames[ctx.focused_frame].buffer->text + ctx.frames[ctx.focused_frame].cursor,
									ctx.frames[ctx.focused_frame].buffer->text_size - ctx.frames[ctx.focused_frame].cursor + 1);
								ctx.frames[ctx.focused_frame].cursor -= 1;
								ctx.frames[ctx.focused_frame].buffer->text_size -= 1;
							}
						}; break;
						case SDL_SCANCODE_RETURN: {
							ctx.moving_col = false;
							char nl = '\n';
							buffer_insert_text(&ctx, ctx.frames[ctx.focused_frame].buffer, &nl, 1, ctx.frames[ctx.focused_frame].cursor);
							ctx.frames[ctx.focused_frame].cursor += 1;
						}; break;
						case SDL_SCANCODE_TAB: {
							ctx.moving_col = false;
							char nl = '\t';
							buffer_insert_text(&ctx, ctx.frames[ctx.focused_frame].buffer, &nl, 1, ctx.frames[ctx.focused_frame].cursor);
							ctx.frames[ctx.focused_frame].cursor += 1;
						}; break;
						case SDL_SCANCODE_UP: {
							int row = 0;
							if (ctx.frames[ctx.focused_frame].cursor > 0) {
								ctx.frames[ctx.focused_frame].cursor--;
							}
							while (ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] != '\n' && ctx.frames[ctx.focused_frame].cursor > 0) {
								if (ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] == '\t') row += 4;
								else row++;
								ctx.frames[ctx.focused_frame].cursor--;
							}
							if (ctx.moving_col) row = ctx.last_row;
							else ctx.last_row = row;
							ctx.moving_col = true;
							if (ctx.frames[ctx.focused_frame].cursor == 0) break;
							ctx.frames[ctx.focused_frame].cursor--;
							while (ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] != '\n' && ctx.frames[ctx.focused_frame].cursor > 0) {
								ctx.frames[ctx.focused_frame].cursor--;
							}
							if (ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] == '\n') ctx.frames[ctx.focused_frame].cursor++;
							while (row > 0 && ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] != '\n') {
								if (ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] == '\t') row -= TAB_WIDTH;
								else row--;
								ctx.frames[ctx.focused_frame].cursor++;
							}
						}; break;
						case SDL_SCANCODE_DOWN: {
							int row = 0;
							if (ctx.frames[ctx.focused_frame].cursor > 0) {
								ctx.frames[ctx.focused_frame].cursor--;
							}
							while (ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] != '\n' && ctx.frames[ctx.focused_frame].cursor > 0) {
								if (ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] == '\t') row += 4;
								else row++;
								ctx.frames[ctx.focused_frame].cursor--;
							}
							if (ctx.moving_col) row = ctx.last_row;
							else ctx.last_row = row;
							ctx.moving_col = true;
							ctx.frames[ctx.focused_frame].cursor++;
							while (ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] != '\n' && ctx.frames[ctx.focused_frame].cursor < ctx.frames[ctx.focused_frame].buffer->text_size) {
								ctx.frames[ctx.focused_frame].cursor++;
							}
							if (ctx.frames[ctx.focused_frame].cursor == ctx.frames[ctx.focused_frame].buffer->text_size) break;
							ctx.frames[ctx.focused_frame].cursor++;
							while (row > 0 && ctx.frames[ctx.focused_frame].cursor < ctx.frames[ctx.focused_frame].buffer->text_size && ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] != '\n') {
								if (ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] == '\t') row -= TAB_WIDTH;
								else row--;
								ctx.frames[ctx.focused_frame].cursor++;
							}
						}; break;
						case SDL_SCANCODE_S: {
							if (!(ctx.keymod & SDL_KMOD_CTRL)) break;
						}; break;
						case SDL_SCANCODE_O: {
							if (!(ctx.keymod & SDL_KMOD_CTRL)) break;
							TODO
						}; break;
						default: {};
					}
				}; break;
				case SDL_EVENT_MOUSE_BUTTON_DOWN: {
					int row = (ev.button.x / 8.0) + 0.7;
					int col = ev.button.y / 12;
					ctx.last_row = row;
					ctx.moving_col = true;
					ctx.frames[ctx.focused_frame].cursor = 0;
					while (col > 0) {
						if (ctx.frames[ctx.focused_frame].cursor >= ctx.frames[ctx.focused_frame].buffer->text_size) break;
						ctx.frames[ctx.focused_frame].cursor++;
						if (ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] == '\n') col--;
					}
					ctx.frames[ctx.focused_frame].cursor++;
					while (row > 0) {
						if (ctx.frames[ctx.focused_frame].cursor >= ctx.frames[ctx.focused_frame].buffer->text_size) break;
						if (ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] == '\n') break;
						if (ctx.frames[ctx.focused_frame].buffer->text[ctx.frames[ctx.focused_frame].cursor] == '\t') {
							ctx.frames[ctx.focused_frame].cursor++;
							row -= TAB_WIDTH;
						} else {
							ctx.frames[ctx.focused_frame].cursor++;
							row--;
						}
					}
				}; break;
				case SDL_EVENT_TEXT_INPUT: {
					ctx.moving_col = false;
					if (ctx.keymod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) break;
					buffer_insert_text(&ctx, ctx.frames[ctx.focused_frame].buffer, ev.text.text, strlen(ev.text.text), ctx.frames[ctx.focused_frame].cursor);
					ctx.frames[ctx.focused_frame].cursor += strlen(ev.text.text);
				}; break;
			}
		}
		if (!SDL_TextInputActive(ctx.window)) {
			SDL_StartTextInput(ctx.window);
		}
		if (!SDL_GetWindowSize(ctx.window, &ctx.win_w, &ctx.win_h)) {
			SDL_Log("Can't get window properties: %s", SDL_GetError());
			// Maybe exiting right away isn't required, but I don't know what should happened so SDL couldn't get window size
			ctx.running = false;
		}
		ctx.keys = SDL_GetKeyboardState(NULL);
		ctx.keymod = SDL_GetModState();
		if (!ctx.running) break;
		SDL_SetRenderDrawColor(ctx.renderer, 0x12, 0x12, 0x12, 0xff);
		SDL_RenderClear(ctx.renderer);
		render_frame(&ctx, &ctx.frames[ctx.focused_frame]);
		if (ctx.input_focus == InputFocus_Ask) {
			SDL_SetRenderDrawColor(ctx.renderer, 0x08, 0x08, 0x08, 0xff);
			SDL_RenderRect(ctx.renderer, &(SDL_FRect){0, 100, ctx.win_w, 12});
		}
		SDL_RenderPresent(ctx.renderer);
	}
#ifdef DEBUG
	free(ctx.frames[ctx.focused_frame].buffer->text);
#endif
	return 0;
}
