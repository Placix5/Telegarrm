cmake_minimum_required(VERSION 3.14)
project(Telegarrm VERSION 0.1.0 LANGUAGES CXX)

# Estándar de C++
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TELEGARRM_BUILD_TESTS "Compilar los tests (ctest)" ON)

# Sin tipo de compilación, CMake no optimiza (-O0) y el parser es entre 5 y 7 veces más lento.
# RelWithDebInfo: -O2 y con símbolos, para poder diagnosticar fallos en la Pi.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Tipo de compilación" FORCE)
endif()

# Dependencias
find_package(Threads REQUIRED)   # cpp-httplib y los hilos de Telegram y sincronización
find_package(SQLite3 REQUIRED)   # Módulo FindSQLite3 (CMake >= 3.14)

# TDLib (interfaz JSON). Se busca en las rutas estándar y en ~/td/tdlib; si está en
# otro sitio, indicar -DTd_DIR=<prefijo>/lib/cmake/Td.
# Sin versión en find_package: TDLib declara compatibilidad exacta y cualquier
# actualización rompería la configuración. El mínimo se comprueba a mano.
find_package(Td REQUIRED PATHS "$ENV{HOME}/td/tdlib")
if(Td_VERSION VERSION_LESS 1.8.0)
    message(FATAL_ERROR "Se necesita TDLib >= 1.8.0 (encontrado ${Td_VERSION})")
endif()
message(STATUS "TDLib ${Td_VERSION}: ${Td_DIR}")

# CMake >= 4.3 renombra el target a SQLite3::SQLite3; las versiones anteriores solo traen SQLite::SQLite3
if(TARGET SQLite3::SQLite3)
    set(TELEGARRM_SQLITE_TARGET SQLite3::SQLite3)
else()
    set(TELEGARRM_SQLITE_TARGET SQLite::SQLite3)
endif()

# Todo salvo main(): lo comparten el ejecutable y los tests
add_library(telegarrm_core STATIC
    src/api.cpp
    src/catalog.cpp
    src/channel_sync.cpp
    src/db_manager.cpp
    src/media_parser.cpp
    src/signal_watcher.cpp
    src/telegram_client.cpp
)
# include/ contiene también las dependencias de solo cabecera: httplib.h y nlohmann/json.hpp
target_include_directories(telegarrm_core PUBLIC include)
# La versión del proyecto se expone al código como TELEGARRM_VERSION ("0.1.0")
target_compile_definitions(telegarrm_core PUBLIC TELEGARRM_VERSION="${PROJECT_VERSION}")
target_link_libraries(telegarrm_core PUBLIC Threads::Threads ${TELEGARRM_SQLITE_TARGET} Td::TdJson)

add_executable(telegarrm src/main.cpp)
target_link_libraries(telegarrm PRIVATE telegarrm_core)

if(TELEGARRM_BUILD_TESTS)
    enable_testing()
    add_executable(telegarrm_tests tests/parser_tests.cpp)
    target_link_libraries(telegarrm_tests PRIVATE telegarrm_core)
    add_test(NAME parser COMMAND telegarrm_tests)
endif()
