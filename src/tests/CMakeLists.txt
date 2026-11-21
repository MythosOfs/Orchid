# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    expect.cpp
    frontend_common/play_time.cpp
    common/bit_field.cpp
    common/bit_util.cpp
    common/cityhash.cpp
    common/describe_exception.cpp
    common/game_log.cpp
    common/container_hash.cpp
    common/display_hdr.cpp
    common/event.cpp
    common/fibers.cpp
    common/flat_allocator.cpp
    common/game_presets.cpp
    common/hex_util.cpp
    common/hitch_diagnostic.cpp
    common/host_memory.cpp
    common/host_memory_fallback.cpp
    common/host_resources.cpp
    common/multi_level_page_table.cpp
    common/page_table.cpp
    common/paged_scalar.cpp
    common/param_package.cpp
    common/queue_lifetime.cpp
    common/range_map.cpp
    common/range_set_move.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/session_counter.cpp
    common/slot_vector.cpp
    common/sparse_large_vector.cpp
    common/unique_function.cpp
    common/user_directories.cpp
    common/zstd_compression.cpp
    audio_core/adpcm_decode.cpp
    core/address_arbiter_read.cpp
    core/address_space.cpp
    core/builtin_font_glyphs.cpp
    core/cheat_parser.cpp
    core/core_timing.cpp
    core/filesystem_buffer_bounds.cpp
    core/filesystem_rename.cpp
    core/hid_emulated_controller_config.cpp
    core/hid_motion_input.cpp
    core/hid_npad_assignment.cpp
    core/kip_blz.cpp
    core/kip_sections.cpp
    core/kip_truncated.cpp
    core/page_heap.cpp
    core/profile_orphans.cpp
    core/read_traps.cpp
    core/gpu_dirty_memory_manager.cpp
    core/nx2_image_layout.cpp
    core/nx2_readback_cache.cpp
    core/nx2_core_readback_capacity.cpp
    core/game_settings.cpp
    core/frame_step_fps.cpp
    core/wonder_fps_patch.cpp
    core/yoshi_4k_patch.cpp
    core/nx2_sysmix_pcm.cpp
    core/hid_joycon2_style.cpp
    core/hid_auto_buttons.cpp
    core/hid_style_set_reconnect.cpp
    core/hid_vibration_value.cpp
    core/nso.cpp
    core/nca_header_key.cpp
    core/nrr.cpp
    core/ncz_decompressor.cpp
    core/key_manager_keys_file.cpp
    core/ldn.cpp
    core/internal_network/socket_proxy.cpp
    core/nx_archive.cpp
    core/nup_writer.cpp
    core/loose_directory.cpp
    core/display_title.cpp
    core/priority_queue.cpp
    core/program_metadata.cpp
    core/vfs_vector.cpp
    core/ips_switch.cpp
    core/vfs_real.cpp
    core/nvnflinger_fence.cpp
    core/partition_filesystem.cpp
    core/eticket_title_key.cpp
    core/keyblob_cmac.cpp
    core/nax_hmac.cpp
    core/romfs.cpp
    core/time_zone_fallback.cpp
    core/service_auto_stub.cpp
    core/internal_network/network.cpp
    video_core/shader_object_pool.cpp
    video_core/aftermath_dump.cpp
    video_core/stall_diagnostics.cpp
    video_core/storage_buffer_pointer_tracking.cpp
    video_core/storage_buffer_control_flow.cpp
    video_core/memory_tracker.cpp
    video_core/buffer_cache_download.cpp
    video_core/report_time.cpp
    video_core/unmapped_read_diagnostic.cpp
    video_core/memory_budget.cpp
    video_core/astc_recompression_override.cpp
    video_core/nx2_video_configuration.cpp
    video_core/nx2_video_gpu_fallback.cpp
    video_core/nx2_decoder_image.cpp
    video_core/hdr_output.cpp
    video_core/nx2_present.cpp
    video_core/nx2_gpu_mappings.cpp
    video_core/nx2_sparse_mappings.cpp
    video_core/nx2_sparse_region.cpp
    video_core/nx2_sparse_tiles.cpp
    video_core/nx2_sparse_images.cpp
    video_core/nx2_sparse_image_layout.cpp
    video_core/nx2_sparse_image_write.cpp
    video_core/nx2_command_sync.cpp
    video_core/nx2_command_ranges.cpp
    video_core/nx2_queued_maps.cpp
    video_core/physical_alias.cpp
    video_core/h264_packet.cpp
    video_core/framebuffer_clear_layers.cpp
    video_core/vertex_repack.cpp
    video_core/vulkan_device_version.cpp
    video_core/vulkan_enumerate.cpp
    video_core/gpu_split_ring.cpp
    video_core/query_copy.cpp
    video_core/multi_range_buffer.cpp
    video_core/shader_reuse_environment.cpp
    video_core/pipeline_cache_file.cpp
    video_core/static_pipeline_cache.cpp
    video_core/ampere_lowering.cpp
    video_core/ampere_matrix.cpp
    video_core/ampere_returns.cpp
    video_core/ampere_f2i.cpp
    video_core/ampere_i2f64.cpp
    video_core/ampere_uniform_fset.cpp
    video_core/compute_storage_buffers.cpp
    video_core/storage_binding_cache.cpp
    video_core/approximate_gpu_writes.cpp
    video_core/pending_download.cpp
    video_core/storage_buffer_descriptor.cpp
    video_core/compute_binding_diagnostic.cpp
    video_core/cooperative_matrix.cpp
    video_core/spirv_storage_images.cpp
    video_core/texture_unswizzle.cpp
    video_core/scaled_image_fits.cpp
    video_core/texture_row_upload.cpp
    video_core/texture_linear_upload.cpp
    video_core/texture_depth_float.cpp
    video_core/texture_query_type.cpp
    video_core/texture_descriptor_bounds.cpp
    video_core/shader_snapshot.cpp
    video_core/texture_r2ur_handle.cpp
    video_core/texture_handles.cpp
    video_core/ampere_kirby.cpp
    video_core/ampere_geometry.cpp
    video_core/ampere_geometry_lowering.cpp
    video_core/geometry_layer_viewport_relative.cpp
    video_core/demote_nested_returns.cpp
    video_core/ampere_geometry_exit.cpp
    video_core/ampere_uniform_lea.cpp
    video_core/ampere_compute_forms.cpp
    video_core/ampere_integer_multiply.cpp
    video_core/ampere_constant_load_offsets.cpp
    video_core/ampere_program_override.cpp
    video_core/x8d24_conversion.cpp
    video_core/ampere_bindless_tex.cpp
    video_core/ampere_sured.cpp
    video_core/ampere_yoshi_surface.cpp
    video_core/ampere_yoshi_shift.cpp
    video_core/ampere_yoshi_lea_carry.cpp
    video_core/ampere_yoshi_uniform_carry.cpp
    video_core/ampere_yoshi_uniform_shift.cpp
    video_core/ampere_yoshi_uniform_gather.cpp
    video_core/ampere_yoshi_cbuf_carry.cpp
    video_core/maxwell_shr.cpp
    video_core/maxwell_fmz.cpp
    video_core/ampere_release_guards.cpp
    video_core/ampere_release_f2i.cpp
    video_core/ampere_uniform_immediate_shift.cpp
    video_core/ampere_bindless_surfaces.cpp
    video_core/ampere_kirby_fsel.cpp
    video_core/ampere_register_pressure.cpp
    video_core/syncpoint_manager.cpp
    video_core/native_macro_fifo.cpp
    video_core/turing_macro_ram.cpp
    video_core/uniform_stream_reuse.cpp
    video_core/sparse_uniform_upload.cpp
    video_core/shader_invalidation.cpp
    video_core/descriptor_table.cpp
    video_core/invalidation_accumulator.cpp
    video_core/maxwell_dma_const_a.cpp
    video_core/spirv_atomic_retry.cpp
    video_core/spirv_min_max_denorms.cpp
    video_core/spirv_screen_linear.cpp
    video_core/usage_tracker.cpp
    video_core/expect_shader_ir.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

