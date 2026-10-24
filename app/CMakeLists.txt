# Main
target_sources(
    ${CMAKE_PROJECT_NAME} PRIVATE
    main.c
)
target_include_directories(
    ${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/
)
