cmake_minimum_required(VERSION 3.14)
project(Telegarrm VERSION 0.1.0 LANGUAGES CXX)

# Estándar de C++
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencias
find_package(Threads REQUIRED)   # cpp-httplib y el hilo de Telegram
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

# Ejecutable
add_executable(telegarrm
    src/main.cpp
    src/api.cpp
    src/channel_sync.cpp
    src/db_manager.cpp
    src/signal_watcher.cpp
    src/telegram_client.cpp
)

# include/ contiene también las dependencias de solo cabecera: httplib.h y nlohmann/json.hpp
target_include_directories(telegarrm PRIVATE include)

# La versión del proyecto se expone al código como TELEGARRM_VERSION ("0.1.0")
target_compile_definitions(telegarrm PRIVATE TELEGARRM_VERSION="${PROJECT_VERSION}")

# CMake >= 4.3 renombra el target a SQLite3::SQLite3; las versiones anteriores solo traen SQLite::SQLite3
if(TARGET SQLite3::SQLite3)
    set(TELEGARRM_SQLITE_TARGET SQLite3::SQLite3)
else()
    set(TELEGARRM_SQLITE_TARGET SQLite::SQLite3)
endif()

target_link_libraries(telegarrm PRIVATE Threads::Threads ${TELEGARRM_SQLITE_TARGET} Td::TdJson)
