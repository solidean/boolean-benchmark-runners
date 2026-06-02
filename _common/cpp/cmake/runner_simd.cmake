# Enable AVX2 (and BMI2 on GCC/Clang) on a runner target, x86/x64 only.
# MSVC's /arch:AVX2 already implies BMI/BMI2/FMA, so no separate BMI2 flag there.
# ARM and other architectures get nothing.
function(runner_enable_simd target)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|x64|amd64|AMD64|i[3-6]86")
    target_compile_options(${target} PRIVATE
      $<$<CXX_COMPILER_ID:MSVC>:/arch:AVX2>
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-mavx2;-mbmi2>
    )
  endif()
endfunction()
