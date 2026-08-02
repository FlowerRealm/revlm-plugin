cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(revlm_plugin LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(TARGET revlm_core)
  # Revlm's own test build embeds this source tree and links the modules to
  # the just-built host library. Published plugins still use the installed SDK
  # package below, so this does not loosen the release ABI contract.
  add_library(RevlmPluginSDK::core ALIAS revlm_core)
else()
  find_package(RevlmPluginSDK CONFIG REQUIRED)
endif()

function(revlm_add_plugin target source)
  add_library(${target} MODULE ${source})
  target_link_libraries(${target} PRIVATE RevlmPluginSDK::core)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/plugins/common")
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-unknown-pragmas)
  set_target_properties(${target} PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "${target}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/${target}")
endfunction()

revlm_add_plugin(OpenAI plugins/OpenAI/src/plugin.cpp)
revlm_add_plugin(Anthropic plugins/Anthropic/src/plugin.cpp)
