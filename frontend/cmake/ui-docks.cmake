target_sources(
  obs-studio
  PRIVATE
    docks/CanvasDocks.cpp
    docks/CanvasDocks.hpp
    docks/MultistreamDock.cpp
    docks/MultistreamDock.hpp
    docks/OBSDock.cpp
    docks/OBSDock.hpp
    youtube/YouTubeBrollRecorder.cpp
    youtube/YouTubeBrollRecorder.hpp
    youtube/YouTubeBrollReview.cpp
    youtube/YouTubeBrollReview.hpp
    youtube/YouTubePipelineDock.cpp
    youtube/YouTubePipelineDock.hpp
    youtube/YouTubeSecretVault.cpp
    youtube/YouTubeSecretVault.hpp
    youtube/YouTubeSettingsWidget.cpp
    youtube/YouTubeSettingsWidget.hpp
    youtube/YouTubeTerminalWidget.cpp
    youtube/YouTubeTerminalWidget.hpp
    youtube/YouTubeWorkflowManager.cpp
    youtube/YouTubeWorkflowManager.hpp
)

# YouTubeWorkflowManager runs scripts/youtube_pipeline.py from the data
# directory, so refresh it on every build instead of leaving a stale copy.
if(OS_WINDOWS)
  target_sources(obs-studio PRIVATE scripts/youtube_pipeline.py)
  add_custom_command(
    TARGET obs-studio
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_DATA_DESTINATION}/obs-studio/scripts"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/scripts/youtube_pipeline.py"
      "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_DATA_DESTINATION}/obs-studio/scripts/youtube_pipeline.py"
    COMMENT "Copy YouTube pipeline script to data directory"
    VERBATIM
  )
  install(
    FILES scripts/youtube_pipeline.py
    DESTINATION "${OBS_DATA_DESTINATION}/obs-studio/scripts"
    COMPONENT Runtime
  )
endif()
