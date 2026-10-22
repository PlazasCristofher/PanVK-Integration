/*
 * panvk_shim.c - shim minimo entre LWJGL/SDL/Mesa-Zink y un ICD Vulkan
 * (PanVK-kbase).
 *
 * Quien carga el handle (LWJGL, SDL, Zink parcheado estilo Pojav) hace dlsym
 * sobre el:
 *   - vkGetInstanceProcAddr   (LWJGL, SDL, Zink)
 *   - vkGetDeviceProcAddr     (Zink upstream lo pide tambien; v4 lo exporta)
 * El ICD (libvulkan_panfrost.so) solo exporta vk_icdGetInstanceProcAddr,
 * vk_icdGetPhysicalDeviceProcAddr y vk_icdNegotiateLoaderICDInterfaceVersion.
 * Este shim exporta SOLO esos dos simbolos y reenvia al ICD, y hace la parte
 * minima del trabajo que normalmente hace el loader:
 *   - negociar la version de interfaz ICD
 *   - vkEnumerateInstanceLayerProperties (lista vacia)
 *   - vkEnumerateInstanceExtensionProperties (anade VK_KHR_android_surface si
 *     el ICD no la anuncia, porque en el modelo loader/ICD la crea el loader)
 *   - vkCreateInstance (quita VK_KHR_android_surface si el ICD no la conoce)
 *   - vkCreateAndroidSurfaceKHR / vkDestroySurfaceKHR (estructura VkIcdSurface*)
 *
 * Diagnostico (v4), por defecto ACTIVADO en esta build:
 *   - Todo el log sale tambien por stderr (el launcher lo guarda en
 *     latest_game.log). PANVK_SHIM_QUIET=1 lo desactiva (queda solo logcat).
 *   - TRACE: cada nombre pedido a vkGetInstanceProcAddr / vkGetDeviceProcAddr
 *     se registra UNA vez, con "ok" o "NULL". PANVK_SHIM_TRACE=0 lo desactiva.
 *   - Resultados (VkResult) de vkCreateInstance, vkEnumeratePhysicalDevices,
 *     vkCreateDevice y vkCreateSwapchainKHR, siempre.
 *   - PROF: tiempos por llamada (submit, present, acquire, waitFences, ...)
 *     agregados cada segundo. PANVK_SHIM_PROF=0 lo desactiva.
 */

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "PanVKShim"

/* PANVK_SHIM_DIAG=1 (por defecto): rastreo, medidor PROF y log a stderr.
 * PANVK_SHIM_DIAG=0: build "limpia": sin envolturas, sin stderr, sin rastreo;
 * solo reenvia y deja en logcat los errores y una linea al cargar. */
#ifndef PANVK_SHIM_DIAG
#define PANVK_SHIM_DIAG 1
#endif

/* Cada mensaje va a logcat y, salvo PANVK_SHIM_QUIET, tambien a stderr. */
static void shim_log(int prio, const char *lvl, const char *fmt, ...)
{
    (void)lvl;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    __android_log_print(prio, TAG, "%s", buf);
#if PANVK_SHIM_DIAG
    if (!getenv("PANVK_SHIM_QUIET")) {
        char line[1100];
        snprintf(line, sizeof(line), "[" TAG " %s] %s\n", lvl, buf);
        fputs(line, stderr);
    }
#endif
}
#define LOGI(...) shim_log(ANDROID_LOG_INFO, "I", __VA_ARGS__)
#define LOGW(...) shim_log(ANDROID_LOG_WARN, "W", __VA_ARGS__)
#define LOGE(...) shim_log(ANDROID_LOG_ERROR, "E", __VA_ARGS__)

#define EXPORT __attribute__((visibility("default")))

#define ICD_FILE_NAME "libvulkan_panfrost.so"
#define ICD_ENV_PATH "PANVK_ICD_PATH"
#define WANTED_ICD_INTERFACE_VERSION 5u

/* ---- Estructuras de superficie del modelo loader/ICD (vk_icd.h) ----------
 * VERIFICAR contra vk_icd.h de Vulkan-Headers: el valor de
 * VK_ICD_WSI_PLATFORM_ANDROID. De memoria es 5 (MIR, WAYLAND, WIN32, XCB,
 * XLIB, ANDROID, ...). (La presentacion en MC 26.3 funciono con 5.) */
