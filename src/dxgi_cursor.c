/*
 * This file is part of txproto.
 *
 * txproto is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * txproto is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with txproto; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define COBJMACROS

#include <pthread.h>

#include <libavutil/error.h>
#include <libavutil/mem.h>

#include <libtxproto/fifo_bufferref.h>
#include <libtxproto/log.h>
#include "os_compat.h"
#include "dxgi_cursor.h"

#define SIZEOF_ARRAY(a) (sizeof((a))/sizeof((a)[0]))

typedef struct DxgiCursor {
    int visible;
    POINT position;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info;

    uint8_t *data;
    uint32_t size;
} DxgiCursor;

typedef struct ArgbCursor {
    uint32_t *data;
    uint32_t size;

    uint32_t width;
    uint32_t height;

    int32_t xhot;
    int32_t yhot;
} ArgbCursor;

struct DxgiCursorHandler {
    SPClass *class;

    /* Worker thread */
    AVBufferRef *fifo;
    HANDLE fifo_event;
    pthread_t sender_thread;

    /* Pipe handle and connection state */
    HANDLE pipe_handle;
    HANDLE completion_event;
    OVERLAPPED overlapped;
    int connected;

    /* Cursor state */
    uint8_t visible;
    ArgbCursor cursor;
};

static void dxgi_cursor_free(void *opaque, uint8_t *data)
{
    DxgiCursor *cursor = (DxgiCursor *)data;

    av_free(cursor->data);
    av_free(cursor);
}

static AVBufferRef *dxgi_cursor_alloc()
{
    DxgiCursor *cursor = av_mallocz(sizeof(*cursor));

    return av_buffer_create((uint8_t *)cursor,
                            sizeof(*cursor),
                            dxgi_cursor_free,
                            NULL,
                            0);
}

static void argb_cursor_free(ArgbCursor *cursor)
{
    if (!cursor->data)
        return;

    cursor->width = 0;
    cursor->height = 0;
    cursor->xhot = 0;
    cursor->yhot = 0;
    cursor->size = 0;

    av_freep(&cursor->data);
}

static int write_to_pipe(DxgiCursorHandler *ctx, const void *data, size_t size)
{
    const uint8_t *u8_data = data;
    DWORD write_idx = 0;
    DWORD written = 0;

    while (size > 0) {
        BOOL err = WriteFile(ctx->pipe_handle,
                             u8_data + write_idx,
                             size,
                             &written,
                             &ctx->overlapped);
        if (!err) {
            if (GetLastError() != ERROR_IO_PENDING) {
                sp_log(ctx, SP_LOG_ERROR, "Async write failed: %ld\n",
                       GetLastError());
                return AVERROR_EXTERNAL;
            }

            DWORD wait_err = WaitForSingleObject(ctx->completion_event, 1000);
            if (wait_err != WAIT_OBJECT_0) {
                sp_log(ctx, SP_LOG_ERROR, "Async write completion failed: %ld\n",
                       GetLastError());
                return AVERROR_EXTERNAL;
            }

            err = GetOverlappedResult(ctx->pipe_handle,
                                      &ctx->overlapped,
                                      &written,
                                      FALSE);
            if (!err) {
                sp_log(ctx, SP_LOG_ERROR, "GetOverlappedResult(): %ld\n",
                       GetLastError());
                return AVERROR_EXTERNAL;
            }
        }

        write_idx += written;
        size -= written;
    }

    return 0;
}

static int send_cursor(DxgiCursorHandler *ctx, const POINT *position)
{
    int err;

    err = write_to_pipe(ctx, &ctx->visible, sizeof(ctx->visible));
    if (err < 0)
        return err;

    if (ctx->visible) {
        /**
         * Position
         *
         * The position must actually be updated with the Hotspot.
         * Otherwise the cursor position has a small delta.
         */
        uint32_t pos_array[] = {
            position->x + ctx->cursor.xhot,
            position->y + ctx->cursor.yhot,
        };

        err = write_to_pipe(ctx, pos_array, sizeof(pos_array));
        if (err < 0)
            return err;

        /* Hotspot */
        err = write_to_pipe(ctx, &ctx->cursor.xhot, sizeof(ctx->cursor.xhot));
        if (err < 0)
            return err;

        err = write_to_pipe(ctx, &ctx->cursor.yhot, sizeof(ctx->cursor.yhot));
        if (err < 0)
            return err;

        /* Shape */
        err = write_to_pipe(ctx, &ctx->cursor.width, sizeof(ctx->cursor.width));
        if (err < 0)
            return err;

        err = write_to_pipe(ctx, &ctx->cursor.height, sizeof(ctx->cursor.height));
        if (err < 0)
            return err;

        err = write_to_pipe(ctx, &ctx->cursor.size, sizeof(ctx->cursor.size));
        if (err < 0)
            return err;

        err = write_to_pipe(ctx, ctx->cursor.data, ctx->cursor.size * sizeof(uint32_t));
        if (err < 0)
            return err;
    }

    return 0;
}

