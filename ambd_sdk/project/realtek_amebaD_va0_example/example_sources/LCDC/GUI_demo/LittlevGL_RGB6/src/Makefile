
include $(MAKE_INCLUDE_GEN)

.PHONY: all clean

#*****************************************************************************#
#                               VARIABLES	                              #
#*****************************************************************************#
DIR = $(SRCDIR)
DIRLV = $(BASEDIR)/component/common/ui/LittlevGL/lvgl

MODULE_IFLAGS = -I$(DIRLV)/ \
		 -I$(IFTDIR)/ \
		 -I$(DIRLV)/lv_core \
 		 -I$(DIRLV)/lv_draw \
		 -I$(DIRLV)/lv_fonts \
		 -I$(DIRLV)/lv_hal \
		 -I$(DIRLV)/lv_misc \
		 -I$(DIRLV)/lv_objx \
		 -I$(DIRLV)/lv_themes \

vpath %.c $(DIR)
#*****************************************************************************#
#                               Source FILE LIST                              #
#*****************************************************************************#
CSRC = $(DIR)/main.c \
       $(DIR)/bsp_lcdc_font.c \
       $(DIR)/bsp_rgb_lcd.c \

#*****************************************************************************#
#                               Object FILE LIST                              #
#*****************************************************************************#
OBJS = $(notdir $(CSRC:.c=.o))

#*****************************************************************************#
#                        RULES TO GENERATE TARGETS                            #
#*****************************************************************************#
all: CLEAN_OBJS CORE_TARGETS COPY_RAM_OBJS
	
#*****************************************************************************#
#              GENERATE OBJECT FILE 
#*****************************************************************************#
CORE_TARGETS: $(OBJS)

#*****************************************************************************#
#                        RULES TO CLEAN TARGETS                               #
#*****************************************************************************#
clean: CLEAN_OBJS
	$(REMOVE) *.o
	$(REMOVE) *.i
	$(REMOVE) *.s
	$(REMOVE) *.d

-include $(DEPS)	
