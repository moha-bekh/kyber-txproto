#include <ctype.h>
#include <sys/capability.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include "iosys_drm_wrapper.h"


#include <libtxproto/utils.h>
#include <libtxproto/log.h>


//// sysadmin scope

static cap_t cap_sys_admin_scope_create(void* ctx, bool verbose) {
    cap_t caps = cap_get_proc();
    if (!caps) {
        if (verbose)
            sp_log(ctx, SP_LOG_ERROR, "failed to get process capabilities\n");
        return NULL;
    }
    cap_value_t sys_admin = CAP_SYS_ADMIN;
    if (cap_set_flag(caps, CAP_EFFECTIVE, 1, &sys_admin, CAP_SET) ||
        cap_set_proc(caps)) {
        if (verbose)
            sp_log(ctx, SP_LOG_ERROR, "failed to get CAP_SYS_ADMIN permission\n");
        cap_free(caps);
        return NULL;
    }
    return caps;
}

static void cap_sys_admin_scope_destroy(void* ctx, cap_t caps) {
    if (!caps)
        return;
    cap_value_t sys_admin = CAP_SYS_ADMIN;
    if (cap_set_flag(caps, CAP_EFFECTIVE, 1, &sys_admin, CAP_CLEAR) ||
        cap_set_proc(caps)) {
        sp_log(ctx, SP_LOG_ERROR, "failed to drop CAP_SYS_ADMIN permission\n");
    }
    cap_free(caps);
}


DrmCardPath* list_drm_card_paths(void) {

    DrmCardPath* cards = NULL;
    int count = 0;
    int capacity = 5;

    const char *base_path = "/dev/dri";

    DIR *dir = opendir(base_path);
    if (dir == NULL) {
        perror("opendir /dev/dri");
        return NULL;
    }

    cards = malloc(capacity * sizeof(DrmCardPath));
    if (cards == NULL) {
        closedir(dir);
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "card", 4) != 0) {
            continue;
        }

        // Check if the character after "card" is a digit
        if (isdigit(entry->d_name[4]) == 0) {
            continue;
        }

        // Verify the rest are digits or end of string
        bool valid = true;
        for (int i = 5; entry->d_name[i] != '\0'; i++) {
            if (isdigit(entry->d_name[i]) == 0) {
                valid = false;
                break;
            }
        }
        if (!valid)
            continue;

        cards[count].path = NULL;
        cards[count].number = -1;

        if (count >= capacity - 1) {
            capacity *= 2;
            DrmCardPath* temp = realloc(cards, capacity * sizeof(DrmCardPath));
            if (temp == NULL) {
                goto error_free_cards;
            }
            cards = temp;
        }

        if (asprintf(&cards[count].path, "%s/%s", base_path, entry->d_name) < 0) {
            cards[count].path = NULL;
            goto error_free_cards;
        }
        cards[count].number = atoi(entry->d_name + 4);
        count++;
    }

    // terminate the array
    cards[count].path = NULL;
    cards[count].number = -1;
    closedir(dir);
    return cards;

error_free_cards:
    for (int i = 0; i < count; i++) {
        free(cards[i].path);
    }
    free(cards);
    closedir(dir);
    return NULL;
}

/**
 * Helper function to free the array returned by list_drm_cards
 */
void free_drm_card_paths(DrmCardPath* cards) {
    if (cards == NULL)
        return;

    for (int i = 0; cards[i].path != NULL; ++i) {
        free(cards[i].path);
    }
    free(cards);
}



// FbWrapper private functions
static bool fb_wrapper_init_fb(FbWrapper* wrapper, drmModeFB* fb) {
    if (!fb) {
        sp_log(wrapper, SP_LOG_ERROR, "drmModeFB is NULL");
        return false;
    }

    wrapper->fb_id = fb->fb_id;
    wrapper->width = fb->width;
    wrapper->height = fb->height;
    wrapper->pixel_format = DRM_FORMAT_XRGB8888;
    wrapper->modifier = DRM_FORMAT_MOD_INVALID;

    memset(wrapper->handles, 0, sizeof(wrapper->handles));
    memset(wrapper->pitches, 0, sizeof(wrapper->pitches));
    memset(wrapper->offsets, 0, sizeof(wrapper->offsets));
    memset(wrapper->dmafd, 0, sizeof(wrapper->dmafd));

    wrapper->handles[0] = fb->handle;
    wrapper->pitches[0] = fb->pitch;
    wrapper->planes = (fb->handle != 0) ? 1 : 0;

    wrapper->m_fb = fb;
    if (wrapper->m_fb2) {
        drmModeFreeFB2(wrapper->m_fb2);
        wrapper->m_fb2 = NULL;
    }

    return true;
}

