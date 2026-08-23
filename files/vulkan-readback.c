#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/vulkan-readback.h"
#include "standard-headers/drm/drm_fourcc.h"
#include "trace.h"

#include <vulkan/vulkan.h>

struct HeliosVulkanReadback {
    QemuDmaBuf *dmabuf;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkImage image;
    VkDeviceMemory image_memory;
    VkBuffer snapshot;
    VkDeviceMemory snapshot_memory;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkFence fence;
    uint8_t *staging_map;
    uint32_t backing_width;
    uint32_t backing_height;
    uint32_t visible_width;
    uint32_t visible_height;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t fourcc;
    uint64_t modifier;
    dev_t dmabuf_dev;
    ino_t dmabuf_ino;
    uint64_t dmabuf_size;
    VkImageLayout producer_layout;
    bool external_ownership;
    bool direct_optimal;
    bool swap_red_blue;
    bool failed;
    uint64_t flushes;
    uint64_t publishes;
};

#define HELIOS_VK_READBACK_CACHE_SIZE 8

struct HeliosVulkanReadbackCache {
    HeliosVulkanReadback *entries[HELIOS_VK_READBACK_CACHE_SIZE];
    HeliosVulkanReadback *active;
    QemuDmaBuf *active_dmabuf;
    uint32_t next;
};

HeliosVulkanReadbackCache *helios_vulkan_readback_cache_new(void)
{
    return g_new0(HeliosVulkanReadbackCache, 1);
}

void helios_vulkan_readback_cache_free(HeliosVulkanReadbackCache *cache)
{
    if (!cache) {
        return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(cache->entries); i++) {
        g_clear_pointer(&cache->entries[i], helios_vulkan_readback_free);
    }
    g_free(cache);
}

HeliosVulkanReadback *helios_vulkan_readback_cache_activate(
    HeliosVulkanReadbackCache *cache, QemuDmaBuf *dmabuf,
    bool direct_optimal)
{
    HeliosVulkanReadback *readback;
    uint32_t slot;

    if (!cache || !dmabuf) {
        return NULL;
    }

    cache->active = NULL;
    cache->active_dmabuf = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(cache->entries); i++) {
        readback = cache->entries[i];
        if (helios_vulkan_readback_matches(readback, dmabuf,
                                           direct_optimal)) {
            cache->active = readback;
            cache->active_dmabuf = dmabuf;
            return readback;
        }
    }

    readback = helios_vulkan_readback_new(dmabuf, direct_optimal);
    if (!readback) {
        return NULL;
    }

    slot = cache->next++ % ARRAY_SIZE(cache->entries);
    g_clear_pointer(&cache->entries[slot], helios_vulkan_readback_free);
    cache->entries[slot] = readback;
    cache->active = readback;
    cache->active_dmabuf = dmabuf;
    return readback;
}

void helios_vulkan_readback_cache_deactivate(
    HeliosVulkanReadbackCache *cache, QemuDmaBuf *dmabuf)
{
    if (cache && (!dmabuf || cache->active_dmabuf == dmabuf)) {
        cache->active = NULL;
        cache->active_dmabuf = NULL;
    }
}

HeliosVulkanReadback *helios_vulkan_readback_cache_active(
    HeliosVulkanReadbackCache *cache)
{
    return cache ? cache->active : NULL;
}

void helios_vulkan_readback_identity(HeliosVulkanReadback *readback,
                                     uint64_t *ino, uint64_t *size,
                                     uint64_t *flushes)
{
    if (ino) {
        *ino = readback ? (uint64_t)readback->dmabuf_ino : 0;
    }
    if (size) {
        *size = readback ? readback->dmabuf_size : 0;
    }
    if (flushes) {
        *flushes = readback ? readback->flushes : 0;
    }
}

