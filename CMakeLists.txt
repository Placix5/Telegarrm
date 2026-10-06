cmake_minimum_required(VERSION 3.14)
project(Telegarrm VERSION 0.1.0 LANGUAGES CXX)

# Estándar de C++
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# TDLib todavía no se usa en el código; se activa con -DTELEGARRM_WITH_TDLIB=ON
option(TELEGARRM_WITH_TDLIB "Enlazar con TDLib (requiere TDLib instalado)" OFF)

# Dependencias
find_package(Threads REQUIRED)   # cpp-httplib y el hilo de Telegram
find_package(SQLite3 REQUIRED)   # Módulo FindSQLite3 (CMake >= 3.14)

# Ejecutable
add_executable(telegarrm
    src/main.cpp
    src/db_manager.cpp
    src/telegram_client.cpp
)

target_include_directories(telegarrm PRIVATE include)

# La versión del proyecto se expone al código como TELEGARRM_VERSION ("0.1.0")
target_compile_definitions(telegarrm PRIVATE TELEGARRM_VERSION="${PROJECT_VERSION}")

# CMake >= 4.3 renombra el target a SQLite3::SQLite3; las versiones anteriores solo traen SQLite::SQLite3
if(TARGET SQLite3::SQLite3)
    set(TELEGARRM_SQLITE_TARGET SQLite3::SQLite3)
else()
    set(TELEGARRM_SQLITE_TARGET SQLite::SQLite3)
endif()

target_link_libraries(telegarrm PRIVATE Threads::Threads ${TELEGARRM_SQLITE_TARGET})

# TDLib: si no está en una ruta estándar, indicar -DTd_DIR=<prefijo>/lib/cmake/Td
if(TELEGARRM_WITH_TDLIB)
    find_package(Td 1.8.0 REQUIRED)
    # Td::TdStatic = interfaz C++ nativa (td/telegram/Client.h).
    # Alternativa: Td::TdJson para la interfaz JSON (td/telegram/td_json_client.h).
    target_link_libraries(telegarrm PRIVATE Td::TdStatic)
    target_compile_definitions(telegarrm PRIVATE TELEGARRM_HAS_TDLIB)
endif()
