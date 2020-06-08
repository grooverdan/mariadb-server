# Copyright (c) 2020 IBM
# Use is subject to license terms.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA 

FILE(REMOVE "${TARGET_LOCATION}")

SET(MRI_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.mri")
FILE(REMOVE ${MRI_SCRIPT})

SET(SCRIPT_CONTENTS "CREATE ${TARGET_LOCATION}\n")

SEPARATE_ARGUMENTS(STATIC_LIBS_LIST UNIX_COMMAND "${STATIC_LIBS}")
FOREACH(LIB ${STATIC_LIBS_LIST})
        STRING(APPEND SCRIPT_CONTENTS "ADDLIB ${LIB}\n")
ENDFOREACH()
STRING(APPEND SCRIPT_CONTENTS "SAVE\nEND\n")
FILE(WRITE ${MRI_SCRIPT} "${SCRIPT_CONTENTS}")

EXECUTE_PROCESS(
        COMMAND ${CMAKE_AR} -M
	INPUT_FILE ${MRI_SCRIPT}
)
EXECUTE_PROCESS(
  COMMAND ${CMAKE_RANLIB} ${TARGET_LOCATION}
)
