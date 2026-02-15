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

#define lerp(from, to, value) ((from) + ((to) - (from)) * (value))

typedef struct {
	size_t size;
	char *text;
} String;

typedef struct {
	char *name;
	// If <= 0, considered untaken
	Sint32 refcount; // Must be changed by end receive function, not by allocate_buffer
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
	bool scroll_lock;
	Frame_Type frame_type;
	Uint32 parent_frame;
	Ask_Option ask_option;
	char *filename;
	SDL_FRect bounds;
	SDL_FPoint scroll_interp;
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
	bool keys[SDL_SCANCODE_COUNT];
	TextBuffer *log_buffer;
	SDL_Keymod keymod;
	SDL_FPoint mouse_pos;
	Uint64 last_render;
	bool should_render;
	bool running;
	bool moving_col; // When cursor was just moving up and down
	Uint32 buffers_count;
	Uint32 buffers_capacity;
	TextBuffer *buffers;
	Uint32 frames_count;
	Uint32 frames_capacity;
	Frame *frames;
	Uint32 *sorted_frames;
	Uint32 focused_frame;
	int render_rotate_fan;
	SDL_FPoint transform;
	SDL_FRect debug_screen_rect;
	Uint64 last_middle_click;
	SDL_FPoint active_cursor_pos;
} Ctx;

static const SDL_Color text_color = {0xe6, 0xe6, 0xe6, SDL_ALPHA_OPAQUE};
static const SDL_Color line_number_color = {0xe6 / 2, 0xe6 / 2, 0xe6 / 2, SDL_ALPHA_OPAQUE};
static const SDL_Color line_number_dimmed_color = {0xe6 / 4, 0xe6 / 4, 0xe6 / 4, SDL_ALPHA_OPAQUE};
static const SDL_Color background_color = {0x04, 0x04, 0x04, SDL_ALPHA_OPAQUE};
static const SDL_Color background_lines_color = {0x00, 0x30, 0x00, SDL_ALPHA_OPAQUE};
static const SDL_Color debug_red __attribute__((unused)) = {0xff, 0x00, 0x00, SDL_ALPHA_OPAQUE};

static inline SDL_Color hsv_to_rgb(SDL_Color hsv) {
	SDL_Color rgb = {0, 0, 0, hsv.a};
	float h = hsv.r / 255.0f * 360.0f;
	float s = hsv.g / 255.0f;
	float v = hsv.b / 255.0f;
	float r, g, b;
	if (s == 0) {
		r = g = b = v;
	} else {
		float sector = h / 60.0f;
		int i = (int)sector;
		float f = sector - i;
		float p = v * (1 - s);
		float q = v * (1 - s * f);
		float t = v * (1 - s * (1 - f));
		switch (i) {
		case 0:  r = v; g = t; b = p; break;
		case 1:  r = q; g = v; b = p; break;
		case 2:  r = p; g = v; b = t; break;
		case 3:  r = p; g = q; b = v; break;
		case 4:  r = t; g = p; b = v; break;
		default: r = v; g = p; b = q; break;
		}
	}
	rgb.r = (Uint8)(r * 255.0f + 0.5f);
	rgb.g = (Uint8)(g * 255.0f + 0.5f);
	rgb.b = (Uint8)(b * 255.0f + 0.5f);
	return rgb;
}

static inline Uint32 reverse_sorted_index(Ctx *ctx, Uint32 sorted_ind) {
	for (Uint32 i = 0; i < ctx->frames_count; ++i) {
		if (ctx->sorted_frames[i] == sorted_ind) return i;
	}
	SDL_assert(!"Sorted index out of bounds");
	return -1;
}

static inline bool is_space_only(Ctx *ctx, size_t text_length, const char text[text_length]) {
	(void) ctx;
	if (text_length == 0) return true;
	for (size_t i = 0; i < text_length; ++i) {
		if (text[i] == '\0') return true;
		if (text[i] != ' ' && text[i] != '\t' && text[i] != '\n') return false;
	}
	return true;
}

static inline void set_color(Ctx *ctx, SDL_Color color) {
	SDL_SetRenderDrawColor(ctx->renderer, color.r, color.g, color.b, color.a);
}

