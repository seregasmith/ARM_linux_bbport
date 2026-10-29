// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Check whether native-size depth/stencil images can be blitted to reduced
 * renderer targets. The renderer needs both directions for live presets.
 * --gamepads: the connected gamepads, "GUID<tab>name" per line (the launcher's controller list,
 * BB_GAMEPAD). --displays: the monitors, "name<tab>WxH<tab>primary (1 or 0)" per line in SDL's
 * order (the launcher's monitor list, BB_DISPLAY; issue #69). --read-input: one key or button for the
 * launcher's controls (below). --device: "vendorID<tab>name" of the GPU the game takes (run.sh:
 * memory model and driver workarounds by vendor and chip). --gpus: every GPU in Vulkan's order
 * (BB_GPU_ID is the index), "index<tab>vendorID<tab>type<tab>name<tab>1 for the one the port takes
 * by itself" per line (the launcher's GPU list). --pad-events: gamepad presses for the launcher's
 * own navigation (below). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>

static VkDeviceSize largest_local_heap(VkPhysicalDevice device) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    VkDeviceSize largest = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            memory.memoryHeaps[i].size > largest) {
            largest = memory.memoryHeaps[i].size;
        }
    }
    return largest;
}

static int better_device(VkPhysicalDevice candidate, VkPhysicalDevice current) {
    VkPhysicalDeviceProperties next, old;
    vkGetPhysicalDeviceProperties(candidate, &next);
    vkGetPhysicalDeviceProperties(current, &old);
    const int next_api = next.apiVersion >= VK_API_VERSION_1_3;
    const int old_api = old.apiVersion >= VK_API_VERSION_1_3;
    if (next_api != old_api) return next_api;
    const int next_discrete = next.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    const int old_discrete = old.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    if (next_discrete != old_discrete) return next_discrete;
    const int next_cpu = next.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    const int old_cpu = old.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    if (next_cpu != old_cpu) return !next_cpu;
    return largest_local_heap(candidate) > largest_local_heap(current);
}

/* --live-resolution: prints 1 when live resolution changes suit the GPU (run.sh, setting
 * live_resolution=auto), else 0. Live scaling keeps the game's post-processing at 1080p and
 * copies scene targets every frame: fine on a strong discrete GPU, 7-8 FPS on the Steam Deck
 * and a GTX 1060. Rule: discrete, at least 8 GB of device memory, and not an NVIDIA GPU older
 * than Turing (no fragment shader barycentrics; its depth/stencil copies take nine draws). */
static int has_extension(VkPhysicalDevice device, const char *name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *list = calloc(count ? count : 1, sizeof(*list));
    int found = 0;
    if (list && vkEnumerateDeviceExtensionProperties(device, NULL, &count, list) == VK_SUCCESS)
        for (uint32_t i = 0; i < count && !found; ++i) found = !strcmp(list[i].extensionName, name);
    free(list);
    return found;
}

static int live_resolution_suits(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    const int discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    const VkDeviceSize memory = largest_local_heap(device);
    const int old_nvidia = props.vendorID == 0x10de &&
                           !has_extension(device, "VK_KHR_fragment_shader_barycentric");
    const int suits = discrete && memory >= (VkDeviceSize)7680 << 20 && !old_nvidia;
    fprintf(stderr, "GPU: %s, %s, %llu MiB: live resolution changes %s\n", props.deviceName,
            discrete ? "discrete" : "integrated or other", (unsigned long long)(memory >> 20),
            suits ? "on" : "off (startup resolution patch)");
    return suits;
}

static int list_gamepads(void) {
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "gamepads: %s\n", SDL_GetError());
        return 1;
    }
    // Some devices (HIDAPI) show up only after events are pumped.
    for (int i = 0; i < 5; ++i) {
        SDL_PumpEvents();
        SDL_Delay(40);
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        char guid[33];
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), guid, sizeof guid);
        const char *name = SDL_GetGamepadNameForID(ids[i]);
        printf("%s\t%s\n", guid, name ? name : "?");
    }
    SDL_free(ids);
    SDL_Quit();
    return 0;
}