#define SHIM_ICD_WSI_PLATFORM_ANDROID 5

typedef struct {
    int platform; /* VkIcdWsiPlatform es un enum de tamano int */
} ShimIcdSurfaceBase;

typedef struct {
    ShimIcdSurfaceBase base;
    ANativeWindow *window;
} ShimIcdSurfaceAndroid;

/* ---- Estado --------------------------------------------------------------- */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void *g_icd = NULL;
static PFN_vkGetInstanceProcAddr g_icd_gipa = NULL;
static uint32_t g_icd_version = 0;

static VkExtensionProperties *g_exts = NULL;
static uint32_t g_ext_count = 0;
static int g_icd_has_android_surface = 0;

static int g_trace = PANVK_SHIM_DIAG;
static int g_prof = PANVK_SHIM_DIAG;

/* gdpa real del ICD: se obtiene con una instancia valida (vkCreateInstance). */
static PFN_vkGetDeviceProcAddr g_real_gdpa = NULL;
static VkInstance g_last_instance = VK_NULL_HANDLE;

typedef VkResult(VKAPI_PTR *PFN_shim_negotiate)(uint32_t *);

static unsigned long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL +
           (unsigned long long)ts.tv_nsec;
}

__attribute__((unused)) static int flag_default_on(const char *name)
{
    const char *v = getenv(name);
    return !(v && v[0] == '0');
}

static void resolve_icd_path(char *out, size_t n)
{
    const char *env = getenv(ICD_ENV_PATH);
    if (env && *env) {
        snprintf(out, n, "%s", env);
        return;
    }

    /* Por defecto: mismo directorio que el shim. */
    Dl_info info;
    if (dladdr((void *)&resolve_icd_path, &info) && info.dli_fname) {
        const char *slash = strrchr(info.dli_fname, '/');
        if (slash) {
            snprintf(out, n, "%.*s/%s", (int)(slash - info.dli_fname),
                     info.dli_fname, ICD_FILE_NAME);
            return;
        }
    }
    snprintf(out, n, "%s", ICD_FILE_NAME);
}

static void build_extension_cache(void)
{
    PFN_vkEnumerateInstanceExtensionProperties fn =
        (PFN_vkEnumerateInstanceExtensionProperties)g_icd_gipa(
            NULL, "vkEnumerateInstanceExtensionProperties");
    if (!fn) {
        LOGE("el ICD no resuelve vkEnumerateInstanceExtensionProperties");
        return;
    }

    uint32_t n = 0;
    if (fn(NULL, &n, NULL) != VK_SUCCESS) {
        LOGE("vkEnumerateInstanceExtensionProperties(count) fallo");
        return;
    }

    g_exts = calloc((size_t)n + 1, sizeof(*g_exts));
    if (!g_exts)
        return;

    if (fn(NULL, &n, g_exts) < 0) {
        LOGE("vkEnumerateInstanceExtensionProperties(list) fallo");
        free(g_exts);
        g_exts = NULL;
        return;
    }
    g_ext_count = n;

    int has_surface = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(g_exts[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME))
            has_surface = 1;
        if (!strcmp(g_exts[i].extensionName,
                    VK_KHR_ANDROID_SURFACE_EXTENSION_NAME))
            g_icd_has_android_surface = 1;
    }

    /* Solo tiene sentido anunciar android_surface si el ICD entiende
     * VK_KHR_surface (las funciones vkGetPhysicalDeviceSurface* son suyas). */
    if (has_surface && !g_icd_has_android_surface) {
        VkExtensionProperties *e = &g_exts[g_ext_count++];
        snprintf(e->extensionName, sizeof(e->extensionName), "%s",
                 VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
        e->specVersion = VK_KHR_ANDROID_SURFACE_SPEC_VERSION;
        LOGI("ICD sin %s: la anuncia el shim",
             VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    }
    LOGI("extensiones de instancia: ICD=%u, anunciadas=%u, android_surface "
         "nativa del ICD=%d",
         n, g_ext_count, g_icd_has_android_surface);
}

static void load_once(void)
{
#if PANVK_SHIM_DIAG
    g_trace = flag_default_on("PANVK_SHIM_TRACE");
    g_prof = flag_default_on("PANVK_SHIM_PROF");
#else
    g_trace = 0;
    g_prof = 0;
#endif

    char path[PATH_MAX];
    resolve_icd_path(path, sizeof(path));

    g_icd = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!g_icd) {
        LOGE("dlopen(%s) fallo: %s", path, dlerror());
        return;
    }

    g_icd_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_icd,
                                                  "vk_icdGetInstanceProcAddr");
    if (!g_icd_gipa) {
        LOGE("vk_icdGetInstanceProcAddr no encontrado en %s: %s", path,
             dlerror());
        dlclose(g_icd);
        g_icd = NULL;
        return;
    }

    PFN_shim_negotiate neg = (PFN_shim_negotiate)dlsym(
        g_icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (neg) {
        uint32_t v = WANTED_ICD_INTERFACE_VERSION;
        VkResult r = neg(&v);
        if (r == VK_SUCCESS) {
            g_icd_version = v;
        } else {
            LOGW("negociacion de interfaz ICD fallo (VkResult=%d)", (int)r);
        }
    } else {
        LOGW("el ICD no exporta vk_icdNegotiateLoaderICDInterfaceVersion");
    }

    LOGI("ICD cargado: %s (interfaz negociada v%u, gipa=%p) build=%s trace=%d "
         "prof=%d",
         path, g_icd_version, (void *)g_icd_gipa,
         PANVK_SHIM_DIAG ? "diag" : "clean", g_trace, g_prof);
    build_extension_cache();
}

