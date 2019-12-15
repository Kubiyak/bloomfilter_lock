optimized_env = Environment(CXX="g++-8", CC="g++-8", CXXFLAGS="--std=c++17 -O2")
optimized_env.VariantDir('build/optimized', './')
optimized = optimized_env.Program('build/optimized/bloomfilter_lock_test', 
['build/optimized/main.cpp'], LIBS=['pthread'])
Depends('build/optimized/bloomfilter_lock_test', ['bloomfilter_lock.hpp', 'bloomfilter_lock_impl.hpp'])
optimized_env.Alias('optimized', optimized)

gprof_env = Environment(CXX="g++-8", CXXFLAGS="--std=c++17 -O2 -pg", LINKFLAGS="-pg")
gprof_env.VariantDir('build/gprof', './') 
gprof = gprof_env.Program('build/gprof/bloomfilter_lock_pg', ['build/gprof/main.cpp'], LIBS=['pthread'])
Depends('build/gprof/bloomfilter_lock_pg', ['bloomfilter_lock.hpp', 'bloomfilter_lock_impl.hpp'])
gprof_env.Alias('gprof', gprof)
