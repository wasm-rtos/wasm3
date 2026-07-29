
## Logs

To enable various logs, modify the defines in `m3_config.h`. These are only
enabled when compiled in debug mode.

```C
# define d_m3LogParse           0   // .wasm binary decoding info
# define d_m3LogModule          0   // Wasm module info
# define d_m3LogRuntime         0   // higher-level runtime information
# define d_m3LogNativeStack     0   // track the memory usage of the C-stack
```

`m3_PrintProfilerInfo()` remains as a compatibility no-op. The removed
metacode-operation profiler does not describe direct WebAssembly instruction
execution.
