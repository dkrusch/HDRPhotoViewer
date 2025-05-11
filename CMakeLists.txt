cmake_minimum_required(VERSION 3.15)
project(HDRViewer VERSION 0.1 LANGUAGES CXX)

# Use Unicode WinMain, no console
add_compile_definitions(UNICODE _UNICODE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include paths
include_directories(
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/third_party/stb_image
  ${PROJECT_SOURCE_DIR}/third_party/d3dx12
)

# Gather sources
file(GLOB_RECURSE SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/*.cpp)

# Build HDRViewer.exe as a GUI app
add_executable(HDRViewer WIN32 ${SOURCE_FILES})

# Link libraries
target_link_libraries(HDRViewer
  PRIVATE
    d3d12
    dxgi
    d3dcompiler
    comdlg32
)

# Now route all output (exe + pdb) to the project root
set(TargetOut ${CMAKE_SOURCE_DIR})
set_target_properties(HDRViewer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${TargetOut}
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${TargetOut}
  PDB_OUTPUT_DIRECTORY_DEBUG       ${TargetOut}
  PDB_OUTPUT_DIRECTORY              ${CMAKE_SOURCE_DIR}
  PDB_OUTPUT_DIRECTORY_RELEASE     ${TargetOut}
)