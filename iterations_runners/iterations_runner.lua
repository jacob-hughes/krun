local ffi = require("ffi")

function emit_per_core_measurements(name, num_cores, tbl, tbl_len)
    io.stdout:write(string.format('"%s": [', name))

    for BM_core = 1, num_cores, 1 do
        io.stdout:write("[")
        for BM_i = 1, tbl_len, 1 do
            io.stdout:write(tbl[BM_core][BM_i])
            if BM_i < tbl_len then
                io.stdout:write(", ")
            end
        end
        io.stdout:write("]")
        if BM_core < num_cores then
            io.stdout:write(", ")
        end
    end
    io.stdout:write("]")
end

function usage()
    io.stderr:write("usage: iterations_runner.lua <benchmark> " ..
                    "<# of iterations> <benchmark param>\n           " ..
                    "<debug flag> [instrumentation dir] [key] " ..
                    "[key pexec index]\n\n")
    io.stderr:write("Arguments in [] are for instrumentation mode only.\n")
    os.exit(1)
end

ffi.cdef[[
    void krun_init(void);
    void krun_done(void);
    void krun_measure(int);
    int krun_get_num_cores(void);
    double krun_get_wallclock(int);
    double krun_get_core_cycles_double(int, int);
    double krun_get_aperf_double(int, int);
    double krun_get_mperf_double(int, int);
]]
local libkruntime = ffi.load("kruntime")

local krun_init = libkruntime.krun_init
local krun_measure = libkruntime.krun_measure
local krun_get_num_cores = libkruntime.krun_get_num_cores
local krun_get_wallclock = libkruntime.krun_get_wallclock
local krun_get_core_cycles_double = libkruntime.krun_get_core_cycles_double
local krun_get_aperf_double = libkruntime.krun_get_aperf_double
local krun_get_mperf_double = libkruntime.krun_get_mperf_double

if #arg < 4 then
    usage()
end

local BM_benchmark = arg[1]
local BM_iters = tonumber(arg[2])
local BM_param = tonumber(arg[3])
local BM_debug = tonumber(arg[4]) > 0
local BM_instrument = #arg >= 5

if BM_instrument and #arg ~= 7 then
    usage()
end

dofile(BM_benchmark)

krun_init()
local BM_num_cores = krun_get_num_cores()

-- Pre-allocate and fill results tables.
-- There doesn't appear to be a way to allocate the array all at once in Lua.
local BM_wallclock_times = {}
for BM_i = 1, BM_iters, 1 do
    BM_wallclock_times[BM_i] = -0.0
end

local BM_cycle_counts = {}
for BM_core = 1, BM_num_cores, 1 do
    BM_cycle_counts[BM_core] = {}
    for BM_i = 1, BM_iters, 1 do
        BM_cycle_counts[BM_core][BM_i] = -0.0
    end
end

local BM_aperf_counts = {}
for BM_core = 1, BM_num_cores, 1 do
    BM_aperf_counts[BM_core] = {}
    for BM_i = 1, BM_iters, 1 do
        BM_aperf_counts[BM_core][BM_i] = -0.0
    end
end

local BM_mperf_counts = {}
for BM_core = 1, BM_num_cores, 1 do
    BM_mperf_counts[BM_core] = {}
    for BM_i = 1, BM_iters, 1 do
        BM_mperf_counts[BM_core][BM_i] = -0.0
    end
end

-- Main loop
for BM_i = 1, BM_iters, 1 do
    if BM_debug then
        io.stderr:write(string.format("[iterations_runner.lua] iteration %d/%d\n", BM_i, BM_iters))
    end

    -- Start timed section
    krun_measure(0);
    run_iter(BM_param)
    krun_measure(1);
    -- End timed section

    -- Compute deltas
    BM_wallclock_times[BM_i] = krun_get_wallclock(1) - krun_get_wallclock(0);

    for BM_core = 1, BM_num_cores, 1 do
        BM_cycle_counts[BM_core][BM_i] =
            krun_get_core_cycles_double(1, BM_core - 1) -
            krun_get_core_cycles_double(0, BM_core - 1)
        BM_aperf_counts[BM_core][BM_i] =
             krun_get_aperf_double(1, BM_core - 1) -
             krun_get_aperf_double(0, BM_core - 1)
        BM_mperf_counts[BM_core][BM_i] =
            krun_get_mperf_double(1, BM_core - 1) -
            krun_get_mperf_double(0, BM_core - 1)
    end
end

-- In LuaJIT, FFI functions are cdata values that are unable to reference any other object owned by
-- the garbage collector. Calling an FFI function which has been cached on the stack (as we've done above) 
-- might fail because the parent FFI clib object may have been GC'd . By explicitly accessing krun_done via libkruntime
-- here we guarantee libkruntime to live until this point.
libkruntime.krun_done()

io.stdout:write("{")

io.stdout:write('"wallclock_times": [')
for BM_i = 1, BM_iters, 1 do
    io.stdout:write(BM_wallclock_times[BM_i])
    if BM_i < BM_iters then
        io.stdout:write(", ")
    end
end
io.stdout:write("], ")

emit_per_core_measurements("core_cycle_counts", BM_num_cores, BM_cycle_counts, BM_iters)
io.stdout:write(", ")
emit_per_core_measurements("aperf_counts", BM_num_cores, BM_aperf_counts, BM_iters)
io.stdout:write(", ")
emit_per_core_measurements("mperf_counts", BM_num_cores, BM_mperf_counts, BM_iters)

io.stdout:write("}\n")
