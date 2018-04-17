CC = g++
RM = rm

SRC += ./uvc_camera.c 
INC += -I./ 

CPPFLAGS += ${INC} -g -Wall 
ALL:uvc_camera 
uvc_camera:${SRC}
	${CC} ${CPPFLAGS} $^ -o $@ 

.PHONY:clean 

clean:
	${RM} *~ uvc_camera -rf


