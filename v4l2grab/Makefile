CC = g++
RM = rm

SRC += ./v4l2grab.c 
SRC += ./yuv.c 
INC += -I./ 
LIBS += -lv4l2 -ljpeg -lpthread
CPPFLAGS += ${INC} -g -fpermissive
ALL:v4l2grab 
v4l2grab:${SRC}
	${CC} ${CPPFLAGS} $^ -o $@ ${LIBS}

.PHONY:clean 

clean:
	${RM} *~ v4l2grab -rf


