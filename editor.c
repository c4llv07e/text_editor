/*
	Each frame stores non-unique pointer for buffers.
	Each buffer stores unique pointer for text. For each file there should be only one buffer.
	There's a global list of frames inside ctx.
	Althrough ctx stores dynamic array of frames, frames shouldn't be moved or resized into smaller size.
	"Ask buffer" (aka emacs minibuffer) should be the frame too to work with keybings.
	Frame type should work as a smaill and controllable polymorphism, i.e. cast the meaning of the buffer,
		not the "class".
	Font is always monospace, because with non-monospaced font: (sorry, acme)
		- converting coordinates to index is hard,
		- computing layout is slower,
		- second aligment after indent doesn't exists,
		- rectangular selection doesn't work.
*/

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#define TEXT_CHUNK_SIZE 256
#define TAB_WIDTH 8
#define UNDO_RING_SIZE 100

#define lerp(from, to, value) ((from) + ((to) - (from)) * (value))

typedef struct {
	size_t size;
	char *text;
} String;

typedef enum {
	Undo_Type_none = 0, // We probably should panic here
	Undo_Type_insert,
	Undo_Type_delete,
} Undo_Type;

typedef enum {
	Undo_Group_none = 0,
	Undo_Group_keyboard,
	Undo_Group_clipboard,
} Undo_Group;

typedef struct Undo_Operation {
	Undo_Type type;
	Undo_Group group;
	Uint32 pos;
	Uint32 len;
	char *data;
} Undo_Operation;

typedef struct {
	char *name;
	// If < 0, considered untaken
	Sint32 refcount; // Must be changed by end receive function, not by allocate_buffer
	size_t text_size;
	size_t text_capacity;
	size_t undos_size;
	size_t undos_cursor; // Stores position to check if redo is possible
	Undo_Operation undos[UNDO_RING_SIZE];
	char *text;
} TextBuffer;

typedef enum {
	Frame_Type_memory = 0, // The most safe one
	Frame_Type_file,
	Frame_Type_ask,
	Frame_Type_search,
} Frame_Type;

typedef enum {
	Search_Status_not_found = 0,
	Search_Status_found,
} Search_Status;

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
	bool searching_mode;
	Uint32 search_frame;
	Uint32 search_cursor;
	Search_Status search_status;
	Ask_Option ask_option;
	char *filename;
	char *line_prefix;
	SDL_FRect bounds_interp;
	SDL_FRect bounds;
	SDL_FPoint scroll_interp;
	SDL_FPoint scroll;
	// I want edit one files in multiple frames, so cursor is needed here
	Uint32 cursor;
	Uint32 selection;
	bool active_selection;
	TextBuffer *buffer;
} Frame;

