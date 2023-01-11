
IF(NOT_FOR_DISTRIBUTION)
  INCLUDE(FindPkgConfig)
  IF(PKG_CONFIG_FOUND)
    pkg_search_module(JOSE REQUIRED jose)
    IF(NOT JOSE_FOUND)
      SET(REASON "Clevis requires jose development package")
    ELSE()
      FIND_PACKAGE(CURL)
      IF(NOT CURL_FOUND)
        SET(REASON "Clevis requires libcurl development package")
      ENDIF()
    ENDIF()
  ELSE()
    SET(REASON "pkg-config not found")
  ENDIF()
ELSE()
  SET(REASON "jose has Apache 2.0 License which is not complatible with GPLv2. Set -DNOT_FOR_DISTRIBUTION=ON if you need it")
ENDIF()

IF(REASON)
  MESSAGE_ONCE(CLEVIS_KEY_MANAGEMENT_NO_JOSE_SDK "Can't build clevis_key_management - ${REASON})")
  ADD_FEATURE_INFO(CLEVIS_KEY_MANAGEMENT "OFF" "Clevis Encryption Key Management Plugin")
ENDIF()
 	 
INCLUDE_DIRECTORIES(${CURL_INCLUDE_DIRS})
INCLUDE_DIRECTORIES(${JOSE_INCLUDE_DIRS})

MYSQL_ADD_PLUGIN(CLEVIS_KEY_MANAGEMENT clevis_key_management_plugin.cc
	LINK_LIBRARIES ${CURL_LIBRARIES} ${JOSE_LIBRARIES}
	MODULE_ONLY COMPONENT clevis-key-management)

ADD_FEATURE_INFO(CLEVIS_KEY_MANAGEMENT "ON" "Clevis Encryption Key Management Plugin")
