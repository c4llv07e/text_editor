/*
	Each frame stores non-unique pointer for buffers.
	Each buffer stores unique pointer for text. For each file there should be only one buffer.
	There's a global list of frames inside ctx.
	Althrough ctx stores dynamic array of frames, frames shouldn't be moved or resized into smaller size.
		- TODO(c4llv07e): Think about frames "defragmentation procedure".
	"Ask buffer" (aka emacs minibuffer) should be the frame too to work with keybings.
	Frame type should work as a smaill and controllable polymorphism, i.e. cast the meaning of the buffer,
		not the "class".
	Font is always monospace, because with non-monospaced font: (sorry, acme)
		- converting coordinates to index is hard,
		- computing layout is slower,
		- second aligment after indent doesn't exists,
		- rectangular selection doesn't work.
*/

#include <SDL3/SDL.h>

#define TEXT_CHUNK_SIZE 256
#define TAB_WIDTH 8

#define CHAR_SIZE SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE
#define LINE_HEIGHT ((int)(CHAR_SIZE * 1.5))

typedef struct {
	size_t size;
	char *text;
} String;

typedef struct {
	size_t text_size;
	size_t text_capacity;
	char *text;
} TextBuffer;

typedef enum {
	Frame_Type_memory = 0, // The most safe one
	Frame_Type_file,
	Frame_Type_ask,
} Frame_Type;

typedef enum {
	Ask_Option_open = 0,
	Ask_Option_save,
} Ask_Option;

typedef struct Frame {
	bool taken;
	bool is_global;
	Frame_Type frame_type;
	Uint32 parent_frame;
	Ask_Option ask_option;
	char *filename;
	SDL_FRect bounds;
	SDL_FPoint scroll;
	// I want edit one files in multiple frames, so cursor is needed here
	Uint32 cursor;
	TextBuffer *buffer;
} Frame;

typedef struct Ctx {
	SDL_Renderer *renderer;
	SDL_Window *window;
	Uint32 last_row;
	int win_w, win_h;
	int linebar_length;
	const bool *keys;
	const char *opened_file;
	SDL_Keymod keymod;
	SDL_FPoint mouse_pos;
	bool running;
	bool moving_col; // When cursor was just moving up and down
	Uint32 frames_count;
	Uint32 frames_capacity;
	Frame *frames;
	Uint32 *sorted_frames;
	Uint32 focused_frame;
	SDL_FPoint transform;
	Uint64 last_middle_click;
} Ctx;

static const SDL_Color text_color = {0xe6, 0xe6, 0xe6, SDL_ALPHA_OPAQUE};

static inline Uint32 reverse_sorted_index(Ctx *ctx, Uint32 sorted_ind) {
	for (Uint32 i = 0; i < ctx->frames_count; ++i) {
		if (ctx->sorted_frames[i] == sorted_ind) return i;
	}
	SDL_assert(!"Sorted index out of bounds");
	return -1;
}

static void buffer_insert_text(Ctx *ctx, TextBuffer *buffer, const char *in, size_t in_len, Uint32 pos) {
	(void)ctx;
	if (in_len == 0) return;
	if (pos > buffer->text_size) pos = buffer->text_size;
	size_t new_size = (size_t)buffer->text_size + in_len;
	if (new_size + 1 > buffer->text_capacity) {
		size_t new_capacity = ((new_size + 1 + TEXT_CHUNK_SIZE - 1) / TEXT_CHUNK_SIZE) * TEXT_CHUNK_SIZE;
		char *new_buf = SDL_realloc(buffer->text, new_capacity);
		if (!new_buf) {
			SDL_Log("Error, failed to reallocate new buffer");
			return;
		}
		buffer->text = new_buf;
		buffer->text_capacity = (Uint32)new_capacity;
	}
	SDL_memmove(buffer->text + pos + in_len,
		buffer->text + pos,
		(size_t)buffer->text_size - (size_t)pos);
	SDL_memcpy(buffer->text + pos, in, in_len);
	buffer->text_size = (Uint32)new_size;
	buffer->text[buffer->text_size] = '\0';
}

