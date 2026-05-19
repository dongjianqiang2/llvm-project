# [AIMV] CMake configuration — install driver + server alongside clang/opt

# Install Python sub-packages preserving directory structure
foreach(subdir driver mcp_server ci test benchmarks config)
  install(
    DIRECTORY ${subdir}/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/aimv/${subdir}"
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
    PATTERN "*.o" EXCLUDE
    PATTERN "*.aimv-tmp*" EXCLUDE
    PATTERN "*.rej" EXCLUDE
    PATTERN "*.bak" EXCLUDE
    PATTERN "sessions/*" EXCLUDE
    PATTERN "backups/*" EXCLUDE
    PATTERN "locks/*" EXCLUDE
  )
endforeach()

# Install top-level __init__.py files
install(
  FILES driver/__init__.py DESTINATION "${CMAKE_INSTALL_DATADIR}/aimv/driver"
)
install(
  FILES mcp_server/__init__.py DESTINATION "${CMAKE_INSTALL_DATADIR}/aimv/mcp_server"
)
install(
  FILES ci/__init__.py DESTINATION "${CMAKE_INSTALL_DATADIR}/aimv/ci"
)
install(
  FILES test/__init__.py DESTINATION "${CMAKE_INSTALL_DATADIR}/aimv/test"
)

# Install entry-point scripts to bin/
install(
  PROGRAMS bin/aimv-driver bin/aimv-server
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
