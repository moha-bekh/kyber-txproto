#ifndef IOSYS_DRM_WRAPPER_H_
#define IOSYS_DRM_WRAPPER_H_

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>
#include <libtxproto/log.h>

typedef struct {
    // For example: HDMI-A or HDMI
    uint32_t type;
    // Equals zero if not applicable
    uint32_t crtcId;
    // For example HDMI-A-{index} or HDMI-{index}
    uint32_t index;
    // ID of the connector
    uint32_t connectorId;
    bool connected;
} DrmConnectorInfo;

typedef struct {
    //this should be the first member
    SPClass* class;

    drmModePlane* m_plane;
    int m_fd;
    uint32_t m_id;
} PlaneWrapper;

typedef struct {
    //this should be the first member
    SPClass* class;

    drmModeFB* m_fb;
    drmModeFB2* m_fb2;
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint64_t modifier;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    int planes;
    int dmafd[4];
    int m_fd;
} FbWrapper;

typedef struct {
    //this should be the first member
    SPClass* class;

    int fd;
    drmModePlaneRes* planes;
} DrmCard;

typedef struct {
    char *path; // Full path (e.g., "/dev/drm/card0")
    int number; // Card number (e.g., 0)
} DrmCardPath;

/**
 * Lists DRM card devices in /dev/drm
 * Returns a NULL-terminated array of drm_card_t structures
 * Caller is responsible for freeing the returned array and its strings
 */
DrmCardPath* list_drm_card_paths(void);

//Helper function to free the array returned by list_drm_card_paths
void free_drm_card_paths(DrmCardPath* paths);

// FbWrapper functions
FbWrapper* fb_wrapper_create_empty(void);
FbWrapper* fb_wrapper_create(int fd, uint32_t bufferId);
void fb_wrapper_destroy(FbWrapper* wrapper);
bool fb_wrapper_is_valid(const FbWrapper* wrapper);
int fb_wrapper_handle_fd(const FbWrapper* wrapper, uint32_t handle);
int fb_wrapper_map_dma_buffer(FbWrapper* wrapper);

// PlaneWrapper functions
PlaneWrapper* plane_wrapper_create(int fd, uint32_t id);
void plane_wrapper_destroy(PlaneWrapper* wrapper);
bool plane_wrapper_is_valid(const PlaneWrapper* wrapper);
bool plane_wrapper_is_cursor(const PlaneWrapper* wrapper);
FbWrapper* plane_wrapper_get_frame_buffer(const PlaneWrapper* wrapper);
drmModeCrtc* plane_wrapper_get_crtc(const PlaneWrapper* wrapper);

// DrmCard functions
DrmCard* drm_card_create(void);
void drm_card_destroy(DrmCard* card);
int drm_card_init(DrmCard* card, const char* devicePath);
drmModeEncoder* drm_card_get_encoder(const DrmCard* card, uint32_t encoderId);
drmModeRes* drm_card_get_res(const DrmCard* card);
drmModeConnector* drm_card_get_connector(const DrmCard* card, uint32_t connectorId);
PlaneWrapper* drm_card_get_plane(const DrmCard* card, uint32_t planeId);
int drm_card_get_monitors(const DrmCard* card, DrmConnectorInfo** monitors);
char* drm_card_get_connector_name(const DrmCard* card, uint32_t planeId);

#endif /* IOSYS_DRM_WRAPPER_H_ */