static inline Uint32 coords_to_text_index(Ctx *ctx, size_t text_length, char text[text_length], float pos) {
	Uint32 visual_char, ind;
	(void) ctx;
	// [ ][ ][ ][ ][ ][ ][ ][ ][a][b][c]
	//       | 2 before
	// | 0 vis_char
	//                      | 7 nvis_char
	//                            | 9 after
	visual_char = 0;
	if (pos / CHAR_SIZE <= -0.4) return 0;
	for (ind = 0; ind < text_length && text[ind] != '\0'; ++ind) {
		float diff = (pos - (float)visual_char * CHAR_SIZE) / CHAR_SIZE;
		if (text[ind] == '\t') {
			visual_char += TAB_WIDTH;
			if (diff <= TAB_WIDTH / 2) return ind;
		} else {
			visual_char += 1;
			if (diff <= 0.6) return ind;
		}
	}
	return ind;
}

static inline bool get_frame_render_rect(Ctx *ctx, Uint32 frame, SDL_FRect *bounds) {
	SDL_assert(bounds != NULL);
	SDL_assert(ctx->frames_count >= frame);
	SDL_FRect frame_bounds = ctx->frames[frame].bounds;
	if (!ctx->frames[frame].is_global) {
		frame_bounds.x += ctx->transform.x;
		frame_bounds.y += ctx->transform.y;
	}
	*bounds = (SDL_FRect) {
		.x = frame_bounds.x,
		.y = frame_bounds.y,
		.w = frame_bounds.w,
		.h = frame_bounds.h,
	};
	return true;
}

static inline bool get_frame_render_lines_rect(Ctx *ctx, Uint32 frame, SDL_FRect *bounds) {
	get_frame_render_rect(ctx, frame, bounds);
	bounds->x += CHAR_SIZE * 4;
	bounds->w -= CHAR_SIZE * 4;
	return true;
}

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
	tmp[out++] = '\0';
	SDL_SetRenderDrawColor(ctx->renderer, text_color.r, text_color.g, text_color.b, text_color.a);
	SDL_RenderDebugText(ctx->renderer, SDL_floor(frame.x), SDL_floor(frame.y), tmp);
#ifdef DEBUG_LAYOUT
	SDL_SetRenderDrawColor(ctx->renderer, 0xff, 0x00, 0x00, 0xff);
	SDL_RenderRect(ctx->renderer, &frame);
#endif
	return;
}

static String get_line(Ctx *ctx, size_t text_size, char text[text_size], Uint32 linenum) {
	char *begin = text;
	(void) ctx;
	while (linenum != 0) {
		if (*begin == '\0') return (String){0};
		if (*begin == '\n') linenum -= 1;
		begin += 1;
	}
	text = begin;
	while (*text != '\0' && *text != '\n') text += 1;
	return (String){.text = begin, .size = text - begin};
}

static Uint32 split_into_lines(Ctx *ctx, Uint32 strings_length, String strings[strings_length], char *text, Uint32 line_offset) {
	(void)ctx;
	char *end = text;
	Sint32 line = -line_offset;
	while (*end != '\0') {
		if ((Sint32)strings_length <= line) break;
		while (*end != '\0' && *end != '\n') end++;
		if (line >= 0) {
			strings[line] = (String) {
				.size = end - text,
				.text = text,
			};
		}
		line += 1;
		if (*end == '\0') break;
		end += 1;
		text = end;
	}
	if ((Sint32)strings_length > line && *end != '\0' && line >= 0) {
		strings[line] = (String) {
			.size = end - text,
			.text = text,
		};
		line += 1;
	}
	return (Uint32)line;
}

