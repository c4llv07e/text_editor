#include <SDL3/SDL.h>
#include <stdbool.h>
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
    const bool *keys;
    bool running;
} Ctx;

int main(int argc, char *argv[argc]) {
    (void)argv;
    Ctx ctx = {0};
    ctx.cursor = 0;
    ctx.text_size = 0;
    ctx.text_capacity = 0;
    ctx.text = NULL;
    ctx.keys = NULL;
    if (!SDL_CreateWindowAndRenderer("test", 0x400, 0x300, SDL_WINDOW_RESIZABLE, &ctx.window, &ctx.renderer)) {
        SDL_Log("Error, can't init renderer: %s", SDL_GetError());
        return -1;
    }
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
                            if (ctx.cursor > ctx.text_size) ctx.cursor = ctx.text_size;
                            size_t new_size = (size_t)ctx.text_size + 1;
                            if (new_size + 1 > ctx.text_capacity) {
                                size_t new_capacity = ((new_size + 1 + TEXT_CHUNK_SIZE - 1) / TEXT_CHUNK_SIZE) * TEXT_CHUNK_SIZE;
                                char *new_buf = realloc(ctx.text, new_capacity);
                                if (!new_buf) {
                                    SDL_Log("Error, failed to reallocate new buffer");
                                    break;
                                }
                                ctx.text = new_buf;
                                ctx.text_capacity = (Uint32)new_capacity;
                            }
                            memmove(ctx.text + ctx.cursor + 1,
                                ctx.text + ctx.cursor,
                                (size_t)ctx.text_size - (size_t)ctx.cursor);
                            ctx.text[ctx.cursor] = '\n';
                            ctx.text_size += 1;
                            ctx.cursor += 1;
                            ctx.text[ctx.text_size] = '\0';
                        }; break;
                        default: {};
                    }
                }; break;
                case SDL_EVENT_TEXT_INPUT: {
                    const char *in = ev.text.text;
                    size_t in_len = strlen(in);
                    if (in_len == 0) break;
                    if (ctx.cursor > ctx.text_size) ctx.cursor = ctx.text_size;
                    size_t new_size = (size_t)ctx.text_size + in_len;
                    if (new_size + 1 > ctx.text_capacity) {
                        size_t new_capacity = ((new_size + 1 + TEXT_CHUNK_SIZE - 1) / TEXT_CHUNK_SIZE) * TEXT_CHUNK_SIZE;
                        char *new_buf = realloc(ctx.text, new_capacity);
                        if (!new_buf) {
                            SDL_Log("Error, failed to reallocate new buffer");
                            break;
                        }
                        ctx.text = new_buf;
                        ctx.text_capacity = (Uint32)new_capacity;
                    }
                    memmove(ctx.text + ctx.cursor + in_len,
                        ctx.text + ctx.cursor,
                        (size_t)ctx.text_size - (size_t)ctx.cursor);
                    memcpy(ctx.text + ctx.cursor, in, in_len);
                    ctx.text_size = (Uint32)new_size;
                    ctx.cursor += (Uint32)in_len;
                    ctx.text[ctx.text_size] = '\0';
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
                        SDL_RenderDebugText(ctx.renderer, 0, line * 12, tmp);
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
                SDL_RenderDebugText(ctx.renderer, 0, line * 12, tmp);
            }
        }
        SDL_SetRenderDrawColor(ctx.renderer, 0xe6, 0xe6, 0xe6, 0xff);
        SDL_RenderRect(ctx.renderer, &(SDL_FRect){ctx.cursor * 8, 0, 2, 8});
        SDL_RenderPresent(ctx.renderer);
    }
#ifdef DEBUG
    free(ctx.text);
#endif
    return 0;
}
