# This file builds the vendored sources that fram uses, from the root `vendor/` directory, and
# isolates them from first-party warnings and analyzers.

cmake_path(SET vendor_dir NORMALIZE "${PROJECT_SOURCE_DIR}/../../vendor")
set(vendor_binary_dir "${PROJECT_BINARY_DIR}/vendor")

add_library(vendor_acutest INTERFACE)
target_include_directories(
  vendor_acutest SYSTEM INTERFACE "$<BUILD_INTERFACE:${vendor_dir}/acutest>"
)

add_library(vendor_copt INTERFACE)
target_include_directories(
  vendor_copt SYSTEM INTERFACE "$<BUILD_INTERFACE:${vendor_dir}/copt>"
)

# libjpeg-turbo's library sources, as its own build lists them. The files in `src/wrapper/` compile
# one precision-generic source once for each sample precision it supports.
add_library(
  vendor_libjpeg_turbo STATIC
  ${vendor_dir}/libjpeg-turbo/src/jaricom.c
  ${vendor_dir}/libjpeg-turbo/src/jcapimin.c
  ${vendor_dir}/libjpeg-turbo/src/jcarith.c
  ${vendor_dir}/libjpeg-turbo/src/jchuff.c
  ${vendor_dir}/libjpeg-turbo/src/jcicc.c
  ${vendor_dir}/libjpeg-turbo/src/jcinit.c
  ${vendor_dir}/libjpeg-turbo/src/jclhuff.c
  ${vendor_dir}/libjpeg-turbo/src/jcmarker.c
  ${vendor_dir}/libjpeg-turbo/src/jcmaster.c
  ${vendor_dir}/libjpeg-turbo/src/jcomapi.c
  ${vendor_dir}/libjpeg-turbo/src/jcparam.c
  ${vendor_dir}/libjpeg-turbo/src/jcphuff.c
  ${vendor_dir}/libjpeg-turbo/src/jctrans.c
  ${vendor_dir}/libjpeg-turbo/src/jdapimin.c
  ${vendor_dir}/libjpeg-turbo/src/jdarith.c
  ${vendor_dir}/libjpeg-turbo/src/jdatadst.c
  ${vendor_dir}/libjpeg-turbo/src/jdatasrc.c
  ${vendor_dir}/libjpeg-turbo/src/jdhuff.c
  ${vendor_dir}/libjpeg-turbo/src/jdicc.c
  ${vendor_dir}/libjpeg-turbo/src/jdinput.c
  ${vendor_dir}/libjpeg-turbo/src/jdlhuff.c
  ${vendor_dir}/libjpeg-turbo/src/jdmarker.c
  ${vendor_dir}/libjpeg-turbo/src/jdmaster.c
  ${vendor_dir}/libjpeg-turbo/src/jdphuff.c
  ${vendor_dir}/libjpeg-turbo/src/jdtrans.c
  ${vendor_dir}/libjpeg-turbo/src/jerror.c
  ${vendor_dir}/libjpeg-turbo/src/jfdctflt.c
  ${vendor_dir}/libjpeg-turbo/src/jmemmgr.c
  ${vendor_dir}/libjpeg-turbo/src/jmemnobs.c
  ${vendor_dir}/libjpeg-turbo/src/jpeg_nbits.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcapistd-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcapistd-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcapistd-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jccoefct-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jccoefct-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jccolor-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jccolor-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jccolor-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcdctmgr-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcdctmgr-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcdiffct-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcdiffct-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcdiffct-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jclossls-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jclossls-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jclossls-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcmainct-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcmainct-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcmainct-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcprepct-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcprepct-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcprepct-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcsample-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcsample-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jcsample-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdapistd-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdapistd-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdapistd-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdcoefct-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdcoefct-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdcolor-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdcolor-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdcolor-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jddctmgr-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jddctmgr-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jddiffct-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jddiffct-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jddiffct-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdlossls-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdlossls-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdlossls-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdmainct-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdmainct-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdmainct-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdmerge-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdmerge-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdpostct-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdpostct-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdpostct-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdsample-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdsample-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jdsample-16.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jfdctfst-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jfdctfst-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jfdctint-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jfdctint-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jidctflt-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jidctflt-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jidctfst-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jidctfst-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jidctint-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jidctint-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jidctred-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jidctred-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jquant1-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jquant1-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jquant2-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jquant2-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jutils-8.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jutils-12.c
  ${vendor_dir}/libjpeg-turbo/src/wrapper/jutils-16.c
)
target_include_directories(
  vendor_libjpeg_turbo SYSTEM
  PUBLIC "$<BUILD_INTERFACE:${vendor_dir}/libjpeg-turbo/src>"
         "$<BUILD_INTERFACE:${vendor_dir}/libjpeg-turbo/config>"
)

