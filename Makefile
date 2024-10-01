all: gcc clang zigcc tcc g++ zigc++ clang++ go zig odin dotnet bflat rust

gcc:
	gcc lzss_c.c -O3 -Wall -o lzss_gcc.exe
	strip --strip-all lzss_gcc.exe

clang:
	clang -D_CRT_SECURE_NO_WARNINGS lzss_c.c -O3 -Wall -o lzss_clang.exe
	llvm-strip --strip-all lzss_clang.exe

zigcc:
	zig cc lzss_c.c -O3 -s -Wall -o lzss_zigcc.exe
	llvm-strip --strip-all lzss_zigcc.exe

tcc:
	tcc lzss_c.c -o lzss_tcc.exe

g++:
	g++ lzss_cpp.cpp -O3 -s -Wall -o lzss_g++.exe
	strip --strip-all lzss_g++.exe

zigc++:
	zig c++ lzss_cpp.cpp -O3 -s -Wall -o lzss_zigc++.exe
	llvm-strip --strip-all lzss_zigc++.exe

clang++:
	clang++ -D_CRT_SECURE_NO_WARNINGS lzss_cpp.cpp -O3 -Wall -o lzss_clang++.exe
	llvm-strip --strip-all lzss_clang++.exe

go:
	go build -ldflags "-s -w" lzss_go.go

zig:
	zig build-exe lzss_zig.zig -O ReleaseFast --gc-sections -fstrip -flto

odin:
	odin build lzss_odin.odin -file -o:aggressive

dotnet:
	dotnet publish lzss_net/lzss_net.csproj -p:PublishProfile=lzss_net/Properties/FolderProfile.pubxml
	cp lzss_net/publish/lzss_net.exe .

bflat:
	bflat build lzss_net\Program.cs -o lzss_bflat.exe -m native -Ot --no-reflection --no-globalization

rust:
	rustc lzss_rust.rs -O -o lzss_rust.exe

file=corpus/alice29.txt

bench: all
	hyperfine -w 10 -N \
	"bun lzss_js.js $(file)" \
	"node lzss_js.js $(file)" \
    "./lzss_gcc.exe $(file)" \
	"./lzss_clang.exe $(file)" \
	"./lzss_tcc.exe $(file)" \
	"./lzss_zigcc.exe $(file)" \
	"./lzss_g++.exe $(file)" \
	"./lzss_zigc++.exe $(file)" \
	"./lzss_clang++.exe $(file)" \
	"./lzss_go.exe $(file)" \
	"./lzss_zig.exe $(file)" \
	"./lzss_odin.exe $(file)" \
	"./lzss_net.exe $(file)" \
	"./lzss_bflat.exe $(file)" \
	"./lzss_rust.exe $(file)" \
	"pypy lzss_py.py $(file)"

clean:
	rm -rf *.exe *.pdb *.ilk *.pdb *.lib *.obj
	rm -rf lzss_net/bin lzss_net/obj lzss_net/publish