bool helios_vulkan_readback_matches(HeliosVulkanReadback *readback,
                                    QemuDmaBuf *dmabuf,
                                    bool direct_optimal)
{
    const int *fds;
    struct stat st;
    int nfds;

    if (!readback || readback->failed || !dmabuf ||
        readback->direct_optimal != direct_optimal ||
        readback->backing_width != qemu_dmabuf_get_backing_width(dmabuf) ||
        readback->backing_height != qemu_dmabuf_get_backing_height(dmabuf) ||
        readback->visible_width != qemu_dmabuf_get_width(dmabuf) ||
        readback->visible_height != qemu_dmabuf_get_height(dmabuf) ||
        readback->origin_x != qemu_dmabuf_get_x(dmabuf) ||
        readback->origin_y != qemu_dmabuf_get_y(dmabuf) ||
        readback->fourcc != qemu_dmabuf_get_fourcc(dmabuf) ||
        readback->modifier != qemu_dmabuf_get_modifier(dmabuf)) {
        return false;
    }

    fds = qemu_dmabuf_get_fds(dmabuf, &nfds);
    return nfds >= 1 && fds[0] >= 0 && fstat(fds[0], &st) == 0 &&
           readback->dmabuf_dev == st.st_dev &&
           readback->dmabuf_ino == st.st_ino &&
           readback->dmabuf_size == qemu_dmabuf_get_allocation_size(dmabuf);
}

static uint32_t find_memory_type(VkPhysicalDevice physical_device,
                                 uint32_t type_bits,
                                 VkMemoryPropertyFlags required)
{
    VkPhysicalDeviceMemoryProperties properties;

    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    return UINT32_MAX;
}

void helios_vulkan_readback_free(HeliosVulkanReadback *readback)
{
    if (!readback) {
        return;
    }

    if (readback->device) {
        vkDeviceWaitIdle(readback->device);
    }
    if (readback->staging_map) {
        vkUnmapMemory(readback->device, readback->staging_memory);
    }
    if (readback->fence) {
        vkDestroyFence(readback->device, readback->fence, NULL);
    }
    if (readback->command_pool) {
        vkDestroyCommandPool(readback->device, readback->command_pool, NULL);
    }
    if (readback->staging) {
        vkDestroyBuffer(readback->device, readback->staging, NULL);
    }
    if (readback->staging_memory) {
        vkFreeMemory(readback->device, readback->staging_memory, NULL);
    }
    if (readback->snapshot) {
        vkDestroyBuffer(readback->device, readback->snapshot, NULL);
    }
    if (readback->snapshot_memory) {
        vkFreeMemory(readback->device, readback->snapshot_memory, NULL);
    }
    if (readback->image) {
        vkDestroyImage(readback->device, readback->image, NULL);
    }
    if (readback->image_memory) {
        vkFreeMemory(readback->device, readback->image_memory, NULL);
    }
    if (readback->device) {
        vkDestroyDevice(readback->device, NULL);
    }
    if (readback->instance) {
        vkDestroyInstance(readback->instance, NULL);
    }
    g_free(readback);
}

