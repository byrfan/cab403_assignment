CC = gcc
flags = -lpthread
build = ./build/
src = ./src/
common = conn.c

car: 
	mkdir -p $(build)
	gcc $(src)car.c $(src)common.c -o $(build)car

controller:
	mkdir -p $(build)
	gcc $(src)controller.c $(src)common.c -o $(build)controller

call:
	mkdir -p $(build)
	gcc $(src)call.c -o $(src)common.c $(build)call

internal:
	mkdir -p $(build)
	gcc $(src)internal.c $(src)common.c -o $(build)internal

safety:
	mkdir -p $(build)
	gcc $(src)safety.c $(src)common.c -o $(build)safety

clean:
	rm -rf $(build)
