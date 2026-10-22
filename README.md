# PanVK integration (Zalith Vulkan load path)

Unofficial reference for loading **PanVK-kbase** on Android Minecraft launchers **without root** and **without replacing** system Vulkan.

This project follows **Zalith Launcher 2's custom driver flow** (plugin + dlopen + VULKAN_PTR), not the system Vulkan loader as the main path for the game.

Not affiliated with Zalith, Mojang, Microsoft, or zenithblue-oss.

## Do not contact the Zalith team for ports

Everything needed to understand and reuse this approach is **in this repository** (docs, patch example, shim).

- If you want PanVK on a **launcher that is not Zalith** and **not a Zalith fork**, **do not contact the Zalith (ZL) developers**.
- If you **own** your launcher (or maintain a fork of another project) and you know **how your launcher loads Vulkan**, adapt this design yourself using the materials here.
- The Zalith team is not support for third-party launchers or custom ports.

Questions about official Zalith features belong on Zalith's own channels. Questions about *this* PanVK method belong in **this** repo's issues.

## How it works (short)

Zalith can load a Vulkan library from a **driver plugin** and pass the handle to the game as VULKAN_PTR.

PanVK's .so is an **ICD** (it exports vk_icdGetInstanceProcAddr, not vkGetInstanceProcAddr).
LWJGL and Zink need vkGetInstanceProcAddr and (for Zink) vkGetDeviceProcAddr on that handle.

So the plugin ships a **shim**:

  Launcher -> dlopen(shim) -> VULKAN_PTR
  Game / Zink -> GIPA + GDPA on that handle
  Shim -> loads PanVK ICD -> Mali (kbase)

Other launchers need an equivalent idea: load the shim (or the same contract), expose GIPA/GDPA to the game, and keep a fallback if load fails.

## What you need

1. **Launcher** changes (see zalith_panvk_*.patch as a Zalith-oriented example, or reimplement the same contract on your load path)
2. **Plugin** with:
   - libvulkan_panvk_shim.so
   - libvulkan_panfrost.so (from https://github.com/zenithblue-oss/panvk-kbase-android — check their license before redistributing)
3. Metadata example (Zalith-style): fclPlugin=true, panvkDriver=true, driver=PanVK-kbase …

## Env vars (when user selects PanVK)

| Variable | Meaning |
|----------|---------|
| DRIVER_PATH | Plugin native lib folder |
| PANVK_SHIM_PATH | Path to the shim .so |
| PANVK_ICD_PATH | Path to the PanVK ICD .so |
| VULKAN_PTR | Set by native code after dlopen (hex handle) |
| MESA_SHADER_CACHE_DIR | Writable cache (recommended) |

Native load order idea (Zalith-style): **PanVK shim -> else Turnip (if any) -> else system libvulkan.so**.

## Files in this repo

- HOW_IT_WORKS.md — full explanation
- zalith_panvk_*.patch — example changes for Zalith Launcher 2
- shim_*/ — shim source and build scripts

## Tested on

Samsung Galaxy A16 5G, Mali-G68 MC2, Android 16
Minecraft 26.x Vulkan + PanVK, and Zink/Kopper on older versions

Experimental. Performance may be lower than OpenGL at full resolution.

## Credits

- Zalith Launcher 2 — plugin / VULKAN_PTR load design used as the reference flow
- zenithblue-oss — PanVK-kbase
- Mesa / panvk
- Panlith + Basalt team

## License

See individual files and upstream projects. Zalith is GPL-3.0 (respect their additional terms if you ship a modified launcher). ICD: zenithblue-oss / Mesa terms.
