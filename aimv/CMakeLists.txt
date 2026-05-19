# [AIMV] CMake configuration — install driver + server alongside clang/opt

# Install the Python package to share/aimv/
install(
  DIRECTORY driver/ mcp_server/ ci/ test/ benchmarks/ config/
  DESTINATION "${CMAKE_INSTALL_DATADIR}/aimv"
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

# Install entry-point scripts to bin/
install(
  PROGRAMS bin/aimv-driver bin/aimv-server
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
