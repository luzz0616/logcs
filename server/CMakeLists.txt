find_package(Threads REQUIRED)

add_executable(logser logser.cpp)
target_link_libraries(logser Threads::Threads dlt_log)

install(TARGETS logser RUNTIME DESTINATION bin)
install(FILES logser.service.in DESTINATION lib/systemd/system/ RENAME logser.service)