# libwebp's encoder and decoder: their sources, the DSP and utility sources they use, and
# libsharpyuv, without the demuxer, the muxer or the programs. Only the tests decode WebP, so the
# decoder's objects stay out of `fram`, which never refers to them. `WEBP_USE_THREAD` makes the
# one-time DSP setup safe when several workers encode at once. It also allows threads inside an
# encode, which the encoder's configuration leaves off.
add_library(
  vendor_libwebp STATIC
  ${vendor_dir}/libwebp/sharpyuv/sharpyuv.c
  ${vendor_dir}/libwebp/sharpyuv/sharpyuv_cpu.c
  ${vendor_dir}/libwebp/sharpyuv/sharpyuv_csp.c
  ${vendor_dir}/libwebp/sharpyuv/sharpyuv_dsp.c
  ${vendor_dir}/libwebp/sharpyuv/sharpyuv_gamma.c
  ${vendor_dir}/libwebp/sharpyuv/sharpyuv_neon.c
  ${vendor_dir}/libwebp/sharpyuv/sharpyuv_sse2.c
  ${vendor_dir}/libwebp/src/dsp/alpha_processing_neon.c
  ${vendor_dir}/libwebp/src/dsp/alpha_processing_sse2.c
  ${vendor_dir}/libwebp/src/dsp/alpha_processing_sse41.c
  ${vendor_dir}/libwebp/src/dec/alpha_dec.c
  ${vendor_dir}/libwebp/src/dec/buffer_dec.c
  ${vendor_dir}/libwebp/src/dec/frame_dec.c
  ${vendor_dir}/libwebp/src/dec/idec_dec.c
  ${vendor_dir}/libwebp/src/dec/io_dec.c
  ${vendor_dir}/libwebp/src/dec/quant_dec.c
  ${vendor_dir}/libwebp/src/dec/tree_dec.c
  ${vendor_dir}/libwebp/src/dec/vp8_dec.c
  ${vendor_dir}/libwebp/src/dec/vp8l_dec.c
  ${vendor_dir}/libwebp/src/dec/webp_dec.c
  ${vendor_dir}/libwebp/src/dsp/alpha_processing.c
  ${vendor_dir}/libwebp/src/dsp/cost_neon.c
  ${vendor_dir}/libwebp/src/dsp/cost_sse2.c
  ${vendor_dir}/libwebp/src/dsp/cost.c
  ${vendor_dir}/libwebp/src/dsp/cpu.c
  ${vendor_dir}/libwebp/src/dsp/dec_clip_tables.c
  ${vendor_dir}/libwebp/src/dsp/dec_neon.c
  ${vendor_dir}/libwebp/src/dsp/dec_sse2.c
  ${vendor_dir}/libwebp/src/dsp/dec_sse41.c
  ${vendor_dir}/libwebp/src/dsp/dec.c
  ${vendor_dir}/libwebp/src/dsp/enc_neon.c
  ${vendor_dir}/libwebp/src/dsp/enc_sse2.c
  ${vendor_dir}/libwebp/src/dsp/enc_sse41.c
  ${vendor_dir}/libwebp/src/dsp/enc.c
  ${vendor_dir}/libwebp/src/dsp/filters_neon.c
  ${vendor_dir}/libwebp/src/dsp/filters_sse2.c
  ${vendor_dir}/libwebp/src/dsp/filters.c
  ${vendor_dir}/libwebp/src/dsp/lossless_avx2.c
  ${vendor_dir}/libwebp/src/dsp/lossless_enc_avx2.c
  ${vendor_dir}/libwebp/src/dsp/lossless_enc_neon.c
  ${vendor_dir}/libwebp/src/dsp/lossless_enc_sse2.c
  ${vendor_dir}/libwebp/src/dsp/lossless_enc_sse41.c
  ${vendor_dir}/libwebp/src/dsp/lossless_enc.c
  ${vendor_dir}/libwebp/src/dsp/lossless_neon.c
  ${vendor_dir}/libwebp/src/dsp/lossless_sse2.c
  ${vendor_dir}/libwebp/src/dsp/lossless_sse41.c
  ${vendor_dir}/libwebp/src/dsp/lossless.c
  ${vendor_dir}/libwebp/src/dsp/rescaler_neon.c
  ${vendor_dir}/libwebp/src/dsp/rescaler_sse2.c
  ${vendor_dir}/libwebp/src/dsp/rescaler.c
  ${vendor_dir}/libwebp/src/dsp/ssim_sse2.c
  ${vendor_dir}/libwebp/src/dsp/ssim.c
  ${vendor_dir}/libwebp/src/dsp/upsampling_neon.c
  ${vendor_dir}/libwebp/src/dsp/upsampling_sse2.c
  ${vendor_dir}/libwebp/src/dsp/upsampling_sse41.c
  ${vendor_dir}/libwebp/src/dsp/upsampling.c
  ${vendor_dir}/libwebp/src/dsp/yuv_neon.c
  ${vendor_dir}/libwebp/src/dsp/yuv_sse2.c
  ${vendor_dir}/libwebp/src/dsp/yuv_sse41.c
  ${vendor_dir}/libwebp/src/dsp/yuv.c
  ${vendor_dir}/libwebp/src/enc/alpha_enc.c
  ${vendor_dir}/libwebp/src/enc/analysis_enc.c
  ${vendor_dir}/libwebp/src/enc/backward_references_cost_enc.c
  ${vendor_dir}/libwebp/src/enc/backward_references_enc.c
  ${vendor_dir}/libwebp/src/enc/config_enc.c
  ${vendor_dir}/libwebp/src/enc/cost_enc.c
  ${vendor_dir}/libwebp/src/enc/filter_enc.c
  ${vendor_dir}/libwebp/src/enc/frame_enc.c
  ${vendor_dir}/libwebp/src/enc/histogram_enc.c
  ${vendor_dir}/libwebp/src/enc/iterator_enc.c
  ${vendor_dir}/libwebp/src/enc/near_lossless_enc.c
  ${vendor_dir}/libwebp/src/enc/picture_csp_enc.c
  ${vendor_dir}/libwebp/src/enc/picture_enc.c
  ${vendor_dir}/libwebp/src/enc/picture_psnr_enc.c
  ${vendor_dir}/libwebp/src/enc/picture_rescale_enc.c
  ${vendor_dir}/libwebp/src/enc/picture_tools_enc.c
  ${vendor_dir}/libwebp/src/enc/predictor_enc.c
  ${vendor_dir}/libwebp/src/enc/quant_enc.c
  ${vendor_dir}/libwebp/src/enc/syntax_enc.c
  ${vendor_dir}/libwebp/src/enc/token_enc.c
  ${vendor_dir}/libwebp/src/enc/tree_enc.c
  ${vendor_dir}/libwebp/src/enc/vp8l_enc.c
  ${vendor_dir}/libwebp/src/enc/webp_enc.c
  ${vendor_dir}/libwebp/src/utils/bit_reader_utils.c
  ${vendor_dir}/libwebp/src/utils/bit_writer_utils.c
  ${vendor_dir}/libwebp/src/utils/color_cache_utils.c
  ${vendor_dir}/libwebp/src/utils/filters_utils.c
  ${vendor_dir}/libwebp/src/utils/huffman_encode_utils.c
  ${vendor_dir}/libwebp/src/utils/huffman_utils.c
  ${vendor_dir}/libwebp/src/utils/palette.c
  ${vendor_dir}/libwebp/src/utils/quant_levels_dec_utils.c
  ${vendor_dir}/libwebp/src/utils/quant_levels_utils.c
  ${vendor_dir}/libwebp/src/utils/random_utils.c
  ${vendor_dir}/libwebp/src/utils/rescaler_utils.c
  ${vendor_dir}/libwebp/src/utils/thread_utils.c
  ${vendor_dir}/libwebp/src/utils/utils.c
)
# A user includes the public headers as `webp/encode.h`, as libwebp installs them. libwebp's own
# sources include every header from the library's root, as `src/dsp/dsp.h`.
target_include_directories(
  vendor_libwebp SYSTEM
  PUBLIC "$<BUILD_INTERFACE:${vendor_dir}/libwebp/src>"
  PRIVATE "${vendor_dir}/libwebp"
)
target_compile_definitions(vendor_libwebp PRIVATE WEBP_USE_THREAD)
target_link_libraries(vendor_libwebp PRIVATE Threads::Threads)
# libwebp calls the math library, which glibc ships apart from the C library. macOS and musl keep
# it in the C library, where a call to `pow` links on its own and there is no `libm` to find.
include(CheckCSourceCompiles)
check_c_source_compiles(
  "#include <math.h>
int main(int argc, char** argv) { (void)argv; return (int)pow((double)argc, 0.5); }"
  FRAM_HAVE_MATH_IN_C_LIBRARY
)
if(NOT FRAM_HAVE_MATH_IN_C_LIBRARY)
  find_library(FRAM_MATH_LIBRARY m REQUIRED)
  target_link_libraries(vendor_libwebp PRIVATE "${FRAM_MATH_LIBRARY}")
