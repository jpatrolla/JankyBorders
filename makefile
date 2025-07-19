FILES = src/main.c src/parse.c src/mach.c src/hashtable.c src/events.c src/windows.c src/yb_props.c src/sidebar.c src/border.c src/animation.c 
LIBS = -framework AppKit -framework CoreVideo -F/System/Library/PrivateFrameworks/ -framework SkyLight -Wl,-U,_SLSGetWindowContext, -ljson-c
LDFLAGS += -L/opt/homebrew/opt/json-c/lib
CFLAGS += -I/opt/homebrew/opt/json-c/include
all: | bin
	clang -std=c99 -O3 -g $(CFLAGS) $(FILES) -o bin/borders $(LDFLAGS) $(LIBS)

debug: | bin
	clang -std=c99 -O0 -g -DDEBUG $(CFLAGS) $(FILES) -o bin/debug $(LDFLAGS) $(LIBS)

asan: | bin
	clang -std=c99 -Wall -g -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -g $(CFLAGS) $(FILES) -o bin/debug $(LDFLAGS) $(LIBS)
	./bin/debug

bin:
	mkdir bin

clean:
	rm -rf bin
