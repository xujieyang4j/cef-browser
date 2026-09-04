function(trail_copy_cef_runtime target)
  if(WIN32 OR (UNIX AND NOT APPLE))
    set(runtime_dir "$<TARGET_FILE_DIR:${target}>")
    COPY_FILES("${target}" "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${runtime_dir}")
    COPY_FILES("${target}" "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${runtime_dir}")
    return()
  endif()

  if(APPLE)
    set(app_bundle_dir "$<TARGET_BUNDLE_DIR:${target}>")
    set(app_frameworks_dir "${app_bundle_dir}/Contents/Frameworks")

    # The framework in a CEF distribution contains the contents for
    # Versions/A. The official CEF macro also creates the symlinks required by
    # a valid macOS framework bundle.
    COPY_MAC_FRAMEWORK("${target}" "${CEF_BINARY_DIR}" "${app_bundle_dir}")

    foreach(helper_suffix_list IN LISTS CEF_HELPER_APP_SUFFIXES)
      string(REPLACE ":" ";" helper_suffix_list "${helper_suffix_list}")
      list(GET helper_suffix_list 1 helper_target_suffix)
      set(helper_target "trail-browser-helper${helper_target_suffix}")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "$<TARGET_BUNDLE_DIR:${helper_target}>"
          "${app_frameworks_dir}/$<TARGET_BUNDLE_DIR_NAME:${helper_target}>"
        VERBATIM
      )
    endforeach()
  endif()
endfunction()