static bool fb_wrapper_init_fb2(FbWrapper* wrapper, drmModeFB2* fb2) {
    if (!fb2) {
        sp_log(wrapper, SP_LOG_ERROR, "drmModeFB2 is NULL");
        return false;
    }

    wrapper->fb_id = fb2->fb_id;
    wrapper->width = fb2->width;
    wrapper->height = fb2->height;
    wrapper->pixel_format = fb2->pixel_format;
    wrapper->modifier = (fb2->flags & DRM_MODE_FB_MODIFIERS) ? fb2->modifier : DRM_FORMAT_MOD_INVALID;

    memcpy(wrapper->handles, fb2->handles, sizeof(wrapper->handles));
    memcpy(wrapper->pitches, fb2->pitches, sizeof(wrapper->pitches));
    memcpy(wrapper->offsets, fb2->offsets, sizeof(wrapper->offsets));
    memset(wrapper->dmafd, 0, sizeof(wrapper->dmafd));

    wrapper->planes = 0;
    for (int i = 0; i < 4; ++i) {
        if (wrapper->handles[i] == 0)
            break;
        ++wrapper->planes;
    }

    if (wrapper->m_fb) {
        drmModeFreeFB(wrapper->m_fb);
        wrapper->m_fb = NULL;
    }
    wrapper->m_fb2 = fb2;

    return true;
}

// FbWrapper public functions
FbWrapper* fb_wrapper_create_empty(void) {
    FbWrapper* wrapper = malloc(sizeof(FbWrapper));
    if (!wrapper) return NULL;

    memset(wrapper, 0, sizeof(FbWrapper));
    wrapper->m_fd = -1;

    sp_class_alloc(wrapper, "drm_fb", SP_TYPE_CONTEXT, NULL);
    return wrapper;
}

FbWrapper* fb_wrapper_create(int fd, uint32_t bufferId) {
    FbWrapper* wrapper = fb_wrapper_create_empty();
    if (!wrapper)
        return NULL;

    wrapper->m_fd = fd;

    cap_t adminScope = cap_sys_admin_scope_create(wrapper, true);
    bool ret = false;

    drmModeFB2* fb2 = drmModeGetFB2(fd, bufferId);
    if (fb2) {
        ret = fb_wrapper_init_fb2(wrapper, fb2);
    }

    if (!ret) {
        drmModeFB* fb = drmModeGetFB(fd, bufferId);
        if (fb) {
            fb_wrapper_init_fb(wrapper, fb);
        }
    }

    cap_sys_admin_scope_destroy(wrapper, adminScope);
    return wrapper;
}

void fb_wrapper_destroy(FbWrapper* wrapper) {
    assert(wrapper);

    if (wrapper->m_fb) {
        drmModeFreeFB(wrapper->m_fb);
    }
    if (wrapper->m_fb2) {
        drmModeFreeFB2(wrapper->m_fb2);
    }

    // Close DMA file descriptors
    for (int i = 0; i < 4; ++i) {
        if (wrapper->dmafd[i] > 0) {
            close(wrapper->dmafd[i]);
        }
    }

    sp_class_free(wrapper);

    free(wrapper);
}

bool fb_wrapper_is_valid(const FbWrapper* wrapper) {
    return wrapper && (wrapper->m_fb || wrapper->m_fb2);
}

int fb_wrapper_handle_fd(const FbWrapper* wrapper, uint32_t handle) {
    assert(wrapper);

    int fd;
    int ret = drmPrimeHandleToFD(wrapper->m_fd, handle, 0, &fd);
    if (ret != 0) {
        return -1;
    }
    return fd;
}

int fb_wrapper_map_dma_buffer(FbWrapper* wrapper) {
    assert(wrapper);

    int i = 0;
    for (; i < 4; ++i) {
        if (wrapper->handles[i] == 0)
            break;
        int ret = drmPrimeHandleToFD(wrapper->m_fd, wrapper->handles[i], 0, &wrapper->dmafd[i]);
        if (ret != 0) {
            sp_log(wrapper, SP_LOG_ERROR, "drmPrimeHandleToFD failed for plane %d", i);
            return -1;
        }
    }
    for (; i < 4; ++i) {
        wrapper->dmafd[i] = 0;
    }
    return 0;
}

// PlaneWrapper functions
PlaneWrapper* plane_wrapper_create(int fd, uint32_t id) {
    PlaneWrapper* wrapper = malloc(sizeof(PlaneWrapper));
    if (!wrapper) {
        sp_log(NULL, SP_LOG_ERROR, "plane_wrapper_create: out of memory\n");
        return NULL;
    }

    sp_class_alloc(wrapper, "drm_plane", SP_TYPE_CONTEXT, NULL);

    wrapper->m_fd = fd;
    wrapper->m_id = id;
    wrapper->m_plane = drmModeGetPlane(fd, id);

    if (!wrapper->m_plane) {
        sp_log(wrapper, SP_LOG_ERROR, "drm_plane failed fd %i, id %u\n", fd, id);
        free(wrapper);
        return NULL;
    }
    return wrapper;
}

