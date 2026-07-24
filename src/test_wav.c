#include "wav_io.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// Teste isolado do wav_io, sem MPI nem OpenMP: escreve um seno, le de volta e
// confere se as amostras sobrevivem ao round-trip dentro da tolerancia de
// quantizacao de 16 bits.
//
// Compilar: gcc -Wall -O2 src/test_wav.c src/wav_io.c -o test_wav -lm
// Rodar:    ./test_wav

int main(void) {
    const uint32_t sample_rate = 44100;
    const uint32_t n = 22050;          // meio segundo
    const double freq = 440.0;

    WavInfo info;
    info.sample_rate  = sample_rate;
    info.num_canais   = 1;
    info.bits_amostra = 16;
    info.num_amostras = n;

    float *original = malloc(n * sizeof(float));
    if (!original) {
        fprintf(stderr, "sem memoria\n");
        return 1;
    }
    for (uint32_t i = 0; i < n; i++) {
        original[i] = 0.8f * (float)sin(2.0 * M_PI * freq * (double)i / sample_rate);
    }

    const char *caminho = "data/saida/_teste_round_trip.wav";
    if (escrever_wav(caminho, &info, original) != 0) {
        fprintf(stderr, "FALHOU: escrever_wav retornou erro\n");
        free(original);
        return 1;
    }

    WavInfo lido;
    float *devolta = NULL;
    if (ler_wav(caminho, &lido, &devolta) != 0) {
        fprintf(stderr, "FALHOU: ler_wav retornou erro\n");
        free(original);
        return 1;
    }

    if (lido.sample_rate != info.sample_rate ||
        lido.num_canais != info.num_canais ||
        lido.bits_amostra != info.bits_amostra ||
        lido.num_amostras != info.num_amostras) {
        fprintf(stderr, "FALHOU: cabecalho lido difere do escrito\n");
        free(original);
        free(devolta);
        return 1;
    }

    float max_erro = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float e = fabsf(original[i] - devolta[i]);
        if (e > max_erro) max_erro = e;
    }

    // Tolerancia de um passo de quantizacao
    const float tolerancia = 1.0f / 32768.0f + 1e-6f;
    printf("Round-trip: %u amostras, erro maximo = %.8f (tolerancia %.8f)\n",
           n, max_erro, tolerancia);

    int ok = (max_erro <= tolerancia);
    printf("%s\n", ok ? "OK: wav_io passou no teste de round-trip." :
                        "FALHOU: erro acima da tolerancia.");

    free(original);
    free(devolta);
    return ok ? 0 : 1;
}
