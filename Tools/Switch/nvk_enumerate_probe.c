// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Minimal loaderless NVK device-enumeration probe for Switch hardware.
// Unlike switch-nvk's full smoke test, this deliberately performs no shim
// self-test before Vulkan so driver initialization is tested from a clean state.

#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <switch.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;

extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);
extern void (*g_drm_shim_log_sink)(const char*);

static FILE* s_log;

static void write_line(const char* text)
{
  if (s_log)
  {
    fputs(text, s_log);
    fflush(s_log);
  }
  fputs(text, stdout);
  fflush(stdout);
}

static void log_line(const char* format, ...)
{
  char buffer[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (s_log)
  {
    fputs(buffer, s_log);
    fputc('\n', s_log);
    fflush(s_log);
  }
  puts(buffer);
  fflush(stdout);
}

int main(void)
{
  s_log = fopen("sdmc:/nvk_enumerate.log", "w");
  const bool sockets_initialized =
      __nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault());
  if (sockets_initialized)
    nxlinkStdio();

  g_drm_shim_log_sink = write_line;
  setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
  setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
  setenv("MESA_LOG_FILE", "sdmc:/nvk_enumerate_mesa.log", 1);
  log_line("probe: entered main; no pre-Vulkan DRM self-test");

  const PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
  log_line("probe: vkCreateInstance entrypoint=%p", (void*)create_instance);
  if (!create_instance)
    goto done;

  const VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "nvk_enumerate_probe",
      .apiVersion = VK_API_VERSION_1_1,
  };
  const VkInstanceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app_info,
  };
  VkInstance instance = VK_NULL_HANDLE;
  VkResult result = create_instance(&create_info, NULL, &instance);
  log_line("probe: vkCreateInstance -> %d", result);
  if (result != VK_SUCCESS)
    goto done;

  const PFN_vkEnumeratePhysicalDevices enumerate_devices =
      (PFN_vkEnumeratePhysicalDevices)vk_icdGetInstanceProcAddr(
          instance, "vkEnumeratePhysicalDevices");
  const PFN_vkDestroyInstance destroy_instance =
      (PFN_vkDestroyInstance)vk_icdGetInstanceProcAddr(instance, "vkDestroyInstance");
  log_line("probe: enumerate entrypoint=%p; calling now", (void*)enumerate_devices);
  if (enumerate_devices)
  {
    uint32_t device_count = 0;
    result = enumerate_devices(instance, &device_count, NULL);
    log_line("probe: vkEnumeratePhysicalDevices -> %d, count=%u", result, device_count);
  }
  if (destroy_instance)
    destroy_instance(instance, NULL);

done:
  log_line("probe: clean exit");
  g_drm_shim_log_sink = NULL;
  if (s_log)
    fclose(s_log);
  if (sockets_initialized)
    socketExit();
  return 0;
}
