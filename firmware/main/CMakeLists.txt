idf_component_register(
    SRCS "app_main.cpp" "cam.cpp" "yolo_detector.cpp" "pad_classifier.cpp" "focus_fsm.cpp" "led.cpp" "settings.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp32-camera esp-dl esp_timer esp_driver_uart esp_driver_gpio esp_partition mbedtls nvs_flash led_strip)

# Pack every .espdl in ../models into one PDL3 file and flash it to the "models"
# partition with `idf.py flash` (`idf.py app-flash` leaves it alone).
# Re-run `idf.py reconfigure` after adding or removing a model file.
idf_build_get_property(component_targets __COMPONENT_TARGETS)
if("___idf_espressif__esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
else()
    idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
endif()
idf_build_get_property(build_dir BUILD_DIR)
idf_build_get_property(python PYTHON)

file(GLOB model_files "${CMAKE_CURRENT_LIST_DIR}/../models/*.espdl")
list(SORT model_files)
set(packed_models "${build_dir}/models_packed.espdl")
add_custom_command(
    OUTPUT ${packed_models}
    COMMENT "Packing models: ${model_files}"
    COMMAND ${python} ${espdl_dir}/fbs_loader/pack_espdl_models.py --model_path ${model_files} --out_file ${packed_models}
    DEPENDS ${model_files}
    VERBATIM)
add_custom_target(pack_models ALL DEPENDS ${packed_models})
add_dependencies(flash pack_models)
esptool_py_flash_to_partition(flash "models" ${packed_models})
