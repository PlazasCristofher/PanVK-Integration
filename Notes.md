English:


# Panlith Project: Notes (2026-10-20)

## What was achieved
- Minecraft Java 26.x with native Vulkan on Mali-G68 MC2 (Samsung A16 5G, Android 16), without root, without modifying the system driver.

- OpenGL on Kopper/Zink with PanVK (tested on 1.8.9).

- Fallback to OpenGL with MobileGlues if PanVK is unavailable.

## How it's linked (summary)
1. Plugin = Android app with metadata fclPlugin=true, driver=<name>, panvkDriver=true and two .so files: libvulkan_panfrost.so (zenithblue-oss ICD) and libvulkan_panvk_shim.so (custom).

2. The launcher (v3 patch on Zalith) reads the plugin and passes DRIVER_PATH, PANVK_SHIM_PATH, and PANVK_ICD_PATH.

3. Native code (egl_bridge.c, loadPanVKVulkan) loads the shim, negotiates with the ICD, and publishes VULKAN_PTR.

4. LWJGL (26.x) and Zink (OpenGL) use VULKAN_PTR. The shim exports vkGetInstanceProcAddr and vkGetDeviceProcAddr (Zink failed without the latter; likely cause, not isolated).
 

Spanish:


 # Panlith Proyecto: notas (2026-10-20)

## Que se logro
- Minecraft Java 26.x con Vulkan nativo en Mali-G68 MC2 (Samsung A16 5G, Android 16), sin root, sin tocar el driver del sistema.
- OpenGL sobre Kopper/Zink con PanVK (probado en 1.8.9).
- Fallback a OpenGL con MobileGlues si no hay PanVK.

## Como se vincula (resumen)
1. Plugin = app Android con meta-data fclPlugin=true, driver=<nombre>, panvkDriver=true y dos .so: libvulkan_panfrost.so (ICD de zenithblue-oss) y libvulkan_panvk_shim.so (propio).
2. Launcher (parche v3 sobre Zalith) lee el plugin y pasa DRIVER_PATH, PANVK_SHIM_PATH, PANVK_ICD_PATH.
3. Codigo nativo (egl_bridge.c, loadPanVKVulkan) carga el shim, negocia con el ICD y publica VULKAN_PTR.
4. LWJGL (26.x) y Zink (OpenGL) usan VULKAN_PTR. El shim exporta vkGetInstanceProcAddr y vkGetDeviceProcAddr (Zink fallaba sin el segundo; causa probable, no aislada).


Datos de prueba (informales, sin controlar)
- Release + shim limpio, MC 26.3, mundo normal, 2340x1080: 100% res 2 chunks 18-25 FPS; 8 chunks 13-23. 25% res: 30-55 (2 chunks), 27-40 (8 chunks).
- Con shim de diagnostico en launcher debug: frame ~50 ms; present ~40 ms (lo que mas pesa). Hipotesis sin confirmar: present espera a la GPU.
- Otro usuario (issue #23 de zenithblue-oss, Mali-G615) ve techo parecido (~20 FPS) con RC4.

## Pendiente
- Issue a zenithblue-oss pidiendo permiso para redistribuir el .so; si no responden, publicar el plugin sin el .so.
- Repo publico "Panlith" con el README principal; repo publico nuevo de Basalt sin .so en el historial (renombrar el privado a Basalt-Driver-private).
- Pruebas A/B de FPS (mismo mundo y punto), probar RC4-lowmem.
- Llaves propias de CurseForge/Microsoft si se quieren esas funciones.
- Aclarar licencia del binario antes de publicar nada que lo incluya.
