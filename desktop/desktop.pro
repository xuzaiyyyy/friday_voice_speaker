QT       += core gui multimedia multimediawidgets network

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

DEFINES += QT_DEPRECATED_WARNINGS

INCLUDEPATH += $$PWD/include
INCLUDEPATH += /usr/include/opencv4

# Prefer the RKNN runtime shipped with this project.  The system copy on some
# Orange Pi images is too old for models converted by RKNN Toolkit2 1.6.0.
RKNN_RUNTIME_DIR = $$PWD/../lib

# Link against the Ubuntu/OpenCV runtime libraries installed on the Orange Pi.
LIBS += -L$$RKNN_RUNTIME_DIR \
        -L/usr/lib \
        -L/usr/lib/aarch64-linux-gnu \
        -lrknnrt \
        -lopencv_core \
        -lopencv_imgproc \
        -lopencv_imgcodecs \
        -lopencv_videoio \
        -lopencv_highgui \
        -lopencv_dnn \
        -lopencv_objdetect \
        -lopencv_calib3d \
        -lopencv_features2d \
        -lopencv_flann \
        -lmysqlclient \
        -ldl \
        -lpthread

unix:!macx {
    QMAKE_LFLAGS += -Wl,-rpath,$$RKNN_RUNTIME_DIR
    QMAKE_LFLAGS += -Wl,-rpath-link,$$RKNN_RUNTIME_DIR
}

# ========================================================
# 4. 源文件列表
# ========================================================
SOURCES += \
    apppaths.cpp \
    calculate.cpp \
    camerapage.cpp \
    chatclient.cpp \
    client.cpp \
    customslider.cpp \
    door.cpp \
    desktopcontrol.cpp \
    desktopidle.cpp \
    expressionwindow.cpp \
    facethread.cpp \
    filemanager.cpp \
    friendiconlabel.cpp \
    friendinfowidget.cpp \
    imagepreview.cpp \
    livecamerawindow.cpp \
    main.cpp \
    mainwindow.cpp \
    musicpage.cpp \
    netcamerapage.cpp \
    photo.cpp \
    selfinfowidget.cpp \
    sendtextedit.cpp \
    serverpage.cpp \
    serverthread.cpp \
    sketchpad.cpp \
    stringtool.cpp \
    uicommand.cpp \
    videoplayer.cpp \
    src/buffer.cpp \
    src/epoller.cpp \
    src/heaptimer.cpp \
    src/httpconn.cpp \
    src/httprequest.cpp \
    src/httpresponse.cpp \
    src/log.cpp \
    src/sqlconnpool.cpp \
    src/webserver.cpp \
    weatherpage.cpp \
    weathertool.cpp

# ========================================================
# 5. 头文件列表
# ========================================================
HEADERS += \
    apppaths.h \
    calculate.h \
    camerapage.h \
    chatclient.h \
    client.h \
    customslider.h \
    door.h \
    desktopcontrol.h \
    desktopidle.h \
    expressionwindow.h \
    facethread.h \
    filemanager.h \
    friendiconlabel.h \
    friendinfowidget.h \
    imagepreview.h \
    livecamerawindow.h \
    mainwindow.h \
    musicpage.h \
    netcamerapage.h \
    photo.h \
    selfinfowidget.h \
    sendtextedit.h \
    serverpage.h \
    serverthread.h \
    sketchpad.h \
    stringtool.h \
    uicommand.h \
    videoplayer.h \
    include/blockqueue.h \
    include/buffer.h \
    include/epoller.h \
    include/heaptimer.h \
    include/httpconn.h \
    include/httprequest.h \
    include/httpresponse.h \
    include/log.h \
    include/sqlconnpool.h \
    include/threadpool.h \
    include/webserver.h \
    weatherdata.h \
    weatherpage.h \
    weathertool.h

# ========================================================
# 6. UI 界面文件
# ========================================================
FORMS += \
    calculate.ui \
    camerapage.ui \
    chatclient.ui \
    client.ui \
    door.ui \
    filemanager.ui \
    friendinfowidget.ui \
    imagepreview.ui \
    mainwindow.ui \
    musicpage.ui \
    netcamerapage.ui \
    photo.ui \
    selfinfowidget.ui \
    serverpage.ui \
    sketchpad.ui \
    videoplayer.ui \
    weatherpage.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# ========================================================
# 7. 资源文件 (清理了重复项)
# ========================================================
RESOURCES += \
    ImageRes.qrc \
    images.qrc
