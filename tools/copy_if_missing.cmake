# Copies SRC to DST only when DST does not exist: a file there may hold the player's own edits
if(NOT EXISTS "${DST}")
    file(COPY_FILE "${SRC}" "${DST}")
endif()