static inline Uint32 count_lines(Ctx *ctx, size_t text_size, char text[text_size]) {
	Uint32 lines = 1;
	(void) ctx;
	if (text_size == 0) return 0;
	for (Uint32 i = 0; i < text_size; ++i) {
		if (text[i] == '\0') return lines;
		if (text[i] == '\n') lines += 1;
	}
	return lines;
}

static void buffer_insert_text(Ctx *ctx, TextBuffer *buffer, const char *in, size_t in_len, Uint32 pos) {
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
	for (Uint32 i = 0; i < ctx->frames_count; ++i) {
		if (!ctx->frames[i].taken) continue;
		if (ctx->frames[i].buffer == buffer) {
			if (ctx->frames[i].cursor == buffer->text_size - 1) ctx->frames[i].scroll_lock = false;
			if (!ctx->frames[i].scroll_lock) {
				Sint32 text_lines = (Sint32)count_lines(ctx, buffer->text_size, buffer->text);
				Sint32 buffer_last_line = (Sint32)SDL_ceil((ctx->frames[i].bounds.h - ctx->frames[i].scroll.y) / LINE_HEIGHT);
				if (text_lines > buffer_last_line) {
					ctx->frames[i].scroll.y = ctx->frames[i].bounds.h - text_lines * LINE_HEIGHT;
				}
			}
		}
	}
	ctx->should_render = true;
}

