#ifndef FFT_H
#define FFT_H

#include <stdint.h>

// FFT Cooley-Tukey radix-2 iterativa, escrita a mao para nao depender de
// libs externas (fftw3). Complexos em dois vetores double (real e imag).

// Transformada in-place. n deve ser potencia de 2.
void fft(double *re, double *im, int n);

// Maior potencia de 2 <= n (ex: 1000 -> 512). Devolve 0 se n < 1.
int maior_potencia_2(int n);

// Frequencia dominante em Hz de um trecho de audio: janela de Hann + FFT no
// maior tamanho potencia de 2 que cabe em n. Devolve 0.0 se n for pequeno.
double frequencia_dominante(const float *bloco, int n, uint32_t sample_rate);

#endif // FFT_H
