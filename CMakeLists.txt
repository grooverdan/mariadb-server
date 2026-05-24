INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb.cmake)
INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb_errors.cmake)
INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb_target_setup.cmake)

# Provide MariaDB server include paths to sub-libraries.
# MYSQL_ADD_PLUGIN normally does this, but it runs after ADD_SUBDIRECTORY.
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include
                    ${CMAKE_SOURCE_DIR}/sql
                    ${CMAKE_BINARY_DIR}/include
                    ${PCRE_INCLUDE_DIRS}
                    ${SSL_INCLUDE_DIRS}
                    ${ZLIB_INCLUDE_DIRS})

# Build sub-libraries.
ADD_SUBDIRECTORY(common)
ADD_SUBDIRECTORY(convertor)
ADD_SUBDIRECTORY(runtime)

# GenError must run before duckdb_error.h is generated.
ADD_DEPENDENCIES(duckdb_error_h GenError)

# Plugin sources (handler layer + UDFs whose extern "C" symbols must be
# in the final .so for dlsym() loading).
SET(DUCKDB_PLUGIN_SOURCES
  ha_duckdb.cc
  ha_duckdb_pushdown.cc
  duckdb_udf.cc
)

MYSQL_ADD_PLUGIN(duckdb ${DUCKDB_PLUGIN_SOURCES}
  STORAGE_ENGINE
  MODULE_ONLY
  COMPONENT duckdb-engine
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/duckdb.cnf
  LINK_LIBRARIES duckdb_runtime duckdb_convertor duckdb_common ${DUCKDB_LIBRARY}
)

IF(TARGET duckdb)
  duckdb_setup_target(duckdb)
  TARGET_INCLUDE_DIRECTORIES(duckdb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/convertor
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
  )
ENDIF()

IF(TARGET duckdb)
  INSTALL(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install.sql
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/uninstall.sql
    DESTINATION ${INSTALL_MYSQLSHAREDIR}/duckdb
    COMPONENT duckdb-engine)
ENDIF()
