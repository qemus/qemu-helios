#ifndef HELIOS_VULKAN_READBACK_H
#define HELIOS_VULKAN_READBACK_H

#include "ui/dmabuf.h"
#include "ui/surface.h"

typedef struct HeliosVulkanReadback HeliosVulkanReadback;
typedef struct HeliosVulkanReadbackCache HeliosVulkanReadbackCache;
typedef struct HeliosVulkanReadbackRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} HeliosVulkanReadbackRect;

HeliosVulkanReadback *helios_vulkan_readback_new(QemuDmaBuf *dmabuf,
                                                 bool direct_optimal);
bool helios_vulkan_readback_matches(HeliosVulkanReadback *readback,
                                    QemuDmaBuf *dmabuf,
                                    bool direct_optimal);
HeliosVulkanReadbackCache *helios_vulkan_readback_cache_new(void);
void helios_vulkan_readback_cache_free(HeliosVulkanReadbackCache *cache);
HeliosVulkanReadback *helios_vulkan_readback_cache_activate(
    HeliosVulkanReadbackCache *cache, QemuDmaBuf *dmabuf,
    bool direct_optimal);
void helios_vulkan_readback_cache_deactivate(
    HeliosVulkanReadbackCache *cache, QemuDmaBuf *dmabuf);
HeliosVulkanReadback *helios_vulkan_readback_cache_active(
    HeliosVulkanReadbackCache *cache);
void helios_vulkan_readback_free(HeliosVulkanReadback *readback);
/*
 * Identity of the DMA-BUF this readback IMPORTED, captured at import time --
 * i.e. the memory the next flush will physically read, independent of whatever
 * the caller believes is currently bound.
 */
void helios_vulkan_readback_identity(HeliosVulkanReadback *readback,
                                     uint64_t *ino, uint64_t *size,
                                     uint64_t *flushes);
/*
 * Capture the producer image into a device-local latest-frame snapshot. A
 * capture always completes the external-ownership round trip before returning;
 * a non-NULL publish rectangle additionally transfers the snapshot to the CPU
 * surface. The two rectangles may differ so accumulated display damage does
 * not inflate every high-rate image capture.
 */
bool helios_vulkan_readback_capture(HeliosVulkanReadback *readback,
                                    DisplaySurface *surface,
                                    const HeliosVulkanReadbackRect *capture,
                                    const HeliosVulkanReadbackRect *publish);

/*
 * Interactive local displays consume every update and keep the original
 * synchronous behavior; only egl-headless applies remote-output pacing.
 */
static inline bool helios_vulkan_readback_flush(
    HeliosVulkanReadback *readback, DisplaySurface *surface,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    HeliosVulkanReadbackRect rect = { x, y, width, height };

    return helios_vulkan_readback_capture(readback, surface, &rect, &rect);
}

/*
 * Publish a previously captured rectangle, including a final frame after
 * input stops. Returns false when the rectangle is invalid or on failure.
 */
bool helios_vulkan_readback_publish(HeliosVulkanReadback *readback,
                                    DisplaySurface *surface,
                                    const HeliosVulkanReadbackRect *rect);

#endif