static int handle_colored_cursor(ArgbCursor *argb_cursor,
                                 DxgiCursor *dxgi_cursor)
{
    argb_cursor->width = dxgi_cursor->shape_info.Width;
    argb_cursor->height = dxgi_cursor->shape_info.Height;

    argb_cursor->data = (uint32_t *)dxgi_cursor->data;
    dxgi_cursor->data = NULL;

    argb_cursor->size = dxgi_cursor->size / sizeof(uint32_t);

    return 0;
}

static int handle_monochrome_cursor(ArgbCursor *argb_cursor,
                                    DxgiCursor *dxgi_cursor)
{
    /* Allocate cursor */
    argb_cursor->width = dxgi_cursor->shape_info.Width;
    argb_cursor->height = dxgi_cursor->shape_info.Height / 2;

    argb_cursor->size = argb_cursor->width * argb_cursor->height;
    argb_cursor->data = av_mallocz(argb_cursor->size * sizeof(uint32_t));

    /* Create ARGB cursor */
    const uint8_t *xor_mask = dxgi_cursor->data;
    const uint8_t *and_mask = dxgi_cursor->data + dxgi_cursor->size / 2;

    int bit_idx = 8;
    int mask_idx = 0;

    for (int i = 0; i < argb_cursor->size; i++) {
        const int and_bit = (and_mask[mask_idx] >> (bit_idx - 1)) & 1;
        const int xor_bit = (xor_mask[mask_idx] >> (bit_idx - 1)) & 1;

        int draw_white_border = 0;

        /* Draw black pixels */
        if (and_bit == 1 && xor_bit == 1) {
            argb_cursor->data[i] = 0xFF000000;
            draw_white_border = 1;
        } else if (and_bit == 0 && xor_bit == 0) {
            argb_cursor->data[i] = 0xFF000000;
            draw_white_border = 1;
        } else if (and_bit == 1 && xor_bit == 0) {
            argb_cursor->data[i] = 0xFFFFFFFF;
        }

        /* Add a white border to make cursor visible on black backgrounds.
         * Avoid to override black parts */
        if (draw_white_border) {
            const int pos_list[] = {
                i - argb_cursor->width - 1,
                i - argb_cursor->width,
                i - argb_cursor->width + 1,
                i - 1,
                i + 1,
                i + argb_cursor->width - 1,
                i + argb_cursor->width,
                i + argb_cursor->width + 1,
            };

            for (size_t pos_idx = 0; pos_idx < SIZEOF_ARRAY(pos_list); pos_idx++) {
                int pos = pos_list[pos_idx];
                if (pos < 0 || pos >= argb_cursor->size)
                    continue;

                if (argb_cursor->data[pos] != 0xFF000000)
                    argb_cursor->data[pos] = 0xFFFFFFFF;
            }
        }

        bit_idx--;
        if (bit_idx == 0) {
            mask_idx++;
            bit_idx = 8;
        }
    }

    return 0;
}

/**
 * Can be triggered by beam cusor with scaling >= 150%.
 */
