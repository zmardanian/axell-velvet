cmake_minimum_required(VERSION 3.22)
project(AxellVelvet VERSION 3.0.2)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG        8.0.15
  GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

# Axell core (shared DSP + look): next to this project, or vendored into external/axell-core for releases
set(AXELL_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/external/axell-core" CACHE PATH "Axell core")
if(NOT EXISTS "${AXELL_CORE_DIR}/CMakeLists.txt")
  set(AXELL_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../axell-core" CACHE PATH "Axell core" FORCE)
endif()
add_subdirectory(${AXELL_CORE_DIR} axell-core)

juce_add_plugin(AxellVelvet
  COMPANY_NAME              "Axell Yobag"
  COMPANY_EMAIL             "axellpetre@protonmail.com"
  BUNDLE_ID                 com.axellyobag.axellvelvet
  PLUGIN_MANUFACTURER_CODE  Axyb
  PLUGIN_CODE               Vlv1
  FORMATS                   VST3
  PRODUCT_NAME              "Axell Velvet"
  IS_SYNTH                  FALSE
  NEEDS_MIDI_INPUT          FALSE
  NEEDS_MIDI_OUTPUT         FALSE
  IS_MIDI_EFFECT            FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD   FALSE
  VST3_CATEGORIES           Fx Dynamics Distortion)

set(VELVET_SOURCES Source/PluginProcessor.cpp Source/PluginEditor.cpp)
set(VELVET_DEFS JUCE_WEB_BROWSER=0 AXELL_THEME_HUE=330 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_sources(AxellVelvet PRIVATE ${VELVET_SOURCES})
target_compile_definitions(AxellVelvet PUBLIC ${VELVET_DEFS})
axell_core_add_gui(AxellVelvet)
target_link_libraries(AxellVelvet PRIVATE juce::juce_audio_utils
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

option(VELVET_SANITIZE "AddressSanitizer + UBSan for the test programs" OFF)
function(velvet_sanitize target)
  if(VELVET_SANITIZE)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

# DSP suite (JUCE-free: the engine headers exactly as they ship)
enable_testing()
add_executable(VelvetDspTests tests/DspTests.cpp)
target_include_directories(VelvetDspTests PRIVATE Source)
target_link_libraries(VelvetDspTests PRIVATE axell_core_dsp)
target_compile_options(VelvetDspTests PRIVATE -Wall -Wextra -Wshadow -Wno-mismatched-new-delete $<$<CONFIG:Release>:-O3>)
velvet_sanitize(VelvetDspTests)
add_test(NAME dsp_suite COMMAND VelvetDspTests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# plugin suite + GUI snapshots (JUCE)
foreach(tool VelvetPluginTests VelvetGuiSnapshot)
  juce_add_console_app(${tool} PRODUCT_NAME "${tool}")
  target_compile_definitions(${tool} PRIVATE ${VELVET_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1 JucePlugin_Name="Axell Velvet")
  target_link_libraries(${tool} PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)
  axell_core_add_gui(${tool})
  velvet_sanitize(${tool})
endforeach()
target_sources(VelvetPluginTests PRIVATE tests/PluginTests.cpp ${VELVET_SOURCES})
target_sources(VelvetGuiSnapshot PRIVATE tests/GuiSnapshot.cpp ${VELVET_SOURCES})
add_test(NAME plugin_suite COMMAND VelvetPluginTests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# independent black-box audit host (from the core)
juce_add_console_app(VelvetAuditHost PRODUCT_NAME "VelvetAuditHost")
target_sources(VelvetAuditHost PRIVATE ${AXELL_CORE_DIR}/tools/AuditHost.cpp)
target_compile_definitions(VelvetAuditHost PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(VelvetAuditHost PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_recommended_config_flags)

# research / benchmark tools (JUCE-free; the shipping engine headers)
foreach(tool VelvetFeatures VelvetDeEss VelvetRender HeatProbe HeatCalibrate)
  add_executable(${tool} tools/${tool}.cpp)
  target_include_directories(${tool} PRIVATE Source)
  target_link_libraries(${tool} PRIVATE axell_core_dsp)
  target_compile_options(${tool} PRIVATE -O3)
endforeach()
