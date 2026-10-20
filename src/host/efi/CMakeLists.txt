# SPDX-License-Identifier: GPL-2.0-only

add_library(i1n1_host_efi STATIC
  plat_x64_stub.c)
add_library(i1n1::host_efi ALIAS i1n1_host_efi)

target_include_directories(i1n1_host_efi PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${I1N1_CORE_SOURCE_DIR}/include>)
target_compile_features(i1n1_host_efi PUBLIC c_std_11)
i1n1_apply_project_defaults(i1n1_host_efi)

if(NOT I1N1_WITH_ENGINE)
  message(STATUS "efi: I1N1_WITH_ENGINE=OFF, skipping i1n1_efi_runner and i1n1.efi")
  return()
endif()

# ---- host build of the runner (tests) ---------------------------------------
add_library(i1n1_efi_runner STATIC runner.c kernel_boot.c proxy_loop.c)
add_library(i1n1::efi_runner ALIAS i1n1_efi_runner)
target_include_directories(i1n1_efi_runner PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${I1N1_CORE_SOURCE_DIR}/include>)
target_link_libraries(i1n1_efi_runner PUBLIC
  i1n1_dbt i1n1_guest i1n1_apple_init i1n1_mmio i1n1_irq i1n1_ept i1n1_memory
  i1n1_platform_core i1n1_boot_glue)
target_compile_features(i1n1_efi_runner PUBLIC c_std_11)
i1n1_apply_project_defaults(i1n1_efi_runner)

# ---- freestanding PE/COFF build ---------------------------------------------
find_program(I1N1_EFI_CLANG NAMES clang)
find_program(I1N1_EFI_LLD_LINK NAMES lld-link)
if(NOT I1N1_EFI_CLANG OR NOT I1N1_EFI_LLD_LINK)
  message(STATUS "efi: clang/lld-link not found, i1n1.efi not built")
  return()
endif()

set(_efi_probe_src "${CMAKE_CURRENT_BINARY_DIR}/efi_probe.c")
file(WRITE "${_efi_probe_src}" "int i1n1_efi_probe(void) { return 0; }\n")
execute_process(
  COMMAND "${I1N1_EFI_CLANG}" --target=x86_64-unknown-windows -ffreestanding
          -c "${_efi_probe_src}" -o "${CMAKE_CURRENT_BINARY_DIR}/efi_probe.o"
  RESULT_VARIABLE _efi_probe_rc OUTPUT_QUIET ERROR_QUIET)
execute_process(
  COMMAND "${I1N1_EFI_CLANG}" -print-resource-dir
  OUTPUT_VARIABLE _efi_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _efi_res_rc ERROR_QUIET)
if(NOT _efi_probe_rc EQUAL 0 OR NOT _efi_res_rc EQUAL 0)
  message(STATUS "efi: ${I1N1_EFI_CLANG} cannot target x86_64-unknown-windows, i1n1.efi not built")
  return()
endif()

set(_efi_root "${PROJECT_SOURCE_DIR}")
set(_efi_core "${I1N1_CORE_SOURCE_DIR}")
set(_efi_engine "${I1N1_ENGINE_SOURCE_DIR}")
set(_efi_platform "${I1N1_PLATFORM_SOURCE_DIR}")
set(_efi_obj_dir "${CMAKE_CURRENT_BINARY_DIR}/efi-obj")
file(MAKE_DIRECTORY "${_efi_obj_dir}")

# Own code is warning-clean (-Werror); imported engine code keeps its upstream
# warning policy (no -Werror) because it is built for an LLP64 target here.
set(_efi_common_flags
  --target=x86_64-unknown-windows -std=c11 -O2 -g0
  -ffreestanding -fno-stack-protector -mno-red-zone -fshort-wchar
  -fno-stack-check -fno-asynchronous-unwind-tables -fno-pic -fno-pie
  -nostdinc -isystem "${_efi_resource_dir}/include"
  -I "${_efi_root}/src/host/efi/freestanding"
  -I "${_efi_root}/src/host/efi"
  -I "${_efi_core}/include"
  -I "${_efi_engine}/include"
  -I "${_efi_platform}/src/dbt" -I "${_efi_platform}/src/guest"
  -I "${_efi_platform}/src/platform/core" -I "${_efi_platform}/src/platform/mmio"
  -I "${_efi_platform}/src/platform/irq" -I "${_efi_platform}/src/platform/avp" -I "${_efi_platform}/src/platform/apple_init"
  -I "${_efi_platform}/src/host/vmx" -I "${_efi_platform}/src/host/memory"
  -Wall -Wextra -Wpedantic)