if (WIN32)
    target_sources(tests PRIVATE common/host_memory_commit.cpp)
endif()

if (ENABLE_LIBUSB AND LINUX)
    target_sources(tests PRIVATE input_common/gc_adapter_libusb.cpp)
    target_link_libraries(tests PRIVATE libusb::usb)
    target_link_options(tests PRIVATE -Wl,--wrap=libusb_init,--wrap=libusb_exit,--wrap=libusb_open_device_with_vid_pid)
endif()

if (LINUX)
    target_sources(tests PRIVATE input_common/silent_controller.cpp
                                 frontend_common/config_persistence.cpp)
    target_link_options(tests PRIVATE -Wl,--wrap=fflush,--wrap=fsync)
endif()

if (TARGET OpenSSL::SSL)
    target_sources(tests PRIVATE core/ssl_peek.cpp)
    target_link_libraries(tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

target_link_libraries(tests PRIVATE common core frontend_common input_common video_core tz)

if (ENABLE_QT)
    target_sources(tests PRIVATE qt_common/qt_config_ban_list.cpp)
    target_link_libraries(tests PRIVATE qt_common input_common Qt6::Core SimpleIni::SimpleIni)
    # QtConfig rewrites global Settings/UISettings, so it runs in its own process.
    add_test(NAME qt_config_ban_list COMMAND tests "[qt_config]")
endif()
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

# Reference texts of the expect tests (tests/expect.h); ORCHID_EXPECT_UPDATE=1 rewrites them.
target_compile_definitions(tests PRIVATE ORCHID_EXPECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/expect")

target_compile_definitions(tests PRIVATE
    ORCHID_NX_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/nx_fixtures")

add_test(NAME tests COMMAND tests)
add_test(NAME service_auto_stub COMMAND tests "[service]")
foreach(keys_test eticket keyblob nax keys_file)
    add_test(NAME crypto_${keys_test} COMMAND tests "[${keys_test}]")
endforeach()

# Separate processes exercise the cached opt-in and destruction order without a GPU or profile.
add_executable(stall_diagnostics_session
    video_core/stall_diagnostics_session.cpp
    ../video_core/stall_diagnostics.cpp)
target_link_libraries(stall_diagnostics_session PRIVATE common ${PLATFORM_LIBRARIES} Threads::Threads)
add_test(NAME stall_diagnostics_enabled COMMAND ${CMAKE_COMMAND} -E env
    EDEN_NX2_STALL_DIAGNOSTICS=1 $<TARGET_FILE:stall_diagnostics_session> enabled)

# Standalone: the frame telemetry and handle trace streams read their switches once per process.
add_executable(debug_stream_contract debug_stream_contract.cpp)
target_link_libraries(debug_stream_contract PRIVATE common ${PLATFORM_LIBRARIES} Threads::Threads)
if (NOT MSVC)
    target_compile_options(debug_stream_contract PRIVATE -Wno-sign-compare)
endif()
set(debug_stream_contract_dir "${CMAKE_CURRENT_BINARY_DIR}/debug_stream_contract")
add_test(NAME debug_stream_contract_clean
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${debug_stream_contract_dir}")
set_tests_properties(debug_stream_contract_clean PROPERTIES FIXTURES_SETUP debug_stream_contract_dir)
add_test(NAME debug_stream_contract_disabled
    COMMAND debug_stream_contract "${debug_stream_contract_dir}" disabled)
add_test(NAME debug_stream_contract_enabled
    COMMAND debug_stream_contract "${debug_stream_contract_dir}")
set_tests_properties(debug_stream_contract_disabled debug_stream_contract_enabled
    PROPERTIES FIXTURES_REQUIRED debug_stream_contract_dir)
# The disabled run checks that nothing is created, so it runs before the enabled one.
set_tests_properties(debug_stream_contract_enabled PROPERTIES DEPENDS debug_stream_contract_disabled)

# Measures the CPU time of a waiting thread, so it runs alone in its own process.
if (WIN32 OR LINUX) # gpu_split_ring.cpp needs a per-thread CPU clock
    add_test(NAME gpu_split_ring COMMAND tests "[gpu_split]")
endif()

# needed for vma
if (NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()

# A test source in no target is never built or run (maxwell_fmz.cpp and debug_stream_contract.cpp
# sat unbuilt this way). Every .cpp here must belong to one of the targets above.
file(GLOB_RECURSE test_sources RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS "*.cpp")
set(listed_test_sources common/host_memory_commit.cpp # added above on Windows only
    input_common/gc_adapter_libusb.cpp # added above with libusb on Linux only
    input_common/silent_controller.cpp # added above on Linux only
    qt_common/qt_config_ban_list.cpp) # added above with the Qt frontend only
foreach(test_target IN ITEMS tests stall_diagnostics_session debug_stream_contract)
    get_target_property(target_sources ${test_target} SOURCES)
    list(APPEND listed_test_sources ${target_sources})
endforeach()
foreach(test_source IN LISTS test_sources)
    if (NOT test_source IN_LIST listed_test_sources)
        message(FATAL_ERROR "src/tests/${test_source} belongs to no test target; list it in src/tests/CMakeLists.txt")
    endif()
endforeach()
