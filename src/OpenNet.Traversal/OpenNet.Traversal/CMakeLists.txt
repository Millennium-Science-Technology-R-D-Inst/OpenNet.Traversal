if(NOT UNIX)
  message(FATAL_ERROR "OpenNet.Traversal targets Linux/POSIX hosts")
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

add_executable(OpenNet.Traversal
  src/main.cpp
  src/config.cpp
  src/coordinator_client.cpp
  src/http_probe_server.cpp
  src/probe.cpp
  src/stun_server.cpp)

target_include_directories(OpenNet.Traversal PRIVATE include)
target_compile_features(OpenNet.Traversal PRIVATE cxx_std_20)
target_compile_options(OpenNet.Traversal PRIVATE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
target_link_libraries(OpenNet.Traversal PRIVATE Threads::Threads CURL::libcurl)

include(GNUInstallDirs)
install(TARGETS OpenNet.Traversal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ../deploy/opennet-traversal.service
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
