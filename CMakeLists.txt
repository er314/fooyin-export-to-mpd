cmake_minimum_required(VERSION 3.16)

project(mpdadd
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Fooyin CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Concurrent Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPDCLIENT REQUIRED IMPORTED_TARGET libmpdclient)

create_fooyin_plugin(mpdadd
    DEPENDS
        Fooyin::Core
        Fooyin::Gui
        Qt6::Concurrent
        PkgConfig::MPDCLIENT
    SOURCES
        mpdwidgetsplugin.h
        mpdwidgetsplugin.cpp
        mpdcontrols.h
        mpdcontrols.cpp
)