set(_efi_own_sources
  src/host/efi/efi_main.c
  src/host/efi/efi_libc.c
  src/host/efi/runner.c
  src/host/efi/kernel_boot.c
  src/host/efi/proxy_loop.c
  src/platform/core/plat_phase.c
  src/platform/core/plat_mmio_guard.c
  src/platform/core/debug_link.c
  src/host/memory/owned_ram.c
  src/host/vmx/ept.c
  src/platform/mmio/mmio_router.c
  src/platform/irq/gicv3_lite.c
  src/platform/avp/avp.c
  src/guest/sha256.c
  src/guest/stop.c
  src/guest/image_load.c
  src/guest/device_tree.c
  src/guest/boot_args.c
  src/guest/boot_policy.c
  src/guest/boot_png.c
  src/guest/stage1.c
  src/guest/pe_state.c
  src/guest/hvc.c
  src/guest/checkpoint.c
  src/guest/boot_hvc.c
  src/guest/engine_hvc.c
  src/guest/vm_sysreg.c
  src/guest/pbus.c
  src/guest/pl011.c
  src/guest/machine.c
  src/guest/machine_dbt.c
  src/guest/adt_finalize.c
  src/guest/boot_dt_policy.c
  src/guest/nvram_image.c
  src/platform/apple_init/sha256.c
  src/platform/apple_init/key_state.c
  src/platform/apple_init/trace.c
  src/platform/apple_init/apple_init_provider.c
  src/dbt/dbt_cache.c
  src/dbt/dbt_chain.c
  src/dbt/dbt_invalidate.c
  src/dbt/dbt_dispatch.c
  src/dbt/dbt_x64.c
  src/dbt/dbt_native.c)

set(_efi_engine_sources
  engine/runtime/isa/a64.c
  engine/runtime/isa/a64_adc_carry.c
  engine/runtime/isa/a64_atomic.c
  engine/runtime/isa/a64_divbit.c
  engine/runtime/isa/a64_fmov.c
  engine/runtime/isa/a64_fp_control.c
  engine/runtime/isa/a64_ldra.c
  engine/runtime/isa/a64_multiply.c
  engine/runtime/isa/a64_simd_integer.c
  engine/runtime/isa/a64_sysreg.c
  engine/runtime/isa/a64_simd_ext.c
  engine/runtime/isa/a64_fp_soft.c
  engine/runtime/crypto/pac_keys.c
  engine/runtime/crypto/pac_pointer.c
  engine/runtime/crypto/pac_qarma5.c
  engine/runtime/mmu/mmu.c
  engine/runtime/mmu/cpu_adapter.c
  engine/runtime/memory/ordering.c)

set(_efi_objects)
foreach(_src IN LISTS _efi_own_sources _efi_engine_sources)
  string(REPLACE "/" "_" _obj_name "${_src}")
  if(_src MATCHES "^engine/")
    string(REGEX REPLACE "^engine/" "" _src_rel "${_src}")
    set(_src_abs "${_efi_engine}/${_src_rel}")
  elseif(_src MATCHES "^src/host/efi/")
    set(_src_abs "${_efi_root}/${_src}")
  else()
    set(_src_abs "${_efi_platform}/${_src}")
  endif()
  set(_obj "${_efi_obj_dir}/${_obj_name}.obj")
  set(_extra)
  if(_src MATCHES "^src/host/efi/")
    set(_extra -Werror)
  endif()
  add_custom_command(
    OUTPUT "${_obj}"
    COMMAND "${I1N1_EFI_CLANG}" ${_efi_common_flags} ${_extra}
            -MD -MF "${_obj}.d" -c "${_src_abs}" -o "${_obj}"
    DEPENDS "${_src_abs}"
    DEPFILE "${_obj}.d"
    COMMENT "efi cc ${_src}"
    VERBATIM)
  list(APPEND _efi_objects "${_obj}")
endforeach()

set(I1N1_EFI_OUTPUT "${CMAKE_BINARY_DIR}/i1n1.efi")
add_custom_command(
  OUTPUT "${I1N1_EFI_OUTPUT}"
  COMMAND "${I1N1_EFI_LLD_LINK}" /nologo /subsystem:efi_application
          /entry:efi_main /nodefaultlib /opt:ref /machine:x64
          "/out:${I1N1_EFI_OUTPUT}" ${_efi_objects}
  DEPENDS ${_efi_objects}
  COMMENT "efi link i1n1.efi"
  VERBATIM)
add_custom_target(i1n1_efi ALL DEPENDS "${I1N1_EFI_OUTPUT}")
message(STATUS "efi: i1n1.efi -> ${I1N1_EFI_OUTPUT}")