/* ---- TRACE: cada nombre pedido, una sola vez ------------------------------ */

#define TRACE_MAX 1024
static char *g_seen[TRACE_MAX];
static unsigned g_seen_n = 0;
static pthread_mutex_t g_seen_mu = PTHREAD_MUTEX_INITIALIZER;

static int first_time(const char *kind, const char *name)
{
    char key[160];
    snprintf(key, sizeof(key), "%s:%s", kind, name);

    int res = 0;
    pthread_mutex_lock(&g_seen_mu);
    unsigned i;
    for (i = 0; i < g_seen_n; i++)
        if (!strcmp(g_seen[i], key))
            break;
    if (i == g_seen_n && g_seen_n < TRACE_MAX) {
        g_seen[g_seen_n++] = strdup(key);
        res = 1;
    }
    pthread_mutex_unlock(&g_seen_mu);
    return res;
}

static PFN_vkVoidFunction traced(const char *kind, const void *handle,
                                 const char *name, PFN_vkVoidFunction r)
{
    if (g_trace && first_time(kind, name))
        LOGI("TRACE %s(%s%s) -> %s", kind, handle ? "" : "NULL,", name,
             r ? "ok" : "NULL");
    return r;
}

/* ---- PROF: estadisticas por llamada ---------------------------------------- */

enum {
    ST_SUBMIT,
    ST_PRESENT,
    ST_ACQUIRE,
    ST_WAITFENCE,
    ST_WAITSEM,
    ST_QWAIT,
    ST_DWAIT,
    ST_COUNT
};
static const char *const st_name[ST_COUNT] = {
    "submit", "present", "acquire", "waitFences", "waitSem",
    "queueWaitIdle", "deviceWaitIdle"};

typedef struct {
    atomic_ullong n, tot, max;
} Stat;
static Stat g_st[ST_COUNT];
static atomic_ullong g_frames, g_frame_tot, g_frame_max;
static atomic_ullong g_last_report, g_last_present;

static void atomic_max_ull(atomic_ullong *a, unsigned long long v)
{
    unsigned long long cur = atomic_load_explicit(a, memory_order_relaxed);
    while (v > cur &&
           !atomic_compare_exchange_weak_explicit(
               a, &cur, v, memory_order_relaxed, memory_order_relaxed)) {
    }
}