static void render_frame(Ctx *ctx, Uint32 frame) {
	String lines[0x100];
	Frame *draw_frame = &ctx->frames[frame];
	char *text = draw_frame->buffer->text;
	SDL_FRect bounds, lines_bounds;
	get_frame_render_rect(ctx, frame, &bounds);
	get_frame_render_lines_rect(ctx, frame, &lines_bounds);
	SDL_assert(SDL_arraysize(lines) >= (lines_bounds.h / LINE_HEIGHT));
	SDL_SetRenderDrawColor(ctx->renderer, 0x12, 0x12, 0x12, SDL_ALPHA_OPAQUE);
	SDL_RenderFillRect(ctx->renderer, &(SDL_FRect) {
		bounds.x,
		bounds.y,
		bounds.w,
		bounds.h,
	});
	Uint32 lines_count = split_into_lines(ctx, SDL_arraysize(lines), lines, text, 0);
	for (Uint32 linenum = 0; linenum < lines_count; ++linenum) {
		SDL_FRect line_bounds = {
			.x = lines_bounds.x,
			.y = lines_bounds.y + linenum * LINE_HEIGHT + draw_frame->scroll.y,
			.w = lines_bounds.w,
			.h = LINE_HEIGHT,
		};
		String line = lines[linenum];
		SDL_SetRenderDrawColor(ctx->renderer, text_color.r, text_color.g, text_color.b, text_color.a);
		render_line(ctx, line_bounds, line.text, line.size);
	}
#ifdef DEBUG_FILES
	if (draw_frame->filename) {
		SDL_SetRenderDrawColor(ctx->renderer, 0x20, 0x20, 0x20, SDL_ALPHA_OPAQUE);
		SDL_RenderRect(ctx->renderer, &(SDL_FRect) {
			bounds.x + bounds.w - SDL_strlen(draw_frame->filename) * CHAR_SIZE,
			bounds.y + bounds.h - LINE_HEIGHT,
			SDL_strlen(draw_frame->filename) * CHAR_SIZE,
			LINE_HEIGHT,
		});
		SDL_SetRenderDrawColor(ctx->renderer, text_color.r, text_color.g, text_color.b, text_color.a);
		SDL_RenderDebugText(ctx->renderer,
			bounds.x + bounds.w - SDL_strlen(draw_frame->filename) * CHAR_SIZE,
			bounds.y + bounds.h - LINE_HEIGHT,
			draw_frame->filename);
	}
#endif
	if (ctx->focused_frame == frame) {
		SDL_SetRenderDrawColor(ctx->renderer, 0x08, 0x38, 0x08, SDL_ALPHA_OPAQUE);
	} else {
		SDL_SetRenderDrawColor(ctx->renderer, 0x08, 0x08, 0x08, SDL_ALPHA_OPAQUE);
	}
	SDL_RenderRect(ctx->renderer, &(SDL_FRect) {
		bounds.x,
		bounds.y,
		bounds.w,
		bounds.h,
	});
}

static TextBuffer *allocate_buffer(Ctx *ctx) {
	(void)ctx;
	TextBuffer *buffer;
	buffer = SDL_malloc(sizeof *buffer);
	if (buffer == NULL) {
		SDL_Log("Error, can't allocate buffer");
		return NULL;
	}
	*buffer = (TextBuffer){0};
	return buffer;
}

static Uint32 append_frame(Ctx *ctx, TextBuffer *buffer, SDL_FRect bounds) {
	if (ctx->frames_capacity <= ctx->frames_count) {
		size_t new_cap = ctx->frames_capacity * 2;
		if (new_cap == 0) {
			new_cap = 8;
		}
		Frame *new_frames = SDL_realloc(ctx->frames, new_cap * (sizeof *ctx->frames));
		if (new_frames == NULL) {
			SDL_Log("Can't reallocate frames array");
			return -1;
		}
		Uint32 *new_sorted_frames = SDL_realloc(ctx->frames, new_cap * (sizeof *ctx->sorted_frames));
		if (new_sorted_frames == NULL) {
			SDL_LogWarn(0, "Can't reallocate sorted frames index");
			SDL_realloc(ctx->frames, ctx->frames_count * (sizeof *ctx->frames));
			return -1;
		}
		ctx->frames = new_frames;
		ctx->sorted_frames = new_sorted_frames;
		ctx->frames_capacity = new_cap;
	}
	Uint32 frame_ind = ctx->frames_count++;
	ctx->frames[frame_ind] = (Frame){
		.taken = true,
		.cursor = 0,
		.scroll = 0,
		.bounds = bounds,
		.buffer = buffer,
	};
	ctx->sorted_frames[frame_ind] = frame_ind;
	return frame_ind;
}

