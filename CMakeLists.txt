cmake_minimum_required(VERSION 3.20)
project(DefconCNProof LANGUAGES C CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This proof requires Windows x64")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
# set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
# set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_SOURCE_DIR}/bin")
  # set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_SOURCE_DIR}/bin")
  # set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_SOURCE_DIR}/bin")
endforeach()

set(CMAKE_CXX_STANDARD 17)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
include(FetchContent)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537)
FetchContent_MakeAvailable(minhook)
add_library(DefconCNProof SHARED src/localization.cpp src/native_font.cpp)
target_link_libraries(DefconCNProof PRIVATE minhook advapi32 gdi32)
add_executable(DefconCNInject src/inject.cpp)
add_executable(DefconCNLaunch WIN32 src/inject.cpp)
target_compile_definitions(DefconCNLaunch PRIVATE DEFCON_CN_LAUNCHER)
target_link_libraries(DefconCNLaunch PRIVATE user32)
add_dependencies(DefconCNLaunch DefconCNProof)
add_custom_target(DefconCNAssets ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DefconCNLaunch>/data/defconcn"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/defconcn"
    "$<TARGET_FILE_DIR:DefconCNLaunch>/data/defconcn"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    "$<TARGET_FILE_DIR:DefconCNLaunch>/README.md"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DefconCNLaunch>/licenses"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${minhook_SOURCE_DIR}/LICENSE.txt"
    "$<TARGET_FILE_DIR:DefconCNLaunch>/licenses/MinHook.txt"
  DEPENDS DefconCNLaunch)
foreach(target DefconCNProof DefconCNInject DefconCNLaunch)
  target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8)
  endif()
endforeach()
