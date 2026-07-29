cmake_minimum_required(VERSION 3.20)
project(Stobe VERSION 0.9.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(STOBE_DIAG_PROFILE "normal" CACHE STRING "Diagnostic crash profile (normal|ui_only|no_hook|hook_no_orig|no_context|no_hook_no_context)")
set_property(CACHE STOBE_DIAG_PROFILE PROPERTY STRINGS normal ui_only no_hook hook_no_orig no_context no_hook_no_context)

if(NOT KENSHI_LIB_INCLUDE_DIR)
  message(FATAL_ERROR "KENSHI_LIB_INCLUDE_DIR is required.")
endif()
if(NOT KENSHI_LIB_LIBRARY)
  message(FATAL_ERROR "KENSHI_LIB_LIBRARY is required.")
endif()
if(NOT BOOST_INCLUDE_DIR)
  message(FATAL_ERROR "BOOST_INCLUDE_DIR is required.")
endif()

get_filename_component(_kenshi_lib_dir "${KENSHI_LIB_LIBRARY}" DIRECTORY)
get_filename_component(_kenshi_lib_root "${_kenshi_lib_dir}" DIRECTORY)

set(_sdk_mygui_lib "${_kenshi_lib_dir}/Libraries/mygui/MyGUIEngine_x64.lib")
set(_sdk_ogre_lib "${_kenshi_lib_dir}/Libraries/ogre/OgreMain_x64.lib")

set(_candidate_lib_dirs
  "${_kenshi_lib_dir}"
  "${_kenshi_lib_dir}/Libraries"
  "${_kenshi_lib_dir}/Libraries/mygui"
  "${_kenshi_lib_dir}/Libraries/ogre"
  "${_kenshi_lib_root}/KenshiLib Source/Libraries"
  "${_kenshi_lib_root}/KenshiLib Source/Libraries/mygui"
  "${_kenshi_lib_root}/KenshiLib Source/Libraries/ogre"
  "${_kenshi_lib_root}/KenshiLib_Examples_deps/KenshiLib/Libraries"
)

if(EXISTS "${_sdk_mygui_lib}")
  set(MYGUI_ENGINE_LIBRARY "${_sdk_mygui_lib}" CACHE FILEPATH "MyGUI library path" FORCE)
else()
  unset(MYGUI_ENGINE_LIBRARY CACHE)
  find_library(MYGUI_ENGINE_LIBRARY
    NAMES MyGUIEngine_x64 MyGUIEngine_x64.lib
    PATHS ${_candidate_lib_dirs}
    NO_DEFAULT_PATH
  )
endif()

if(EXISTS "${_sdk_ogre_lib}")
  set(OGRE_MAIN_LIBRARY "${_sdk_ogre_lib}" CACHE FILEPATH "Ogre library path" FORCE)
else()
  unset(OGRE_MAIN_LIBRARY CACHE)
  find_library(OGRE_MAIN_LIBRARY
    NAMES OgreMain_x64 OgreMain_x64.lib
    PATHS ${_candidate_lib_dirs}
    NO_DEFAULT_PATH
  )
endif()

if(NOT MYGUI_ENGINE_LIBRARY)
  message(FATAL_ERROR "MyGUIEngine_x64.lib not found in candidate paths.")
endif()
if(NOT OGRE_MAIN_LIBRARY)
  message(FATAL_ERROR "OgreMain_x64.lib not found in candidate paths.")
endif()

add_library(Stobe SHARED
  src/AudioPlayback.cpp
  src/AutonomyController.cpp
  src/AutonomyExecutor.cpp
  src/AutonomyMonitor.cpp
  src/AutonomyProtocol.cpp
  src/AutonomySafetyProbe.cpp
  src/AutonomySafetyProbePolicy.cpp
  src/ChatUI.cpp
  src/ChatUIGlobals.cpp
  src/ChatBox.cpp
  src/Comm.cpp
  src/Context.cpp
  src/Functions.cpp
  src/Globals.cpp
  src/KenshiAiCompat.cpp
  src/JournalWindow.cpp
  src/StartingWindow.cpp
  src/AiNpcInfoWindow.cpp
  src/KenshiRvaCompat.cpp
  src/StobeIdentityRename.cpp
  src/StobeText.cpp
  src/StobeTiming.cpp
  src/main.cpp
  src/SettingsWindow.cpp
  src/Utils.cpp
  src/WelcomeWindow.cpp
)

target_include_directories(Stobe PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${KENSHI_LIB_INCLUDE_DIR}"
  "${KENSHI_LIB_INCLUDE_DIR}/ogre"
  "${KENSHI_LIB_INCLUDE_DIR}/mygui"
  "${BOOST_INCLUDE_DIR}"
)

if(EXISTS "${BOOST_INCLUDE_DIR}/stage/lib")
  target_link_directories(Stobe PRIVATE "${BOOST_INCLUDE_DIR}/stage/lib")
endif()

target_link_libraries(Stobe PRIVATE
  "${KENSHI_LIB_LIBRARY}"
  "${MYGUI_ENGINE_LIBRARY}"
  "${OGRE_MAIN_LIBRARY}"
  winhttp.lib
  winmm.lib
  ws2_32.lib
)

if(MSVC)
  target_compile_definitions(Stobe PRIVATE
    UNICODE
    _UNICODE
    BOOST_ALL_NO_LIB
    BOOST_ERROR_CODE_HEADER_ONLY
    BOOST_SYSTEM_NO_DEPRECATED
  )
  # /EHa: async exception handling (required for __try/__except in hooks)
  # No runtime library override needed ??? v100 toolset enforces correct CRT automatically
  target_compile_options(Stobe PRIVATE /EHa)
endif()

if(STOBE_DIAG_PROFILE STREQUAL "normal")
  target_compile_definitions(Stobe PRIVATE STOBE_DIAG_PROFILE_NORMAL=1)
elseif(STOBE_DIAG_PROFILE STREQUAL "ui_only")
  target_compile_definitions(Stobe PRIVATE STOBE_DIAG_UI_ONLY=1 STOBE_DIAG_NO_CONTEXT=1)
elseif(STOBE_DIAG_PROFILE STREQUAL "no_hook")
  target_compile_definitions(Stobe PRIVATE STOBE_DIAG_NO_HOOK=1)
elseif(STOBE_DIAG_PROFILE STREQUAL "hook_no_orig")
  target_compile_definitions(Stobe PRIVATE STOBE_DIAG_NO_ORIG=1)
elseif(STOBE_DIAG_PROFILE STREQUAL "no_context")
  target_compile_definitions(Stobe PRIVATE STOBE_DIAG_NO_CONTEXT=1)
elseif(STOBE_DIAG_PROFILE STREQUAL "no_hook_no_context")
  target_compile_definitions(Stobe PRIVATE STOBE_DIAG_NO_HOOK=1 STOBE_DIAG_NO_CONTEXT=1)
else()
  message(FATAL_ERROR "Unknown STOBE_DIAG_PROFILE value: ${STOBE_DIAG_PROFILE}")
endif()

set_target_properties(Stobe PROPERTIES OUTPUT_NAME "Stobe")


