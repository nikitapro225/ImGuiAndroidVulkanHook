# WIP : ImGui Android Vulkan Hook

This project demonstrates an overlay implementation using ImGui with Vulkan for Android applications. It includes function hooks to inject a custom graphical menu and intercept touch events. Designed for educational and experimental purposes only.

## Features
- Vulkan-based rendering for ImGui overlays.
- Hooking Vulkan functions (`vkQueueSubmit`, `vkCreateSwapchainKHR`) to integrate ImGui.
- Customizable mod menu example with touch event handling.
- Android Native Window support.

## Requirements
- **Android NDK** for building native libraries.
- Vulkan-capable Android device.
- External libraries:
  - [ImGui v1.95.x](https://github.com/ocornut/imgui)
  - [Dobby Hooking Library](https://github.com/jmpews/Dobby)
  - Vulkan SDK