static inline void debug_rect(Ctx *ctx, SDL_FRect *rect, SDL_Color color) {
	set_color(ctx, color);
	SDL_RenderRect(ctx->renderer, rect);
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

static inline Uint32 bytes_to_visual(Ctx *ctx, size_t text_length, char text[text_length], Uint32 byte) {
	Uint32 visual_char = 0;
	(void) ctx;
	for (Uint32 ind = 0; ind < byte && ind < text_length; ++ind) {
		if (text[ind] == '\t') {
			visual_char += TAB_WIDTH;
		} else {
			visual_char += 1;
		}
	}
	return visual_char;
}

bool get_frame_render_rect(Ctx *ctx, Uint32 frame, SDL_FRect *bounds) {
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
	bounds->y += SDL_max(0, ctx->frames[frame].scroll.y);
	bounds->h -= SDL_max(0, ctx->frames[frame].scroll.y);
	bounds->h = SDL_max(bounds->h, 0);
	return true;
}

static inline bool get_frame_render_lines_numbers_rect(Ctx *ctx, Uint32 frame, SDL_FRect *bounds) {
	get_frame_render_rect(ctx, frame, bounds);
	bounds->w = CHAR_SIZE * 4;
	bounds->y += SDL_max(0, ctx->frames[frame].scroll.y);
	bounds->h -= SDL_max(0, ctx->frames[frame].scroll.y);
	bounds->h = SDL_max(bounds->h, 0);
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
	if (text_size == 0) return (String){0};
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
	char *last = text - 1;
	Sint32 line = -line_offset;
	if (text == NULL) {
		if (line >= 0) {
			strings[line] = (String) {
				.size = 0,
				.text = text,
			};
			return 1;
		}
		return 0;
	}
	while (*end != '\0') {
		if ((Sint32)strings_length <= line) break;
		while (*end != '\0' && *end != '\n') end++;
		if (line >= 0) {
			strings[line] = (String) {
				.size = end - text,
				.text = text,
			};
		}
		last = end;
		line += 1;
		if (*end == '\0') break;
		end += 1;
		text = end;
	}
	if ((Sint32)strings_length > line && last != end && line >= 0) {
		strings[line] = (String) {
			.size = end - text,
			.text = text,
		};
		line += 1;
	}
	return SDL_max(0, line);
}

static void render_frame(Ctx *ctx, Uint32 frame) {
	String lines[0x100];
	Frame *draw_frame = &ctx->frames[frame];
	char *text = draw_frame->buffer->text;
	SDL_FRect bounds, lines_bounds, lines_numbers_bounds;
	get_frame_render_rect(ctx, frame, &bounds);
	get_frame_render_lines_rect(ctx, frame, &lines_bounds);
	get_frame_render_lines_numbers_rect(ctx, frame, &lines_numbers_bounds);
	SDL_assert(SDL_arraysize(lines) >= (lines_bounds.h / LINE_HEIGHT));
	SDL_SetRenderDrawColor(ctx->renderer, 0x12, 0x12, 0x12, SDL_ALPHA_OPAQUE);
	SDL_RenderFillRect(ctx->renderer, &(SDL_FRect) {
		bounds.x,
		bounds.y,
		bounds.w,
		bounds.h,
	});
	if (SDL_fabs(ctx->frames[frame].scroll_interp.y - ctx->frames[frame].scroll.y) >= 0.01) {
		float speed = 0.3;
		ctx->frames[frame].scroll_interp.y = lerp(ctx->frames[frame].scroll_interp.y, ctx->frames[frame].scroll.y, speed);
		ctx->should_render = true;
	}
	Uint32 lines_count;
	Uint32 line_start = SDL_max(0, SDL_floor(-ctx->frames[frame].scroll_interp.y / LINE_HEIGHT));
	lines_count = split_into_lines(ctx, SDL_arraysize(lines), lines, text, line_start);
	for (Uint32 linenum = 0; linenum < lines_count; ++linenum) {
		SDL_FRect line_bounds = {
			.x = lines_bounds.x,
			.y = lines_bounds.y + linenum * LINE_HEIGHT,
			.w = lines_bounds.w,
			.h = LINE_HEIGHT,
		};
		if (line_bounds.y >= lines_bounds.y + lines_bounds.h) break;
		String line = lines[linenum];
		SDL_SetRenderDrawColor(ctx->renderer, text_color.r, text_color.g, text_color.b, text_color.a);
		render_line(ctx, line_bounds, line.text, line.size);
		if (line.text - text <= draw_frame->cursor &&
			((linenum + 1 >= lines_count) || (lines[linenum + 1].text - text > draw_frame->cursor))) {
			SDL_FPoint actual_cursor_pos = {
				.x = line_bounds.x + bytes_to_visual(ctx, line.size, line.text, draw_frame->cursor - (line.text - text)) * CHAR_SIZE,
				.y = line_bounds.y,
			};
			float speed = 0.4;
			Uint32 width = 2;
			if (ctx->focused_frame == frame &&
				(((SDL_fabs(actual_cursor_pos.x - ctx->active_cursor_pos.x) >= 0.01) ||
				  (SDL_fabs(actual_cursor_pos.y - ctx->active_cursor_pos.y) >= 0.01)))) {
				width = SDL_max(width, SDL_log(SDL_abs(ctx->active_cursor_pos.x - lerp(ctx->active_cursor_pos.x, actual_cursor_pos.x, speed))) * 2);
				ctx->active_cursor_pos.x = lerp(ctx->active_cursor_pos.x, actual_cursor_pos.x, speed);
				ctx->active_cursor_pos.y = lerp(ctx->active_cursor_pos.y, actual_cursor_pos.y, speed);
				ctx->should_render = true;
			}
			if (ctx->focused_frame == frame) {
				SDL_RenderFillRect(ctx->renderer, &(SDL_FRect) {
					.x = ctx->active_cursor_pos.x,
					.y = ctx->active_cursor_pos.y,
					.w = width,
					.h = LINE_HEIGHT,
				});
			} else {
				SDL_RenderRect(ctx->renderer, &(SDL_FRect) {
					.x = actual_cursor_pos.x,
					.y = actual_cursor_pos.y,
					.w = CHAR_SIZE,
					.h = LINE_HEIGHT,
				});
			}
		}
	}
	set_color(ctx, line_number_color);
	for (Uint32 linenum = line_start; (linenum - line_start) * LINE_HEIGHT < lines_numbers_bounds.h; ++linenum) {
#ifdef LINE_NUMS_DIM_SPACE
		if (linenum > lines_count + line_start || lines[linenum].size <= 0) set_color(ctx, line_number_dimmed_color);
		else set_color(ctx, line_number_color);
#else
		if (linenum == lines_count + line_start) set_color(ctx, line_number_dimmed_color);
#endif
		SDL_RenderDebugTextFormat(ctx->renderer, lines_numbers_bounds.x, lines_numbers_bounds.y + (linenum - line_start) * LINE_HEIGHT,
				"%u", linenum);
	}
#ifdef DEBUG_SCROLL
	if (draw_frame->scroll_lock)
		SDL_SetRenderDrawColor(ctx->renderer, 0xcc, 0x20, 0x20, SDL_ALPHA_OPAQUE);
	else
		SDL_SetRenderDrawColor(ctx->renderer, 0x20, 0xcc, 0x20, SDL_ALPHA_OPAQUE);
	SDL_RenderFillRect(ctx->renderer, &(SDL_FRect) {
		bounds.x + bounds.w - 0x10,
		bounds.y,
		0x10, 0x10,
	});
#endif
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

static TextBuffer *allocate_buffer(Ctx *ctx, char *name) {
	(void)ctx;
	if (ctx->buffers_capacity <= ctx->buffers_count) {
		size_t new_cap = ctx->buffers_capacity * 2;
		if (new_cap == 0) {
			new_cap = 8;
		}
		TextBuffer *new_buffers = SDL_realloc(ctx->buffers, new_cap * (sizeof *ctx->buffers));
		if (new_buffers == NULL) {
			SDL_Log("Can't reallocate buffers array");
			return NULL;
		}
		ctx->buffers = new_buffers;
		ctx->buffers_capacity = new_cap;
	}
	TextBuffer *buffer = &ctx->buffers[ctx->buffers_count++];
	*buffer = (TextBuffer){0};
	buffer->name = name;
	return buffer;
}

static void set_focused_frame(Ctx *ctx, Uint32 frame) {
	ctx->focused_frame = frame;
	Uint32 first = frame;
	Uint32 sorted_ind = reverse_sorted_index(ctx, frame);
	if (sorted_ind != 0)
		SDL_memmove(&ctx->sorted_frames[1], &ctx->sorted_frames[0], sorted_ind * sizeof (*ctx->sorted_frames));
	ctx->sorted_frames[0] = first;
}

static bool handle_frame_mouse_click(Ctx *ctx, Uint32 frame, SDL_FPoint point) {
	SDL_FRect bounds;
	Frame *draw_frame = &ctx->frames[frame];
	get_frame_render_lines_rect(ctx, frame, &bounds);
	draw_frame->scroll_lock = true;
	if (point.y < bounds.y) {
		draw_frame->cursor = 0;
		return true;
	}
	Uint32 linenum = (point.y - bounds.y) / LINE_HEIGHT;
	String line = get_line(ctx, draw_frame->buffer->text_size, draw_frame->buffer->text, (Uint32)linenum);
	if (line.text == NULL) {
		draw_frame->cursor = draw_frame->buffer->text_size;
		return true;
	}
	// Don't fucking reallocate sized strings which only point is zero copy.
	SDL_assert(line.text >= draw_frame->buffer->text && line.text <= draw_frame->buffer->text + draw_frame->buffer->text_size);
	draw_frame->cursor = line.text - draw_frame->buffer->text + coords_to_text_index(ctx, line.size, line.text, point.x - bounds.x);
	return true;
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
		Uint32 *new_sorted_frames = SDL_realloc(ctx->sorted_frames, new_cap * (sizeof *ctx->sorted_frames));
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
	buffer->refcount += 1;
	ctx->sorted_frames[frame_ind] = frame_ind;
	return frame_ind;
}

static Uint32 find_any_frame(Ctx *ctx) {
	for (Uint32 i = 0; i < ctx->frames_count; ++i) {
		if (ctx->frames[i].taken) return i;
	}
	SDL_LogError(0, "No more frames, creating one");
	TextBuffer *buffer = allocate_buffer(ctx, "scratch");
	if (buffer == NULL) {
		SDL_LogError(0, "Can't allocate buffer");
		return -1;
	}
	return append_frame(ctx, buffer, (SDL_FRect){0, 0, ctx->win_w, ctx->win_h});
}

static Uint32 create_ask_frame(Ctx *ctx, Ask_Option option, Uint32 parent) {
	TextBuffer *buffer = allocate_buffer(ctx, "ask buffer");
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

static void log_handler(void *userdata, int category, SDL_LogPriority priority, const char *message) {
	(void) category;
	(void) priority;
	Ctx *ctx = (Ctx *)userdata;
	buffer_insert_text(ctx, ctx->log_buffer, message, SDL_strlen(message), ctx->log_buffer->text_size);
	buffer_insert_text(ctx, ctx->log_buffer, "\n", 1, ctx->log_buffer->text_size);
}

static void render_background(Ctx *ctx) {
	set_color(ctx, background_color);
	SDL_RenderClear(ctx->renderer);
	set_color(ctx, background_lines_color);
	for (float x = 0 + (int)ctx->transform.x % 0x40; x < ctx->win_w; x += 0x40) {
		SDL_RenderLine(ctx->renderer, x, 0, x, ctx->win_h);
	}
	for (float y = 0 + (int)ctx->transform.y % 0x40; y < ctx->win_h; y += 0x40) {
		SDL_RenderLine(ctx->renderer, 0, y, ctx->win_w, y);
	}

}

static void render(Ctx *ctx, bool debug_screen) {
	ctx->should_render = false;
	if (debug_screen) {
		SDL_PixelFormat format = SDL_GetWindowPixelFormat(ctx->window);
		SDL_Texture *texture = SDL_CreateTexture(ctx->renderer, format, SDL_TEXTUREACCESS_TARGET, ctx->win_w * 2, ctx->win_h * 2);
		if (texture == NULL) {
			SDL_LogWarn(0, "Can't create texture for debug screen: %s", SDL_GetError());
			goto debug_screen_exit;
		}
		SDL_SetRenderTarget(ctx->renderer, texture);
		SDL_Rect viewport = {
			.x = ctx->win_w * 3/4,
			.y = ctx->win_h * 3/4,
			.w = ctx->win_w,
			.h = ctx->win_h,
		};
		SDL_FRect viewportf;
		SDL_RectToFRect(&viewport, &viewportf);
		ctx->transform.x += viewport.x;
		ctx->transform.y += viewport.y;
		render(ctx, false);
		ctx->transform.x -= viewport.x;
		ctx->transform.y -= viewport.y;
		debug_screen_exit:
		SDL_SetRenderTarget(ctx->renderer, NULL);
		if (texture) {
			SDL_FRect screen_rect_borders = ctx->debug_screen_rect;
			screen_rect_borders.x -= 1;
			screen_rect_borders.y -= 1;
			screen_rect_borders.w += 2;
			screen_rect_borders.h += 2;
			SDL_RenderTexture(ctx->renderer, texture, &viewportf, NULL);
			SDL_SetRenderDrawColor(ctx->renderer, 0xff, 0xff, 0x00, SDL_ALPHA_OPAQUE);
			SDL_RenderRect(ctx->renderer, &screen_rect_borders);
			SDL_RenderTexture(ctx->renderer, texture, NULL, &ctx->debug_screen_rect);
		}
		SDL_RenderPresent(ctx->renderer);
		return;
	}
	render_background(ctx);
	for (Uint32 i = ctx->frames_count - 1; i != (Uint32)-1; --i) {
		Uint32 sorted_frame = ctx->sorted_frames[i];
		if (!ctx->frames[sorted_frame].taken) continue;
		if (ctx->frames[sorted_frame].is_global) continue;
		render_frame(ctx, sorted_frame);
	}
	// First render default frames, then global, so global always on top
	for (Uint32 i = ctx->frames_count - 1; i != (Uint32)-1; --i) {
		Uint32 sorted_frame = ctx->sorted_frames[i];
		if (!ctx->frames[sorted_frame].taken) continue;
		if (!ctx->frames[sorted_frame].is_global) continue;
		render_frame(ctx, sorted_frame);
	}
#ifdef DEBUG_BUFFERS
	for (Uint32 i = 0; i < ctx->buffers_count; ++i) {
		SDL_SetRenderDrawColor(ctx->renderer, 0xff, 0x00, 0xff, 0xff);
		SDL_RenderDebugTextFormat(ctx->renderer, 200, LINE_HEIGHT * i, "%" SDL_PRIu32 " %" SDL_PRIs32 " %s", i, ctx->buffers[i].refcount, ctx->buffers[i].name);
	}
#endif
#ifdef DEBUG_SORT
	for (Uint32 i = 0; i < ctx->frames_count; ++i) {
		SDL_SetRenderDrawColor(ctx->renderer, 0x00, 0xff, 0xff, 0xff);
		SDL_RenderDebugTextFormat(ctx->renderer, 400, LINE_HEIGHT * i, "%" SDL_PRIu32 " %" SDL_PRIu32, i, ctx->sorted_frames[i]);
	}
#endif
	SDL_SetRenderDrawColor(ctx->renderer, 0x22, 0x22, 0x22, 0xff);
	SDL_RenderFillRect(ctx->renderer, &(SDL_FRect) {
		0, 0, 0x10, 0x10,
	});
	SDL_SetRenderDrawColor(ctx->renderer, 0xcc, 0xcc, 0xcc, 0xff);
	SDL_RenderFillRect(ctx->renderer, &(SDL_FRect) {
		(ctx->render_rotate_fan % 2) * 0x10 / 2, (ctx->render_rotate_fan / 2) * 0x10 / 2, 0x10 / 2, 0x10 / 2,
	});
	SDL_RenderPresent(ctx->renderer);
	ctx->last_render = SDL_GetTicks();
	ctx->render_rotate_fan = (ctx->render_rotate_fan + 1) % 4;
}

int main(int argc, char *argv[argc]) {
	(void)argv;
	Ctx ctx = {0};
	ctx.win_w = 0x300;
	ctx.win_h = 0x200;
	ctx.debug_screen_rect = (SDL_FRect){
		.x = ctx.win_w * 3/4,
		.y = ctx.win_h * 1/20,
		.w = ctx.win_w,
		.h = ctx.win_h,
	};
	SDL_SetAppMetadata("Text editor", "1.0", "c4ll.text-editor");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		SDL_LogError(0, "Couldn't initialize SDL: %s", SDL_GetError());
		return -1;
	}
	TextBuffer *buffer = allocate_buffer(&ctx, "scratch");
	if (buffer == NULL) {
		SDL_Log("Error, can't allocate first buffer");
		return -1;
	}
	Uint32 main_frame = append_frame(&ctx, buffer, (SDL_FRect){0, 0, 0x300 / 2, 0x200});
	if (main_frame == (Uint32)-1) {
		SDL_Log("Error, can't create first frame");
		return -1;
	}
	ctx.log_buffer = allocate_buffer(&ctx, "logs");
	if (ctx.log_buffer == NULL) {
		SDL_LogError(0, "Can't create buffer for logs");
		return -1;
	}
	Uint32 log_frame = append_frame(&ctx, ctx.log_buffer, (SDL_FRect){0x300 / 2, 0, 0x300 / 2, 0x200});
	if (log_frame == (Uint32)-1) {
		SDL_LogError(0, "Can't create log frame");
	}
	SDL_SetLogOutputFunction(log_handler, &ctx);
	Uint64 window_flags = SDL_WINDOW_RESIZABLE;
	if (!SDL_CreateWindowAndRenderer("test", ctx.win_w, ctx.win_h, window_flags, &ctx.window, &ctx.renderer)) {
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
#endif
	ctx.running = true;
	ctx.should_render = true;
	while (ctx.running) {
		SDL_Event ev;
		if (ctx.should_render || SDL_WaitEventTimeout(NULL, 150)) {
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
								current_frame->scroll_lock = true;
								if (current_frame->cursor > 0) current_frame->cursor -= 1;
								ctx.debug_screen_rect.x -= 10;
								ctx.should_render = true;
							}; break;
							case SDL_SCANCODE_RIGHT: {
								ctx.moving_col = false;
								current_frame->scroll_lock = true;
								if (current_frame->cursor < current_frame->buffer->text_size) current_frame->cursor += 1;
								ctx.debug_screen_rect.x += 10;
								ctx.should_render = true;
							}; break;
							case SDL_SCANCODE_BACKSPACE: {
								ctx.moving_col = false;
								if (current_frame->cursor > 0 && current_frame->buffer->text_size > 0) {
									SDL_memmove(current_frame->buffer->text + current_frame->cursor - 1,
										current_frame->buffer->text + current_frame->cursor,
										current_frame->buffer->text_size - current_frame->cursor + 1);
									current_frame->cursor -= 1;
									current_frame->buffer->text_size -= 1;
									ctx.should_render = true;
								}
							}; break;
							case SDL_SCANCODE_ESCAPE: {
								if (current_frame->frame_type == Frame_Type_ask) {
									current_frame->taken = false;
									ctx.focused_frame = find_any_frame(&ctx);
									current_frame = &ctx.frames[ctx.focused_frame];
									ctx.should_render = true;
									break;
								}
							}; break;
							case SDL_SCANCODE_RETURN: {
								ctx.moving_col = false;
								char nl = '\n';
								if (current_frame->frame_type == Frame_Type_ask) {
									if (current_frame->ask_option == Ask_Option_save) {
										Frame *parent_frame = &ctx.frames[current_frame->parent_frame];
										parent_frame->filename =
											SDL_strndup(current_frame->buffer->text, current_frame->buffer->text_size);
										current_frame->taken = false;
										ctx.focused_frame = current_frame->parent_frame;
										current_frame = &ctx.frames[ctx.focused_frame];
										SDL_SaveFile(current_frame->filename, current_frame->buffer->text, current_frame->buffer->text_size);
										SDL_LogInfo(0, "Saved buffer into %s", current_frame->filename);
										ctx.should_render = true;
									} else if (current_frame->ask_option == Ask_Option_open) {
										Frame *parent_frame = &ctx.frames[current_frame->parent_frame];
										parent_frame->filename =
											SDL_strndup(current_frame->buffer->text, current_frame->buffer->text_size);
										parent_frame->buffer = allocate_buffer(&ctx, SDL_strdup(parent_frame->filename));
										if (parent_frame->buffer == NULL) {
											SDL_LogError(0, "Can't allocate buffer for this file");
											return -1;
										}
										parent_frame->buffer->text = SDL_LoadFile(parent_frame->filename, &parent_frame->buffer->text_capacity);
										parent_frame->buffer->text_size = parent_frame->buffer->text_capacity;
										current_frame->taken = false;
										ctx.focused_frame = current_frame->parent_frame;
										current_frame = &ctx.frames[ctx.focused_frame];
										ctx.should_render = true;
									} else {
										SDL_LogError(0, ("Unknown ask option: %" SDL_PRIu32), (Uint32)current_frame->ask_option);
									}
									break;
								}
								buffer_insert_text(&ctx, current_frame->buffer, &nl, 1, current_frame->cursor);
								current_frame->cursor += 1;
								ctx.should_render = true;
							}; break;
							case SDL_SCANCODE_TAB: {
								ctx.moving_col = false;
								char nl = '\t';
								buffer_insert_text(&ctx, current_frame->buffer, &nl, 1, current_frame->cursor);
								current_frame->cursor += 1;
								ctx.should_render = true;
							}; break;
							case SDL_SCANCODE_UP: {
								int row = 0;
								current_frame->scroll_lock = true;
								ctx.debug_screen_rect.y -= 10;
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
								ctx.should_render = true;
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
								current_frame->scroll_lock = true;
								ctx.debug_screen_rect.y += 10;
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
								ctx.should_render = true;
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
									if (current_frame->filename == NULL || ctx.keymod & SDL_KMOD_SHIFT) {
										Uint32 ask_frame = create_ask_frame(&ctx, Ask_Option_save, ctx.focused_frame);
										if (ask_frame == (Uint32)-1) {
											SDL_Log("Error, can't open ask frame");
											break;
										}
										ctx.focused_frame = ask_frame;
										current_frame = &ctx.frames[ctx.focused_frame];
										ctx.should_render = true;
									} else {
										SDL_SaveFile(current_frame->filename, current_frame->buffer->text, current_frame->buffer->text_size);
										SDL_LogInfo(0, "Saved buffer into %s", current_frame->filename);
									}
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
									ctx.should_render = true;
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
									ctx.should_render = true;
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
									ctx.should_render = true;
								} else if (ctx.keymod & SDL_KMOD_ALT) {
									for (Uint32 i = ctx.focused_frame + 1; i != ctx.focused_frame; ++i) {
										if (i > ctx.frames_count) i = 0;
										if (!ctx.frames[i].taken) continue;
										ctx.focused_frame = i;
										current_frame = &ctx.frames[ctx.focused_frame];
										ctx.should_render = true;
										break;
									}
								}
							}; break;
							default: {};
						}
						SDL_Scancode scancode = ev.key.scancode;
						if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT) {
							ctx.keys[scancode] = ev.type == SDL_EVENT_KEY_DOWN;
						}
					} break;
					case SDL_EVENT_KEY_UP: {
						SDL_Scancode scancode = ev.key.scancode;
						if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT) {
							ctx.keys[scancode] = ev.type == SDL_EVENT_KEY_DOWN;
						}
					}; break;
					case SDL_EVENT_MOUSE_BUTTON_DOWN: {
						SDL_FPoint point = {ev.button.x, ev.button.y};
						if (ev.button.button == SDL_BUTTON_LEFT) {
							if (ctx.keymod & SDL_KMOD_CTRL) {
							} else if (ctx.keymod & SDL_KMOD_ALT) {
							} else {
								for (Uint32 i = 0; i < ctx.frames_count; ++i) {
									Uint32 sframei = ctx.sorted_frames[i];
									if (ctx.frames[sframei].is_global) continue;
									SDL_FRect bounds;
									get_frame_render_rect(&ctx, sframei, &bounds);
									if (SDL_PointInRectFloat(&point, &bounds)) {
										set_focused_frame(&ctx, sframei);
										handle_frame_mouse_click(&ctx, sframei, point);
										goto end;
									}
								}
								for (Uint32 i = 0; i < ctx.frames_count; ++i) {
									Uint32 sframei = ctx.sorted_frames[i];
									if (!ctx.frames[sframei].is_global) continue;
									SDL_FRect bounds;
									get_frame_render_rect(&ctx, sframei, &bounds);
									if (SDL_PointInRectFloat(&point, &bounds)) {
										set_focused_frame(&ctx, sframei);
										handle_frame_mouse_click(&ctx, sframei, point);
										goto end;
									}
								}
								end:
								ctx.should_render = true;
								break;
							}
						} else if (ev.button.button == SDL_BUTTON_MIDDLE) {
							Uint64 time = SDL_GetTicks();
							if (time - ctx.last_middle_click <= 300) {
								ctx.transform = (SDL_FPoint){0};
							}
							ctx.last_middle_click = time;
							ctx.should_render = true;
						}
					}; break;
					case SDL_EVENT_MOUSE_MOTION: {
						ctx.mouse_pos.x = ev.motion.x;
						ctx.mouse_pos.y = ev.motion.y;
						if (ev.motion.state & SDL_BUTTON_MMASK) {
							ctx.transform.x += ev.motion.xrel;
							ctx.transform.y += ev.motion.yrel;
							ctx.should_render = true;
						} else if (ev.motion.state & SDL_BUTTON_LMASK) {
							if (ctx.keymod & SDL_KMOD_CTRL) {
								current_frame->bounds.x += ev.motion.xrel;
								current_frame->bounds.y += ev.motion.yrel;
								ctx.should_render = true;
							}
						} else if (ev.motion.state & SDL_BUTTON_RMASK) {
							if (ctx.keymod & SDL_KMOD_CTRL) {
								current_frame->bounds.w += ev.motion.xrel;
								current_frame->bounds.h += ev.motion.yrel;
								ctx.should_render = true;
							}
						}
					} break;
					case SDL_EVENT_MOUSE_WHEEL: {
						current_frame->scroll.y += ev.wheel.y * 32;
						current_frame->scroll_lock = true;
						SDL_LogTrace(0, "Scroll %d to %f", ctx.focused_frame, current_frame->scroll.y);
						ctx.should_render = true;
					} break;
					case SDL_EVENT_TEXT_INPUT: {
						ctx.moving_col = false;
						if (ctx.keymod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) break;
						buffer_insert_text(&ctx, current_frame->buffer, ev.text.text, SDL_strlen(ev.text.text), current_frame->cursor);
						current_frame->cursor += SDL_strlen(ev.text.text);
						ctx.should_render = true;
					}; break;
					case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
						ctx.win_w = ev.window.data1;
						ctx.win_h = ev.window.data2;
						SDL_LogDebug(0, "Window resized to %ux%u", ctx.win_w, ctx.win_h);
						ctx.should_render = 1;
					} break;
					case SDL_EVENT_WINDOW_EXPOSED: {
						SDL_LogTrace(0, "Window exposed");
						ctx.should_render = 1;
					} break;
				}
			}
		}
		if (!SDL_TextInputActive(ctx.window)) {
			SDL_StartTextInput(ctx.window);
		}
		ctx.keymod = SDL_GetModState();
		if (!ctx.running) break;
		// if (SDL_GetTicks() - ctx.last_render >= 1000) ctx.should_render = true;
		if (ctx.should_render) {
			render(&ctx, false);
		}
	}
#ifdef DEBUG
	// To make valgrind happy.
	SDL_free(ctx.frames[ctx.focused_frame].buffer->text);
#endif
	return 0;
}
