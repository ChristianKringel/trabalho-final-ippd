#ifndef WAV_IO_H
#define WAV_IO_H

#include <stdint.h>

// Leitura e escrita de .wav PCM 16 bits. Guarda o cabecalho da entrada para
// reconstruir a saida no mesmo formato.

typedef struct {
    uint32_t sample_rate;   // ex: 44100 Hz
    uint16_t num_canais;    // 1 = mono, 2 = estereo
    uint16_t bits_amostra;  // ex: 16
    uint32_t num_amostras;  // por canal
} WavInfo;

// Le o .wav inteiro. Aloca em *amostras floats em [-1, 1], intercalados por
// canal. Retorna 0 em sucesso, != 0 em erro (com mensagem em stderr).
// O chamador deve dar free em *amostras.
int ler_wav(const char *caminho, WavInfo *info, float **amostras);

// Escreve o .wav a partir de floats em [-1, 1], reconstruindo o cabecalho de
// 44 bytes. Valores fora da faixa sofrem clipping. Retorna 0 em sucesso.
int escrever_wav(const char *caminho, const WavInfo *info, const float *amostras);

#endif // WAV_IO_H
