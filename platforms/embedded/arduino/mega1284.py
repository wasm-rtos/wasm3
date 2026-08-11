Import("env")

# PRE scripts update the global construction environment before PlatformIO
# clones it for dependent libraries such as wasm3.
env.Append(
    CPPDEFINES=[
        ("d_m3CodePageAlignSize", 512),
        ("d_m3HasSnapshot", 0),
    ]
)
