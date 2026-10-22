/*
 * test_shim.c - prueba aislada: carga el shim con dlopen, resuelve
 * vkGetInstanceProcAddr con dlsym (igual que LWJGL/SDL), crea una instancia,
 * enumera dispositivos y muestra deviceName / driverName / driverInfo.
 *
 * No prueba superficies ni presentacion (necesita una ventana, no un binario
 * de adb shell). Eso es la fase 3b, dentro de una app.
 *
 * Uso: ./test_panvk_shim [ruta/libvulkan_panvk_shim.so]
 * Codigos de salida: 0 ok, 2 dlopen/dlsym, 3 globales, 4 instancia, 5 sin GPU.
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *shim = argc > 1 ? argv[1] : "./libvulkan_panvk_shim.so";

    void *h = dlopen(shim, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "FAIL dlopen(%s): %s\n", shim, dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    if (!gipa) {
        fprintf(stderr, "FAIL dlsym(vkGetInstanceProcAddr): %s\n", dlerror());
        return 2;
    }
    printf("OK   shim cargado, vkGetInstanceProcAddr=%p\n", (void *)gipa);

    /* Zink (Mesa upstream) tambien hace dlsym de vkGetDeviceProcAddr. */
    PFN_vkGetDeviceProcAddr gdpa_sym =
        (PFN_vkGetDeviceProcAddr)dlsym(h, "vkGetDeviceProcAddr");
    printf("%s dlsym(vkGetDeviceProcAddr)=%p\n", gdpa_sym ? "OK  " : "FAIL",
           (void *)gdpa_sym);
    if (!gdpa_sym)
        return 2;

    /* 1. Funciones globales (instance == NULL), las que usa VK.GlobalCommands */
    static const struct {
        const char *name;
        int required;
    } globals[] = {
        {"vkGetInstanceProcAddr", 1},
        {"vkCreateInstance", 1},
        {"vkEnumerateInstanceExtensionProperties", 1},
        {"vkEnumerateInstanceLayerProperties", 1},
        {"vkEnumerateInstanceVersion", 0},
    };
    int missing = 0;
    for (size_t i = 0; i < sizeof(globals) / sizeof(globals[0]); i++) {
        PFN_vkVoidFunction f = gipa(NULL, globals[i].name);
        printf("%s %s -> %p\n", f ? "OK  " : (globals[i].required ? "FAIL" : "WARN"),
               globals[i].name, (void *)f);
        if (!f && globals[i].required)
            missing++;
    }
    if (missing)
        return 3;

    uint32_t inst_ver = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion ev =
        (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
    if (ev)
        ev(&inst_ver);
    printf("INFO version de instancia: %u.%u.%u\n", VK_API_VERSION_MAJOR(inst_ver),
           VK_API_VERSION_MINOR(inst_ver), VK_API_VERSION_PATCH(inst_ver));

    /* 2. Extensiones de instancia */
    PFN_vkEnumerateInstanceExtensionProperties enum_ext =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(
            NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t ext_n = 0;
    enum_ext(NULL, &ext_n, NULL);
    VkExtensionProperties *exts = calloc(ext_n + 1, sizeof(*exts));
    enum_ext(NULL, &ext_n, exts);
    int has_surface = 0, has_android = 0;
    printf("INFO %u extensiones de instancia:\n", ext_n);
    for (uint32_t i = 0; i < ext_n; i++) {
        printf("       %s (v%u)\n", exts[i].extensionName, exts[i].specVersion);
        if (!strcmp(exts[i].extensionName, "VK_KHR_surface"))
            has_surface = 1;
        if (!strcmp(exts[i].extensionName, "VK_KHR_android_surface"))
            has_android = 1;
    }
    free(exts);
    printf("INFO VK_KHR_surface=%d VK_KHR_android_surface=%d\n", has_surface,
           has_android);

    /* 3. Instancia */
    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "test_panvk_shim",
                             .apiVersion = inst_ver >= VK_API_VERSION_1_1
                                               ? VK_API_VERSION_1_1
                                               : VK_API_VERSION_1_0};
    const char *want_exts[2];
    uint32_t want_n = 0;
    if (has_surface)
        want_exts[want_n++] = "VK_KHR_surface";
    if (has_surface && has_android)
        want_exts[want_n++] = "VK_KHR_android_surface";
    printf("INFO habilitando %u extension(es) de superficie\n", want_n);
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app,
                               .enabledExtensionCount = want_n,
                               .ppEnabledExtensionNames = want_exts};
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = create(&ci, NULL, &inst);
    printf("%s vkCreateInstance -> %d\n", r == VK_SUCCESS ? "OK  " : "FAIL", (int)r);
    if (r != VK_SUCCESS)
        return 4;

    PFN_vkEnumeratePhysicalDevices enum_pd =
        (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties get_props =
        (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceProperties2 get_props2 =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
    PFN_vkDestroyInstance destroy =
        (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    if (!enum_pd || !get_props || !destroy) {
        fprintf(stderr, "FAIL funciones de instancia sin resolver (%p %p %p)\n",
                (void *)enum_pd, (void *)get_props, (void *)destroy);
        return 4;
    }

    uint32_t n = 0;
    r = enum_pd(inst, &n, NULL);
    printf("%s vkEnumeratePhysicalDevices -> %d, %u dispositivo(s)\n",
           (r == VK_SUCCESS && n) ? "OK  " : "FAIL", (int)r, n);
    if (r != VK_SUCCESS || n == 0) {
        destroy(inst, NULL);
        return 5;
    }

    VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
    enum_pd(inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        get_props(pds[i], &p);
        printf("INFO [%u] deviceName=\"%s\" apiVersion=%u.%u.%u vendor=0x%x device=0x%x\n",
               i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               p.vendorID, p.deviceID);

        if (get_props2 && p.apiVersion >= VK_API_VERSION_1_2) {
            VkPhysicalDeviceDriverProperties dp = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 p2 = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = &dp};
            get_props2(pds[i], &p2);
            printf("INFO [%u] driverName=\"%s\" driverInfo=\"%s\"\n", i,
                   dp.driverName, dp.driverInfo);
            printf("INFO [%u] driverInfo contiene \"PanVK\": %s\n", i,
                   strstr(dp.driverInfo, "PanVK") ? "si" : "NO");
        } else {
            printf("WARN [%u] sin driverName/driverInfo (props2=%p, api<1.2)\n", i,
                   (void *)get_props2);
        }
    }

    /* 4. Funciones de superficie (solo comprobar que resuelven) */
    PFN_vkVoidFunction cs = gipa(inst, "vkCreateAndroidSurfaceKHR");
    PFN_vkVoidFunction ds = gipa(inst, "vkDestroySurfaceKHR");
    PFN_vkVoidFunction sup = gipa(inst, "vkGetPhysicalDeviceSurfaceSupportKHR");
    printf("%s vkCreateAndroidSurfaceKHR=%p vkDestroySurfaceKHR=%p "
           "vkGetPhysicalDeviceSurfaceSupportKHR=%p\n",
           (cs && ds && sup) ? "OK  " : "WARN", (void *)cs, (void *)ds,
           (void *)sup);

    free(pds);
    destroy(inst, NULL);
    printf("OK   prueba aislada completada\n");
    return 0;
}