typedef struct Ctx {
	SDL_Renderer *renderer;
	SDL_Window *window;
	TTF_Font *font;
	float font_size;
	float font_width;
	float line_height;
	Uint32 last_row;
	SDL_Texture *space_texture;
	SDL_Texture *tab_texture;
	SDL_Texture *overflow_cursor_texture;
	int win_w, win_h;
	bool keys[SDL_SCANCODE_COUNT];
	TextBuffer *log_buffer;
	SDL_Keymod keymod;
	SDL_FPoint mouse_pos;
	double deltatime;
	double perf_freq;
	Uint64 last_render;
	bool should_render;
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
static const SDL_Color prefix_color = {0x86, 0xf6, 0x86, SDL_ALPHA_OPAQUE};
static const SDL_Color selection_rect_color = {0xe6 / 3, 0xe6 / 2, 0xe6 / 3, SDL_ALPHA_OPAQUE};
static const SDL_Color selection_color = {0xe6 / 3 / 2, 0xe6 / 2 / 2, 0xe6 / 3 / 2, SDL_ALPHA_OPAQUE / 2};
static const SDL_Color line_number_color = {0xe6 / 2, 0xe6 / 2, 0xe6 / 2, SDL_ALPHA_OPAQUE};
static const SDL_Color line_number_dimmed_color = {0xe6 / 4, 0xe6 / 4, 0xe6 / 4, SDL_ALPHA_OPAQUE};
static const SDL_Color search_background_color = {0x63, 0x63, 0x24, SDL_ALPHA_OPAQUE};
static const SDL_Color background_color = {0x04, 0x04, 0x04, SDL_ALPHA_OPAQUE};
static const SDL_Color background_color_error = {0x63, 0x24, 0x24, SDL_ALPHA_OPAQUE};
static const SDL_Color background_lines_color = {0x00, 0x30, 0x00, SDL_ALPHA_OPAQUE};

static const SDL_Color debug_red __attribute__((unused)) = {0xff, 0x00, 0x00, SDL_ALPHA_OPAQUE};
static const SDL_Color debug_yellow __attribute__((unused)) = {0xff, 0xff, 0x00, SDL_ALPHA_OPAQUE};
static const SDL_Color debug_green __attribute__((unused)) = {0x00, 0xff, 0x00, SDL_ALPHA_OPAQUE};
static const SDL_Color debug_blue __attribute__((unused)) = {0x00, 0x00, 0xff, SDL_ALPHA_OPAQUE};
static const SDL_Color debug_black __attribute__((unused)) = {0x00, 0x00, 0x00, SDL_ALPHA_OPAQUE};

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

static inline bool frame_has_line_numbers(Ctx *ctx, Uint32 frame) {
	return (ctx->frames[frame].frame_type == Frame_Type_memory
		|| ctx->frames[frame].frame_type == Frame_Type_file);
}

static inline bool frame_is_multiline(Ctx *ctx, Uint32 frame) {
	return (ctx->frames[frame].frame_type == Frame_Type_memory
		|| ctx->frames[frame].frame_type == Frame_Type_file);
}

static inline Uint32 reverse_sorted_index(Ctx *ctx, Uint32 sorted_ind) {
	for (Uint32 i = 0; i < ctx->frames_count; ++i) {
		if (ctx->sorted_frames[i] == sorted_ind) return i;
	}
	SDL_assert(!"Sorted index out of bounds");
	return -1;
}

static void undo_clear_after_cursor(Ctx *ctx, Uint32 buffer) {
	SDL_assert(ctx->buffers[buffer].refcount > 0);
	TextBuffer *buf = &ctx->buffers[buffer];
	for (Uint32 i = buf->undos_cursor; i < buf->undos_size; ++i) {
		SDL_free(buf->undos[i].data);
	}
	buf->undos_size = buf->undos_cursor;
}

// I will kill myself if one of the ops data won't be allocated with SDL_malloc
static void push_undo_op(Ctx *ctx, Uint32 buffer, Undo_Operation op) {
	SDL_assert(ctx->buffers[buffer].refcount > 0);
	TextBuffer *buf = &ctx->buffers[buffer];
	if (buf->undos_cursor > 0) {
		Undo_Operation *prev_op = &buf->undos[buf->undos_cursor - 1];
		if (op.type == Undo_Type_insert) {
			if (op.type == prev_op->type && op.pos == (prev_op->pos + prev_op->len)) {
				if ((op.data[0] == '\n') == (prev_op->data[prev_op->len - 1] == '\n')) {
					undo_clear_after_cursor(ctx, buffer);
					prev_op->data = SDL_realloc(prev_op->data, prev_op->len + op.len);
					SDL_assert(prev_op->data != NULL);
					SDL_memcpy(prev_op->data + prev_op->len, op.data, op.len);
					SDL_free(op.data);
					prev_op->len += op.len;
					return;
				}
			}
		} else if (op.type == Undo_Type_delete) {
			if (op.type == prev_op->type && (op.pos + op.len) == prev_op->pos) {
				undo_clear_after_cursor(ctx, buffer);
				op.data = SDL_realloc(op.data, op.len + prev_op->len);
				SDL_assert(op.data != NULL);
				SDL_memcpy(op.data + op.len, prev_op->data, prev_op->len);
				SDL_free(prev_op->data);
				prev_op->data = op.data;
				prev_op->pos = op.pos;
				prev_op->len += op.len;
				return;
			}
		}
	}
	if (buf->undos_cursor >= UNDO_RING_SIZE) {
		SDL_free(buf->undos[0].data);
		SDL_memmove(buf->undos, buf->undos + 1, (UNDO_RING_SIZE - 1) * sizeof *buf->undos);
		buf->undos_cursor -= 1;
	}
	// [][]
	// SIZE = 2
	// | 0
	//   | 1
	//     | 2
	buf->undos[buf->undos_cursor] = op;
	buf->undos_cursor += 1;
	undo_clear_after_cursor(ctx, buffer);
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

static void buffer_delete_text_no_undo(Ctx *ctx, Uint32 bufid, Uint32 from, Uint32 to) {
	TextBuffer *buffer = &ctx->buffers[bufid];
	SDL_assert(buffer->refcount > 0);
	SDL_assert(to >= from);
	SDL_memmove(buffer->text + from, buffer->text + to,
		buffer->text_size - to + 1);
	for (Uint32 i = 0; i < ctx->frames_count; ++i) {
		if (!ctx->frames[i].taken) continue;
		if (ctx->frames[i].buffer != buffer) continue;
		if (ctx->frames[i].cursor >= to) ctx->frames[i].cursor -= to - from;
		if (ctx->frames[i].selection >= to) ctx->frames[i].selection -= to - from;
	}
	buffer->text_size -= to - from;
	ctx->should_render = true;
}

static void buffer_delete_text(Ctx *ctx, Uint32 bufid, Uint32 from, Uint32 to, Undo_Group undo_group) {
	TextBuffer *buffer = &ctx->buffers[bufid];
	SDL_assert(buffer->refcount > 0);
	SDL_assert(to >= from);
	char *data = SDL_strndup(buffer->text + from, to - from);
	buffer_delete_text_no_undo(ctx, bufid, from, to);
	push_undo_op(ctx, bufid, (Undo_Operation) {
		.type = Undo_Type_delete,
		.group = undo_group,
		.pos = from,
		.len = to - from,
		.data = data,
	});
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

static void buffer_insert_text_no_undo(Ctx *ctx, TextBuffer *buffer, const char *in, size_t in_len, Uint32 pos) {
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
			if (ctx->frames[i].cursor >= pos) ctx->frames[i].cursor += in_len;
			if (ctx->frames[i].selection >= pos) ctx->frames[i].selection += in_len;
			if (frame_is_multiline(ctx, i)) {
				if (!ctx->frames[i].scroll_lock) {
					Sint32 text_lines = (Sint32)count_lines(ctx, buffer->text_size, buffer->text);
					Sint32 buffer_last_line = (Sint32)SDL_ceil((ctx->frames[i].bounds.h - ctx->frames[i].scroll.y) / ctx->line_height);
					if (text_lines >= buffer_last_line) {
						ctx->frames[i].scroll.y = ctx->frames[i].bounds.h - (text_lines + 5.0) * ctx->line_height;
					}
				}
			}
		}
	}
	ctx->should_render = true;
}

static void buffer_insert_text(Ctx *ctx, TextBuffer *buffer, const char *in, size_t in_len, Uint32 pos, Undo_Group undo_group) {
	buffer_insert_text_no_undo(ctx, buffer, in, in_len, pos);
	push_undo_op(ctx, (buffer - &ctx->buffers[0]) / sizeof *buffer, (Undo_Operation) {
		.type = Undo_Type_insert,
		.group = undo_group,
		.pos = pos,
		.len = in_len,
		.data = SDL_strndup(in, in_len),
	});
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
	if (pos / ctx->font_width <= -0.4) return 0;
	for (ind = 0; ind < text_length && text[ind] != '\0'; ++ind) {
		float diff = (pos - (float)visual_char * ctx->font_width) / ctx->font_width;
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

static inline Uint32 string_to_visual(Ctx *ctx, size_t text_length, const char text[text_length]) {
	(void) ctx;
	Uint32 visual_char = 0;
	if (text_length == 0) return 0;
	while (true) {
		Uint32 ch = SDL_StepUTF8(&text, &text_length);
		if (ch == '\0') break;
		if (ch == '\t') {
			visual_char += 8;
		} else {
			visual_char += 1;
		}
	}
	return visual_char;
}

static inline void frame_scroll_to_line(Ctx *ctx, Uint32 frame, Sint32 line) {
	ctx->frames[frame].scroll.y = -line * ctx->line_height;
}

static inline void frame_scroll_to_line_centered(Ctx *ctx, Uint32 frame, Sint32 line) {
	line -= ctx->frames[frame].bounds.h / ctx->line_height / 2;
	ctx->frames[frame].scroll.y = -line * ctx->line_height;
}

static void update_search(Ctx *ctx, Uint32 search_frame) {
	SDL_assert(ctx->frames[search_frame].taken);
	Uint32 parent_frame = ctx->frames[search_frame].parent_frame;
	SDL_assert(ctx->frames[parent_frame].taken);
	if (ctx->frames[search_frame].buffer->text_size == 0) {
		ctx->frames[search_frame].search_status = Search_Status_not_found;
		ctx->should_render = true;
		return;
	}
	char *first_found_ptr = SDL_strnstr(ctx->frames[parent_frame].buffer->text + ctx->frames[parent_frame].cursor,
		ctx->frames[search_frame].buffer->text,
		ctx->frames[parent_frame].buffer->text_size - ctx->frames[parent_frame].cursor);
	if (first_found_ptr == NULL) {
		ctx->frames[search_frame].search_status = Search_Status_not_found;
		ctx->should_render = true;
		return;
	}
	ctx->frames[search_frame].search_status = Search_Status_found;
	ctx->frames[parent_frame].search_cursor = first_found_ptr - ctx->frames[parent_frame].buffer->text;
	Uint32 line = count_lines(ctx, ctx->frames[parent_frame].search_cursor, ctx->frames[parent_frame].buffer->text);
	frame_scroll_to_line_centered(ctx, parent_frame, line);
	ctx->should_render = true;
}

static bool get_frame_render_rect(Ctx *ctx, Uint32 frame, SDL_FRect *bounds) {
	SDL_assert(bounds != NULL);
	SDL_assert(ctx->frames_count >= frame);
	SDL_FRect frame_bounds = ctx->frames[frame].bounds_interp;
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

static inline bool get_frame_line_prefix_rect(Ctx *ctx, Uint32 frame, SDL_FRect *bounds) {
	get_frame_render_rect(ctx, frame, bounds);
	bounds->w = 0;
	if (ctx->frames[frame].line_prefix != NULL) {
		float prefix_length = (SDL_utf8strlen(ctx->frames[frame].line_prefix)) * ctx->font_width;
		bounds->w = prefix_length;
	}
	return true;
}

static inline bool get_frame_render_text_rect(Ctx *ctx, Uint32 frame, SDL_FRect *bounds) {
	get_frame_render_rect(ctx, frame, bounds);
	if (frame_has_line_numbers(ctx, frame)) {
		bounds->x += ctx->font_width * 4;
		bounds->w -= ctx->font_width * 4;
		bounds->y += SDL_max(0, ctx->frames[frame].scroll_interp.y);
		bounds->h -= SDL_max(0, ctx->frames[frame].scroll_interp.y);
		bounds->h = SDL_max(bounds->h, 0);
	}
	if (ctx->frames[frame].line_prefix != NULL) {
		float prefix_length = (SDL_utf8strlen(ctx->frames[frame].line_prefix)) * ctx->font_width;
		bounds->x += prefix_length;
		bounds->w -= prefix_length;
	}
	return true;
}

static inline bool get_frame_render_lines_numbers_rect(Ctx *ctx, Uint32 frame, SDL_FRect *bounds) {
	get_frame_render_rect(ctx, frame, bounds);
	bounds->w = ctx->font_width * 4;
	bounds->y += SDL_max(0, ctx->frames[frame].scroll_interp.y);
	bounds->h -= SDL_max(0, ctx->frames[frame].scroll_interp.y);
	bounds->h = SDL_max(bounds->h, 0);
	return true;
}

static inline int draw_text(Ctx *ctx, float x, float y, SDL_Color color, size_t text_length, const char text[text_length]) {
	if (text == NULL) return 0;
	SDL_Surface *surface = TTF_RenderText_Blended(ctx->font, text, text_length, color);
	if (surface == NULL) {
		SDL_LogWarn(0, "Can't render text |%.*s|: %s", (int)text_length, text, SDL_GetError());
		return 0;
	}
	SDL_assert(surface != NULL);
	SDL_Texture *texture = SDL_CreateTextureFromSurface(ctx->renderer, surface);
	SDL_assert(texture != NULL);
	SDL_DestroySurface(surface);
	SDL_RenderTexture(ctx->renderer, texture, NULL, &(SDL_FRect) {
		.x = SDL_floor(x),
		.y = SDL_floor(y),
		.w = texture->w,
		.h = texture->h,
	});
	int res = texture->w;
	SDL_DestroyTexture(texture);
	return res;
}

static inline int draw_text_fmt(Ctx *ctx, float x, float y, SDL_Color color, SDL_PRINTF_FORMAT_STRING const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(5);
static inline int draw_text_fmt(Ctx *ctx, float x, float y, SDL_Color color, SDL_PRINTF_FORMAT_STRING const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	if (SDL_strcmp(fmt, "%s") == 0) {
		const char *str = va_arg(ap, const char *);
		va_end(ap);
		return draw_text(ctx, x, y, color, 0, str);
	}
	char *str = NULL;
	const int rc = SDL_vasprintf(&str, fmt, ap);
	va_end(ap);
	if (rc == -1) {
		SDL_LogWarn(0, "Can't allocate with vasprintf format |%s|", fmt);
		return false;
	}
	int ret = draw_text(ctx, x, y, color, rc, str);
	SDL_free(str);
	return ret;
}

static void render_line(Ctx *ctx, SDL_FRect frame, size_t text_size, const char text[text_size]) {
	size_t accum = 0;
	if (text_size == 0) return;
	while (text_size > 0 && accum < text_size) {
		if (SDL_floor(frame.w / ctx->font_width) <= 0) break;
		if (text[accum] == '\t') {
			if (accum != 0) {
				int offset = draw_text(ctx, frame.x, frame.y, text_color, SDL_min(accum, frame.w / ctx->font_width), text);
				frame.x += offset;
				frame.w -= offset;
			}
			SDL_RenderTexture(ctx->renderer, ctx->tab_texture, NULL, &(SDL_FRect) {
				.x = frame.x,
				.y = frame.y,
				.w = ctx->font_width * TAB_WIDTH,
				.h = ctx->font_size,
			});
			frame.x += ctx->font_width * TAB_WIDTH;
			frame.w -= ctx->font_width * TAB_WIDTH;
			text += accum + 1;
			text_size -= accum + 1;
			accum = 0;
		} else if (text[accum] == ' ') {
			if (accum != 0) {
				int offset = draw_text(ctx, frame.x, frame.y, text_color, SDL_min(accum, frame.w / ctx->font_width), text);
				frame.x += offset;
				frame.w -= offset;
			}
			SDL_RenderTexture(ctx->renderer, ctx->space_texture, NULL, &(SDL_FRect) {
				.x = frame.x,
				.y = frame.y,
				.w = ctx->font_width,
				.h = ctx->font_size,
			});
			frame.x += ctx->font_width;
			frame.w -= ctx->font_width;
			text += accum + 1;
			text_size -= accum + 1;
			accum = 0;
		} else {
			accum += 1;
		}
	}
	if (accum != 0 && frame.w > 0) {
		draw_text(ctx, frame.x, frame.y, text_color, SDL_min(accum, frame.w / ctx->font_width), text);
	}
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
	char *end = text;
	char *last = text - 1;
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

static void frame_beggining_line(Ctx *ctx, Uint32 frame) {
	Uint32 cp;
	Frame *current_frame = &ctx->frames[frame];
	ctx->moving_col = false;
	current_frame->scroll_lock = true;
	const char *cur = current_frame->buffer->text + current_frame->cursor;
	if (current_frame->buffer->text_size == 0) return;
	do {
		cp = SDL_StepBackUTF8(current_frame->buffer->text, &cur);
	} while (cp != 0 && cp != '\n');
	if (cp == '\n') SDL_StepUTF8(&cur, 0);
	current_frame->cursor = cur - current_frame->buffer->text;
	ctx->should_render = true;
}

static void frame_beggining_spaced_line(Ctx *ctx, Uint32 frame) {
	Uint32 cp;
	Frame *current_frame = &ctx->frames[frame];
	ctx->moving_col = false;
	current_frame->scroll_lock = true;
	const char *cur = current_frame->buffer->text + current_frame->cursor;
	if (current_frame->buffer->text_size == 0) return;
	do {
		cp = SDL_StepBackUTF8(current_frame->buffer->text, &cur);
	} while (cp != 0 && cp != '\n');
	if (cp == '\n') {
		cp = SDL_StepUTF8(&cur, 0);
	}
	do {
		cp = SDL_StepUTF8(&cur, 0);
	} while (cp == ' ' || cp == '\t');
	if (cp != 0) SDL_StepBackUTF8(current_frame->buffer->text, &cur);
	current_frame->cursor = cur - current_frame->buffer->text;
	ctx->should_render = true;
}

static void frame_end_line(Ctx *ctx, Uint32 frame) {
	Uint32 cp;
	Frame *current_frame = &ctx->frames[frame];
	ctx->moving_col = false;
	current_frame->scroll_lock = true;
	const char *cur = current_frame->buffer->text + current_frame->cursor;
	if (current_frame->buffer->text_size == 0) return;
	do {
		cp = SDL_StepUTF8(&cur, 0);
	} while (cp != 0 && cp != '\n');
	if (cp == '\n') cp = SDL_StepBackUTF8(current_frame->buffer->text, &cur);
	current_frame->cursor = cur - current_frame->buffer->text;
	ctx->should_render = true;
}

static void frame_previous_char(Ctx *ctx, Uint32 frame) {
	Frame *current_frame = &ctx->frames[frame];
	ctx->moving_col = false;
	current_frame->scroll_lock = true;
	if (current_frame->buffer->text_size == 0) return;
	const char *text = &current_frame->buffer->text[current_frame->cursor];
	SDL_StepBackUTF8(current_frame->buffer->text, &text);
	current_frame->cursor = text - current_frame->buffer->text;
	ctx->should_render = true;
}

static void frame_next_char(Ctx *ctx, Uint32 frame) {
	Frame *current_frame = &ctx->frames[frame];
	ctx->moving_col = false;
	current_frame->scroll_lock = true;
	if (current_frame->buffer->text_size == 0) return;
	const char *text = &current_frame->buffer->text[current_frame->cursor];
	size_t len = current_frame->buffer->text_size - current_frame->cursor;
	SDL_StepUTF8(&text, &len);
	current_frame->cursor = text - current_frame->buffer->text;
	ctx->should_render = true;
}

static void frame_previous_line(Ctx *ctx, Uint32 frame) {
	Frame *current_frame = &ctx->frames[frame];
	int row = 0;
	current_frame->scroll_lock = true;
	if (current_frame->buffer->text_size == 0) return;
	SDL_FRect bounds;
	const char *cur = current_frame->buffer->text + current_frame->cursor;
	Uint32 cp = -1;
	while (true) {
		cp = SDL_StepBackUTF8(current_frame->buffer->text, &cur);
		if (cp == '\n' || cp == 0) break;
		if (cp == '\t') row += TAB_WIDTH;
		else row += 1;
	}
	if (cp == '\0') goto update_cursor;
	do {
		cp = SDL_StepBackUTF8(current_frame->buffer->text, &cur);
	} while (cp != '\n' && cp != '\0');
	if (cp == '\n') cp = SDL_StepUTF8(&cur, 0);
	if (ctx->moving_col) row = ctx->last_row;
	else ctx->last_row = row;
	ctx->moving_col = true;
	ctx->should_render = true;
	while (row > 0) {
		cp = SDL_StepUTF8(&cur, 0);
		if (cp == '\n') {
			cp = SDL_StepBackUTF8(current_frame->buffer->text, &cur);
			break;
		}
		if (cp == '\t') row -= TAB_WIDTH;
		else row -= 1;
	};
update_cursor:
	current_frame->cursor = cur - current_frame->buffer->text;
	get_frame_render_text_rect(ctx, frame, &bounds);
	Uint32 line_start = SDL_floor((-current_frame->scroll.y) / ctx->line_height);
	String start_line = get_line(ctx, current_frame->buffer->text_size, current_frame->buffer->text, line_start);
	if (start_line.text != NULL && start_line.text - current_frame->buffer->text > current_frame->cursor) {
		current_frame->scroll.y = -((line_start * ctx->line_height) - bounds.h);
	}
}

static void frame_next_line(Ctx *ctx, Uint32 frame) {
	Frame *current_frame = &ctx->frames[frame];
	int row = 0;
	current_frame->scroll_lock = true;
	if (current_frame->buffer->text_size == 0) return;
	SDL_FRect bounds;
	const char *cur = current_frame->buffer->text + current_frame->cursor;
	Uint32 cp = -1;
	while (true) {
		cp = SDL_StepBackUTF8(current_frame->buffer->text, &cur);
		if (cp == '\n' || cp == 0) break;
		if (cp == '\t') row += TAB_WIDTH;
		else row += 1;
	}
	if (cp == '\n') cp = SDL_StepUTF8(&cur, 0);
	while (true) {
		cp = SDL_StepUTF8(&cur, 0);
		if (cp == '\n' || cp == 0) break;
	}
	if (ctx->moving_col) row = ctx->last_row;
	else ctx->last_row = row;
	ctx->moving_col = true;
	ctx->should_render = true;
	while (row > 0) {
		cp = SDL_StepUTF8(&cur, 0);
		if (cp == '\n') {
			cp = SDL_StepBackUTF8(current_frame->buffer->text, &cur);
			break;
		}
		if (cp == '\t') row -= TAB_WIDTH;
		else row -= 1;
	};
	current_frame->cursor = cur - current_frame->buffer->text;
	get_frame_render_text_rect(ctx, frame, &bounds);
	Sint32 line_end = SDL_floor((bounds.h - SDL_min(0, current_frame->scroll.y)) / ctx->line_height);
	if (line_end < 0) line_end = 0;
	String end_line = get_line(ctx, current_frame->buffer->text_size, current_frame->buffer->text, line_end);
	if (end_line.text != NULL && end_line.text - current_frame->buffer->text < current_frame->cursor) {
		current_frame->scroll.y = -(line_end * ctx->line_height);
	}
}

static void render_frame(Ctx *ctx, Uint32 frame) {
	String lines[0x100];
	Frame *draw_frame = &ctx->frames[frame];
	char *text = draw_frame->buffer->text;
	SDL_FRect bounds, lines_bounds, lines_numbers_bounds;
	get_frame_render_rect(ctx, frame, &bounds);
	get_frame_render_text_rect(ctx, frame, &lines_bounds);
	get_frame_render_lines_numbers_rect(ctx, frame, &lines_numbers_bounds);
	SDL_assert(SDL_arraysize(lines) >= (lines_bounds.h / ctx->line_height));
	if (draw_frame->frame_type == Frame_Type_search) {
		if (draw_frame->search_status == Search_Status_not_found) {
			set_color(ctx, background_color_error);
		} else {
			SDL_SetRenderDrawColor(ctx->renderer, 0x12, 0x12, 0x12, SDL_ALPHA_OPAQUE);
		}
	} else {
		SDL_SetRenderDrawColor(ctx->renderer, 0x12, 0x12, 0x12, SDL_ALPHA_OPAQUE);
	}
	SDL_RenderFillRect(ctx->renderer, &(SDL_FRect) {
		bounds.x,
		bounds.y,
		bounds.w,
		bounds.h,
	});
	if (SDL_fabs(ctx->frames[frame].scroll_interp.y - ctx->frames[frame].scroll.y) >= 0.01) {
		float speed = 10;
		ctx->frames[frame].scroll_interp.y = lerp(ctx->frames[frame].scroll_interp.y, ctx->frames[frame].scroll.y, speed * ctx->deltatime);
		ctx->should_render = true;
	}
	Uint32 lines_count;
	Uint32 line_start = SDL_max(0, SDL_floor(-ctx->frames[frame].scroll_interp.y / ctx->line_height));
	lines_count = split_into_lines(ctx, SDL_arraysize(lines), lines, text, line_start);
	Uint32 selection_min = SDL_min(draw_frame->cursor, draw_frame->selection);
	Uint32 selection_max = SDL_max(draw_frame->cursor, draw_frame->selection);
	for (Uint32 linenum = 0; linenum < lines_count; ++linenum) {
		SDL_FRect line_bounds = {
			.x = lines_bounds.x,
			.y = lines_bounds.y + linenum * ctx->line_height + SDL_fmod(SDL_min(0, draw_frame->scroll_interp.y), ctx->line_height),
			.w = lines_bounds.w,
			.h = ctx->line_height,
		};
		if (line_bounds.y + line_bounds.h > lines_bounds.y + lines_bounds.h + 4) break;
		String line = lines[linenum];
		if (draw_frame->line_prefix != NULL) {
			Uint32 prefix_size = SDL_utf8strlen(draw_frame->line_prefix);
			float prefix_width = prefix_size * ctx->font_width;
			draw_text(ctx, line_bounds.x - prefix_width, line_bounds.y, prefix_color, prefix_size, draw_frame->line_prefix);
		}
		if (draw_frame->active_selection) {
			set_color(ctx, selection_color);
			if ((line.text + line.size >= draw_frame->buffer->text + selection_min && line.text <= draw_frame->buffer->text + selection_min) &&
				(line.text + line.size >= draw_frame->buffer->text + selection_max && line.text <= draw_frame->buffer->text + selection_max)) {
				SDL_FRect selection_oneline_rect = {
					.x = line_bounds.x + string_to_visual(ctx, SDL_min(line.size, selection_min - (line.text - text)), line.text) * ctx->font_width,
					.y = line_bounds.y,
					.h = line_bounds.h,
				};
				selection_oneline_rect.w = SDL_min((string_to_visual(ctx, SDL_min(line.size, selection_max - (line.text - text)), line.text) * ctx->font_width) - selection_oneline_rect.x + line_bounds.x,
					line_bounds.w - selection_oneline_rect.x + line_bounds.x);
				if (selection_oneline_rect.x < line_bounds.x + line_bounds.w) {
					SDL_RenderFillRect(ctx->renderer, &selection_oneline_rect);
				}
			} else if (line.text + line.size >= draw_frame->buffer->text + selection_min && line.text <= draw_frame->buffer->text + selection_min) {
				SDL_FRect selection_min_rect = {
					.x = line_bounds.x + string_to_visual(ctx, SDL_min(line.size, selection_min - (line.text - text)), line.text) * ctx->font_width,
					.y = line_bounds.y,
					.h = line_bounds.h,
				};
				selection_min_rect.w = line_bounds.w - selection_min_rect.x + line_bounds.x;
				SDL_RenderFillRect(ctx->renderer, &selection_min_rect);
			} else if (line.text >= draw_frame->buffer->text + selection_min && line.text + line.size <= draw_frame->buffer->text + selection_max) {
				SDL_FRect selection_intermediate_rect = {
					.x = line_bounds.x,
					.y = line_bounds.y,
					.w = line_bounds.w,
					.h = line_bounds.h,
				};
				SDL_RenderFillRect(ctx->renderer, &selection_intermediate_rect);
			} else if (line.text + line.size >= draw_frame->buffer->text + selection_max && line.text <= draw_frame->buffer->text + selection_max) {
				SDL_FRect selection_max_rect = {
					.x = line_bounds.x,
					.y = line_bounds.y,
					.w = SDL_min(string_to_visual(ctx, SDL_min(line.size, selection_max - (line.text - text)), line.text) * ctx->font_width, line_bounds.w),
					.h = line_bounds.h,
				};
				SDL_RenderFillRect(ctx->renderer, &selection_max_rect);
			}
		}
		set_color(ctx, search_background_color);
		if (draw_frame->searching_mode && ctx->frames[draw_frame->search_frame].buffer->text_size > 0) {
			const char *search_cursor = line.text;
			while (true) {
				search_cursor = SDL_strnstr(search_cursor, ctx->frames[draw_frame->search_frame].buffer->text, line.text + line.size - search_cursor);
				if (search_cursor == NULL) break;
				SDL_FRect search_hi_rect = {
					.x = line_bounds.x + string_to_visual(ctx, search_cursor - line.text, line.text) * ctx->font_width,
					.y = line_bounds.y,
					.w = ctx->frames[draw_frame->search_frame].buffer->text_size * ctx->font_width,
					.h = line_bounds.h,
				};
				search_hi_rect.w = SDL_min(search_hi_rect.w, line_bounds.w - search_hi_rect.x + line_bounds.x);
				if (search_hi_rect.x < line_bounds.x + line_bounds.w) {
					SDL_RenderFillRect(ctx->renderer, &search_hi_rect);
				}
				search_cursor += ctx->frames[draw_frame->search_frame].buffer->text_size;
			}
		}
		render_line(ctx, line_bounds, line.size, line.text);
		if (line.text - text <= draw_frame->cursor &&
			((linenum + 1 >= lines_count) || (lines[linenum + 1].text - text > draw_frame->cursor))) {
			SDL_FPoint actual_cursor_pos = {
				.x = line_bounds.x + string_to_visual(ctx, SDL_min(line.size, draw_frame->cursor - (line.text - text)), line.text) * ctx->font_width,
				.y = line_bounds.y,
			};
			float speed = 30;
			Uint32 width = 2;
			if (ctx->focused_frame == frame &&
				(((SDL_fabs(actual_cursor_pos.x - ctx->active_cursor_pos.x) >= 0.01) ||
				  (SDL_fabs(actual_cursor_pos.y - ctx->active_cursor_pos.y) >= 0.01)))) {
				width = SDL_max(width, SDL_log(SDL_abs(ctx->active_cursor_pos.x - lerp(ctx->active_cursor_pos.x, actual_cursor_pos.x, speed * ctx->deltatime))) * 2);
				ctx->active_cursor_pos.x = lerp(ctx->active_cursor_pos.x, actual_cursor_pos.x, SDL_min(1, speed * ctx->deltatime));
				ctx->active_cursor_pos.y = lerp(ctx->active_cursor_pos.y, actual_cursor_pos.y, SDL_min(1, speed * ctx->deltatime));
				ctx->should_render = true;
			}
			SDL_FRect cursor_rect = {
				.x = actual_cursor_pos.x,
				.y = actual_cursor_pos.y,
				.w = ctx->font_width,
				.h = ctx->line_height,
			};
			if (ctx->focused_frame == frame) {
				cursor_rect.x = ctx->active_cursor_pos.x;
				cursor_rect.y = ctx->active_cursor_pos.y;
				cursor_rect.w = width;
			}
			if (ctx->frames[frame].frame_type != Frame_Type_ask) {
				SDL_GetRectIntersectionFloat(&bounds, &cursor_rect, &cursor_rect);
			}
			cursor_rect.w = SDL_max(0, cursor_rect.w);
			cursor_rect.h = SDL_max(0, cursor_rect.h);
			set_color(ctx, text_color);
			if (cursor_rect.x >= bounds.x + bounds.w) {
				if (ctx->overflow_cursor_texture) {
					cursor_rect.x = bounds.x + bounds.w - ctx->font_width * 1.5;
					cursor_rect.w = ctx->font_width;
					SDL_RenderTexture(ctx->renderer, ctx->overflow_cursor_texture, NULL, &cursor_rect);
				} else {
					cursor_rect.x = bounds.x + bounds.w - 12;
					cursor_rect.w = 12;
					set_color(ctx, debug_red);
					SDL_RenderFillRect(ctx->renderer, &cursor_rect);
				}
			} else if (ctx->focused_frame == frame) {
				SDL_RenderFillRect(ctx->renderer, &cursor_rect);
			} else {
				SDL_RenderRect(ctx->renderer, &cursor_rect);
			}
		}
		if (line.text - text <= draw_frame->selection &&
			((linenum + 1 >= lines_count) || (lines[linenum + 1].text - text > draw_frame->selection))) {
			SDL_FRect selection_rect = {
				.x = line_bounds.x + string_to_visual(ctx, SDL_min(line.size, draw_frame->selection - (line.text - text)), line.text) * ctx->font_width,
				.y = line_bounds.y,
				.w = ctx->font_width,
				.h = ctx->font_size,
			};
			if (selection_rect.x < line_bounds.x + line_bounds.w) {
				set_color(ctx, selection_rect_color);
				SDL_RenderRect(ctx->renderer, &selection_rect);
			}
		}
	}
	if (frame_has_line_numbers(ctx, frame)) {
		SDL_Color current_color = line_number_color;
		for (Uint32 linenum = line_start; (linenum - line_start + 1) * ctx->line_height < lines_numbers_bounds.h; ++linenum) {
#ifdef LINE_NUMS_DIM_SPACE
			if (linenum > lines_count + line_start || lines[linenum].size <= 0) current_color = line_number_dimmed_color;
			else current_color = line_number_color;
#else
			if (linenum == lines_count + line_start) current_color = line_number_dimmed_color;
#endif
			draw_text_fmt(ctx, lines_numbers_bounds.x, lines_numbers_bounds.y + (linenum - line_start) * ctx->line_height + SDL_fmod(SDL_min(0, draw_frame->scroll_interp.y), ctx->line_height), current_color, "%u", linenum);
		}
	}
#ifdef DEBUG_UNDO
	for (Uint32 i = 0; i < draw_frame->buffer->undos_size; ++i) {
		Undo_Operation op = draw_frame->buffer->undos[i];
		SDL_FPoint start = {50, 200};
		draw_text_fmt(ctx, start.x, start.y + i * ctx->line_height, debug_green, "%u (%d %u - %u) %.*s", i, (int)op.type, op.pos, op.len, (int)op.len, op.data);
	}
#endif
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
		SDL_RenderFillRect(ctx->renderer, &(SDL_FRect) {
			bounds.x + bounds.w - SDL_strlen(draw_frame->filename) * ctx->font_width,
			bounds.y + bounds.h - ctx->line_height,
			SDL_strlen(draw_frame->filename) * ctx->font_width,
			ctx->line_height,
		});
		draw_text(ctx, bounds.x + bounds.w - SDL_strlen(draw_frame->filename) * ctx->font_width,
			bounds.y + bounds.h - ctx->line_height, text_color, 0, draw_frame->filename);
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
#ifdef DEBUG_LAYOUT
	set_color(ctx, debug_red);
	SDL_RenderRect(ctx->renderer, &lines_bounds);
	set_color(ctx, debug_blue);
	SDL_RenderLine(ctx->renderer, lines_bounds.x, bounds.y + draw_frame->scroll_interp.y, lines_bounds.x + lines_bounds.w, bounds.y + draw_frame->scroll_interp.y);
#endif
}

static TextBuffer *allocate_buffer(Ctx *ctx, char *name) {
	for (Uint32 i = 0; i < ctx->buffers_count; ++i) {
		if (ctx->buffers[i].refcount > 0) continue;
		ctx->buffers[i] = (TextBuffer){
			.name = name,
		};
		return &ctx->buffers[i];
	}
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
	*buffer = (TextBuffer){
		.name = name,
	};
	return buffer;
}

static inline float vec_len(const SDL_FPoint vec) {
	return SDL_sqrt(vec.x * vec.x + vec.y * vec.y);
}

static inline float point_to_line_dist(SDL_FPoint point, SDL_FPoint start, SDL_FPoint end) {
	SDL_FPoint rel_point = {point.x - start.x, point.y - start.y};
	SDL_FPoint vec = {end.x - start.x, end.y - start.y};
	SDL_FPoint norm_vec = {vec.x / vec_len(vec), vec.y / vec_len(vec)};
	float dot_prod = SDL_min(SDL_fabs(norm_vec.x * rel_point.x + norm_vec.y * rel_point.y), vec_len(vec));
	SDL_FPoint inter = {start.x + norm_vec.x * dot_prod, start.y + norm_vec.y * dot_prod};
	SDL_FPoint dist_vec = {inter.x - point.x, inter.y - point.y};
	return SDL_sqrt(dist_vec.x * dist_vec.x + dist_vec.y * dist_vec.y);
}

static void set_focused_frame(Ctx *ctx, Uint32 frame) {
	ctx->focused_frame = frame;
	Uint32 first = frame;
	Uint32 sorted_ind = reverse_sorted_index(ctx, frame);
	if (sorted_ind != 0)
		SDL_memmove(&ctx->sorted_frames[1], &ctx->sorted_frames[0], sorted_ind * sizeof (*ctx->sorted_frames));
	ctx->sorted_frames[0] = first;
}

static char *utf8_go_forward(size_t text_size, char text[text_size], Uint32 count) {
	for (; count != 0; --count) {
		if (text_size <= 0) return text;
		SDL_StepUTF8((const char **)&text, &text_size);
	}
	return text;
}

static bool generate_overflow_cursor(Ctx *ctx) {
	SDL_Surface *cursor_overflow_surface = SDL_CreateSurface(ctx->font_width * 2, ctx->font_size * 2, SDL_PIXELFORMAT_RGBA8888);
	if (!cursor_overflow_surface) {
		SDL_LogWarn(0, "Error, can't create surface for cursor overflow texture: %s", SDL_GetError());
		return false;
	}
	if (!SDL_LockSurface(cursor_overflow_surface)) {
		SDL_LogWarn(0, "Can't lock cursor overflow cursor surface: %s", SDL_GetError());
		SDL_DestroySurface(cursor_overflow_surface);
		return false;
	}
	float width = 2.5;
	float min_rot = SDL_min(cursor_overflow_surface->h / 2, cursor_overflow_surface->w) - width;
	for (int y = 0; y < cursor_overflow_surface->h; ++y) {
		for (int x = 0; x < cursor_overflow_surface->w; ++x) {
			float dist1 = width - point_to_line_dist((SDL_FPoint){x, y+0.5}, (SDL_FPoint){2, 2}, (SDL_FPoint){min_rot, min_rot});
			float dist2 = width - point_to_line_dist((SDL_FPoint){x, y+0.5}, (SDL_FPoint){2, min_rot * 2 - 2}, (SDL_FPoint){min_rot, min_rot});
			float value = SDL_clamp(SDL_max(dist1, dist2) * 0xcc, 0x00, 0xff);
			SDL_Color color = {value, value, value, value};
			((Uint32 *)((Uint8 *)cursor_overflow_surface->pixels + y * cursor_overflow_surface->pitch))[x]
				= SDL_MapSurfaceRGBA(cursor_overflow_surface, color.r, color.g, color.b, color.a);
		}
	}
	SDL_UnlockSurface(cursor_overflow_surface);
	ctx->overflow_cursor_texture = SDL_CreateTextureFromSurface(ctx->renderer, cursor_overflow_surface);
	SDL_DestroySurface(cursor_overflow_surface);
	if (ctx->overflow_cursor_texture == NULL) {
		SDL_LogWarn(0, "Can't convert cursor overflow surface into texture: %s", SDL_GetError());
		return false;
	}
	return true;
}

static bool generate_space_texture(Ctx *ctx) {
	SDL_Surface *space_surface = SDL_CreateSurface(ctx->font_width, ctx->font_size, SDL_PIXELFORMAT_RGBA8888);
	if (!space_surface) {
		SDL_LogWarn(0, "Can't create surface for space texture: %s", SDL_GetError());
		return false;
	}
	if (!SDL_LockSurface(space_surface)) {
		SDL_LogWarn(0, "Can't lock space surface: %s", SDL_GetError());
		SDL_DestroySurface(space_surface);
		return false;
	}
	float width = 1.8;
	SDL_FPoint center = {space_surface->w / 2, space_surface->h / 2.1};
	for (int y = 0; y < space_surface->h; ++y) {
		for (int x = 0; x < space_surface->w; ++x) {
			float dist = width - SDL_sqrt((center.y - y) * (center.y - y) + (center.x - x) * (center.x - x));
			float value = SDL_clamp(dist * 0xee, 0x00, 0xff);
			SDL_Color color = {value, value, value, value / 2.2};
			((Uint32 *)((Uint8 *)space_surface->pixels + y * space_surface->pitch))[x]
				= SDL_MapSurfaceRGBA(space_surface, color.r, color.g, color.b, color.a);
		}
	}
	SDL_UnlockSurface(space_surface);
	ctx->space_texture = SDL_CreateTextureFromSurface(ctx->renderer, space_surface);
	SDL_DestroySurface(space_surface);
	if (ctx->space_texture == NULL) {
		SDL_LogWarn(0, "Can't convert space surface into texture: %s", SDL_GetError());
		return false;
	}
	return true;
}

static bool generate_tab_texture(Ctx *ctx) {
	SDL_Surface *tab_surface = SDL_CreateSurface(ctx->font_width * TAB_WIDTH, ctx->font_size, SDL_PIXELFORMAT_RGBA8888);
	if (!tab_surface) {
		SDL_LogWarn(0, "Can't create surface for tab texture: %s", SDL_GetError());
		return false;
	}
	if (!SDL_LockSurface(tab_surface)) {
		SDL_LogWarn(0, "Can't lock tab surface: %s", SDL_GetError());
		SDL_DestroySurface(tab_surface);
		return false;
	}
	for (int y = 0; y < tab_surface->h; ++y) {
		for (int x = 0; x < tab_surface->w; ++x) {
			float x0 = (x == 0) * 0xff;
			float value = SDL_clamp(x0, 0x00, 0xff);
			SDL_Color color = {value, value, value, value};
			((Uint32 *)((Uint8 *)tab_surface->pixels + y * tab_surface->pitch))[x]
				= SDL_MapSurfaceRGBA(tab_surface, color.r, color.g, color.b, color.a);
		}
	}
	SDL_UnlockSurface(tab_surface);
	ctx->tab_texture = SDL_CreateTextureFromSurface(ctx->renderer, tab_surface);
	SDL_DestroySurface(tab_surface);
	if (ctx->tab_texture == NULL) {
		SDL_LogWarn(0, "Can't convert tab surface into texture: %s", SDL_GetError());
		return false;
	}
	return true;
}

static bool handle_frame_mouse_click(Ctx *ctx, Uint32 frame, SDL_FPoint point) {
	SDL_FRect bounds;
	Frame *draw_frame = &ctx->frames[frame];
	get_frame_render_text_rect(ctx, frame, &bounds);
	draw_frame->scroll_lock = true;
	if (point.y < bounds.y) {
		draw_frame->cursor = 0;
		return true;
	}
	Uint32 linenum = (point.y - bounds.y - SDL_min(0, draw_frame->scroll_interp.y)) / ctx->line_height;
	String line = get_line(ctx, draw_frame->buffer->text_size, draw_frame->buffer->text, (Uint32)linenum);
	if (line.text == NULL) {
		draw_frame->cursor = draw_frame->buffer->text_size;
		return true;
	}
	// Don't fucking reallocate sized strings which only point is zero copy.
	SDL_assert(line.text >= draw_frame->buffer->text && line.text <= draw_frame->buffer->text + draw_frame->buffer->text_size);
	Uint32 char_ind = coords_to_text_index(ctx, line.size, line.text, point.x - bounds.x);
	draw_frame->cursor = utf8_go_forward(line.size, line.text, char_ind) - draw_frame->buffer->text;
	return true;
}

static Uint32 append_frame(Ctx *ctx, TextBuffer *buffer, SDL_FRect bounds) {
	for (Uint32 i = 0; i < ctx->frames_count; ++i) {
		if (ctx->frames[i].taken) continue;
		ctx->frames[i] = (Frame){
			.taken = true,
			.cursor = 0,
			.scroll = 0,
			.bounds = bounds,
			.buffer = buffer,
		};
		buffer->refcount += 1;
		return i;
	}
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
		.selection = 0,
		.active_selection = false,
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
		if (ctx->frames[ctx->sorted_frames[i]].taken) return ctx->sorted_frames[i];
	}
	SDL_LogError(0, "No more frames, creating one");
	TextBuffer *buffer = allocate_buffer(ctx, "scratch");
	if (buffer == NULL) {
		SDL_LogError(0, "Can't allocate buffer");
		return -1;
	}
	return append_frame(ctx, buffer, (SDL_FRect){0, 0, ctx->win_w, ctx->win_h});
}

static Uint32 create_ask_frame(Ctx *ctx, Ask_Option option, Uint32 parent, char *prefix) {
	TextBuffer *buffer = allocate_buffer(ctx, "ask buffer");
	if (buffer == NULL) {
		SDL_Log("Error, can't allocate ask buffer");
		return -1;
	}
	Uint32 frame = append_frame(ctx, buffer, (SDL_FRect){0, ctx->win_h - ctx->line_height, ctx->win_w, ctx->line_height});
	if (frame == (Uint32)-1) {
		SDL_Log("Error, can't append ask buffer");
		return -1;
	}
	ctx->frames[frame].frame_type = Frame_Type_ask;
	ctx->frames[frame].is_global = true;
	ctx->frames[frame].parent_frame = parent;
	ctx->frames[frame].ask_option = option;
	ctx->frames[frame].line_prefix = prefix;
	return frame;
}

static void log_handler(void *userdata, int category, SDL_LogPriority priority, const char *message) {
	(void) category;
	(void) priority;
	Ctx *ctx = (Ctx *)userdata;
	buffer_insert_text_no_undo(ctx, ctx->log_buffer, message, SDL_strlen(message), ctx->log_buffer->text_size);
	buffer_insert_text_no_undo(ctx, ctx->log_buffer, "\n", 1, ctx->log_buffer->text_size);
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
		draw_text_fmt(ctx, 200, ctx->line_height * i, (SDL_Color) {0xff, 0x00, 0xff, 0xff}, "%" SDL_PRIu32 " %" SDL_PRIs32 " %s", i, ctx->buffers[i].refcount, ctx->buffers[i].name);
	}
#endif
#ifdef DEBUG_SORT
	for (Uint32 i = 0; i < ctx->frames_count; ++i) {
		SDL_Color color = {0x00, 0xff, 0xff, 0xff};
		if (!ctx->frames[ctx->sorted_frames[i]].taken) color = (SDL_Color){0xff, 0xcc, 0xcc, 0xff};
		draw_text_fmt(ctx, 400, ctx->line_height * i, color, "%" SDL_PRIu32 " %" SDL_PRIu32, i, ctx->sorted_frames[i]);
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
#if 0
	SDL_FRect test_pos = {
		.x = 10,
		.y = 10,
		.w = 10 * ctx->font_width * TAB_WIDTH,
		.h = 10 * ctx->font_size,
	};
	set_color(ctx, debug_black);
	SDL_RenderFillRect(ctx->renderer, &test_pos);
	SDL_RenderTexture(ctx->renderer, ctx->tab_texture, NULL, &test_pos);
#endif
	SDL_RenderPresent(ctx->renderer);
	ctx->render_rotate_fan = (ctx->render_rotate_fan + 1) % 4;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
	Ctx *ctx = (Ctx *)appstate;
	Uint64 current_time = SDL_GetPerformanceCounter();
	ctx->deltatime = (current_time - ctx->last_render) / ctx->perf_freq;
	if (!SDL_TextInputActive(ctx->window)) {
		SDL_StartTextInput(ctx->window);
	}
	ctx->keymod = SDL_GetModState();
	for (Uint32 i = 0; i < ctx->frames_count; ++i) {
		if (!ctx->frames[i].taken) continue;
		if (SDL_fabs(ctx->frames[i].bounds_interp.x - ctx->frames[i].bounds.x) >= 0.01 ||
			SDL_fabs(ctx->frames[i].bounds_interp.y - ctx->frames[i].bounds.y) >= 0.01 ||
			SDL_fabs(ctx->frames[i].bounds_interp.w - ctx->frames[i].bounds.w) >= 0.01 ||
			SDL_fabs(ctx->frames[i].bounds_interp.h - ctx->frames[i].bounds.h) >= 0.01) {
			float speed = 30;
			ctx->frames[i].bounds_interp.x = lerp(ctx->frames[i].bounds_interp.x, ctx->frames[i].bounds.x, SDL_min(1, speed * ctx->deltatime));
			ctx->frames[i].bounds_interp.y = lerp(ctx->frames[i].bounds_interp.y, ctx->frames[i].bounds.y, SDL_min(1, speed * ctx->deltatime));
			ctx->frames[i].bounds_interp.w = lerp(ctx->frames[i].bounds_interp.w, ctx->frames[i].bounds.w, SDL_min(1, speed * ctx->deltatime));
			ctx->frames[i].bounds_interp.h = lerp(ctx->frames[i].bounds_interp.h, ctx->frames[i].bounds.h, SDL_min(1, speed * ctx->deltatime));
			ctx->should_render = true;
		}
	}
	if (ctx->should_render) {
		render(ctx, false);
	}
	ctx->last_render = current_time;
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
	(void) argc;
	(void) argv;
	Ctx *ctx;
	ctx = *appstate = SDL_malloc(sizeof *ctx);
	if (ctx == NULL) {
		SDL_Log("Error, can't allocate ctx");
		return SDL_APP_FAILURE;
	}
	*ctx = (Ctx) {0};
	ctx->win_w = 0x300;
	ctx->win_h = 0x200;
	ctx->debug_screen_rect = (SDL_FRect){
		.x = ctx->win_w * 3/4,
		.y = ctx->win_h * 1/20,
		.w = ctx->win_w,
		.h = ctx->win_h,
	};
	ctx->active_cursor_pos = (SDL_FPoint){0};
	SDL_SetAppMetadata("Text editor", "1.0", "c4ll.text-editor");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		SDL_LogCritical(0, "Couldn't initialize SDL: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}
	if (!TTF_Init()) {
		SDL_LogCritical(0, "Can't init TTF: %s\n", SDL_GetError());
		return SDL_APP_FAILURE;
	}
	TextBuffer *buffer;
	if (argc <= 1) {
		buffer = allocate_buffer(ctx, "scratch");
		if (buffer == NULL) {
			SDL_Log("Error, can't allocate first buffer");
			return SDL_APP_FAILURE;
		}
	} else {
		char *filepath = argv[1];
		buffer = allocate_buffer(ctx, filepath);
		if (buffer == NULL) {
			SDL_Log("Error, can't allocate buffer for file");
			return SDL_APP_FAILURE;
		}
		buffer->text = SDL_LoadFile(filepath, &buffer->text_capacity);
		buffer->text_size = buffer->text_capacity;
		if (buffer->text == NULL) {
			SDL_LogInfo(0, "First file %s doesn't exists, creating", filepath);
		} else {
			SDL_LogInfo(0, "Opening first file %s", filepath);
		}
	}
	Uint32 main_frame = append_frame(ctx, buffer, (SDL_FRect){0, 0, ctx->win_w / 2, ctx->win_h});
	if (main_frame == (Uint32)-1) {
		SDL_Log("Error, can't create first frame");
		return SDL_APP_FAILURE;
	}
	if (argc > 1) {
		ctx->frames[main_frame].filename = SDL_strdup(argv[1]);
		ctx->frames[main_frame].scroll_lock = true;
	}
	ctx->log_buffer = allocate_buffer(ctx, "logs");
	if (ctx->log_buffer == NULL) {
		SDL_LogError(0, "Can't create buffer for logs");
		return SDL_APP_FAILURE;
	}
	ctx->log_buffer->refcount += 1;
	Uint32 log_frame = append_frame(ctx, ctx->log_buffer, (SDL_FRect){0x300 / 2, 0, 0x300 / 2, 0x200});
	if (log_frame == (Uint32)-1) {
		SDL_LogError(0, "Can't create log frame");
	}
	Uint64 window_flags = SDL_WINDOW_RESIZABLE;
	if (!SDL_CreateWindowAndRenderer("editor", ctx->win_w, ctx->win_h, window_flags, &ctx->window, &ctx->renderer)) {
		SDL_Log("Error, can't init renderer: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}
	ctx->font_size = 12;
	ctx->line_height = ctx->font_size * 1.2;
	const char *fontpath = "/usr/share/fonts/TTF/liberation/LiberationMono-Regular.ttf";
	ctx->font = TTF_OpenFont(fontpath, ctx->font_size);
	if (ctx->font == NULL) {
		SDL_LogCritical(0, "Can't open font \"%s\": %s", fontpath, SDL_GetError());
		return SDL_APP_FAILURE;
	}
	int font_width_int;
	TTF_GetGlyphMetrics(ctx->font, 'w', NULL, NULL, NULL, NULL, &font_width_int);
	ctx->font_width = (float)font_width_int;
	/*
	ctx->text_engine = TTF_CreateRendererTextEngine(ctx->renderer);
	if (ctx->text_engine == NULL) {
		SDL_LogCritical(0, "Can't create text engine: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}
	*/
	generate_overflow_cursor(ctx);
	generate_tab_texture(ctx);
	generate_space_texture(ctx);
	if (!SDL_SetRenderVSync(ctx->renderer, 1)) {
		SDL_Log("Warning, can't enable vsync: %s", SDL_GetError());
	}
	ctx->perf_freq = (double)SDL_GetPerformanceFrequency();
	ctx->last_render = SDL_GetPerformanceCounter();
	ctx->should_render = true;
	SDL_SetLogOutputFunction(log_handler, ctx);
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
	Ctx *ctx = (Ctx *)appstate;
	Frame *current_frame = &ctx->frames[ctx->focused_frame];
	switch (event->type) {
		case SDL_EVENT_QUIT: {
			return SDL_APP_SUCCESS;
		}; break;
		case SDL_EVENT_KEY_DOWN: {
			switch (event->key.scancode) {
				case SDL_SCANCODE_LEFT: {
					ctx->debug_screen_rect.x -= 10;
					frame_previous_char(ctx, ctx->focused_frame);
				}; break;
				case SDL_SCANCODE_RIGHT: {
					ctx->debug_screen_rect.x += 10;
					frame_next_char(ctx, ctx->focused_frame);
				}; break;
				case SDL_SCANCODE_BACKSPACE: {
					ctx->moving_col = false;
					if (current_frame->cursor > 0 && current_frame->buffer->text_size > 0) {
						const char *current = current_frame->buffer->text + current_frame->cursor;
						const char *previous = current;
						SDL_StepBackUTF8(current_frame->buffer->text, &previous);
						size_t diff = current - previous;
						buffer_delete_text(ctx, (current_frame->buffer - ctx->buffers) / sizeof current_frame->buffer, current_frame->cursor - diff, current_frame->cursor, Undo_Group_keyboard);
					}
					if (current_frame->frame_type == Frame_Type_search) {
						update_search(ctx, ctx->focused_frame);
						ctx->should_render = true;
					}
				}; break;
				case SDL_SCANCODE_ESCAPE: {
					if (current_frame->frame_type == Frame_Type_ask) {
						current_frame->taken = false;
						current_frame->buffer->refcount -= 1;
						ctx->focused_frame = find_any_frame(ctx);
						current_frame = &ctx->frames[ctx->focused_frame];
						ctx->should_render = true;
						break;
					} else if (current_frame->frame_type == Frame_Type_search) {
						current_frame->taken = false;
						current_frame->buffer->refcount -= 1;
						ctx->frames[current_frame->parent_frame].searching_mode = false;
						ctx->focused_frame = current_frame->parent_frame;
						current_frame = &ctx->frames[ctx->focused_frame];
						ctx->should_render = true;
						break;
					}
				}; break;
				case SDL_SCANCODE_RETURN: { /* ENTER (for fast searching) */
					ctx->moving_col = false;
					char nl = '\n';
					if (current_frame->frame_type == Frame_Type_ask) {
						if (current_frame->ask_option == Ask_Option_save) {
							Frame *parent_frame = &ctx->frames[current_frame->parent_frame];
							parent_frame->filename =
								SDL_strndup(current_frame->buffer->text, current_frame->buffer->text_size);
							current_frame->buffer->refcount -= 1;
							current_frame->taken = false;
							ctx->focused_frame = current_frame->parent_frame;
							current_frame = &ctx->frames[ctx->focused_frame];
							if (!SDL_SaveFile(current_frame->filename, current_frame->buffer->text, current_frame->buffer->text_size)) {
								SDL_LogWarn(0, "Can't save buffer into %s: %s", current_frame->filename, SDL_GetError());
							} else {
								SDL_LogInfo(0, "Saved buffer into %s", current_frame->filename);
							}
							ctx->should_render = true;
						} else if (current_frame->ask_option == Ask_Option_open) {
							Frame *parent_frame = &ctx->frames[current_frame->parent_frame];
							parent_frame->filename =
								SDL_strndup(current_frame->buffer->text, current_frame->buffer->text_size);
							parent_frame->buffer->refcount -= 1;
							parent_frame->buffer = allocate_buffer(ctx, SDL_strdup(parent_frame->filename));
							if (parent_frame->buffer == NULL) {
								SDL_LogError(0, "Can't allocate buffer for this file");
								return SDL_APP_FAILURE;
							}
							SDL_free(parent_frame->buffer->text);
							parent_frame->buffer->text = SDL_LoadFile(parent_frame->filename, &parent_frame->buffer->text_capacity);
							if (parent_frame->buffer->text == NULL) {
								SDL_LogInfo(0, "File %s doesn't exists, creating", parent_frame->filename);
							} else {
								SDL_LogInfo(0, "Opened file %s", parent_frame->filename);
							}
							parent_frame->scroll_lock = true;
							parent_frame->cursor = 0;
							parent_frame->buffer->text_size = parent_frame->buffer->text_capacity;
							parent_frame->buffer->refcount += 1;
							current_frame->taken = false;
							current_frame->buffer->refcount -= 1;
							ctx->focused_frame = current_frame->parent_frame;
							current_frame = &ctx->frames[ctx->focused_frame];
							ctx->should_render = true;
						} else {
							SDL_LogError(0, ("Unknown ask option: %" SDL_PRIu32), (Uint32)current_frame->ask_option);
						}
						break;
					} else if (current_frame->frame_type == Frame_Type_search) {
						current_frame->buffer->refcount -= 1;
						current_frame->taken = false;
						ctx->frames[current_frame->parent_frame].searching_mode = false;
						if (current_frame->search_status == Search_Status_found) {
							ctx->frames[current_frame->parent_frame].cursor = ctx->frames[current_frame->parent_frame].search_cursor;
						}
						ctx->focused_frame = current_frame->parent_frame;
						current_frame = &ctx->frames[ctx->focused_frame];
						ctx->should_render = true;
						break;
					}
					if (frame_is_multiline(ctx, ctx->focused_frame)) {
						buffer_insert_text(ctx, current_frame->buffer, &nl, 1, current_frame->cursor, Undo_Group_keyboard);
						ctx->should_render = true;
					}
				}; break;
				case SDL_SCANCODE_TAB: {
					ctx->moving_col = false;
					char nl = '\t';
					buffer_insert_text(ctx, current_frame->buffer, &nl, 1, current_frame->cursor, Undo_Group_keyboard);
					ctx->should_render = true;
				}; break;
				case SDL_SCANCODE_UP: {
					ctx->debug_screen_rect.y -= 10;
					frame_previous_line(ctx, ctx->focused_frame);
				}; break;
				case SDL_SCANCODE_DOWN: {
					ctx->debug_screen_rect.y += 10;
					frame_next_line(ctx, ctx->focused_frame);
				}; break;
				default: {};
			}
			switch (event->key.key) {
				case SDLK_SPACE: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						current_frame->selection = current_frame->cursor;
						current_frame->active_selection = true;
						ctx->should_render = true;
					}
				} break;
				case SDLK_F: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						frame_next_char(ctx, ctx->focused_frame);
					}
				}; break;
				case SDLK_S: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						if (current_frame->filename == NULL || ctx->keymod & SDL_KMOD_SHIFT) {
							Uint32 ask_frame = create_ask_frame(ctx, Ask_Option_save, ctx->focused_frame, "Save to: ");
							if (ask_frame == (Uint32)-1) {
								SDL_Log("Error, can't open ask frame");
								break;
							}
							ctx->focused_frame = ask_frame;
							current_frame = &ctx->frames[ctx->focused_frame];
							ctx->should_render = true;
						} else {
							SDL_SaveFile(current_frame->filename, current_frame->buffer->text, current_frame->buffer->text_size);
							SDL_LogInfo(0, "Saved buffer into %s", current_frame->filename);
						}
					}
				}; break;
				case SDLK_Q: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						if (current_frame->frame_type == Frame_Type_search) {
							if (current_frame->search_status == Search_Status_not_found) break;
							ctx->frames[current_frame->parent_frame].cursor =
								ctx->frames[current_frame->parent_frame].search_cursor
								+ current_frame->buffer->text_size;
							update_search(ctx, ctx->focused_frame);
						} else {
							current_frame->searching_mode = true;
							current_frame->search_cursor = current_frame->cursor;
							char *buffer_name;
							SDL_asprintf(&buffer_name, "%d search", ctx->focused_frame);
							TextBuffer *search_buffer = allocate_buffer(ctx, buffer_name);
							if (search_buffer == NULL) {
								SDL_LogError(0, "Can't create buffer for ask frame\n");
								break;
							}
							#define SEARCH_MARGIN 0x20
							SDL_FRect bounds = {
								.x = current_frame->bounds.x + SEARCH_MARGIN,
								.y = current_frame->bounds.y + SEARCH_MARGIN,
								.w = current_frame->bounds.w - SEARCH_MARGIN * 2,
								.h = ctx->font_size,
							};
							Uint32 search_frame = append_frame(ctx, search_buffer, bounds);
							ctx->frames[search_frame].frame_type = Frame_Type_search;
							ctx->frames[search_frame].parent_frame = ctx->focused_frame;
							ctx->frames[search_frame].search_status = Search_Status_not_found;
							current_frame->search_frame = search_frame;
							set_focused_frame(ctx, search_frame);
							current_frame = &ctx->frames[ctx->focused_frame];
							ctx->should_render = true;
						}
					}
				} break;
				case SDLK_SLASH: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						if (ctx->keymod & SDL_KMOD_SHIFT) {
							if (current_frame->buffer->undos_cursor >= current_frame->buffer->undos_size) break;
							Undo_Operation op = current_frame->buffer->undos[current_frame->buffer->undos_cursor];
							if (op.type == Undo_Type_insert) {
								buffer_insert_text_no_undo(ctx, current_frame->buffer, op.data, op.len, op.pos);
								current_frame->buffer->undos_cursor += 1;
							} else if (op.type == Undo_Type_delete) {
								buffer_delete_text_no_undo(ctx, (current_frame->buffer - ctx->buffers) / sizeof *current_frame->buffer, op.pos, op.pos + op.len);
								current_frame->buffer->undos_cursor += 1;
							} else {
								SDL_assert(!"Unknown Undo type operation");
							}
							ctx->should_render = true;
						} else {
							if (current_frame->buffer->undos_cursor <= 0) break;
							Undo_Operation op = current_frame->buffer->undos[current_frame->buffer->undos_cursor - 1];
							if (op.type == Undo_Type_insert) {
								buffer_delete_text_no_undo(ctx, (current_frame->buffer - ctx->buffers) / sizeof *current_frame->buffer, op.pos, op.pos + op.len);
								current_frame->buffer->undos_cursor -= 1;
							} else if (op.type == Undo_Type_delete) {
								buffer_insert_text_no_undo(ctx, current_frame->buffer, op.data, op.len, op.pos);
								current_frame->buffer->undos_cursor -= 1;
							} else {
								SDL_assert(!"Unknown Undo type operation");
							}
							ctx->should_render = true;
						}
					}
				} break;
				case SDLK_P: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						frame_previous_line(ctx, ctx->focused_frame);
					}
				}; break;
				case SDLK_A: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						frame_beggining_line(ctx, ctx->focused_frame);
					}
				}; break;
				case SDLK_E: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						frame_end_line(ctx, ctx->focused_frame);
					}
				}; break;
				case SDLK_M: {
					if (ctx->keymod & SDL_KMOD_ALT) {
						frame_beggining_spaced_line(ctx, ctx->focused_frame);
					}
				}; break;
				case SDLK_N: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						frame_next_line(ctx, ctx->focused_frame);
					}
				}; break;
				case SDLK_B: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						frame_previous_char(ctx, ctx->focused_frame);
					} else if (ctx->keymod & SDL_KMOD_ALT) {
						current_frame->bounds.w /= 2;
						SDL_FRect bounds = current_frame->bounds;
						bounds.x += bounds.w;
						Uint32 frame = append_frame(ctx, current_frame->buffer, bounds);
						if (frame == (Uint32)-1) {
							SDL_Log("Error, can't open new frame");
							break;
						}
						set_focused_frame(ctx, frame);
						current_frame = &ctx->frames[ctx->focused_frame];
						ctx->should_render = true;
					}
				}; break;
				case SDLK_W: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						current_frame->active_selection = false;
						ctx->moving_col = false;
						Uint32 selection_min = SDL_min(current_frame->cursor, current_frame->selection);
						Uint32 selection_max = SDL_max(current_frame->cursor, current_frame->selection);
						char ch = current_frame->buffer->text[selection_max];
						current_frame->buffer->text[selection_max] = '\0';
						SDL_SetClipboardText(current_frame->buffer->text + selection_min);
						current_frame->buffer->text[selection_max] = ch;
						buffer_delete_text(ctx, (current_frame->buffer - ctx->buffers) / sizeof *current_frame->buffer, selection_min, selection_max, Undo_Group_clipboard);
					} else if (ctx->keymod & SDL_KMOD_ALT) {
						current_frame->active_selection = false;
						ctx->moving_col = false;
						Uint32 selection_min = SDL_min(current_frame->cursor, current_frame->selection);
						Uint32 selection_max = SDL_max(current_frame->cursor, current_frame->selection);
						char ch = current_frame->buffer->text[selection_max];
						current_frame->buffer->text[selection_max] = '\0';
						SDL_SetClipboardText(current_frame->buffer->text + selection_min);
						current_frame->buffer->text[selection_max] = ch;
					}
				} break;
				case SDLK_Y: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						current_frame->active_selection = false;
						char *text = SDL_GetClipboardText();
						buffer_insert_text(ctx, current_frame->buffer, text, SDL_strlen(text), current_frame->cursor, Undo_Group_clipboard);
						SDL_free(text);
					}
				} break;
				case SDLK_V: {
					if (ctx->keymod & SDL_KMOD_ALT) {
						current_frame->bounds.h /= 2;
						SDL_FRect bounds = current_frame->bounds;
						bounds.y += bounds.h;
						Uint32 frame = append_frame(ctx, current_frame->buffer, bounds);
						if (frame == (Uint32)-1) {
							SDL_Log("Error, can't open new frame");
							break;
						}
						set_focused_frame(ctx, frame);
						current_frame = &ctx->frames[ctx->focused_frame];
						current_frame->scroll_lock = true;
						ctx->should_render = true;
					}
				}; break;
				case SDLK_G: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						if (current_frame->active_selection) {
							current_frame->active_selection = false;
							ctx->should_render = true;
							break;
						} else {
							if (current_frame->frame_type == Frame_Type_search) {
								current_frame->taken = false;
								current_frame->buffer->refcount -= 1;
								ctx->frames[current_frame->parent_frame].searching_mode = false;
								ctx->focused_frame = current_frame->parent_frame;
								current_frame = &ctx->frames[ctx->focused_frame];
								ctx->should_render = true;
								break;
							}
						}
					}
				} break;
				case SDLK_X: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						Uint32 temp = current_frame->selection;
						current_frame->selection = current_frame->cursor;
						current_frame->cursor = temp;
						ctx->moving_col = false;
						ctx->should_render = true;
					} else if (ctx->keymod & SDL_KMOD_ALT) {
						current_frame->buffer->refcount -= 1;
						current_frame->taken = false;
						ctx->focused_frame = find_any_frame(ctx);
						current_frame = &ctx->frames[ctx->focused_frame];
						ctx->should_render = true;
					}
				} break;
				case SDLK_O: {
					if (ctx->keymod & SDL_KMOD_CTRL) {
						Uint32 ask_frame = create_ask_frame(ctx, Ask_Option_open, ctx->focused_frame, "Open: ");
						if (ask_frame == (Uint32)-1) {
							SDL_Log("Error, can't open ask frame");
							break;
						}
						ctx->focused_frame = ask_frame;
						current_frame = &ctx->frames[ask_frame];
						ctx->should_render = true;
					} else if (ctx->keymod & SDL_KMOD_ALT) {
						for (Uint32 i = ctx->focused_frame + 1; i != ctx->focused_frame; ++i) {
							if (i > ctx->frames_count) i = 0;
							if (!ctx->frames[i].taken) continue;
							set_focused_frame(ctx, i);
							current_frame = &ctx->frames[ctx->focused_frame];
							ctx->should_render = true;
							break;
						}
					}
				}; break;
			}
			SDL_Scancode scancode = event->key.scancode;
			if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT) {
				ctx->keys[scancode] = event->type == SDL_EVENT_KEY_DOWN;
			}
		} break;
		case SDL_EVENT_KEY_UP: {
			SDL_Scancode scancode = event->key.scancode;
			if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT) {
				ctx->keys[scancode] = event->type == SDL_EVENT_KEY_DOWN;
			}
		}; break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN: {
			SDL_FPoint point = {event->button.x, event->button.y};
			if (event->button.button == SDL_BUTTON_LEFT) {
				if (ctx->keymod & SDL_KMOD_CTRL) {
				} else if (ctx->keymod & SDL_KMOD_ALT) {
				} else {
					for (Uint32 i = 0; i < ctx->frames_count; ++i) {
						Uint32 sframei = ctx->sorted_frames[i];
						if (ctx->frames[sframei].is_global) continue;
						SDL_FRect bounds;
						get_frame_render_rect(ctx, sframei, &bounds);
						if (SDL_PointInRectFloat(&point, &bounds)) {
							set_focused_frame(ctx, sframei);
							handle_frame_mouse_click(ctx, sframei, point);
							goto end;
						}
					}
					for (Uint32 i = 0; i < ctx->frames_count; ++i) {
						Uint32 sframei = ctx->sorted_frames[i];
						if (!ctx->frames[sframei].is_global) continue;
						SDL_FRect bounds;
						get_frame_render_rect(ctx, sframei, &bounds);
						if (SDL_PointInRectFloat(&point, &bounds)) {
							set_focused_frame(ctx, sframei);
							handle_frame_mouse_click(ctx, sframei, point);
							goto end;
						}
					}
					end:
					ctx->should_render = true;
					break;
				}
			} else if (event->button.button == SDL_BUTTON_MIDDLE) {
				Uint64 time = SDL_GetTicks();
				if (time - ctx->last_middle_click <= 300) {
					ctx->transform = (SDL_FPoint){0};
				}
				ctx->last_middle_click = time;
				ctx->should_render = true;
			}
		}; break;
		case SDL_EVENT_MOUSE_MOTION: {
			ctx->mouse_pos.x = event->motion.x;
			ctx->mouse_pos.y = event->motion.y;
			if (event->motion.state & SDL_BUTTON_MMASK) {
				ctx->transform.x += event->motion.xrel;
				ctx->transform.y += event->motion.yrel;
				ctx->should_render = true;
			} else if (event->motion.state & SDL_BUTTON_LMASK) {
				if (ctx->keymod & SDL_KMOD_CTRL) {
					current_frame->bounds.x += event->motion.xrel;
					current_frame->bounds.y += event->motion.yrel;
					ctx->should_render = true;
				}
			} else if (event->motion.state & SDL_BUTTON_RMASK) {
				if (ctx->keymod & SDL_KMOD_CTRL) {
					current_frame->bounds.w += event->motion.xrel;
					current_frame->bounds.h += event->motion.yrel;
					ctx->should_render = true;
				}
			}
		} break;
		case SDL_EVENT_MOUSE_WHEEL: {
			if (frame_is_multiline(ctx, ctx->focused_frame)) {
				current_frame->scroll.y += event->wheel.y * ctx->line_height * 3;
				current_frame->scroll_lock = true;
				SDL_LogTrace(0, "Scroll %d to %f", ctx->focused_frame, current_frame->scroll.y);
				ctx->should_render = true;
			}
		} break;
		case SDL_EVENT_TEXT_INPUT: {
			if (ctx->keymod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) break;
			current_frame->active_selection = false;
			ctx->moving_col = false;
			buffer_insert_text(ctx, current_frame->buffer, event->text.text, SDL_strlen(event->text.text), current_frame->cursor, Undo_Group_keyboard);
			if (current_frame->frame_type == Frame_Type_search) {
				update_search(ctx, ctx->focused_frame);
			}
			ctx->should_render = true;
		}; break;
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
			ctx->win_w = event->window.data1;
			ctx->win_h = event->window.data2;
			SDL_LogDebug(0, "Window resized to %ux%u", ctx->win_w, ctx->win_h);
			ctx->should_render = 1;
		} break;
		case SDL_EVENT_WINDOW_EXPOSED: {
			SDL_LogTrace(0, "Window exposed");
			ctx->should_render = 1;
		} break;
	}
	return SDL_APP_CONTINUE;
}

