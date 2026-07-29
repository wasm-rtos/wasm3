.PHONY: format

all:

format:
	# Convert tabs to spaces
	find ./source -type f -iname '*.[cpp\|c\|h]' -exec bash -c 'expand -t 4 "$$0" | sponge "$$0"' {} \;
	# Remove trailing whitespaces
	find ./source -type f -iname '*.[cpp\|c\|h]' -exec sed -i 's/ *$$//' {} \;

test_rv64:
	riscv64-linux-gnu-gcc -DDEBUG -Dd_m3HasWASI \
		-I./source ./source/*.c ./platforms/app/main.c \
		-O3 -g0 -flto -lm -static \
		-o wasm3-rv64

	cd test; python3 run-wasi-test.py --fast --exec "qemu-riscv64-static ../wasm3-rv64"
	rm ./wasm3-rv64

test_cpp:
	clang -xc++ -Dd_m3HasWASI \
		-I./source ./source/*.c ./platforms/app/main.c \
		-O3 -g0 -flto -lm -static \
		-o wasm3-cpp
	cd test; python3 run-wasi-test.py --fast --exec "../wasm3-cpp"
	rm ./wasm3-cpp