endif()

# The SIMD routines exist for x86-64 and AArch64, and any other target uses the C routines.
# libjpeg-turbo's are NASM sources on x86-64 and Neon intrinsics on AArch64. libwebp chooses its
# routines at run time: its SSE2 and Neon routines need nothing beyond what the compiler enables,
# and its SSE4.1 and AVX2 routines need their instruction set enabled for their own sources only,
# while the `WEBP_HAVE_*` definitions tell the dispatchers in every other source that they exist.
# A macOS build names its architecture in `CMAKE_OSX_ARCHITECTURES`. A build for more than one uses
# libjpeg-turbo's C routines, and only the SSE2 or Neon ones of libwebp, which each architecture's
# compiler enables by default.
set(vendor_processor "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  set(vendor_processor "${CMAKE_OSX_ARCHITECTURES}")
endif()
if(vendor_processor STREQUAL "x86_64")
  if(APPLE)
    set(CMAKE_ASM_NASM_OBJECT_FORMAT macho64)
  else()
    set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
  endif()
  include(CheckLanguage)
  check_language(ASM_NASM)
  if(NOT CMAKE_ASM_NASM_COMPILER)
    message(FATAL_ERROR "x86-64 builds need NASM for libjpeg-turbo's SIMD routines; install 'nasm'")
  endif()
  enable_language(ASM_NASM)
  # The NASM sources are a library of their own, because the vendored-code profile and the warning
  # switch below are C compiler options that NASM does not accept.
  add_library(
    vendor_libjpeg_turbo_simd OBJECT
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jsimdcpu.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jfdctflt-sse.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jccolor-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jcgray-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jchuff-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jcphuff-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jcsample-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jdcolor-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jdmerge-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jdsample-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jfdctfst-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jfdctint-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jidctflt-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jidctfst-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jidctint-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jidctred-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jquantf-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jquanti-sse2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jccolor-avx2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jcgray-avx2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jcsample-avx2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jdcolor-avx2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jdmerge-avx2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jdsample-avx2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jfdctint-avx2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jidctint-avx2.asm
    ${vendor_dir}/libjpeg-turbo/simd/x86_64/jquanti-avx2.asm
  )
  # The object format selects the matching NASM macros. `__CET__` adds the indirect-branch markers
  # that a control-flow-protected ELF link expects, and they are no-ops elsewhere. `PIC` keeps every
  # data reference position-independent, as the executable is.
  if(APPLE)
    set(fram_jpeg_nasm_format MACHO)
  else()
    set(fram_jpeg_nasm_format ELF __CET__)
  endif()
  target_compile_definitions(
    vendor_libjpeg_turbo_simd PRIVATE ${fram_jpeg_nasm_format} __x86_64__ PIC
  )
  target_include_directories(
    vendor_libjpeg_turbo_simd PRIVATE "${vendor_dir}/libjpeg-turbo/simd/nasm/"
                                           "${vendor_dir}/libjpeg-turbo/simd/x86_64/"
  )
  target_sources(
    vendor_libjpeg_turbo PRIVATE ${vendor_dir}/libjpeg-turbo/simd/jsimd.c
                                      $<TARGET_OBJECTS:vendor_libjpeg_turbo_simd>
  )
  target_compile_definitions(vendor_libjpeg_turbo PRIVATE JPEG_SIMD_ARCHITECTURE=X86_64)
  set_property(
    SOURCE ${vendor_dir}/libwebp/src/dsp/alpha_processing_sse41.c
           ${vendor_dir}/libwebp/src/dsp/dec_sse41.c
           ${vendor_dir}/libwebp/src/dsp/enc_sse41.c
           ${vendor_dir}/libwebp/src/dsp/lossless_enc_sse41.c
           ${vendor_dir}/libwebp/src/dsp/lossless_sse41.c
           ${vendor_dir}/libwebp/src/dsp/upsampling_sse41.c
           ${vendor_dir}/libwebp/src/dsp/yuv_sse41.c
    APPEND
    PROPERTY COMPILE_OPTIONS -msse4.1
  )
  set_property(
    SOURCE ${vendor_dir}/libwebp/src/dsp/lossless_avx2.c ${vendor_dir}/libwebp/src/dsp/lossless_enc_avx2.c
    APPEND
    PROPERTY COMPILE_OPTIONS -mavx2
  )
  target_compile_definitions(vendor_libwebp PRIVATE WEBP_HAVE_SSE41 WEBP_HAVE_AVX2)
elseif(vendor_processor MATCHES "^(aarch64|arm64)$")
  target_sources(
    vendor_libjpeg_turbo
    PRIVATE ${vendor_dir}/libjpeg-turbo/simd/jsimd.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/aarch64/jsimdcpu.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/aarch64/jchuff-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jccolor-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jcgray-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jcphuff-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jcsample-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jdcolor-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jdmerge-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jdsample-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jfdctfst-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jfdctint-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jidctfst-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jidctint-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jidctred-neon.c
            ${vendor_dir}/libjpeg-turbo/simd/arm/jquanti-neon.c
  )
  target_compile_definitions(vendor_libjpeg_turbo PRIVATE JPEG_SIMD_ARCHITECTURE=ARM64)
endif()

add_library(vendor_mustache STATIC ${vendor_dir}/mustache4c/mustache.c)
target_include_directories(
  vendor_mustache SYSTEM PUBLIC "$<BUILD_INTERFACE:${vendor_dir}/mustache4c>"
)

add_library(vendor_stb STATIC src/formats/stb.c)
target_include_directories(
  vendor_stb SYSTEM PUBLIC "$<BUILD_INTERFACE:${vendor_dir}/stb>"
)
# `src/formats/stb.c` compiles the resizer, and routes its allocator hooks to `image_alloc` in
# `src/formats/image.c`, so it includes that module's header, which includes `shared/str.h`.
target_include_directories(vendor_stb PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(vendor_stb PRIVATE shared)

add_library(vendor_tomlc17 STATIC ${vendor_dir}/tomlc17/tomlc17.c)
target_include_directories(
  vendor_tomlc17 SYSTEM PUBLIC "$<BUILD_INTERFACE:${vendor_dir}/tomlc17>"
)

foreach(target IN ITEMS vendor_libjpeg_turbo vendor_libwebp vendor_mustache
                        vendor_stb vendor_tomlc17
)
  target_compile_options(${target} PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
  target_link_libraries(${target} PRIVATE apps_build_vendor)
endforeach()
