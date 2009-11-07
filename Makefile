
objs=src/ti_da830.o
app=build/g_play

CFLAGS=`pkg-config --cflags gstreamer-0.10`
LDFLAGS=`pkg-config --libs gstreamer-0.10`

all: $(app)

$(app): $(objs)
	gcc -o $(app) $(LDFLAGS) $(objs)

clean:
	rm $(objs) $(app)

test_:
	@echo $(CFLAGS)
	@echo $(LDFLAGS)

.phony: clean
