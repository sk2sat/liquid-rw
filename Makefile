TARGET	= liquid-rw
INIT_DIR= mk_init
INIT	= $(INIT_DIR)/init.prof
OBJS	= main.o

OUT_DIR	= out
PROF_0	= $(OUT_DIR)/output_00000.prof
GIF	= $(OUT_DIR)/out.gif

CC	= gcc
CXX	= g++
CXXFLAGS= -std=c++14 -g

%.o : %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

all:
	make $(TARGET)

init:
	rm $(INIT)
	make $(INIT)
run:$(TARGET) $(INIT)
	./$(TARGET) $(INIT)

gif:$(GIF)

gifup:$(GIF)
	./upload.py $(GIF)

autorun:
	make run > log.txt
	./upload.py log.txt
	rm log.txt
	make gifup

$(TARGET):$(OBJS)
	$(CXX) -o $@ $^

$(INIT):$(INIT_DIR)/mk_init.cc
	make -C $(INIT_DIR) run

$(GIF):$(PROF_0)
	gnuplot plot.gp