HeliosVulkanReadback *helios_vulkan_readback_new(QemuDmaBuf *dmabuf,
                                                 bool direct_optimal)
{
    HeliosVulkanReadback *readback = NULL;
    VkPhysicalDevice *physical_devices = NULL;
    VkQueueFamilyProperties *queue_families = NULL;
    const int *fds;
    uint32_t physical_device_count = 0;
    uint32_t queue_family_count = 0;
    uint32_t fourcc, num_planes;
    uint64_t modifier;
    uint64_t dmabuf_size = 0;
    int nfds;
    int import_fd = -1;
    VkResult result;

#define VK_NEW(call) do {                                                   \
    result = (call);                                                        \
    if (result != VK_SUCCESS) {                                             \
        error_report("vulkan-readback: %s failed: %d", #call, result);     \
        goto fail;                                                          \
    }                                                                       \
} while (0)

    num_planes = qemu_dmabuf_get_num_planes(dmabuf);
    fourcc = qemu_dmabuf_get_fourcc(dmabuf);
    modifier = qemu_dmabuf_get_modifier(dmabuf);
    if (num_planes != 1 ||
        (fourcc != DRM_FORMAT_XRGB8888 && fourcc != DRM_FORMAT_ARGB8888 &&
         fourcc != DRM_FORMAT_XBGR8888 && fourcc != DRM_FORMAT_ABGR8888) ||
        (!direct_optimal &&
         modifier != DRM_FORMAT_MOD_INVALID &&
         modifier != DRM_FORMAT_MOD_LINEAR)) {
        return NULL;
    }

    fds = qemu_dmabuf_get_fds(dmabuf, &nfds);
    if (nfds < 1 || fds[0] < 0) {
        return NULL;
    }

    if (direct_optimal) {
        dmabuf_size = qemu_dmabuf_get_allocation_size(dmabuf);
        if (!dmabuf_size) {
            return NULL;
        }
    }

    readback = g_new0(HeliosVulkanReadback, 1);
    readback->dmabuf = dmabuf;
    readback->backing_width = qemu_dmabuf_get_backing_width(dmabuf);
    readback->backing_height = qemu_dmabuf_get_backing_height(dmabuf);
    readback->visible_width = qemu_dmabuf_get_width(dmabuf);
    readback->visible_height = qemu_dmabuf_get_height(dmabuf);
    readback->origin_x = qemu_dmabuf_get_x(dmabuf);
    readback->origin_y = qemu_dmabuf_get_y(dmabuf);
    readback->fourcc = fourcc;
    readback->modifier = modifier;
    {
        struct stat st;

        if (fstat(fds[0], &st) < 0) {
            goto fail;
        }
        readback->dmabuf_dev = st.st_dev;
        readback->dmabuf_ino = st.st_ino;
        readback->dmabuf_size = qemu_dmabuf_get_allocation_size(dmabuf);
    }
    readback->direct_optimal = direct_optimal;
    readback->swap_red_blue =
        fourcc == DRM_FORMAT_XBGR8888 || fourcc == DRM_FORMAT_ABGR8888;
    readback->producer_layout = direct_optimal
        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        : VK_IMAGE_LAYOUT_GENERAL;
    readback->external_ownership = direct_optimal;
    if (!readback->backing_width || !readback->backing_height ||
        !readback->visible_width || !readback->visible_height ||
        readback->origin_x >= readback->backing_width ||
        readback->origin_y >= readback->backing_height ||
        readback->visible_width >
            readback->backing_width - readback->origin_x ||
        readback->visible_height >
            readback->backing_height - readback->origin_y) {
        goto fail;
    }

    VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "qemu-helios-readback",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
    };
    VK_NEW(vkCreateInstance(&instance_info, NULL, &readback->instance));

    VK_NEW(vkEnumeratePhysicalDevices(readback->instance,
                                      &physical_device_count, NULL));
    if (!physical_device_count) {
        error_report("vulkan-readback: Vulkan has no physical device");
        goto fail;
    }
    physical_devices = g_new0(VkPhysicalDevice, physical_device_count);
    VK_NEW(vkEnumeratePhysicalDevices(readback->instance,
                                      &physical_device_count,
                                      physical_devices));
    for (uint32_t i = 0; i < physical_device_count; i++) {
        VkPhysicalDeviceProperties properties;

        vkGetPhysicalDeviceProperties(physical_devices[i], &properties);
        if (!readback->physical_device || properties.vendorID == 0x10de) {
            readback->physical_device = physical_devices[i];
        }
        if (properties.vendorID == 0x10de) {
            error_report("vulkan-readback: device: %s",
                         properties.deviceName);
            break;
        }
    }
    g_clear_pointer(&physical_devices, g_free);

    vkGetPhysicalDeviceQueueFamilyProperties(readback->physical_device,
                                             &queue_family_count, NULL);
    queue_families = g_new0(VkQueueFamilyProperties, queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(readback->physical_device,
                                             &queue_family_count,
                                             queue_families);
    readback->queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < queue_family_count; i++) {
        if (queue_families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
            readback->queue_family = i;
            break;
        }
    }
    g_clear_pointer(&queue_families, g_free);
    if (readback->queue_family == UINT32_MAX) {
        error_report("vulkan-readback: Vulkan has no transfer queue");
        goto fail;
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = readback->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    const char *device_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = ARRAY_SIZE(device_extensions),
        .ppEnabledExtensionNames = device_extensions,
    };
    VK_NEW(vkCreateDevice(readback->physical_device, &device_info, NULL,
                          &readback->device));
    vkGetDeviceQueue(readback->device, readback->queue_family, 0,
                     &readback->queue);

    VkExternalMemoryImageCreateInfo external_image = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
            readback->device, "vkGetMemoryFdPropertiesKHR");
    if (!get_memory_fd_properties) {
        error_report("vulkan-readback: vkGetMemoryFdPropertiesKHR unavailable");
        goto fail;
    }
    VkMemoryFdPropertiesKHR fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    VK_NEW(get_memory_fd_properties(
        readback->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        fds[0], &fd_properties));

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = readback->swap_red_blue
            ? VK_FORMAT_R8G8B8A8_UNORM
            : VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {
            readback->backing_width,
            readback->backing_height,
            1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = direct_optimal
            ? VK_IMAGE_TILING_OPTIMAL
            : VK_IMAGE_TILING_LINEAR,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = direct_optimal
            ? VK_IMAGE_LAYOUT_UNDEFINED
            : VK_IMAGE_LAYOUT_PREINITIALIZED,
    };
    const VkImageUsageFlags image_usages[] = {
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };
    uint32_t image_usage_count = direct_optimal ? 1 : ARRAY_SIZE(image_usages);
    if (direct_optimal) {
        image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }
    for (uint32_t i = 0; i < image_usage_count; i++) {
        VkMemoryRequirements image_requirements;

        image_info.usage = image_usages[i];
        result = vkCreateImage(readback->device, &image_info, NULL,
                               &readback->image);
        if (result != VK_SUCCESS) {
            readback->image = VK_NULL_HANDLE;
            continue;
        }
        vkGetImageMemoryRequirements(readback->device, readback->image,
                                     &image_requirements);
        if (direct_optimal && image_requirements.size != dmabuf_size) {
            error_report("vulkan-readback: OPTIMAL DMA-BUF shape mismatch "
                         "required=%" PRIu64 " fd_size=%" PRIu64,
                         (uint64_t)image_requirements.size, dmabuf_size);
            vkDestroyImage(readback->device, readback->image, NULL);
            readback->image = VK_NULL_HANDLE;
            continue;
        }
        uint32_t image_memory_type = find_memory_type(
            readback->physical_device,
            image_requirements.memoryTypeBits & fd_properties.memoryTypeBits,
            0);
        if (image_memory_type == UINT32_MAX) {
            vkDestroyImage(readback->device, readback->image, NULL);
            readback->image = VK_NULL_HANDLE;
            continue;
        }

        import_fd = dup(fds[0]);
        if (import_fd < 0) {
            error_report("vulkan-readback: DMA-BUF dup failed: %s",
                         strerror(errno));
            goto fail;
        }
        VkImportMemoryFdInfoKHR import_info = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            .fd = import_fd,
        };
        VkMemoryDedicatedAllocateInfo dedicated_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = &import_info,
            .image = readback->image,
        };
        VkMemoryAllocateInfo image_memory_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicated_info,
            .allocationSize = image_requirements.size,
            .memoryTypeIndex = image_memory_type,
        };
        result = vkAllocateMemory(readback->device, &image_memory_info, NULL,
                                  &readback->image_memory);
        if (result != VK_SUCCESS) {
            close(import_fd);
            import_fd = -1;
            vkDestroyImage(readback->device, readback->image, NULL);
            readback->image = VK_NULL_HANDLE;
            continue;
        }
        import_fd = -1;
        VK_NEW(vkBindImageMemory(readback->device, readback->image,
                                 readback->image_memory, 0));
        error_report("vulkan-readback: DMA-BUF import tiling=%s "
                     "usage=0x%x flags=0x%x size=%" PRIu64
                     " modifier=0x%" PRIx64,
                     direct_optimal ? "OPTIMAL" : "LINEAR",
                     image_info.usage, image_info.flags,
                     (uint64_t)image_requirements.size, modifier);
        break;
    }
    if (!readback->image || !readback->image_memory) {
        error_report("vulkan-readback: DMA-BUF import rejected all usages");
        goto fail;
    }

    VkDeviceSize staging_size =
        (VkDeviceSize)readback->backing_width * readback->backing_height * 4;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = staging_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_NEW(vkCreateBuffer(readback->device, &buffer_info, NULL,
                          &readback->snapshot));
    VkMemoryRequirements snapshot_requirements;
    vkGetBufferMemoryRequirements(readback->device, readback->snapshot,
                                  &snapshot_requirements);
    uint32_t snapshot_memory_type = find_memory_type(
        readback->physical_device, snapshot_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (snapshot_memory_type == UINT32_MAX) {
        error_report("vulkan-readback: no device-local snapshot memory type");
        goto fail;
    }
    VkMemoryAllocateInfo snapshot_memory_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = snapshot_requirements.size,
        .memoryTypeIndex = snapshot_memory_type,
    };
    VK_NEW(vkAllocateMemory(readback->device, &snapshot_memory_info, NULL,
                            &readback->snapshot_memory));
    VK_NEW(vkBindBufferMemory(readback->device, readback->snapshot,
                              readback->snapshot_memory, 0));

    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_NEW(vkCreateBuffer(readback->device, &buffer_info, NULL,
                          &readback->staging));
    VkMemoryRequirements staging_requirements;
    vkGetBufferMemoryRequirements(readback->device, readback->staging,
                                  &staging_requirements);
    uint32_t staging_memory_type = find_memory_type(
        readback->physical_device, staging_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (staging_memory_type == UINT32_MAX) {
        error_report("vulkan-readback: no cached Vulkan staging memory type");
        goto fail;
    }
    VkMemoryAllocateInfo staging_memory_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = staging_requirements.size,
        .memoryTypeIndex = staging_memory_type,
    };
    VK_NEW(vkAllocateMemory(readback->device, &staging_memory_info, NULL,
                            &readback->staging_memory));
    VK_NEW(vkBindBufferMemory(readback->device, readback->staging,
                              readback->staging_memory, 0));
    VK_NEW(vkMapMemory(readback->device, readback->staging_memory, 0,
                       staging_size, 0, (void **)&readback->staging_map));

    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = readback->queue_family,
    };
    VK_NEW(vkCreateCommandPool(readback->device, &command_pool_info, NULL,
                               &readback->command_pool));
    VkCommandBufferAllocateInfo command_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = readback->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_NEW(vkAllocateCommandBuffers(readback->device, &command_buffer_info,
                                    &readback->command_buffer));
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_NEW(vkCreateFence(readback->device, &fence_info, NULL,
                         &readback->fence));

    error_report("vulkan-readback: %s DMA-BUF ready %ux%u "
                 "modifier=0x%" PRIx64,
                 direct_optimal ? "OPTIMAL" : "LINEAR",
                 readback->backing_width, readback->backing_height, modifier);
    return readback;

