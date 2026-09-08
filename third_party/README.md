# Third-party code

The bundled Lua sources provide the scripting VM and carry their own
[license](lua/LICENSE). Keeping them here separates upstream runtime code from
TomoKV's command integration in `src/cmd/scripting.cc`.
