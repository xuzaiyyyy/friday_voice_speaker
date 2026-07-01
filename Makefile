.PHONY: all clean

all:
	cmake -S . -B build
	cmake --build build -j

clean:
	cmake --build build --target clean