static void record(int idx, unsigned long long dt)
{
    atomic_fetch_add_explicit(&g_st[idx].n, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_st[idx].tot, dt, memory_order_relaxed);
    atomic_max_ull(&g_st[idx].max, dt);
}

static void maybe_report(unsigned long long now)
{
    unsigned long long last = atomic_load(&g_last_report);
    if (last == 0) {
        atomic_compare_exchange_strong(&g_last_report, &last, now);
        return;
    }
    if (now - last < 1000000000ULL)
        return;
    /* El hilo que gana el intercambio es el que imprime. */
    if (!atomic_compare_exchange_strong(&g_last_report, &last, now))
        return;

    double elapsed_ms = (double)(now - last) / 1e6;
    unsigned long long frames = atomic_exchange(&g_frames, 0);
    unsigned long long ftot = atomic_exchange(&g_frame_tot, 0);
    unsigned long long fmax = atomic_exchange(&g_frame_max, 0);

    char buf[900];
    int o = snprintf(buf, sizeof(buf),
                     "PROF %.0fms frames=%llu fps=%.1f frame avg=%.1fms "
                     "max=%.1fms",
                     elapsed_ms, frames,
                     elapsed_ms > 0 ? frames * 1000.0 / elapsed_ms : 0.0,
                     frames ? ftot / 1e6 / (double)frames : 0.0, fmax / 1e6);
    for (int i = 0; i < ST_COUNT && o > 0 && (size_t)o < sizeof(buf); i++) {
        unsigned long long n = atomic_exchange(&g_st[i].n, 0);
        unsigned long long tot = atomic_exchange(&g_st[i].tot, 0);
        unsigned long long mx = atomic_exchange(&g_st[i].max, 0);
        if (!n)
            continue;
        o += snprintf(buf + o, sizeof(buf) - (size_t)o,
                      " | %s n=%llu avg=%.2fms max=%.1fms sum=%.0fms",
                      st_name[i], n, tot / 1e6 / (double)n, mx / 1e6,
                      tot / 1e6);
    }
    LOGI("%s", buf);
}

#define TIMED(NAME, ST, RET, PARAMS, ARGS)                                     \
    static PFN_vk##NAME real_##NAME;                                           \
    static VKAPI_ATTR RET VKAPI_CALL timed_##NAME PARAMS                       \
    {                                                                          \
        unsigned long long t0 = now_ns();                                      \
        RET r = real_##NAME ARGS;                                              \
        record(ST, now_ns() - t0);                                             \
        return r;                                                              \
    }

TIMED(QueueSubmit, ST_SUBMIT, VkResult,
      (VkQueue q, uint32_t n, const VkSubmitInfo *i, VkFence f), (q, n, i, f))
TIMED(QueueSubmit2, ST_SUBMIT, VkResult,
      (VkQueue q, uint32_t n, const VkSubmitInfo2 *i, VkFence f), (q, n, i, f))
TIMED(QueueSubmit2KHR, ST_SUBMIT, VkResult,
      (VkQueue q, uint32_t n, const VkSubmitInfo2 *i, VkFence f), (q, n, i, f))
TIMED(AcquireNextImageKHR, ST_ACQUIRE, VkResult,
      (VkDevice d, VkSwapchainKHR s, uint64_t t, VkSemaphore sem, VkFence f,
       uint32_t *idx),
      (d, s, t, sem, f, idx))
TIMED(AcquireNextImage2KHR, ST_ACQUIRE, VkResult,
      (VkDevice d, const VkAcquireNextImageInfoKHR *info, uint32_t *idx),
      (d, info, idx))
TIMED(WaitForFences, ST_WAITFENCE, VkResult,
      (VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t),
      (d, n, f, all, t))
TIMED(WaitSemaphores, ST_WAITSEM, VkResult,
      (VkDevice d, const VkSemaphoreWaitInfo *info, uint64_t t), (d, info, t))
TIMED(WaitSemaphoresKHR, ST_WAITSEM, VkResult,
      (VkDevice d, const VkSemaphoreWaitInfo *info, uint64_t t), (d, info, t))
