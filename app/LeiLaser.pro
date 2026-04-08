QT       += core gui network serialport

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = LeiLaserUI
TEMPLATE = app
CONFIG += c++11

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated
DEFINES += QT_DEPRECATED_WARNINGS

SOURCES += \
    src/main.cpp \
    src/ui/mainwindow.cpp \
    src/core/serial_comm.cpp \
    src/core/cam_engine.cpp \
    src/network/wifi_server.cpp

HEADERS += \
    src/ui/mainwindow.h \
    src/core/serial_comm.h \
    src/core/cam_engine.h \
    src/network/wifi_server.h

INCLUDEPATH += $$PWD/src

# RK3566 Linuxfb deploy rules
target.path = /opt/LeiLaser
INSTALLS += target
