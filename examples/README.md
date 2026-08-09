# raylib-on-vulkan examples

The upstream raylib examples suite ported to the Vulkan backend, organized in
the same categories as upstream (`core`, `shapes`, `textures`, `text`,
`models`, `shaders`, `audio`, `others`), plus raylib-on-vulkan-specific additions such
as `others/vk_custom_commands.c`. See `PORTING.md` for the porting rules and
per-category `STATUS.md` for verification reports.

## Building the examples

The examples assume the `raylib-on-vulkan` library in `../src`.

### With GNU make

- `make` builds all examples (the library is built on demand)
- `make [category]` builds one category (e.g. `make core`)
- `make [category]/[name]` builds a single example (e.g. `make core/core_basic_window`)
- `make PLATFORM=WAYLAND` selects the backend: `X11 WAYLAND WIN32 MACOS ANDROID`

Binaries are placed beside their sources; run them from the category directory
so they find `resources/`.

### With CMake

Configured from the repository root with `-DBUILD_EXAMPLES=ON` (default).
Binaries build to `<build>/examples/<category>/` next to a copied `resources/`
dir — run them from that directory.
