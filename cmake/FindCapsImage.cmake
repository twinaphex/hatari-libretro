
IF (CAPSIMAGE_INCLUDE_DIR)
  # Already in cache, be silent
  SET(CAPSIMAGE_FIND_QUIETLY TRUE)
ENDIF (CAPSIMAGE_INCLUDE_DIR)

FIND_PATH(CAPSIMAGE_INCLUDE_DIR caps/capsimage.h)

FIND_LIBRARY(CAPSIMAGE_LIBRARY NAMES capsimage)

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(CAPSIMAGE DEFAULT_MSG
                                  CAPSIMAGE_LIBRARY CAPSIMAGE_INCLUDE_DIR)

MARK_AS_ADVANCED(CAPSIMAGE_LIBRARY CAPSIMAGE_INCLUDE_DIR)