static int handle_masked_colored_cursor(ArgbCursor *argb_cursor,
                                        DxgiCursor *dxgi_cursor)
{
    argb_cursor->width = dxgi_cursor->shape_info.Width;
    argb_cursor->height = dxgi_cursor->shape_info.Height;

    argb_cursor->data = (uint32_t *)dxgi_cursor->data;
    dxgi_cursor->data = NULL;

    argb_cursor->size = dxgi_cursor->size / sizeof(uint32_t);

    for (int i = 0; i < argb_cursor->size; i++) {
        /**
         * MSDN: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ne-dxgi1_2-dxgi_outdupl_pointer_shape_type
         *
         * Mask value is 0, the RGB value should replace the screen pixel.
         *
         * Mask value is 0xFF, an XOR operation is performed on the RGB value
         * and the screen pixel.
         *
         * Note: Mask value is the MSB.
         */
        if ((argb_cursor->data[i] >> 24) == 0) {
            argb_cursor->data[i] |= 0xFF000000;
        } else if (argb_cursor->data[i] == 0xFF000000) {
            /**
             * Cursor RGB values are all 0x00, and will be
             * replaced by the screen value.
             */
            argb_cursor->data[i] = 0;
        } else {
            /**
             * Not sure how to trigger this kind of cursor outside of the beam
             * case.
             *
             * Beam cursor values are:
             * - 0xFF000000 for the transparent part
             * - 0xFFFFFFFF for the dark part
             *
             * Let's try something that make the beam cursor visible:
             * - 0xFF000000 should be fully transparent
             * - 0xFFFFFFFF should be dark with a medium transparency
             */
            argb_cursor->data[i] = ~argb_cursor->data[i];
            argb_cursor->data[i] &= 0x00FFFFFF;
            argb_cursor->data[i] |= 0x80000000;
        }
    }

    return 0;
}

static int close_pipe(DxgiCursorHandler *ctx)
{
    if (ctx->completion_event != INVALID_HANDLE_VALUE) {
        CloseHandle(ctx->completion_event);
        ctx->completion_event = INVALID_HANDLE_VALUE;
    }

    if (ctx->pipe_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(ctx->pipe_handle);
        ctx->pipe_handle = INVALID_HANDLE_VALUE;
    }

    return 0;
}

static int create_pipe(DxgiCursorHandler *ctx)
{
    BOOL err;

    ctx->pipe_handle = CreateNamedPipe(
        TEXT("\\\\.\\pipe\\TxprotoCursorPipe"),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE,
        1,
        4096 * 8, /* 4096 is the average cursor size */
        0,
        NMPWAIT_USE_DEFAULT_WAIT,
        NULL);
    if (ctx->pipe_handle == INVALID_HANDLE_VALUE) {
        return AVERROR_EXTERNAL;
    }

    ctx->completion_event = CreateEvent(NULL, TRUE, TRUE, NULL);
    if (!ctx->completion_event) {
        goto fail;
    }

    ctx->overlapped.hEvent = ctx->completion_event;

    err = ConnectNamedPipe(ctx->pipe_handle, &ctx->overlapped);
    if (err == FALSE && GetLastError() != ERROR_IO_PENDING) {
        sp_log(ctx, SP_LOG_ERROR, "ConnectNamedPipe(): %ld\n", GetLastError());
        goto fail;
    }

    return 0;

fail:
    close_pipe(ctx);

    return AVERROR_EXTERNAL;
}

static int handle_cursor(DxgiCursorHandler *ctx, DxgiCursor *handle)
{
    int updated = 0;

    if (!handle->visible) {
        sp_log(ctx, SP_LOG_TRACE, "Hide cursor\n");

        updated = 1;

        /* Keep the previous cursor because it can be shown later */
        ctx->visible = 0;
    } else if (handle->data) {
        sp_log(ctx, SP_LOG_TRACE, "Update cursor shape\n");

        updated = 1;
        ctx->visible = 1;

        /* Get the properties of the new cursor */
        argb_cursor_free(&ctx->cursor);
        ctx->cursor.xhot = handle->shape_info.HotSpot.x;
        ctx->cursor.yhot = handle->shape_info.HotSpot.y;

        switch (handle->shape_info.Type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
            handle_colored_cursor(&ctx->cursor, handle);
            break;

        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
            handle_monochrome_cursor(&ctx->cursor, handle);
            break;

        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
            handle_masked_colored_cursor(&ctx->cursor, handle);
            break;

        default:
            sp_log(ctx, SP_LOG_WARN, "Unexpected shape type\n");
            break;
        }
    } else if (!ctx->visible) {
        sp_log(ctx, SP_LOG_TRACE, "Show cursor\n");
        updated = 1;

        ctx->visible = 1;
    }

    return updated;
}

