#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_CHUNK_SIZE 256

typedef struct {
    SDL_Renderer *renderer;
    SDL_Window *window;
    Uint32 cursor;
    Uint32 text_size;
    Uint32 text_capacity;
    char *text;
    int linebar_length;
    const bool *keys;
    bool running;
} Ctx;

static void insert_text(Ctx *ctx, const char *in, size_t in_len) {
    if (in_len == 0) return;
    if (ctx->cursor > ctx->text_size) ctx->cursor = ctx->text_size;
    size_t new_size = (size_t)ctx->text_size + in_len;
    if (new_size + 1 > ctx->text_capacity) {
        size_t new_capacity = ((new_size + 1 + TEXT_CHUNK_SIZE - 1) / TEXT_CHUNK_SIZE) * TEXT_CHUNK_SIZE;
        char *new_buf = realloc(ctx->text, new_capacity);
        if (!new_buf) {
            SDL_Log("Error, failed to reallocate new buffer");
            return;
        }
        ctx->text = new_buf;
        ctx->text_capacity = (Uint32)new_capacity;
    }
    memmove(ctx->text + ctx->cursor + in_len,
        ctx->text + ctx->cursor,
        (size_t)ctx->text_size - (size_t)ctx->cursor);
    memcpy(ctx->text + ctx->cursor, in, in_len);
    ctx->text_size = (Uint32)new_size;
    ctx->cursor += (Uint32)in_len;
    ctx->text[ctx->text_size] = '\0';
}

