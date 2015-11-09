class AssertionError < RuntimeError
end

def clock_gettime_monotonic()
    # XXX this is for a patched JRuby/truffle only for now! 
    #
    # JRuby does not yet provide access to the (raw) monotonic clock, and
    # adding support is non-trivial (not as simple as adding the C constant).
    # For now we patch JRuby/Truffle to expose our libkruntime function.
    Truffle::Primitive.clock_gettime_monotonic()
end

def assert(cond)
    if not cond
        raise AssertionError
    end
end

# main
if __FILE__ == $0
    if ARGV.length != 3
        puts "usage: iterations_runner.rb <benchmark> <#iterations> <benchmark_param>\n"
        Kernel.exit(1)
    end

    benchmark, iters, param = ARGV
    iters = Integer(iters)
    param = Integer(param)

    assert benchmark.end_with?(".rb")
    require("#{benchmark}")

    STDOUT.write "["
    for krun_iter_num in 0..iters - 1 do  # inclusive upper bound
        STDERR.write "[iterations_runner.rb] iteration #{krun_iter_num + 1}/#{iters}\n"
	STDERR.flush  # JRuby doesn't flush on newline.

        start_time = clock_gettime_monotonic()
        run_iter(param)
        stop_time = clock_gettime_monotonic()

        intvl = stop_time - start_time
        STDOUT.write String(intvl)
        if krun_iter_num < iters - 1 then
            STDOUT.write ", "
        end
    end
    STDOUT.write "]"
end
