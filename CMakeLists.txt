# mlvm/CMakeLists.txt
# root CMake file for mlvm project.

#--------------------------------------------------------------------
# set min version and deployment target -- before project
#--------------------------------------------------------------------

cmake_minimum_required(VERSION 3.5)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

IF(APPLE)
  SET(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for Mac OS X" FORCE)
  SET(CMAKE_OSX_DEPLOYMENT_TARGET "10.14" CACHE STRING "Minimum OS X deployment version")
ENDIF(APPLE)


#--------------------------------------------------------------------
# project and version
#--------------------------------------------------------------------

set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE) 

project(mlvm)

set(VERSION_MAJOR "0")
set(VERSION_MINOR "1")
set(VERSION_PATCH "0")
set(VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

option(BUILD_EXAMPLES "Build the examples" ON)
option(BUILD_TESTS "Build the tests" OFF)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

#--------------------------------------------------------------------
# Compiler flags
#--------------------------------------------------------------------

 set(CMAKE_CXX_STANDARD 17)
 set(CMAKE_CXX_STANDARD_REQUIRED True)
 set(CMAKE_CXX_VISIBILITY_PRESET hidden)
 set(CMAKE_VISIBILITY_INLINES_HIDDEN OFF)
 
 if(APPLE)
   # For now, explicitly disable C++17 alignment feature
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-aligned-new")
 elseif(WIN32)
   # no unknown pragma warning
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4068")
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:alignedNew-")
 endif()

if(MSVC)
    # arcane thing about setting runtime library flags
    cmake_policy(SET CMP0091 NEW)
    add_compile_options(
        $<$<CONFIG:>:/MT>
        $<$<CONFIG:Debug>:/MTd>
        $<$<CONFIG:Release>:/MT>
    )
endif()


#--------------------------------------------------------------------
# Choose library output name
#--------------------------------------------------------------------

# creates the library mlvm-debug in debug configuration
set(mlvm_NAME mlvm$<$<CONFIG:Debug>:-debug>)

#--------------------------------------------------------------------
# find madronalib
# MacOS: /usr/local/include/madronalib
# Windows: C:/Program Files/madronalib/include
#--------------------------------------------------------------------

if(APPLE)
    include(GNUInstallDirs)
    set (MADRONALIB_INCLUDE_DIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}/madronalib")
    set (MADRONALIB_LIBRARY_DIR ${CMAKE_INSTALL_FULL_LIBDIR})
elseif(WIN32)
    if(EXISTS "C:/Program Files/madronalib/lib/madrona.lib")
        set (MADRONALIB_INCLUDE_DIR "C:/Program Files/madronalib/include")
        set (MADRONALIB_LIBRARY_DIR "C:/Program Files/madronalib/lib")
    else()
        set (MADRONALIB_INCLUDE_DIR "C:/Program Files (x86)/madronalib/include")
        set (MADRONALIB_LIBRARY_DIR "C:/Program Files (x86)/madronalib/lib")
    endif()
else()
endif()

# add -debug suffix to link debug madronalib for debug builds
set(madronalib_NAME madrona$<$<CONFIG:Debug>:-debug>)

message("madronalib headers should be in: " ${MADRONALIB_INCLUDE_DIR} )
message("madronalib library should be in: " ${MADRONALIB_LIBRARY_DIR}/${madronalib_NAME} )

#--------------------------------------------------------------------
# Enforce out of source build
#--------------------------------------------------------------------

if(CMAKE_BINARY_DIR EQUAL CMAKE_SOURCE_DIR)
  message(FATAL_ERROR "mlvm requires an out of source build.")
endif()

#--------------------------------------------------------------------
# Add include directories
#--------------------------------------------------------------------

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

#--------------------------------------------------------------------
# collect mlvm sources and headers
#--------------------------------------------------------------------

file(GLOB APP_SOURCES "source/*.cpp")
file(GLOB APP_HEADERS "source/*.h")
file(GLOB EXT_HEADERS "include/*.h")

# set the sources - we include the headers only to make them 
# navigable within IDEs

set(mlvm_SOURCES
    ${APP_SOURCES}
    ${APP_HEADERS}
    ${EXT_HEADERS}
    )

set(MLVM_INCLUDE_DIRS
        source
        ${MLVG_SOURCE_DIR}
        ${MLVG_SOURCE_DIR}/external
)

# send binary output to the current build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

#--------------------------------------------------------------------
# set source groups for some source above
#--------------------------------------------------------------------

message("mlvm sources: " ${mlvm_SOURCES} )
# or don't

#--------------------------------------------------------------------
# create and install mlvm library and headers
#--------------------------------------------------------------------

set(target mlvm)

add_library(${target} STATIC ${mlvm_SOURCES})

set_target_properties(${target} PROPERTIES
                      OUTPUT_NAME "${mlvm_NAME}"
                      VERSION ${mlvm_VERSION}
                      SOVERSION ${mlvm_VERSION_MAJOR}
                      POSITION_INDEPENDENT_CODE ON
                      FOLDER "mlvm")
                      
target_include_directories(${target} PRIVATE ${RTAUDIO_HEADERS})
target_include_directories(${target} PRIVATE ${MADRONALIB_INCLUDE_DIR})
                      
# choose driver for Rtaudio
if(WIN32)
    target_compile_definitions(${target} PRIVATE __WINDOWS_WASAPI__)
elseif(APPLE)
    target_compile_definitions(${target} PRIVATE __MACOSX_CORE__)
elseif(LINUX_JACK)
    target_compile_definitions(${target} PRIVATE __UNIX_JACK__)
    target_link_libraries(${target} PRIVATE jack pthread)
elseif(LINUX_PULSE)
    target_compile_definitions(${target} PRIVATE __LINUX_PULSE__)
    target_link_libraries(${target} PRIVATE pulse pthread)
elseif(LINUX_ASOUND)
    target_compile_definitions(${target} PRIVATE __LINUX_ALSA__)
    target_link_libraries(${target} PRIVATE asound pthread)
else()
    message(FATAL, "On Linux, one of LINUX_JACK, LINUX_PULSE, or LINUX_ASOUND must be set.")
endif()

if(APPLE)
    set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")
endif()

include(GNUInstallDirs)

if(WIN32)
    set(CMAKE_INSTALL_LIBDIR "C:/Program Files/mlvm/lib")
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
      set(CMAKE_INSTALL_PREFIX "C:/Program Files/mlvm" CACHE PATH "..." FORCE)
    endif()
endif()

install(
    TARGETS ${target}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

message("mlvm destination: " ${CMAKE_INSTALL_LIBDIR} )

#--------------------------------------------------------------------
# install headers
#--------------------------------------------------------------------

if(APPLE)
    set(INCLUDES_INSTALL_DIR "include/mlvm")
elseif(WIN32)
    set(INCLUDES_INSTALL_DIR "include")
endif()

install(FILES include/mlvm.h DESTINATION ${INCLUDES_INSTALL_DIR})

install(FILES 
    ${EXT_HEADERS}
    DESTINATION 
    # PERMISSIONS OWNER_READ GROUP_READ WORLD_READ
    ${INCLUDES_INSTALL_DIR}
  )

message("mlvm headers destination: " ${INCLUDES_INSTALL_DIR} )

#--------------------------------------------------------------------
# build examples
#--------------------------------------------------------------------

set(ML_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

function(MAKE_EXAMPLE EXAMPLE_APP_NAME EXAMPLE_SOURCE_FILE)

    set(
        EXAMPLE_SOURCES
        ${ML_ROOT}/examples/${EXAMPLE_SOURCE_FILE}
    )

    add_executable(${EXAMPLE_APP_NAME} ${EXAMPLE_SOURCES})

    target_include_directories(${EXAMPLE_APP_NAME} PRIVATE
            ${ML_ROOT}/external
            ${MADRONALIB_INCLUDE_DIR}
    )


    # add madronalib
    target_include_directories(${EXAMPLE_APP_NAME} PRIVATE ${MADRONALIB_INCLUDE_DIR})
    if(APPLE)
        target_link_libraries(${EXAMPLE_APP_NAME} PRIVATE "${MADRONALIB_LIBRARY_DIR}/lib${madronalib_NAME}.a")
    elseif(WIN32)
        target_link_libraries(${EXAMPLE_APP_NAME} PRIVATE "${MADRONALIB_LIBRARY_DIR}/${madronalib_NAME}.lib")
    endif()


    target_link_libraries(${EXAMPLE_APP_NAME} PRIVATE mlvm)


# Add platform libraries

# note: ffft and utf are header-only
# cJSON, aes256 and others added as source

if(APPLE)
    target_link_libraries(${target} "-framework Carbon")
    target_link_libraries(${target} "-framework CoreServices")
    target_link_libraries(${target} "-framework CoreMIDI")
    target_link_libraries(${target} "-framework AudioToolbox")
    target_link_libraries(${target} "-framework AudioUnit")
    target_link_libraries(${target} "-framework CoreAudio")
    target_link_libraries(${target} "-framework Foundation")
    target_link_libraries(${target} "-framework OpenGL")
    target_link_libraries(${target} "-framework GLUT")
else(APPLE)
    # target_link_libraries(${target} ${DNSSD_LIBRARIES})
    target_link_libraries(${target} winmm.lib)
endif()


    set_target_properties(${EXAMPLE_APP_NAME}
        PROPERTIES
        MACOSX_BUNDLE TRUE

        # not working, see below for workaround adding to plist 
        # MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/MacOSXBundleInfo.plist.in 

        MACOSX_BUNDLE_BUNDLE_NAME ${EXAMPLE_APP_NAME}
        MACOSX_BUNDLE_BUNDLE_VERSION "0.9.0"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    # get directory of .app package Contents
    set(PACKAGE_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/bin/${EXAMPLE_APP_NAME}.app/Contents"
        )    

    # add missing keys to plist for Mac OS
    if(APPLE)
        set(ADD_PLIST_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/add-plist-value.sh")
        set(RATIONALE "This app requires microphone access.")
        add_custom_command(
            TARGET ${EXAMPLE_APP_NAME} POST_BUILD
            COMMAND ${ADD_PLIST_SCRIPT} NSMicrophoneUsageDescription "This app requires microphone access." "${PACKAGE_DIR}/Info.plist"
            VERBATIM
        )
    endif()
endfunction()

#--------------------------------------------------------------------
# build all examples
#--------------------------------------------------------------------

if(BUILD_EXAMPLES)
    make_example(Example0 example0.cpp)
endif()

#--------------------------------------------------------------------
# build tests
#--------------------------------------------------------------------

#if(BUILD_TESTS)
#    set(target tests)
#    file(GLOB TEST_SOURCES "tests/*.*")
#    add_executable(tests ${TEST_SOURCES})
#    set_target_properties(tests PROPERTIES EXCLUDE_FROM_ALL TRUE)
#    add_dependencies(tests mlvm)
#    target_link_libraries(tests mlvm)
#endif()

#--------------------------------------------------------------------
# Including custom cmake rules
#--------------------------------------------------------------------

set(mlvm_SOURCES_internal
    ${APP_SOURCES}
    ${APP_HEADERS}
    )

# Adding clang-format target if executable is found
find_program(CLANG_FORMAT "clang-format")
if(CLANG_FORMAT)
  add_custom_target(
    clang-format
    COMMAND /opt/homebrew/bin/clang-format
    -i
    --verbose
    -style=file
    ${mlvm_SOURCES_internal}
    )
endif()



