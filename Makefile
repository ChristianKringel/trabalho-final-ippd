# Build do processamento de audio em lote (MPI + OpenMP).
#
# Alvos:
#   make           compila o binario processa_audio
#   make clean     remove objetos e binarios
#   make test      gera um audio pequeno e roda uma execucao de sanidade
#   make test_wav  round-trip do wav_io, sem MPI (so gcc)
#   make check     bateria de corretude (invariancia em np e em threads)
#   make bench     corretude + varredura de desempenho, grava em docs/

CC      = mpicc
CFLAGS  = -Wall -O2 -fopenmp
LDFLAGS = -lm

SRC = src/main.c src/wav_io.c src/mpi_utils.c src/filtros.c src/fft.c src/metrica.c
OBJ = $(SRC:.c=.o)
BIN = processa_audio

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN) test_wav

test: $(BIN)
	python3 scripts/gerar_wav_teste.py --duracao 1.0 --saida data/entrada/sanidade.wav
	mpirun -np 2 ./$(BIN) data/entrada/sanidade.wav data/saida/sanidade_saida.wav data/saida/sanidade_metrica.csv

test_wav:
	gcc -Wall -O2 -Isrc src/test_wav.c src/wav_io.c -o test_wav -lm
	./test_wav

check:
	./scripts/benchmark.sh --so-corretude

bench:
	./scripts/benchmark.sh

.PHONY: all clean test test_wav check bench