fail:
    g_free(physical_devices);
    g_free(queue_families);
    if (import_fd >= 0) {
        close(import_fd);
    }
    helios_vulkan_readback_free(readback);
    return NULL;

#undef VK_NEW
}

static void helios_vulkan_readback_copy_to_surface(
    HeliosVulkanReadback *readback, DisplaySurface *surface,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    uint32_t src_x = readback->origin_x + x;
    uint32_t src_y = readback->origin_y + y;
    uint8_t *src = readback->staging_map +
        ((size_t)src_y * readback->backing_width + src_x) * 4;
    uint8_t *dst = surface_data(surface) +
        (size_t)y * surface_stride(surface) + (size_t)x * 4;

    for (uint32_t row = 0; row < height; row++) {
        if (readback->swap_red_blue) {
            for (uint32_t col = 0; col < width; col++) {
                dst[col * 4 + 0] = src[col * 4 + 2];
                dst[col * 4 + 1] = src[col * 4 + 1];
                dst[col * 4 + 2] = src[col * 4 + 0];
                dst[col * 4 + 3] = src[col * 4 + 3];
            }
        } else {
            memcpy(dst, src, (size_t)width * 4);
        }
        src += (size_t)readback->backing_width * 4;
        dst += surface_stride(surface);
    }
}