static int list_displays(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "displays: %s\n", SDL_GetError());
        return 1;
    }
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    int count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    for (int i = 0; ids && i < count; ++i) {
        const char *name = SDL_GetDisplayName(ids[i]);
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(ids[i]);
        printf("%s\t%dx%d\t%d\n", name && *name ? name : "?", mode ? mode->w : 0,
               mode ? mode->h : 0, ids[i] == primary);
    }
    SDL_free(ids);
    SDL_Quit();
    return 0;
}

/* --read-input key|pad: a small window; prints "key <SDL key name>" ("Mouse Left", "Wheel Up", ...
 * for the mouse) or "pad <SDL button name>"|pad: a small window; prints "key <SDL key name>" or "pad <SDL button name>"
 * (lefttrigger/righttrigger for the triggers) for the first key or gamepad button pressed, the
 * names bbport.ini's key.* and pad.* lines take. Escape, closing it or 15 s: nothing. */
static int read_input(const char *kind) {
    const int want_key = strcmp(kind, "pad") != 0, want_pad = strcmp(kind, "key") != 0;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "read-input: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    const char *prompt = want_key && want_pad ? "Press a key, a mouse button or a gamepad button"
                         : want_key           ? "Press a key or a mouse button"
                                              : "Press a gamepad button";
    if (!SDL_CreateWindowAndRenderer("bbport", 520, 90, 0, &window, &renderer)) {
        fprintf(stderr, "read-input: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    const Uint64 end = SDL_GetTicks() + 15000;
    int done = 0;
    while (!done && SDL_GetTicks() < end) {
        SDL_SetRenderDrawColor(renderer, 24, 24, 28, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
        SDL_RenderDebugText(renderer, 16, 30, prompt);
        SDL_RenderDebugText(renderer, 16, 50, "Escape: cancel");
        SDL_RenderPresent(renderer);
        SDL_Event e;
        while (!done && SDL_WaitEventTimeout(&e, 50)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                done = 1;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                SDL_OpenGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    done = 1;
                } else if (want_key) {
                    printf("key %s\n", SDL_GetScancodeName(e.key.scancode));
                    done = 1;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: /* the names runtime_pad.c's key.* lines take */
                if (want_key && e.button.button >= SDL_BUTTON_LEFT && e.button.button <= SDL_BUTTON_X2) {
                    static const char *const names[] = {"", "Mouse Left", "Mouse Middle", "Mouse Right",
                                                        "Mouse X1", "Mouse X2"};
                    printf("key %s\n", names[e.button.button]);
                    done = 1;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (want_key && e.wheel.y != 0) {
                    const float y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
                    printf("key %s\n", y > 0 ? "Wheel Up" : "Wheel Down");
                    done = 1;
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (want_pad) {
                    printf("pad %s\n", SDL_GetGamepadStringForButton((SDL_GamepadButton)e.gbutton.button));
                    done = 1;
                }
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (want_pad && e.gaxis.value > 16000 &&
                    (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
                    printf("pad %s\n", e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "lefttrigger" : "righttrigger");
                    done = 1;
                }
                break;
            default:
                break;
            }
        }
    }
    fflush(stdout);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

/* --pad-events [GUID]: gamepad presses for the launcher's gamepad navigation, one line each:
 * "down NAME [LABEL]" and "up NAME". NAME: SDL's button name (a, b, x, y by position: south,
 * east, west, north; dpup, dpdown, dpleft, dpright, leftshoulder, rightshoulder, back, start,
 * guide, leftstick, rightstick, ...), lefttrigger / righttrigger, or a stick direction (left-up,
 * left-down, left-left, left-right, right-up, ...). LABEL, for the face buttons where SDL knows
 * it: what the button says (A, B, X, Y, cross, circle, square, triangle; Nintendo layouts have A
 * on the east button). "reset" when a gamepad comes or goes: whatever was held is let go. With
 * GUID only that gamepad while it is connected (issue #15: wheels and other controllers), else
 * every one. "tick" every 2 s: once the launcher is gone the write fails and the helper ends. */
#define PAD_SLOTS 16
typedef struct {
    SDL_JoystickID id;
    SDL_Gamepad *pad;
    char guid[33];
    int axis[SDL_GAMEPAD_AXIS_COUNT]; /* -1, 0, 1: the stick direction or trigger held */
} PadSlot;

static int pad_line(const char *verb, const char *name, const char *label) {
    if (label) {
        printf("%s %s %s\n", verb, name, label);
    } else if (name) {
        printf("%s %s\n", verb, name);
    } else {
        printf("%s\n", verb);
    }
    return fflush(stdout) == 0 && !ferror(stdout);
}

static const char *pad_axis_name(int axis, int direction) {
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return direction < 0 ? "left-left" : "left-right";
    case SDL_GAMEPAD_AXIS_LEFTY: return direction < 0 ? "left-up" : "left-down";
    case SDL_GAMEPAD_AXIS_RIGHTX: return direction < 0 ? "right-left" : "right-right";
    case SDL_GAMEPAD_AXIS_RIGHTY: return direction < 0 ? "right-up" : "right-down";
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "lefttrigger";
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "righttrigger";
    default: return NULL;
    }
}

static const char *pad_label_name(SDL_GamepadButtonLabel label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "triangle";
    default: return NULL;
    }
}

static PadSlot *pad_slot(PadSlot *slots, SDL_JoystickID id) {
    for (int i = 0; i < PAD_SLOTS; ++i)
        if (slots[i].pad && slots[i].id == id) return &slots[i];
    return NULL;
}

/* The chosen gamepad only while it is connected, else any. */
static int pad_wanted(const PadSlot *slots, const PadSlot *slot, const char *want) {
    if (!want || !*want) return 1;
    int chosen_here = 0;
    for (int i = 0; i < PAD_SLOTS; ++i)
        if (slots[i].pad && !SDL_strcasecmp(slots[i].guid, want)) chosen_here = 1;
    return !chosen_here || !SDL_strcasecmp(slot->guid, want);
}

static int pad_events(const char *want) {
    /* No window here: the launcher decides whether its own window takes the presses. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "pad-events: %s\n", SDL_GetError());
        return 1;
    }
    PadSlot slots[PAD_SLOTS];
    SDL_memset(slots, 0, sizeof slots);
    int alive = pad_line("ready", NULL, NULL);
    Uint64 next_tick = SDL_GetTicks() + 2000;
    while (alive) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 500)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                alive = 0;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad_slot(slots, e.gdevice.which)) {
                    for (int i = 0; i < PAD_SLOTS; ++i) {
                        if (slots[i].pad) continue;
                        SDL_Gamepad *pad = SDL_OpenGamepad(e.gdevice.which);
                        if (!pad) break;
                        SDL_memset(&slots[i], 0, sizeof slots[i]);
                        slots[i].id = e.gdevice.which;
                        slots[i].pad = pad;
                        SDL_GUIDToString(SDL_GetGamepadGUIDForID(e.gdevice.which), slots[i].guid,
                                         sizeof slots[i].guid);
                        alive = pad_line("reset", NULL, NULL);
                        break;
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED: {
                PadSlot *slot = pad_slot(slots, e.gdevice.which);
                if (slot) {
                    SDL_CloseGamepad(slot->pad);
                    SDL_memset(slot, 0, sizeof *slot);
                    alive = pad_line("reset", NULL, NULL);
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                PadSlot *slot = pad_slot(slots, e.gbutton.which);
                if (!slot || !pad_wanted(slots, slot, want)) break;
                const SDL_GamepadButton button = (SDL_GamepadButton)e.gbutton.button;
                const char *name = SDL_GetGamepadStringForButton(button);
                if (!name) break;
                if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    alive = pad_line("down", name, pad_label_name(SDL_GetGamepadButtonLabel(slot->pad, button)));
                } else {
                    alive = pad_line("up", name, NULL);
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                PadSlot *slot = pad_slot(slots, e.gaxis.which);
                const int axis = e.gaxis.axis;
                if (!slot || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT || !pad_axis_name(axis, 1) ||
                    !pad_wanted(slots, slot, want))
                    break;
                /* Half way pressed, a quarter let go (a stick resting a little off centre). */
                const int value = e.gaxis.value;
                const int trigger = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                const int held = slot->axis[axis];
                int now = held;
                if (held > 0 && value < 8192) now = 0;
                if (held < 0 && value > -8192) now = 0;
                if (now == 0) now = value > 16384 ? 1 : value < -16384 && !trigger ? -1 : 0;
                if (now != held) {
                    if (held) alive = pad_line("up", pad_axis_name(axis, held), NULL);
                    if (alive && now) alive = pad_line("down", pad_axis_name(axis, now), NULL);
                    slot->axis[axis] = now;
                }
                break;
            }
            default:
                break;
            }
        }
        if (alive && SDL_GetTicks() >= next_tick) {
            alive = pad_line("tick", NULL, NULL);
            next_tick = SDL_GetTicks() + 2000;
        }
    }
    for (int i = 0; i < PAD_SLOTS; ++i)
        if (slots[i].pad) SDL_CloseGamepad(slots[i].pad);
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--read-input")) {
        return read_input(argc > 2 ? argv[2] : "any");
    }
    if (argc > 1 && !strcmp(argv[1], "--gamepads")) {
        return list_gamepads();
    }
    if (argc > 1 && !strcmp(argv[1], "--pad-events")) {
        return pad_events(argc > 2 ? argv[2] : NULL);
    }
    if (argc > 1 && !strcmp(argv[1], "--displays")) {
        return list_displays();
    }
    const int live_mode = argc > 1 && !strcmp(argv[1], "--live-resolution");
    const int device_mode = argc > 1 && !strcmp(argv[1], "--device");
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bbport scene scaling probe",
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&create, NULL, &instance) != VK_SUCCESS) {
        fputs("GPU scene scaling: cannot create Vulkan instance\n", stderr);
        return 1;
    }
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS || !count) {
        fputs("GPU scene scaling: no Vulkan device\n", stderr);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices || vkEnumeratePhysicalDevices(instance, &count, devices) != VK_SUCCESS) {
        fputs("GPU scene scaling: cannot enumerate Vulkan devices\n", stderr);
        free(devices);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--gpus")) {
        VkPhysicalDevice best = devices[0];
        for (uint32_t i = 1; i < count; ++i)
            if (better_device(devices[i], best)) best = devices[i];
        static const char *const types[] = {"other", "integrated", "discrete", "virtual", "cpu"};
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devices[i], &props);
            const unsigned type = props.deviceType <= VK_PHYSICAL_DEVICE_TYPE_CPU ? props.deviceType : 0;
            printf("%u\t%#06x\t%s\t%s\t%d\n", i, props.vendorID, types[type], props.deviceName,
                   devices[i] == best);
        }
        free(devices);
        vkDestroyInstance(instance, NULL);
        return 0;
    }
    /* Match vk_instance.cpp's default ranking or its explicit BB_GPU_ID index. */
    VkPhysicalDevice selected = devices[0];
    const char *gpu_id = getenv("BB_GPU_ID");
    if (gpu_id && atoi(gpu_id) >= 0) {
        const unsigned long index = strtoul(gpu_id, NULL, 10);
        if (index >= count) {
            fputs("GPU scene scaling: BB_GPU_ID is outside the device list\n", stderr);
            free(devices);
            vkDestroyInstance(instance, NULL);
            return 1;
        }
        selected = devices[index];
    } else {
        for (uint32_t i = 1; i < count; ++i)
            if (better_device(devices[i], selected)) selected = devices[i];
    }
    if (device_mode) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(selected, &props);
        printf("%#06x\t%s\n", props.vendorID, props.deviceName);
        free(devices);
        vkDestroyInstance(instance, NULL);
        return 0;
    }
    if (live_mode) {
        printf("%d\n", live_resolution_suits(selected));
        free(devices);
        vkDestroyInstance(instance, NULL);
        return 0;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(selected, &props);
    const struct { VkFormat format; const char *name; } formats[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, "RGBA8"},
        {VK_FORMAT_R8G8B8A8_SRGB, "RGBA8 sRGB"},
        {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11"},
        {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"},
        {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32S8"},
    };
    int supported = 1;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        VkFormatProperties features;
        vkGetPhysicalDeviceFormatProperties(selected, formats[i].format, &features);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                              VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((features.optimalTilingFeatures & required) != required) {
            fprintf(stderr, "GPU scene scaling: %s lacks blit support for %s\n",
                    props.deviceName, formats[i].name);
            supported = 0;
        }
    }
    if (supported) fprintf(stderr, "GPU scene scaling: %s supports live presets\n", props.deviceName);
    free(devices);
    vkDestroyInstance(instance, NULL);
    return supported ? 0 : 1;
}