TIMED(QueueWaitIdle, ST_QWAIT, VkResult, (VkQueue q), (q))
TIMED(DeviceWaitIdle, ST_DWAIT, VkResult, (VkDevice d), (d))

static PFN_vkQueuePresentKHR real_QueuePresentKHR;
static atomic_uint g_present_errs;
static VKAPI_ATTR VkResult VKAPI_CALL
timed_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *info)
{
    unsigned long long t0 = now_ns();
    VkResult r = real_QueuePresentKHR(q, info);
    unsigned long long t1 = now_ns();
    record(ST_PRESENT, t1 - t0);

    unsigned long long prev = atomic_exchange(&g_last_present, t1);
    if (prev) {
        atomic_fetch_add(&g_frame_tot, t1 - prev);
        atomic_max_ull(&g_frame_max, t1 - prev);
    }
    atomic_fetch_add(&g_frames, 1);

    if (r != VK_SUCCESS && atomic_fetch_add(&g_present_errs, 1) < 5)
        LOGW("vkQueuePresentKHR -> VkResult=%d", (int)r);

    maybe_report(t1);
    return r;
}

/* ---- Envolturas que SIEMPRE se instalan: registran el VkResult -------------- */

static void join_names(char *out, size_t n, uint32_t count,
                       const char *const *names)
{
    size_t o = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < count && o + 2 < n; i++) {
        int w = snprintf(out + o, n - o, "%s%s", i ? "," : "", names[i]);
        if (w < 0 || (size_t)w >= n - o) {
            snprintf(out + n - 4, 4, "...");
            break;
        }
        o += (size_t)w;
    }
}

static PFN_vkEnumeratePhysicalDevices real_EnumeratePhysicalDevices;
static VKAPI_ATTR VkResult VKAPI_CALL
logged_EnumeratePhysicalDevices(VkInstance inst, uint32_t *pCount,
                                VkPhysicalDevice *pDevs)
{
    VkResult r = real_EnumeratePhysicalDevices(inst, pCount, pDevs);
    LOGI("vkEnumeratePhysicalDevices(%s) -> VkResult=%d count=%u",
         pDevs ? "list" : "count", (int)r, pCount ? *pCount : 0);
    return r;
}

static PFN_vkCreateDevice real_CreateDevice;
static VKAPI_ATTR VkResult VKAPI_CALL
logged_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *pInfo,
                    const VkAllocationCallbacks *pAlloc, VkDevice *pDev)
{
    VkResult r = real_CreateDevice(pd, pInfo, pAlloc, pDev);
    char exts[700];
    join_names(exts, sizeof(exts), pInfo ? pInfo->enabledExtensionCount : 0,
               pInfo ? pInfo->ppEnabledExtensionNames : NULL);
    LOGI("vkCreateDevice -> VkResult=%d extensions(%u)=[%s]", (int)r,
         pInfo ? pInfo->enabledExtensionCount : 0, exts);
    return r;
}

static PFN_vkCreateSwapchainKHR real_CreateSwapchainKHR;
static VKAPI_ATTR VkResult VKAPI_CALL
logged_CreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *pInfo,
                          const VkAllocationCallbacks *pAlloc,
                          VkSwapchainKHR *pSc)
{
    VkResult r = real_CreateSwapchainKHR(d, pInfo, pAlloc, pSc);
    if (pInfo)
        LOGI("vkCreateSwapchainKHR -> VkResult=%d images=%u format=%d "
             "extent=%ux%u presentMode=%d usage=0x%x",
             (int)r, pInfo->minImageCount, (int)pInfo->imageFormat,
             pInfo->imageExtent.width, pInfo->imageExtent.height,
             (int)pInfo->presentMode, (unsigned)pInfo->imageUsage);
    else
        LOGI("vkCreateSwapchainKHR -> VkResult=%d (sin info)", (int)r);
    return r;
}

/* ---- Tabla de envolturas ----------------------------------------------------- */

typedef struct {
    const char *name;
    void **real;
    PFN_vkVoidFunction wrapper;
    int always; /* 1: siempre; 0: solo si PROF esta activo */
} WrapEnt;

