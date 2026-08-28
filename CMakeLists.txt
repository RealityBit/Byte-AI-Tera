find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(SQLite3 REQUIRED)

set(TARGET llama-wiki-chat)
add_executable(${TARGET} wiki-chat.cpp
                          modules/wiki_fetch.cpp
                          modules/news_fetch.cpp
                          modules/datetime_fetch.cpp
                          modules/math_fetch.cpp
                          modules/weather_fetch.cpp
                          modules/quick_response.cpp
                          modules/unit_fetch.cpp
                          modules/memory_store.cpp
                          modules/scheduler.cpp
                          modules/category_fetch.cpp
                          modules/model_fetch.cpp
                          modules/training_log.cpp)
target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS ${TARGET} RUNTIME)
target_link_libraries(${TARGET} PRIVATE llama llama-common-base CURL::libcurl nlohmann_json::nlohmann_json SQLite3::SQLite3 ${CMAKE_THREAD_LIBS_INIT})
target_compile_features(${TARGET} PRIVATE cxx_std_17)