#ifdef DEBUG_QUIT
static void buffer_deallocate(Ctx *ctx, Uint32 bufid) {
	TextBuffer *buffer = &ctx->buffers[bufid];
	buffer->undos_cursor = 0;
	undo_clear_after_cursor(ctx, bufid);
	if (buffer->text != NULL)
		SDL_free(buffer->text);
	buffer->refcount = 0;
}

static void frame_deallocate(Ctx *ctx, Frame *frame) {
	(void)ctx;
	if (frame->filename != NULL)
		SDL_free(frame->filename);
	frame->buffer->refcount -= 1;
	frame->taken = false;
}
#endif

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
	Ctx *ctx = (Ctx *)appstate;
	(void) ctx;
	(void) result;
#ifdef DEBUG_QUIT
	TTF_CloseFont(ctx->font);
	for (Uint32 i = 0; i < ctx->frames_count; ++i) {
		frame_deallocate(ctx, &ctx->frames[i]);
	}
	SDL_free(ctx->frames);
	SDL_free(ctx->sorted_frames);
	for (Uint32 i = 0; i < ctx->buffers_count; ++i) {
		buffer_deallocate(ctx, i);
	}
	SDL_free(ctx->buffers);
	SDL_DestroyTexture(ctx->space_texture);
	SDL_DestroyTexture(ctx->tab_texture);
	SDL_DestroyTexture(ctx->overflow_cursor_texture);
	SDL_DestroyRenderer(ctx->renderer);
	SDL_DestroyWindow(ctx->window);
#endif
	// TODO(c4llv07e): Deallocate all buffers in debug for valgrind.
}
