set(proj python-couchdb)

# Set dependency list
set(${proj}_DEPENDENCIES python python-setuptools)

# Include dependent projects if any
ExternalProject_Include_Dependencies(${proj} PROJECT_VAR proj DEPENDS_VAR ${proj}_DEPENDENCIES)

if(${CMAKE_PROJECT_NAME}_USE_SYSTEM_${proj})
  # XXX - Add a test checking if <proj> is available
endif()

if(NOT DEFINED ${CMAKE_PROJECT_NAME}_USE_SYSTEM_${proj})
  set(${CMAKE_PROJECT_NAME}_USE_SYSTEM_${proj} ${${CMAKE_PROJECT_NAME}_USE_SYSTEM_python})
endif()

if(NOT ${CMAKE_PROJECT_NAME}_USE_SYSTEM_${proj})

  ExternalProject_Add(${proj}
    ${${proj}_EP_ARGS}
    URL "https://pypi.python.org/packages/9a/e8/c3c8da6d00145aaca07f2b784794917613dad26532068da4e8392dc48d7f/CouchDB-1.1.tar.gz"
    URL_MD5 "2ed5ad2a477fd3cb472ed6dc5a381ff3"
    SOURCE_DIR ${proj}
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ${PYTHON_EXECUTABLE} setup.py install
    DEPENDS
      ${${proj}_DEPENDENCIES}
    )

else()
  ExternalProject_Add_Empty(${proj} DEPENDS ${${proj}_DEPENDENCIES})
endif()