static Uint32 find_any_frame(Ctx *ctx) {
	for (Uint32 i = 0; i < ctx->frames_count; ++i) {
		if (ctx->frames[i].taken) return i;
	}
	SDL_LogError(0, "No more frames, creating one");
	TextBuffer *buffer = allocate_buffer(ctx);
	if (buffer == NULL) {
		SDL_LogError(0, "Can't allocate buffer");
		return -1;
	}
	return append_frame(ctx, buffer, (SDL_FRect){0, 0, ctx->win_w, ctx->win_h});
}

static Uint32 create_ask_frame(Ctx *ctx, Ask_Option option, Uint32 parent) {
	TextBuffer *buffer = allocate_buffer(ctx);
	if (buffer == NULL) {
		SDL_Log("Error, can't allocate ask buffer");
		return -1;
	}
	Uint32 frame = append_frame(ctx, buffer, (SDL_FRect){0, ctx->win_h - LINE_HEIGHT, ctx->win_w, LINE_HEIGHT});
	if (frame == (Uint32)-1) {
		SDL_Log("Error, can't append ask buffer");
		return -1;
	}
	ctx->frames[frame].frame_type = Frame_Type_ask;
	ctx->frames[frame].is_global = true;
	ctx->frames[frame].parent_frame = parent;
	ctx->frames[frame].ask_option = option;
	return frame;
}

