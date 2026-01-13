#include <libavutil/buffer.h>

#include <libtxproto/txproto_main.h>
#include <libtxproto/io.h>

#include "iosys_common.h"
#include "libtxproto/log.h"

AVBufferRef *sp_io_alloc(TXMainContext *ctx,
                         const char **api_list,
                         event_fn source_event_cb,
                         event_free source_event_free,
                         size_t callback_ctx_size)
{
    int err = 0;

    if (api_list) {
        sp_log(ctx, SP_LOG_DEBUG, "sp_io_alloc: api_list provided:");
        for (int i = 0; api_list[i]; i++)
            sp_log(ctx, SP_LOG_DEBUG, "  api_list[%d] = \"%s\"", i, api_list[i]);
    } else {
        sp_log(ctx, SP_LOG_DEBUG, "sp_io_alloc: api_list is NULL (all backends)");
    }

    for (int i = 0; (api_list && api_list[i]); i++) {
        int j;
        for (j = 0; j < sp_compiled_apis_len; j++)
            if (!strcmp(api_list[i], sp_compiled_apis[j]->name))
                break;
        if (j == sp_compiled_apis_len) {
            char temp[99] = { 0 };
            snprintf(temp, sizeof(temp), "%s", api_list[i]);
            sp_log(ctx, SP_LOG_ERROR, "API \"%s\" not found!", temp);
            return NULL;
        }
    }

    if (!ctx->io_api_ctx)
        ctx->io_api_ctx = av_mallocz(sizeof(*ctx->io_api_ctx)*sp_compiled_apis_len);

    /* Initialize I/O APIs */
    int initialized_apis = 0, apis_initialized = 0;
    int nvfbc_available = 0;
    int drm_available = 0;

    /* First pass: check if nvfbc/drm is already initialized or try to initialize it first */
    for (int i = 0; i < sp_compiled_apis_len; i++) {
        if (!strcmp(sp_compiled_apis[i]->name, "nvfbc") || !strcmp(sp_compiled_apis[i]->name, "drm")) {
            int *available = !strcmp(sp_compiled_apis[i]->name, "nvfbc") ? &nvfbc_available : &drm_available;
            if (ctx->io_api_ctx[i]) {
                sp_log(ctx, SP_LOG_DEBUG, "First pass: \"%s\" already initialized", sp_compiled_apis[i]->name);
                *available = 1;
            } else {
                int found = 0;
                for (int j = 0; (api_list && api_list[j]); j++) {
                    if (!strcmp(api_list[j], sp_compiled_apis[i]->name)) {
                        found = 1;
                        break;
                    }
                }
                if (!api_list || found) {
                    sp_log(ctx, SP_LOG_DEBUG, "First pass: initializing \"%s\"...", sp_compiled_apis[i]->name);
                    err = sp_compiled_apis[i]->init_sys(&ctx->io_api_ctx[i]);
                    if (err == 0) {
                        sp_log(ctx, SP_LOG_DEBUG, "First pass: \"%s\" initialized OK", sp_compiled_apis[i]->name);
                        *available = 1;
                        initialized_apis++;
                        apis_initialized++;
                    } else if (api_list && err != AVERROR(ENOSYS)) {
                        sp_log(ctx, SP_LOG_ERROR, "Unable to load API \"%s\": %s!",
                               sp_compiled_apis[i]->name, av_err2str(err));
                        return NULL;
                    } else {
                        sp_log(ctx, SP_LOG_DEBUG, "First pass: \"%s\" failed: %s", sp_compiled_apis[i]->name, av_err2str(err));
                    }
                } else {
                    sp_log(ctx, SP_LOG_DEBUG, "First pass: skipping \"%s\" (not in api_list)", sp_compiled_apis[i]->name);
                }
            }
        }
    }

    /* Second pass: initialize remaining APIs, skip xcb/wayland if nvfbc/drm is available */
    for (int i = 0; i < sp_compiled_apis_len; i++) {
        if (!strcmp(sp_compiled_apis[i]->name, "nvfbc") || !strcmp(sp_compiled_apis[i]->name, "drm"))
            continue; /* Already handled */

        if (ctx->io_api_ctx[i]) {
            initialized_apis++;
            continue;
        }

        /* Skip xcb and wayland if nvfbc or drm is available */
        if ((nvfbc_available || drm_available) &&
            (!strcmp(sp_compiled_apis[i]->name, "xcb") ||
             !strcmp(sp_compiled_apis[i]->name, "wayland"))) {
            sp_log(ctx, SP_LOG_DEBUG, "Skipping %s backend (%s available)",
                   sp_compiled_apis[i]->name, nvfbc_available ? "nvfbc" : "drm");
            continue;
        }

        int found = 0;
        for (int j = 0; (api_list && api_list[j]); j++) {
            if (!strcmp(api_list[j], sp_compiled_apis[i]->name)) {
                found = 1;
                break;
            }
        }
        if (api_list && !found) {
            sp_log(ctx, SP_LOG_DEBUG, "Second pass: skipping \"%s\" (not in api_list)", sp_compiled_apis[i]->name);
            continue;
        }

        sp_log(ctx, SP_LOG_DEBUG, "Second pass: initializing \"%s\"...", sp_compiled_apis[i]->name);
        err = sp_compiled_apis[i]->init_sys(&ctx->io_api_ctx[i]);
        if (!api_list && (err == AVERROR(ENOSYS) || err == AVERROR(ENOTSUP))) {
            sp_log(ctx, SP_LOG_DEBUG, "Second pass: \"%s\" not available (%s)", sp_compiled_apis[i]->name, av_err2str(err));
            continue;
        } else if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to load API \"%s\": %s!",
                   sp_compiled_apis[i]->name, av_err2str(err));
            return NULL;
        }

        sp_log(ctx, SP_LOG_DEBUG, "Second pass: \"%s\" initialized OK", sp_compiled_apis[i]->name);
        initialized_apis++;
        apis_initialized++;
    }

    if (!initialized_apis) {
        if (api_list)
            sp_log(ctx, SP_LOG_ERROR, "No requested I/O API(s) available of the %d enabled at build time.\n",
                   sp_compiled_apis_len);
        else
            sp_log(ctx, SP_LOG_WARN, "No I/O APIs available.\n");

        return NULL;
    } else if (apis_initialized) {
        sp_log(ctx, SP_LOG_DEBUG, "%i I/O(s) initialized.\n", apis_initialized);
    }

    /* Prepare event */
    SPEventType type = SP_EVENT_TYPE_SOURCE    | SP_EVENT_ON_CHANGE |
                       SP_EVENT_FLAG_IMMEDIATE | SP_EVENT_FLAG_UNIQUE;

    return sp_event_create(source_event_cb,
                           source_event_free,
                           callback_ctx_size,
                           NULL,
                           type,
                           ctx,
                           NULL);
}