void plane_wrapper_destroy(PlaneWrapper* wrapper) {
    if (!wrapper) return;

    if (wrapper->m_plane) {
        drmModeFreePlane(wrapper->m_plane);
    }
    sp_class_free(wrapper);
    free(wrapper);
}

bool plane_wrapper_is_valid(const PlaneWrapper* wrapper) {
    return wrapper && wrapper->m_plane;
}

bool plane_wrapper_is_cursor(const PlaneWrapper* wrapper) {
    if (!plane_wrapper_is_valid(wrapper)) return false;

    drmModeObjectProperties* properties = drmModeObjectGetProperties(
        wrapper->m_fd, wrapper->m_plane->plane_id, DRM_MODE_OBJECT_PLANE);
    if (!properties) {
        sp_log(wrapper, SP_LOG_ERROR, "failed to get properties for plane %x\n", wrapper->m_plane->plane_id);
        return false;
    }

    bool is_cursor = true; // default assumption
    for (uint32_t i = 0; i < properties->count_props; ++i) {
        drmModePropertyRes* prop = drmModeGetProperty(wrapper->m_fd, properties->props[i]);
        if (prop && strcmp(prop->name, "type") == 0) {
            is_cursor = (properties->prop_values[i] == DRM_PLANE_TYPE_CURSOR);
            drmModeFreeProperty(prop);
            break;
        }
        if (prop) drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(properties);
    return is_cursor;
}

FbWrapper* plane_wrapper_get_frame_buffer(const PlaneWrapper* wrapper) {
    if (!plane_wrapper_is_valid(wrapper)) return NULL;

    uint32_t bufferId = wrapper->m_plane->fb_id;
    if (bufferId == 0) {
        return NULL;
    }
    return fb_wrapper_create(wrapper->m_fd, bufferId);
}

drmModeCrtc* plane_wrapper_get_crtc(const PlaneWrapper* wrapper) {
    if (!plane_wrapper_is_valid(wrapper)) return NULL;
    return drmModeGetCrtc(wrapper->m_fd, wrapper->m_plane->crtc_id);
}


///// DrmCard implementation

DrmCard* drm_card_create(void) {
    DrmCard* card = malloc(sizeof(DrmCard));
    if (!card)
        return NULL;

    memset(card, 0, sizeof(DrmCard));
    card->fd = -1;
    sp_class_alloc(card, "drm_card", SP_TYPE_CONTEXT, NULL);
    return card;
}

void drm_card_destroy(DrmCard* card) {
    if (!card) return;

    if (card->planes) {
        drmModeFreePlaneResources(card->planes);
    }
    if (card->fd >= 0) {
        close(card->fd);
    }
    sp_class_free(card);
    free(card);
}

int drm_card_init(DrmCard* card, const char* devicePath, bool verbose) {
    assert(card);

    cap_t adminScope = cap_sys_admin_scope_create(card, verbose);

    card->fd = open(devicePath, O_RDWR);
    if (card->fd < 0) {
        if (verbose)
            sp_log(card, SP_LOG_ERROR, "failed to open drm device %s", devicePath);
        cap_sys_admin_scope_destroy(NULL, adminScope);
        return -1;
    }

    uint64_t cap = 0;
    drmGetCap(card->fd, DRM_CAP_DUMB_BUFFER, &cap);
    if (cap != 1 && verbose) {
        sp_log(card, SP_LOG_WARN, "DRM_CAP_DUMB_BUFFER is missing");
    }

    if (drmSetClientCap(card->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 && verbose) {
        perror("Cannot tell drm to expose all planes; the rest will very likely fail");
    }

    if (drmSetClientCap(card->fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0 && verbose) {
        sp_log(card, SP_LOG_WARN, "GPU driver doesn't support atomic mode-setting");
    }

    card->planes = drmModeGetPlaneResources(card->fd);
    if (!card->planes) {
        if (verbose)
            sp_log(card, SP_LOG_ERROR, "failed to get plane resources");
        cap_sys_admin_scope_destroy(card, adminScope);
        return -1;
    }

    cap_sys_admin_scope_destroy(card, adminScope);
    return 0;
}

drmModeEncoder* drm_card_get_encoder(const DrmCard* card, uint32_t encoderId) {
    assert(card);
    return drmModeGetEncoder(card->fd, encoderId);
}

drmModeRes* drm_card_get_res(const DrmCard* card) {
    assert(card);
    return drmModeGetResources(card->fd);
}

drmModeConnector* drm_card_get_connector(const DrmCard* card, uint32_t connectorId) {
    assert(card);
    return drmModeGetConnector(card->fd, connectorId);
}

PlaneWrapper* drm_card_get_plane(const DrmCard* card, uint32_t planeId) {
    assert(card);
    return plane_wrapper_create(card->fd, planeId);
}

int drm_card_get_monitors(const DrmCard* card, DrmConnectorInfo** monitors) {
    if (!card || !monitors) return 0;

    drmModeRes* resources = drm_card_get_res(card);
    if (!resources) {
        sp_log(card, SP_LOG_ERROR, "Couldn't get connector resources");
        return 0;
    }

    *monitors = malloc(sizeof(DrmConnectorInfo) * resources->count_connectors);
    if (!*monitors) {
        drmModeFreeResources(resources);
        return 0;
    }

    int count = 0;
    for (int i = 0; i < resources->count_connectors; ++i) {
        uint32_t connectorId = resources->connectors[i];
        drmModeConnector* connector = drm_card_get_connector(card, connectorId);
        if (!connector) continue;

        uint32_t crtcId = 0;
        if (connector->encoder_id != 0) {
            drmModeEncoder* encoder = drm_card_get_encoder(card, connector->encoder_id);
            if (encoder) {
                crtcId = encoder->crtc_id;
                drmModeFreeEncoder(encoder);
            }
        }

        (*monitors)[count].type = connector->connector_type;
        (*monitors)[count].crtcId = crtcId;
        (*monitors)[count].index = i;
        (*monitors)[count].connectorId = connectorId;
        (*monitors)[count].connected = (connector->connection == DRM_MODE_CONNECTED);

        drmModeFreeConnector(connector);
        count++;
    }

    drmModeFreeResources(resources);
    return count;
}

static const char* drm_connector_type_to_name(uint32_t connector_type) {
    switch (connector_type) {
        case DRM_MODE_CONNECTOR_VGA: return "VGA";
        case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
        case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
        case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
        case DRM_MODE_CONNECTOR_Composite: return "Composite";
        case DRM_MODE_CONNECTOR_SVIDEO: return "S-video";
        case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
        case DRM_MODE_CONNECTOR_Component: return "Component";
        case DRM_MODE_CONNECTOR_9PinDIN: return "9PinDIN";
        case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
        case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
        case DRM_MODE_CONNECTOR_TV: return "TV";
        case DRM_MODE_CONNECTOR_eDP: return "eDP";
        case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
        case DRM_MODE_CONNECTOR_DSI: return "DSI";
        case DRM_MODE_CONNECTOR_DPI: return "DPI";
        case DRM_MODE_CONNECTOR_WRITEBACK: return "Writeback";
        case DRM_MODE_CONNECTOR_SPI: return "SPI";
        case DRM_MODE_CONNECTOR_USB: return "USB";
        default: return "Unknown";
    }
}

char* drm_card_get_connector_name(const DrmCard* card, uint32_t planeId) {
    if (!card) return NULL;

    drmModeRes* resources = drm_card_get_res(card);
    if (!resources) return NULL;

    PlaneWrapper* plane = drm_card_get_plane(card, planeId);
    if (!plane || !plane->m_plane) {
        drmModeFreeResources(resources);
        if (plane) plane_wrapper_destroy(plane);
        return NULL;
    }

    uint32_t crtc_id = plane->m_plane->crtc_id;
    plane_wrapper_destroy(plane);

    if (crtc_id == 0) {
        drmModeFreeResources(resources);
        return NULL;
    }

    // Find the connector associated with this CRTC
    for (int i = 0; i < resources->count_connectors; ++i) {
        drmModeConnector* connector = drm_card_get_connector(card, resources->connectors[i]);
        if (!connector) continue;

        if (connector->encoder_id != 0) {
            drmModeEncoder* encoder = drm_card_get_encoder(card, connector->encoder_id);
            if (encoder && encoder->crtc_id == crtc_id) {
                const char* type_name = drm_connector_type_to_name(connector->connector_type);
                char* name = NULL;
                if (asprintf(&name, "%s-%d", type_name, connector->connector_type_id) < 0)
                    name = NULL;

                drmModeFreeEncoder(encoder);
                drmModeFreeConnector(connector);
                drmModeFreeResources(resources);
                return name;
            }
            if (encoder) drmModeFreeEncoder(encoder);
        }
        drmModeFreeConnector(connector);
    }

    drmModeFreeResources(resources);
    return NULL;
}