static void *sender_thread(void *arg)
{
    DxgiCursorHandler *ctx = arg;
    AVBufferRef *ref;
    int err;

    sp_set_thread_name_self(sp_class_get_name(ctx));

    err = create_pipe(ctx);
    if (err < 0)
        return NULL;

    while (1) {
        if (!ctx->connected) {
            HANDLE handles[] = {
                ctx->completion_event,
                ctx->fifo_event
            };

            DWORD ret = WaitForMultipleObjects(SIZEOF_ARRAY(handles), handles, FALSE, 0);
            if (ret == WAIT_TIMEOUT) {
                continue;
            } else if (ret == WAIT_OBJECT_0) {
                sp_log(ctx, SP_LOG_TRACE, "Client connected\n");

                ResetEvent(ctx->completion_event);
                ctx->connected = 1;

                POINT position;
                position.x = 0;
                position.y = 0;

                err = send_cursor(ctx, &position);
                if (err < 0) {
                    close_pipe(ctx);
                    ctx->connected = 0;

                    err = create_pipe(ctx);
                    if (err < 0) {
                        break;
                    }
                }

            } else if (ret == WAIT_OBJECT_0 + 1) {
                sp_log(ctx, SP_LOG_DEBUG, "Got cursor while client isn't connected\n");

                ResetEvent(ctx->fifo_event);

                ref = sp_bufferref_fifo_pop(ctx->fifo);
                if (!ret)
                    break;

                DxgiCursor *handle = (DxgiCursor *)ref->data;
                handle_cursor(ctx, handle);
            } else {
                sp_log(ctx, SP_LOG_ERROR, "WaitForMultipleObjects() failed: %ld\n",
                       GetLastError());
                break;
            }

        } else {
            ref = sp_bufferref_fifo_pop(ctx->fifo);
            if (!ref)
                break;

            DxgiCursor *handle = (DxgiCursor *)ref->data;

            int send = handle_cursor(ctx, handle);
            if (send) {
                err = send_cursor(ctx, &handle->position);
                if (err < 0) {
                    close_pipe(ctx);
                    ctx->connected = 0;

                    err = create_pipe(ctx);
                    if (err < 0) {
                        break;
                    }
                }
            }

            av_buffer_unref(&ref);
        }
    }

    close_pipe(ctx);

    return NULL;
}

int sp_dxgi_cursor_handler_init(DxgiCursorHandler **out_ctx)
{
    DxgiCursorHandler *ctx = av_mallocz(sizeof(*ctx));

    ctx->pipe_handle = INVALID_HANDLE_VALUE;

    int err = sp_class_alloc(ctx, "dxgi_cursor", SP_TYPE_SCRIPT, NULL);
    if (err < 0)
        return err;

    ctx->fifo = sp_bufferref_fifo_create(out_ctx, 16, BUFFERREF_FIFO_BLOCK_NO_INPUT);
    ctx->fifo_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    pthread_create(&ctx->sender_thread, NULL, sender_thread, ctx);

    *out_ctx = ctx;

    return 0;
}

void sp_dxgi_cursor_handler_uninit(DxgiCursorHandler **s)
{
    if (!s)
        return;

    DxgiCursorHandler *ctx = *s;

    sp_bufferref_fifo_push(ctx->fifo, NULL);
    pthread_join(ctx->sender_thread, NULL);

    argb_cursor_free(&ctx->cursor);

    CloseHandle(ctx->fifo_event);
    av_buffer_unref(&ctx->fifo);

    sp_class_free(ctx);
    av_freep(ctx);
}

int sp_dxgi_cursor_handler_send(DxgiCursorHandler *ctx,
                        IDXGIOutputDuplication *output_duplication,
                        const DXGI_OUTDUPL_FRAME_INFO *frame_info)
{
    if (!frame_info->LastMouseUpdateTime.QuadPart)
        return 0;

    AVBufferRef *ref = dxgi_cursor_alloc();
    DxgiCursor *dxgi_cursor = (DxgiCursor *)ref->data;

    const DXGI_OUTDUPL_POINTER_POSITION *position = &frame_info->PointerPosition;
    dxgi_cursor->visible = position->Visible;
    dxgi_cursor->position = position->Position;

    if (frame_info->PointerShapeBufferSize) {
        dxgi_cursor->size = frame_info->PointerShapeBufferSize;
        dxgi_cursor->data = av_mallocz(dxgi_cursor->size);

        UINT required_size;
        HRESULT hr;
        hr = IDXGIOutputDuplication_GetFramePointerShape(output_duplication,
                                                         frame_info->PointerShapeBufferSize,
                                                         dxgi_cursor->data,
                                                         &required_size,
                                                         &dxgi_cursor->shape_info);
        if (FAILED(hr)) {
            return 0;
        }
    }

    sp_bufferref_fifo_push(ctx->fifo, ref);
    SetEvent(ctx->fifo_event);

    av_buffer_unref(&ref);

    return 0;
}