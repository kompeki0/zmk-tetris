# CMakeLists.txt (module root)
if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources(
    app PRIVATE src/behavior_tetris.c
  )
  zephyr_include_directories(include)
endif()
