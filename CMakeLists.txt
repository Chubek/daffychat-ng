cmake_minimum_required(VERSION 3.16)

project(daffychat LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(daffychat-server server.cpp)
target_include_directories(daffychat-server PRIVATE third_party/uWebSockets/src)
target_include_directories(daffychat-server PRIVATE third_party/uSockets/src)
target_include_directories(daffychat-server PRIVATE third_party/json/include)
target_include_directories(daffychat-server PRIVATE third_party/libconfuse/src)
target_compile_definitions(daffychat-server PRIVATE LIBUS_NO_SSL LIBUS_USE_EPOLL)

target_sources(daffychat-server PRIVATE
  third_party/uSockets/src/context.c
  third_party/uSockets/src/loop.c
  third_party/uSockets/src/socket.c
  third_party/uSockets/src/udp.c
  third_party/uSockets/src/bsd.c
  third_party/uSockets/src/eventing/epoll_kqueue.c
  third_party/libconfuse/src/confuse.c
)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libconfuse_lexer.c
  COMMAND flex -Pcfg_yy -o${CMAKE_CURRENT_BINARY_DIR}/libconfuse_lexer.c ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libconfuse/src/lexer.l
  DEPENDS third_party/libconfuse/src/lexer.l
  VERBATIM
)
target_sources(daffychat-server PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/libconfuse_lexer.c)
set_source_files_properties(
  third_party/libconfuse/src/confuse.c
  third_party/libconfuse/src/fmemopen.c
  third_party/libconfuse/src/reallocarray.c
  ${CMAKE_CURRENT_BINARY_DIR}/libconfuse_lexer.c
  PROPERTIES COMPILE_DEFINITIONS HAVE_CONFIG_H
)

target_link_libraries(daffychat-server PRIVATE pthread z m)

install(TARGETS daffychat-server RUNTIME DESTINATION bin)
install(FILES client.html DESTINATION share/daffychat)
install(FILES guide.html DESTINATION share/daffychat)
install(FILES manifest.webmanifest DESTINATION share/daffychat)
install(FILES sw.js DESTINATION share/daffychat)
install(FILES icon.svg DESTINATION share/daffychat)
install(FILES config/daffychat.conf DESTINATION etc/daffychat)
install(FILES daffychat.service DESTINATION lib/systemd/system)
install(FILES daffychat@.service DESTINATION lib/systemd/system)
install(FILES daffychat.user.service DESTINATION share/daffychat)
install(PROGRAMS scripts/setup-user-service.sh DESTINATION share/daffychat)

add_executable(daffychat-ws-smoke tools/ws_smoke.cpp)
target_compile_features(daffychat-ws-smoke PRIVATE cxx_std_17)