int main(int argc, char *argv[argc]) {
    (void)argv;
    Ctx ctx = {0};
    ctx.cursor = 0;
    ctx.text_size = 0;
    ctx.text_capacity = 0;
    ctx.text = NULL;
    ctx.keys = NULL;
    ctx.linebar_length = 3;
    if (!SDL_CreateWindowAndRenderer("test", 0x400, 0x300, SDL_WINDOW_RESIZABLE, &ctx.window, &ctx.renderer)) {
        SDL_Log("Error, can't init renderer: %s", SDL_GetError());
        return -1;
    }
#ifdef DEBUG
    const char *text = "int main(void) {\n    return 0;\n}";
    insert_text(&ctx, text, strlen(text));
    insert_text(&ctx, text, strlen(text));
    ctx.cursor = strlen(text);
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
                            if (ctx.cursor > 0) ctx.cursor -= 1;
                        }; break;
                        case SDL_SCANCODE_RIGHT: {
                            if (ctx.cursor < ctx.text_size) ctx.cursor += 1;
                        }; break;
                        case SDL_SCANCODE_BACKSPACE: {
                            if (ctx.cursor > 0 && ctx.text_size > 0) {
                                memmove(ctx.text + ctx.cursor - 1, ctx.text + ctx.cursor, ctx.text_size - ctx.cursor + 1); // +1 to move the null terminator
                                ctx.cursor -= 1;
                                ctx.text_size -= 1;
                            }
                        }; break;
                        case SDL_SCANCODE_RETURN: {
                            char nl = '\n';
                            insert_text(&ctx, &nl, 1);
                        }; break;
                        case SDL_SCANCODE_TAB: {
                            char nl = '\t';
                            insert_text(&ctx, &nl, 1);
                        }; break;
                        case SDL_SCANCODE_UP: {
                            int row = 0;
                            if (ctx.cursor > 0) {ctx.cursor--; row++; }
                            while (ctx.text[ctx.cursor] != '\n' && ctx.cursor > 0) {
                                ctx.cursor--;
                                row++;
                            }
                            if (ctx.cursor == 0) break;
                            ctx.cursor--;
                            while (ctx.text[ctx.cursor] != '\n' && ctx.cursor > 0) {
                                ctx.cursor--;
                            }
                            if (ctx.text[ctx.cursor] == '\n') ctx.cursor++;
                            for (row--; row && ctx.text[ctx.cursor] != '\n'; row--) ctx.cursor++;
                        }; break;
                        case SDL_SCANCODE_DOWN: {
                            int row = 0;
                            if (ctx.cursor > 0) {ctx.cursor--; row++; }
                            while (ctx.text[ctx.cursor] != '\n' && ctx.cursor > 0) {
                                ctx.cursor--;
                                row++;
                            }
                            if (ctx.cursor == 0) row++;
                            ctx.cursor++;
                            while (ctx.text[ctx.cursor] != '\n' && ctx.cursor < ctx.text_size) {
                                ctx.cursor++;
                            }
                            if (ctx.cursor == ctx.text_size) break;
                            ctx.cursor++;
                            row--;
                            while (row && ctx.cursor < ctx.text_size && ctx.text[ctx.cursor] != '\n') {
                                ctx.cursor++;
                                row--;
                            }
                        }; break;
                        default: {};
                    }
                }; break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                    int row = (ev.button.x / 8.0) - ctx.linebar_length + 0.7;
                    int col = ev.button.y / 12;
                    ctx.cursor = 0;
                    while (col > 0) {
                        if (ctx.cursor >= ctx.text_size) break;
                        ctx.cursor++;
                        if (ctx.text[ctx.cursor] == '\n') col--;
                    }
                    ctx.cursor++;
                    while (row > 0) {
                        if (ctx.cursor >= ctx.text_size) break;
                        if (ctx.text[ctx.cursor] == '\n') break;
                        ctx.cursor++;
                        row--;
                    }
                }; break;
                case SDL_EVENT_TEXT_INPUT: {
                    insert_text(&ctx, ev.text.text, strlen(ev.text.text));
                }; break;
            }
        }
        if (!SDL_TextInputActive(ctx.window)) {
            SDL_StartTextInput(ctx.window);
        }
        ctx.keys = SDL_GetKeyboardState(NULL);
        if (!ctx.running) break;
        SDL_SetRenderDrawColor(ctx.renderer, 0x12, 0x12, 0x12, 0xff);
        SDL_RenderClear(ctx.renderer);
        if (ctx.text_size != 0) {
            SDL_SetRenderDrawColor(ctx.renderer, 0xe6, 0xe6, 0xe6, 0xff);
            const char *start = ctx.text;
            const char *end = ctx.text;
            int line = 0;
            while (*end) {
                if (*end == '\n') {
                    size_t len = end - start;
                    if (len > 0) {
                        char tmp[1024];
                        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
                        memcpy(tmp, start, len);
                        tmp[len] = '\0';
                        SDL_RenderDebugText(ctx.renderer, 8 * (ctx.linebar_length), line * 12, tmp);
                    }
                    start = end + 1;
                    line++;
                }
                end++;
            }
            if (start != end) {
                size_t len = end - start;
                char tmp[1024];
                if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
                memcpy(tmp, start, len);
                tmp[len] = '\0';
                SDL_RenderDebugText(ctx.renderer, 8 * (ctx.linebar_length), line * 12, tmp);
            }
            int max_lines = line;
            for (int line = 0; line <= max_lines; ++line) {
                char tmp[0x10];
                sprintf(tmp, "%d", line + 1);
                SDL_RenderDebugText(ctx.renderer, 0, line * 12, tmp);
            }
        }
        int cursor_line = 0, cursor_col = 0;
        if (ctx.text_size != 0) {
            const char *p = ctx.text;
            int line = 0, col = 0;
            for (Uint32 i = 0; i < ctx.cursor && p[i]; ++i) {
                if (p[i] == '\n') {
                    line++;
                    col = 0;
                } else {
                    col++;
                }
            }
            cursor_line = line;
            cursor_col = col;
        }
        SDL_SetRenderDrawColor(ctx.renderer, 0xe6, 0xe6, 0xe6, 0xff);
        SDL_RenderRect(ctx.renderer, &(SDL_FRect){8 * (ctx.linebar_length + cursor_col), cursor_line * 12, 2, 12});
        SDL_RenderPresent(ctx.renderer);
    }
#ifdef DEBUG
    free(ctx.text);
#endif
    return 0;
}