static bool helios_vulkan_readback_clip_rect(
    HeliosVulkanReadback *readback, DisplaySurface *surface,
    const HeliosVulkanReadbackRect *rect,
    HeliosVulkanReadbackRect *clipped)
{
    if (!readback || !surface || !rect || !rect->width || !rect->height ||
        rect->x >= surface_width(surface) ||
        rect->y >= surface_height(surface) ||
        rect->x >= readback->visible_width ||
        rect->y >= readback->visible_height) {
        return false;
    }

    *clipped = *rect;
    clipped->width = MIN(
        clipped->width,
        MIN(readback->visible_width - clipped->x,
            (uint32_t)surface_width(surface) - clipped->x));
    clipped->height = MIN(
        clipped->height,
        MIN(readback->visible_height - clipped->y,
            (uint32_t)surface_height(surface) - clipped->y));
    return true;
}

static void helios_vulkan_readback_record_publish(
    HeliosVulkanReadback *readback)
{
    VkDeviceSize size = (VkDeviceSize)readback->backing_width *
                        readback->backing_height * 4;
    VkBufferMemoryBarrier snapshot = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = readback->snapshot,
        .offset = 0,
        .size = size,
    };
    VkBufferCopy copy = {
        .size = size,
    };
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = readback->staging,
        .offset = 0,
        .size = size,
    };

    vkCmdPipelineBarrier(readback->command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 1, &snapshot, 0, NULL);
    vkCmdCopyBuffer(readback->command_buffer, readback->snapshot,
                    readback->staging, 1, &copy);
    vkCmdPipelineBarrier(readback->command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, NULL, 1, &host, 0, NULL);
}

