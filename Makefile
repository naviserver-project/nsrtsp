ifndef NAVISERVER
    NAVISERVER  = /usr/local/ns
endif

#
# Module name
#
MOD      =  nsrtsp.so

#
# Objects to build.
#
OBJS     = nsrtsp.o

#LIVE     = /usr/lib/live-media
LIVE      = live

CFLAGS	 += -I$(LIVE)/BasicUsageEnvironment/include \
            -I$(LIVE)/UsageEnvironment/include \
            -I$(LIVE)/groupsock/include \
            -I$(LIVE)/liveMedia/include

MODLIBS  += $(LIVE)/liveMedia/libliveMedia.a \
            $(LIVE)/BasicUsageEnvironment/libBasicUsageEnvironment.a \
            $(LIVE)/UsageEnvironment/libUsageEnvironment.a \
            $(LIVE)/groupsock/libgroupsock.a

include  $(NAVISERVER)/include/Makefile.module

CC	 = g++
LDSO     = g++ -shared


