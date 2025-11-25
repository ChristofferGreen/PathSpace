add_executable(path_renderer2d_benchmark
    ui/path_renderer2d_benchmark.cpp
)

target_link_libraries(path_renderer2d_benchmark PRIVATE PathSpace)

add_executable(widget_pipeline_benchmark
    ui/widget_pipeline_benchmark.cpp
)

target_link_libraries(widget_pipeline_benchmark PRIVATE PathSpace)

add_executable(undo_journal_benchmark
    history/undo_journal_benchmark.cpp
)

target_link_libraries(undo_journal_benchmark PRIVATE PathSpace)