bool helios_vulkan_readback_capture(HeliosVulkanReadback *readback,
                                    DisplaySurface *surface,
                                    const HeliosVulkanReadbackRect *capture,
                                    const HeliosVulkanReadbackRect *publish)
{
    VkResult result;
    int64_t start_ns, submit_ns, wait_ns, copy_ns;
    uint32_t src_x, src_y;
    HeliosVulkanReadbackRect captured, published;

#define VK_FLUSH(call) do {                                                 \
    result = (call);                                                        \
    if (result != VK_SUCCESS) {                                             \
        error_report("vulkan-readback: %s failed: %d", #call, result);     \
        readback->failed = true;                                            \
        return false;                                                       \
    }                                                                       \
} while (0)

    if (!readback || readback->failed ||
        !helios_vulkan_readback_clip_rect(readback, surface,
                                          capture, &captured) ||
        (publish &&
         !helios_vulkan_readback_clip_rect(readback, surface,
                                           publish, &published))) {
        return false;
    }

    src_x = readback->origin_x + captured.x;
    src_y = readback->origin_y + captured.y;

    start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    VK_FLUSH(vkResetFences(readback->device, 1, &readback->fence));
    VK_FLUSH(vkResetCommandPool(readback->device, readback->command_pool, 0));
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_FLUSH(vkBeginCommandBuffer(readback->command_buffer, &begin_info));

    VkImageMemoryBarrier acquire = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = readback->producer_layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = readback->external_ownership
            ? VK_QUEUE_FAMILY_EXTERNAL
            : VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = readback->external_ownership
            ? readback->queue_family
            : VK_QUEUE_FAMILY_IGNORED,
        .image = readback->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(readback->command_buffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &acquire);

    VkBufferImageCopy region = {
        .bufferOffset = ((VkDeviceSize)src_y * readback->backing_width +
                         src_x) * 4,
        .bufferRowLength = readback->backing_width,
        .bufferImageHeight = readback->backing_height,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .imageOffset = { src_x, src_y, 0 },
        .imageExtent = { captured.width, captured.height, 1 },
    };
    vkCmdCopyImageToBuffer(readback->command_buffer, readback->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback->snapshot, 1, &region);

    VkImageMemoryBarrier release = acquire;
    release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    release.dstAccessMask = 0;
    release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    release.newLayout = readback->producer_layout;
    release.srcQueueFamilyIndex = readback->external_ownership
        ? readback->queue_family
        : VK_QUEUE_FAMILY_IGNORED;
    release.dstQueueFamilyIndex = readback->external_ownership
        ? VK_QUEUE_FAMILY_EXTERNAL
        : VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(readback->command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, NULL, 0, NULL, 1, &release);

    if (publish) {
        helios_vulkan_readback_record_publish(readback);
    }
    VK_FLUSH(vkEndCommandBuffer(readback->command_buffer));

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &readback->command_buffer,
    };
    VK_FLUSH(vkQueueSubmit(readback->queue, 1, &submit_info,
                           readback->fence));
    submit_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    VK_FLUSH(vkWaitForFences(readback->device, 1, &readback->fence,
                             VK_TRUE, UINT64_MAX));
    wait_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (publish) {
        helios_vulkan_readback_copy_to_surface(readback, surface,
                                               published.x, published.y,
                                               published.width,
                                               published.height);
        readback->publishes++;
    }
    copy_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    readback->flushes++;
    trace_helios_vulkan_capture(
        readback->dmabuf_ino, readback->flushes, publish != NULL,
        captured.width, captured.height,
        submit_ns - start_ns, wait_ns - submit_ns, copy_ns - wait_ns);
    if (readback->flushes == 1 || copy_ns - start_ns > 50000000 ||
        (readback->flushes % 4096) == 0) {
        error_report("vulkan-readback: capture #%" PRIu64
                     " publish #%" PRIu64 " %ux%u submit=%.3f ms "
                     "wait=%.3f ms cpu=%.3f ms total=%.3f ms",
                     readback->flushes, readback->publishes,
                     captured.width, captured.height,
                     (submit_ns - start_ns) / 1e6,
                     (wait_ns - submit_ns) / 1e6,
                     (copy_ns - wait_ns) / 1e6,
                     (copy_ns - start_ns) / 1e6);
    }
    return true;

