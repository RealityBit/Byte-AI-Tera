find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

set(TARGET llama-wiki-chat)
add_executable(${TARGET} wiki-chat.cpp wiki_fetch.cpp news_fetch.cpp datetime_fetch.cpp math_fetch.cpp training_log.cpp)
install(TARGETS ${TARGET} RUNTIME)
target_link_libraries(${TARGET} PRIVATE llama llama-common-base CURL::libcurl nlohmann_json::nlohmann_json ${CMAKE_THREAD_LIBS_INIT})
target_compile_features(${TARGET} PRIVATE cxx_std_17)
