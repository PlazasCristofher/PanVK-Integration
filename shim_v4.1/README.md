# panvk_shim (v4)

Shim `libvulkan_panvk_shim.so`: exporta **solo** `vkGetInstanceProcAddr` y
`vkGetDeviceProcAddr` (lo que hacen `dlsym` LWJGL, SDL y Zink/Mesa) y reenvia a
`vk_icdGetInstanceProcAddr` de `libvulkan_panfrost.so` (PanVK-kbase RC3). No
toca `libvulkan.so` del sistema.

**Estado:** v3 compilado y probado en Mali-G68 (prueba aislada y Minecraft 26.3
nativo). v4 probado en PC con un ICD de mentira (logica de rastreo, medidor y
simbolos exportados); **pendiente de probar en el telefono**.

## Que hace el shim

| Funcion | Que hace | Motivo |
|---|---|---|
| `vkGetInstanceProcAddr` | exportada; reenvia al ICD salvo lo de abajo | LWJGL/SDL/Zink la buscan con `dlsym` |
| `vkGetDeviceProcAddr` | **nueva en v4**; exportada; usa la del ICD (obtenida con la instancia creada) | Zink upstream la pide con `dlsym` y aborta con "ZINK: failed to get proc address" si falta (HIPOTESIS: era el fallo de Kopper Zink) |
| `vkEnumerateInstanceLayerProperties` | lista vacia | trabajo del loader |
| `vkEnumerateInstanceExtensionProperties` | lista del ICD + `VK_KHR_android_surface` si falta | en el modelo loader/ICD la anuncia el loader |
| `vkCreateInstance` | quita `VK_KHR_android_surface` si el ICD no la conoce; registra el resultado | el ICD la rechazaria |
| `vkCreateAndroidSurfaceKHR` / `vkDestroySurfaceKHR` | estructura `VkIcdSurfaceAndroid` | el loader lo haria |

Negocia la version de interfaz ICD (v5), abre el ICD con `RTLD_LOCAL`, resuelve
su ruta con `PANVK_ICD_PATH` (o el directorio del shim) y oculta todos los
simbolos salvo los dos exportados.

## Dos builds

| Script | Salida | Contenido |
|---|---|---|
| `tools\build.ps1` | `build\libvulkan_panvk_shim.so` | **diag**: rastreo + PROF + log a stderr |
| `tools\build_clean.ps1` | `build_clean\libvulkan_panvk_shim.so` | **limpia** (`PANVK_SHIM_DIAG=0`): solo reenvia; en logcat deja errores y una linea al cargar |

Ambas exportan los mismos dos simbolos. La linea `ICD cargado: ... build=clean|diag`
dice cual esta cargada. El costo del diag es pequeno (dos `clock_gettime` y unos
atomicos por llamada); la build limpia sirve para confirmarlo comparando FPS.

## Diagnostico (build diag)

Todo va a logcat (tag `PanVKShim`) **y a stderr**, asi que sale en el
`latest_game.log` del launcher. Variables de entorno:

| Variable | Efecto |
|---|---|
| `PANVK_SHIM_QUIET=1` | no escribe a stderr (solo logcat) |
| `PANVK_SHIM_TRACE=0` | desactiva el rastreo de nombres |
| `PANVK_SHIM_PROF=0` | desactiva el medidor de tiempos |

Que se ve en el log:

- `TRACE gipa(...)` / `TRACE gdpa(...)`: cada nombre pedido, una sola vez, con
  `ok` o `NULL`. Muchos `NULL` son normales (extensiones que el ICD no tiene).
  Lo util es ver donde se detiene la secuencia de Zink.
- `vkCreateInstance`, `vkEnumeratePhysicalDevices`, `vkCreateDevice`,
  `vkCreateSwapchainKHR` -> `VkResult` y datos (extensiones, formato, tamano).
- `PROF`: una linea por segundo con fps, tiempo de frame (promedio y maximo) y,
  por llamada, `n`, promedio, maximo y suma en ms: `submit`, `present`,
  `acquire`, `waitFences`, `waitSem`, `queueWaitIdle`, `deviceWaitIdle`.
  Como leerla: si `waitFences`/`acquire`/`present` suman cerca de 1000 ms/s, el
  juego espera a la GPU o a la pantalla; si `submit` suma mucho, el tiempo se
  va dentro del driver; si ninguna suma mucho, el cuello esta fuera de Vulkan.

Limitaciones: el rastreo guarda hasta 1024 nombres; si el programa pide una
funcion por un camino que no pasa por el shim (por ejemplo `dlsym` directo de
otra funcion en el handle), el shim no la ve.

## Pendiente de verificar

1. Valor de `VK_ICD_WSI_PLATFORM_ANDROID` (puesto 5; la presentacion de MC 26.3 funciono).
2. Si Zink + Kopper llega a elegir dispositivo con PanVK tras exportar `vkGetDeviceProcAddr`.
3. Que el Mesa de Kopper/Ironized (binario) pida solo esos dos simbolos con `dlsym`.
4. Si `/dev/mali0` es accesible para el usuario `shell` de adb (si no, probar desde una app).