int sp_io_init(TXMainContext *ctx,
               AVBufferRef *source_event,
               const char **api_list)
{
    int err;

    /* Initialize APIs */
    sp_log(ctx, SP_LOG_DEBUG, "sp_io_init: dispatching events to initialized backends");
    for (int i = 0; i < sp_compiled_apis_len; i++) {
        if (!ctx->io_api_ctx[i]) {
            sp_log(ctx, SP_LOG_DEBUG, "sp_io_init: \"%s\" not initialized, skip", sp_compiled_apis[i]->name);
            continue;
        }
        int found = 0;
        for (int j = 0; (api_list && api_list[j]); j++) {
            if (!strcmp(api_list[j], sp_compiled_apis[i]->name)) {
                found = 1;
                break;
            }
        }
        if (api_list && !found) {
            sp_log(ctx, SP_LOG_DEBUG, "sp_io_init: \"%s\" not in api_list, skip", sp_compiled_apis[i]->name);
            continue;
        }

        /* Add and immediately run the event to signal all devices */
        sp_log(ctx, SP_LOG_DEBUG, "sp_io_init: dispatching event to \"%s\"", sp_compiled_apis[i]->name);
        err = sp_compiled_apis[i]->ctrl(ctx->io_api_ctx[i],
                                        SP_EVENT_CTRL_NEW_EVENT | SP_EVENT_FLAG_IMMEDIATE,
                                        source_event);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to add event to API \"%s\": %s!",
                   sp_compiled_apis[i]->name, av_err2str(err));
            return err;
        }
        sp_log(ctx, SP_LOG_DEBUG, "sp_io_init: \"%s\" event dispatched OK", sp_compiled_apis[i]->name);
    }

    sp_bufferlist_append_noref(ctx->ext_buf_refs, source_event);

    return 0;
}

