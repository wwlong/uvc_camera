HEADERS       = \
                window.h \
                glhelper.h \
    camerasource.h \
    faceviewwidget.h \
    mainwidget.h \
    videofilesource.h \
    framesource.h \
    arcfaceengine.h \
    key.h
SOURCES       = \
                main.cpp \
                window.cpp \
                glhelper.cpp \
    camerasource.cpp \
    faceviewwidget.cpp \
    mainwidget.cpp \
    videofilesource.cpp \
    arcfaceengine.cpp

QT           += widgets  multimedia sql
win32:LIBS       +=  -L../lib/linux_x64 -llibarcsoft_fsdk_face_tracking -llibarcsoft_fsdk_face_recognition
unix:LIBS        +=  -L../lib/linux_x64  -larcsoft_fsdk_face_tracking -larcsoft_fsdk_face_recognition