#undef VK_FLUSH
}

bool helios_vulkan_readback_publish(HeliosVulkanReadback *readback,
                                    DisplaySurface *surface,
                                    const HeliosVulkanReadbackRect *rect)
{
    VkResult result;
    HeliosVulkanReadbackRect clipped;
    int64_t start_ns, submit_ns, wait_ns, copy_ns;

#define VK_PUBLISH(call) do {                                               \
    result = (call);                                                        \
    if (result != VK_SUCCESS) {                                             \
        error_report("vulkan-readback: %s failed: %d", #call, result);     \
        readback->failed = true;                                            \
        return false;                                                       \
    }                                                                       \
} while (0)

    if (!readback || readback->failed ||
        !helios_vulkan_readback_clip_rect(readback, surface,
                                          rect, &clipped)) {
        return false;
    }

    start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    VK_PUBLISH(vkResetFences(readback->device, 1, &readback->fence));
    VK_PUBLISH(vkResetCommandPool(readback->device,
                                  readback->command_pool, 0));
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_PUBLISH(vkBeginCommandBuffer(readback->command_buffer, &begin_info));

    helios_vulkan_readback_record_publish(readback);
    VK_PUBLISH(vkEndCommandBuffer(readback->command_buffer));

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &readback->command_buffer,
    };
    VK_PUBLISH(vkQueueSubmit(readback->queue, 1, &submit_info,
                             readback->fence));
    submit_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    VK_PUBLISH(vkWaitForFences(readback->device, 1, &readback->fence,
                               VK_TRUE, UINT64_MAX));
    wait_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    helios_vulkan_readback_copy_to_surface(readback, surface,
                                           clipped.x, clipped.y,
                                           clipped.width, clipped.height);
    copy_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    readback->publishes++;
    trace_helios_vulkan_publish(
        readback->dmabuf_ino, readback->publishes,
        clipped.width, clipped.height,
        submit_ns - start_ns, wait_ns - submit_ns, copy_ns - wait_ns);
    return true;

#undef VK_PUBLISH
}