AVBufferRef *sp_io_create(TXMainContext *ctx,
                          uint32_t identifier,
                          AVDictionary *opts)
{
    int i = 0;
    AVBufferRef *entry = NULL;

    /* Check if a specific grab backend is requested */
    AVDictionaryEntry *backend_entry = av_dict_get(opts, "grab_backend", NULL, 0);
    const char *requested_backend = backend_entry ? backend_entry->value : NULL;

    sp_log(ctx, SP_LOG_DEBUG, "sp_io_create: identifier=0x%X, requested_backend=%s",
           identifier, requested_backend ? requested_backend : "(none)");

    for (; i < sp_compiled_apis_len; i++) {
        if (!ctx->io_api_ctx[i]) {
            sp_log(ctx, SP_LOG_DEBUG, "sp_io_create: \"%s\" not initialized, skip",
                   sp_compiled_apis[i]->name);
            continue;
        }

        /* When a specific backend is requested, skip all other video backends */
        if (requested_backend) {
            static const char *video_backends[] = {
                "drm", "nvfbc", "wayland", "xcb", "dxgi", NULL
            };
            int is_video = 0;
            for (int j = 0; video_backends[j]; j++) {
                if (strcmp(sp_compiled_apis[i]->name, video_backends[j]) == 0) {
                    is_video = 1;
                    break;
                }
            }
            if (is_video && strcmp(sp_compiled_apis[i]->name, requested_backend) != 0) {
                sp_log(ctx, SP_LOG_DEBUG, "sp_io_create: skipping video backend \"%s\" (requested: %s)",
                       sp_compiled_apis[i]->name, requested_backend);
                continue;
            }
        }

        sp_log(ctx, SP_LOG_DEBUG, "sp_io_create: searching \"%s\" for entry 0x%X",
               sp_compiled_apis[i]->name, identifier);
        if ((entry = sp_compiled_apis[i]->ref_entry(ctx->io_api_ctx[i], identifier)))
            break;
    }

    if (!entry) {
        if (requested_backend)
            sp_log(ctx, SP_LOG_ERROR, "Entry 0x%X not found with backend '%s'!\n",
                   identifier, requested_backend);
        else
            sp_log(ctx, SP_LOG_ERROR, "Entry 0x%X not found!\n", identifier);
        return NULL;
    }

    if (requested_backend) {
        sp_log(ctx, SP_LOG_INFO, "Using '%s' backend for video capture",
               sp_compiled_apis[i]->name);
    }

    sp_log(ctx, SP_LOG_DEBUG, "Using '%s' backend for entry 0x%X\n",
           sp_compiled_apis[i]->name, identifier);

    int err = sp_compiled_apis[i]->init_io(ctx->io_api_ctx[i], entry, opts);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to init IO: %s!", av_err2str(err));
        av_buffer_unref(&entry);
        return NULL;
    }

    struct tx_metrics *metrics = tx_get_metrics(ctx);
    if (metrics && sp_compiled_apis[i]->set_metrics)
        sp_compiled_apis[i]->set_metrics(ctx->io_api_ctx[i], metrics);

    sp_bufferlist_append_noref(ctx->ext_buf_refs, entry);

    return entry;
}
