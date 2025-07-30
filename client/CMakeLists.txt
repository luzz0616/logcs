add_library(logcliapi SHARED logcliapi.c)
target_link_libraries(logcliapi pthread resolv)

install(TARGETS logcliapi LIBRARY DESTINATION lib)
install(FILES logcliapi.h DESTINATION include)