#define W(n, always_)                                                          \
    {"vk" #n, (void **)&real_##n, (PFN_vkVoidFunction)timed_##n, always_}
#define WL(n)                                                                  \
    {"vk" #n, (void **)&real_##n, (PFN_vkVoidFunction)logged_##n, 1}

static const WrapEnt g_wraps[] = {
    W(QueueSubmit, 0),         W(QueueSubmit2, 0),
    W(QueueSubmit2KHR, 0),     W(QueuePresentKHR, 0),
    W(AcquireNextImageKHR, 0), W(AcquireNextImage2KHR, 0),
    W(WaitForFences, 0),       W(WaitSemaphores, 0),
    W(WaitSemaphoresKHR, 0),   W(QueueWaitIdle, 0),
    W(DeviceWaitIdle, 0),      WL(EnumeratePhysicalDevices),
    WL(CreateDevice),          WL(CreateSwapchainKHR),
};

static PFN_vkVoidFunction maybe_wrap(const char *name, PFN_vkVoidFunction real)
{
    if (!real || !PANVK_SHIM_DIAG)
        return real;
    for (size_t i = 0; i < sizeof(g_wraps) / sizeof(g_wraps[0]); i++) {
        if (strcmp(g_wraps[i].name, name))
            continue;
        if (!g_wraps[i].always && !g_prof)
            return real;
        *g_wraps[i].real = (void *)real;
        return g_wraps[i].wrapper;
    }
    return real;
}

/* ---- Funciones que el shim implementa en lugar del loader ----------------- */

static VKAPI_ATTR VkResult VKAPI_CALL
shim_EnumerateInstanceLayerProperties(uint32_t *pCount, VkLayerProperties *pProps)
{
    (void)pProps;
    if (!pCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pCount = 0;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
shim_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                          uint32_t *pCount,
                                          VkExtensionProperties *pProps)
{
    if (pLayerName)
        return VK_ERROR_LAYER_NOT_PRESENT;
    if (!pCount || !g_exts)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (!pProps) {
        *pCount = g_ext_count;
        return VK_SUCCESS;
    }

    uint32_t n = *pCount < g_ext_count ? *pCount : g_ext_count;
    memcpy(pProps, g_exts, (size_t)n * sizeof(*g_exts));
    *pCount = n;
    return n < g_ext_count ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
shim_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkInstance *pInstance)
{
    PFN_vkCreateInstance real =
        (PFN_vkCreateInstance)g_icd_gipa(NULL, "vkCreateInstance");
    if (!real)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r;
    if (g_icd_has_android_surface || !pCreateInfo ||
        pCreateInfo->enabledExtensionCount == 0) {
        r = real(pCreateInfo, pAllocator, pInstance);
    } else {
        /* El ICD no conoce VK_KHR_android_surface: no se la pasamos. */
        const char **names =
            malloc((size_t)pCreateInfo->enabledExtensionCount * sizeof(char *));
        if (!names)
            return VK_ERROR_OUT_OF_HOST_MEMORY;

        uint32_t n = 0;
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            const char *name = pCreateInfo->ppEnabledExtensionNames[i];
            if (strcmp(name, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME) != 0)
                names[n++] = name;
        }

        VkInstanceCreateInfo copy = *pCreateInfo;
        copy.enabledExtensionCount = n;
        copy.ppEnabledExtensionNames = names;
        r = real(&copy, pAllocator, pInstance);
        free(names);
    }

    if (r == VK_SUCCESS && pInstance)
        g_last_instance = *pInstance;

    if (PANVK_SHIM_DIAG && pCreateInfo) {
        char exts[700];
        join_names(exts, sizeof(exts), pCreateInfo->enabledExtensionCount,
                   pCreateInfo->ppEnabledExtensionNames);
        const VkApplicationInfo *ai = pCreateInfo->pApplicationInfo;
        LOGI("vkCreateInstance -> VkResult=%d app=%s apiVersion=%u.%u.%u "
             "extensions(%u)=[%s]",
             (int)r,
             ai && ai->pApplicationName ? ai->pApplicationName : "(null)",
             ai ? VK_API_VERSION_MAJOR(ai->apiVersion) : 0,
             ai ? VK_API_VERSION_MINOR(ai->apiVersion) : 0,
             ai ? VK_API_VERSION_PATCH(ai->apiVersion) : 0,
             pCreateInfo->enabledExtensionCount, exts);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
shim_CreateAndroidSurfaceKHR(VkInstance instance,
                             const VkAndroidSurfaceCreateInfoKHR *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkSurfaceKHR *pSurface)
{
    (void)instance;
    (void)pAllocator;
    if (!pCreateInfo || !pCreateInfo->window || !pSurface) {
        LOGE("vkCreateAndroidSurfaceKHR: argumentos invalidos");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    ShimIcdSurfaceAndroid *s = calloc(1, sizeof(*s));
    if (!s)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    s->base.platform = SHIM_ICD_WSI_PLATFORM_ANDROID;
    s->window = pCreateInfo->window;
    *pSurface = (VkSurfaceKHR)(uintptr_t)s;
    if (PANVK_SHIM_DIAG)
        LOGI("vkCreateAndroidSurfaceKHR -> ok window=%p", (void *)s->window);
    return VK_SUCCESS;
}

/* Solo libera superficies creadas por shim_CreateAndroidSurfaceKHR. */
static VKAPI_ATTR void VKAPI_CALL
shim_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                       const VkAllocationCallbacks *pAllocator)
{
    (void)instance;
    (void)pAllocator;
    if (surface != VK_NULL_HANDLE)
        free((void *)(uintptr_t)surface);
}

/* ---- Simbolos exportados ----------------------------------------------------- */

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    pthread_once(&g_once, load_once);
    if (!g_icd_gipa || !pName)
        return NULL;

    if (instance != VK_NULL_HANDLE && g_last_instance == VK_NULL_HANDLE)
        g_last_instance = instance;

#define MAP(name, fn)                                                          \
    do {                                                                       \
        if (!strcmp(pName, name))                                              \
            return traced("gipa", instance, name, (PFN_vkVoidFunction)(fn));   \
    } while (0)

    MAP("vkGetInstanceProcAddr", vkGetInstanceProcAddr);
    MAP("vkGetDeviceProcAddr", vkGetDeviceProcAddr);
    MAP("vkEnumerateInstanceLayerProperties",
        shim_EnumerateInstanceLayerProperties);
    MAP("vkEnumerateInstanceExtensionProperties",
        shim_EnumerateInstanceExtensionProperties);
    MAP("vkCreateInstance", shim_CreateInstance);

    if (!g_icd_has_android_surface) {
        MAP("vkCreateAndroidSurfaceKHR", shim_CreateAndroidSurfaceKHR);
        MAP("vkDestroySurfaceKHR", shim_DestroySurfaceKHR);
    }
#undef MAP

    PFN_vkVoidFunction r = g_icd_gipa(instance, pName);
    r = maybe_wrap(pName, r);
    return traced("gipa", instance, pName, r);
}

/* Zink (Mesa upstream) hace dlsym de vkGetDeviceProcAddr sobre el mismo
 * handle que vkGetInstanceProcAddr y aborta con "ZINK: failed to get proc
 * address" si falta. El ICD no lo exporta: lo obtenemos con una instancia. */
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    pthread_once(&g_once, load_once);
    if (!g_icd_gipa || !pName)
        return NULL;

    if (!strcmp(pName, "vkGetDeviceProcAddr"))
        return traced("gdpa", device, pName,
                      (PFN_vkVoidFunction)vkGetDeviceProcAddr);

    if (!g_real_gdpa && g_last_instance != VK_NULL_HANDLE)
        g_real_gdpa = (PFN_vkGetDeviceProcAddr)g_icd_gipa(
            g_last_instance, "vkGetDeviceProcAddr");
    if (!g_real_gdpa) {
        if (g_trace && first_time("gdpa-noreal", pName))
            LOGW("vkGetDeviceProcAddr(%s): el ICD aun no tiene instancia", pName);
        return NULL;
    }

    PFN_vkVoidFunction r = g_real_gdpa(device, pName);
    r = maybe_wrap(pName, r);
    return traced("gdpa", device, pName, r);
}