int main(int argc, char *argv[argc]) {
	(void)argv;
	Ctx ctx = {0};
	SDL_SetAppMetadata("Text editor", "1.0", "c4ll.text-editor");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		SDL_LogError(0, "Couldn't initialize SDL: %s", SDL_GetError());
		return -1;
	}
	TextBuffer *buffer = allocate_buffer(&ctx);
	if (buffer == NULL) {
		SDL_Log("Error, can't allocate first buffer");
		return -1;
	}
	Uint32 main_frame = append_frame(&ctx, buffer, (SDL_FRect){0, 0, 0x300, 0x200});
	if (main_frame == (Uint32)-1) {
		SDL_Log("Error, can't create first frame");
		return -1;
	}
	ctx.keys = NULL;
	ctx.linebar_length = 3;
	ctx.opened_file = NULL;
	Uint64 window_flags =  SDL_WINDOW_RESIZABLE;
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
			Frame *current_frame = &ctx.frames[ctx.focused_frame];
			switch (ev.type) {
				case SDL_EVENT_QUIT: {
					ctx.running = false;
					break;
				}; break;
				case SDL_EVENT_KEY_DOWN: {
					switch (ev.key.scancode) {
						case SDL_SCANCODE_LEFT: {
							ctx.moving_col = false;
							if (current_frame->cursor > 0) current_frame->cursor -= 1;
						}; break;
						case SDL_SCANCODE_RIGHT: {
							ctx.moving_col = false;
							if (current_frame->cursor < current_frame->buffer->text_size) current_frame->cursor += 1;
						}; break;
						case SDL_SCANCODE_BACKSPACE: {
							ctx.moving_col = false;
							if (current_frame->cursor > 0 && current_frame->buffer->text_size > 0) {
								SDL_memmove(current_frame->buffer->text + current_frame->cursor - 1,
									current_frame->buffer->text + current_frame->cursor,
									current_frame->buffer->text_size - current_frame->cursor + 1);
								current_frame->cursor -= 1;
								current_frame->buffer->text_size -= 1;
							}
						}; break;
						case SDL_SCANCODE_ESCAPE: {
							if (current_frame->frame_type == Frame_Type_ask) {
								current_frame->taken = false;
								ctx.focused_frame = find_any_frame(&ctx);
								current_frame = &ctx.frames[ctx.focused_frame];
								break;
							}
						}; break;
						case SDL_SCANCODE_RETURN: {
							ctx.moving_col = false;
							char nl = '\n';
							if (current_frame->frame_type == Frame_Type_ask) {
								if (current_frame->ask_option == Ask_Option_save) {
									ctx.frames[current_frame->parent_frame].filename =
										SDL_strndup(current_frame->buffer->text, current_frame->buffer->text_size);
									current_frame->taken = false;
									ctx.focused_frame = current_frame->parent_frame;
									current_frame = &ctx.frames[ctx.focused_frame];
									SDL_assert_release(0 && "TODO");
								} else if (current_frame->ask_option == Ask_Option_open) {
									Frame *parent_frame = &ctx.frames[current_frame->parent_frame];
									parent_frame->filename =
										SDL_strndup(current_frame->buffer->text, current_frame->buffer->text_size);
									parent_frame->buffer = allocate_buffer(&ctx);
									if (parent_frame->buffer == NULL) {
										SDL_LogError(0, "Can't allocate buffer for this file");
										return -1;
									}
									parent_frame->buffer->text = SDL_LoadFile(parent_frame->filename, &parent_frame->buffer->text_capacity);
									parent_frame->buffer->text_size = parent_frame->buffer->text_capacity;
									current_frame->taken = false;
									ctx.focused_frame = current_frame->parent_frame;
									current_frame = &ctx.frames[ctx.focused_frame];
								} else {
									SDL_LogError(0, ("Unknown ask option: %" SDL_PRIu32), (Uint32)current_frame->ask_option);
								}
								break;
							}
							buffer_insert_text(&ctx, current_frame->buffer, &nl, 1, current_frame->cursor);
							current_frame->cursor += 1;
						}; break;
						case SDL_SCANCODE_TAB: {
							ctx.moving_col = false;
							char nl = '\t';
							buffer_insert_text(&ctx, current_frame->buffer, &nl, 1, current_frame->cursor);
							current_frame->cursor += 1;
						}; break;
						case SDL_SCANCODE_UP: {
							int row = 0;
							if (current_frame->cursor > 0) {
								current_frame->cursor--;
							}
							while (current_frame->buffer->text[current_frame->cursor] != '\n' && current_frame->cursor > 0) {
								if (current_frame->buffer->text[current_frame->cursor] == '\t') row += 4;
								else row++;
								current_frame->cursor--;
							}
							if (ctx.moving_col) row = ctx.last_row;
							else ctx.last_row = row;
							ctx.moving_col = true;
							if (current_frame->cursor == 0) break;
							current_frame->cursor--;
							while (current_frame->buffer->text[current_frame->cursor] != '\n' && current_frame->cursor > 0) {
								current_frame->cursor--;
							}
							if (current_frame->buffer->text[current_frame->cursor] == '\n') current_frame->cursor++;
							while (row > 0 && current_frame->buffer->text[current_frame->cursor] != '\n') {
								if (current_frame->buffer->text[current_frame->cursor] == '\t') row -= TAB_WIDTH;
								else row--;
								current_frame->cursor++;
							}
						}; break;
						case SDL_SCANCODE_DOWN: {
							int row = 0;
							if (current_frame->cursor > 0) {
								current_frame->cursor--;
							}
							while (current_frame->buffer->text[current_frame->cursor] != '\n' && current_frame->cursor > 0) {
								if (current_frame->buffer->text[current_frame->cursor] == '\t') row += 4;
								else row++;
								current_frame->cursor--;
							}
							if (ctx.moving_col) row = ctx.last_row;
							else ctx.last_row = row;
							ctx.moving_col = true;
							current_frame->cursor++;
							while (current_frame->buffer->text[current_frame->cursor] != '\n' && current_frame->cursor < current_frame->buffer->text_size) {
								current_frame->cursor++;
							}
							if (current_frame->cursor == current_frame->buffer->text_size) break;
							current_frame->cursor++;
							while (row > 0 && current_frame->cursor < current_frame->buffer->text_size && current_frame->buffer->text[current_frame->cursor] != '\n') {
								if (current_frame->buffer->text[current_frame->cursor] == '\t') row -= TAB_WIDTH;
								else row--;
								current_frame->cursor++;
							}
						}; break;
						case SDL_SCANCODE_S: {
							if (ctx.keymod & SDL_KMOD_CTRL) {
								Uint32 ask_frame = create_ask_frame(&ctx, Ask_Option_save, ctx.focused_frame);
								if (ask_frame == (Uint32)-1) {
									SDL_Log("Error, can't open ask frame");
									break;
								}
								ctx.focused_frame = ask_frame;
								current_frame = &ctx.frames[ctx.focused_frame];
							}
						}; break;
						case SDL_SCANCODE_B: {
							if (ctx.keymod & SDL_KMOD_ALT) {
								current_frame->bounds.w /= 2;
								SDL_FRect bounds = current_frame->bounds;
								bounds.x += bounds.w;
								Uint32 frame = append_frame(&ctx, current_frame->buffer, bounds);
								if (frame == (Uint32)-1) {
									SDL_Log("Error, can't open new frame");
									break;
								}
								ctx.focused_frame = frame;
								current_frame = &ctx.frames[ctx.focused_frame];
							}
						}; break;
						case SDL_SCANCODE_V: {
							if (ctx.keymod & SDL_KMOD_ALT) {
								current_frame->bounds.h /= 2;
								SDL_FRect bounds = current_frame->bounds;
								bounds.y += bounds.h;
								Uint32 frame = append_frame(&ctx, current_frame->buffer, bounds);
								if (frame == (Uint32)-1) {
									SDL_Log("Error, can't open new frame");
									break;
								}
								ctx.focused_frame = frame;
								current_frame = &ctx.frames[ctx.focused_frame];
							}
						}; break;
						case SDL_SCANCODE_O: {
							if (ctx.keymod & SDL_KMOD_CTRL) {
								Uint32 ask_frame = create_ask_frame(&ctx, Ask_Option_open, ctx.focused_frame);
								if (ask_frame == (Uint32)-1) {
									SDL_Log("Error, can't open ask frame");
									break;
								}
								ctx.focused_frame = ask_frame;
								current_frame = &ctx.frames[ask_frame];
							} else if (ctx.keymod & SDL_KMOD_ALT) {
								for (Uint32 i = ctx.focused_frame + 1; i != ctx.focused_frame; ++i) {
									if (i > ctx.frames_count) i = 0;
									if (!ctx.frames[i].taken) continue;
									ctx.focused_frame = i;
									current_frame = &ctx.frames[ctx.focused_frame];
									break;
								}
							}
						}; break;
						default: {};
					}
				}; break;
				case SDL_EVENT_MOUSE_BUTTON_DOWN: {
					if (ev.button.button == SDL_BUTTON_LEFT) {
						if (ctx.keymod & SDL_KMOD_CTRL) {
						} else if (ctx.keymod & SDL_KMOD_ALT) {
						} else {
							for (Uint32 i = 0; i < ctx.frames_count; ++i) {
								SDL_FRect bounds;
								SDL_FPoint point = {ev.button.x, ev.button.y};
								get_frame_render_rect(&ctx, ctx.sorted_frames[i], &bounds);
								if (SDL_PointInRectFloat(&point, &bounds)) {
									ctx.focused_frame = ctx.sorted_frames[i];
									Uint32 first = ctx.sorted_frames[i];
									if (i != 0)
										SDL_memmove(&ctx.sorted_frames[1], &ctx.sorted_frames[0], i * sizeof (*ctx.sorted_frames));
									ctx.sorted_frames[0] = first;
									SDL_FRect bounds;
									Frame *frame = &ctx.frames[first];
									get_frame_render_lines_rect(&ctx, first, &bounds);
									if (ev.button.y < bounds.y) {
										frame->cursor = 0;
										break;
									}
									Uint32 linenum = (ev.button.y - bounds.y) / LINE_HEIGHT;
									String line = get_line(&ctx, frame->buffer->text_size, frame->buffer->text, (Uint32)linenum);
									if (line.text == NULL) {
										frame->cursor = frame->buffer->text_size;
										break;
									}
									// Don't fucking reallocate sized strings which only point is zero copy.
									SDL_assert(line.text >= frame->buffer->text && line.text <= frame->buffer->text + frame->buffer->text_size);
									frame->cursor = line.text - frame->buffer->text + coords_to_text_index(&ctx, line.size, line.text, ev.button.x - bounds.x);
									break;
								}
							}
						}
					} else if (ev.button.button == SDL_BUTTON_MIDDLE) {
						Uint64 time = SDL_GetTicks();
						if (time - ctx.last_middle_click <= 300) {
							ctx.transform = (SDL_FPoint){0};
						}
						ctx.last_middle_click = time;
					}
				}; break;
				case SDL_EVENT_MOUSE_MOTION: {
					ctx.mouse_pos.x = ev.motion.x;
					ctx.mouse_pos.y = ev.motion.y;
					if (ev.motion.state & SDL_BUTTON_MMASK) {
						ctx.transform.x += ev.motion.xrel;
						ctx.transform.y += ev.motion.yrel;
					} else if (ev.motion.state & SDL_BUTTON_LMASK) {
						if (ctx.keymod & SDL_KMOD_CTRL) {
							current_frame->bounds.x += ev.motion.xrel;
							current_frame->bounds.y += ev.motion.yrel;
						}
					} else if (ev.motion.state & SDL_BUTTON_RMASK) {
						if (ctx.keymod & SDL_KMOD_CTRL) {
							current_frame->bounds.w += ev.motion.xrel;
							current_frame->bounds.h += ev.motion.yrel;
						}
					}
				} break;
				case SDL_EVENT_MOUSE_WHEEL: {
					current_frame->scroll.y += ev.wheel.y * 32;
				} break;
				case SDL_EVENT_TEXT_INPUT: {
					ctx.moving_col = false;
					if (ctx.keymod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) break;
					buffer_insert_text(&ctx, current_frame->buffer, ev.text.text, SDL_strlen(ev.text.text), current_frame->cursor);
					current_frame->cursor += SDL_strlen(ev.text.text);
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
			return -1;
		}
		ctx.keys = SDL_GetKeyboardState(NULL);
		ctx.keymod = SDL_GetModState();
		if (!ctx.running) break;
		SDL_SetRenderDrawColor(ctx.renderer, 0x04, 0x04, 0x04, 0xff);
		SDL_RenderClear(ctx.renderer);
		for (Uint32 i = ctx.frames_count - 1; i != (Uint32)-1; --i) {
			Uint32 sorted_frame = ctx.sorted_frames[i];
			if (!ctx.frames[sorted_frame].taken) continue;
			render_frame(&ctx, sorted_frame);
		}
#ifdef DEBUG_SORT
		for (Uint32 i = 0; i < ctx.frames_count; ++i) {
			SDL_SetRenderDrawColor(ctx.renderer, 0x00, 0xff, 0xff, 0xff);
			SDL_RenderDebugTextFormat(ctx.renderer, 400, LINE_HEIGHT * i, "%" SDL_PRIu32 " %" SDL_PRIu32, i, ctx.sorted_frames[i]);
		}
#endif
		SDL_RenderPresent(ctx.renderer);
	}
#ifdef DEBUG
	// To make valgrind happy.
	SDL_free(ctx.frames[ctx.focused_frame].buffer->text);
#endif
	return 0;
}
