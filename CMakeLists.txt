cmake_minimum_required(VERSION 3.10)
project(Chat_Server)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Поиск необходимых пакетов
find_package(PkgConfig REQUIRED)

# Поиск MySQL Connector/C++
find_path(MYSQL_CONNECTOR_INCLUDE_DIR
    NAMES mysql_driver.h
    PATHS /usr/include/mysql-cppconn-8
          /usr/local/include/mysql-cppconn-8
          /usr/include/mysql
          /usr/local/include/mysql
)

find_library(MYSQL_CONNECTOR_LIB
    NAMES mysqlcppconn8 mysqlcppconn
    PATHS /usr/lib/x86_64-linux-gnu
          /usr/local/lib
          /usr/lib
)

# Поиск MySQL клиентской библиотеки
find_library(MYSQL_CLIENT_LIB
    NAMES mysqlclient
    PATHS /usr/lib/x86_64-linux-gnu
          /usr/local/lib/mysql
          /usr/lib/mysql
)

# Поиск OpenSSL для SHA1 (если используется)
find_package(OpenSSL REQUIRED)

# Проверка найденных библиотек
if(MYSQL_CONNECTOR_INCLUDE_DIR AND MYSQL_CONNECTOR_LIB AND MYSQL_CLIENT_LIB)
    message(STATUS "Found MySQL Connector/C++: ${MYSQL_CONNECTOR_LIB}")
    message(STATUS "MySQL Include Dir: ${MYSQL_CONNECTOR_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "MySQL Connector/C++ not found!")
endif()

if(OpenSSL_FOUND)
    message(STATUS "Found OpenSSL: ${OPENSSL_VERSION}")
endif()

# Добавляем исполняемый файл
add_executable(Chat_Server 
    Chat_c++.cpp
    Chat.cpp
    DBmanager.cpp
)

# Включаем директории
target_include_directories(Chat_Server PRIVATE
    ${MYSQL_CONNECTOR_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
)

# Линкуем библиотеки
target_link_libraries(Chat_Server PRIVATE
    ${MYSQL_CONNECTOR_LIB}
    ${MYSQL_CLIENT_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
    dl
)

# Добавляем флаги компиляции
target_compile_options(Chat_Server PRIVATE
    -std=c++17
    -pthread
)